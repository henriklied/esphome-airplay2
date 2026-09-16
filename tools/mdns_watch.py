"""Differential mDNS watcher for AirPlay grouping fields.

Watches _airplay._tcp / _raop._tcp TXT records and logs every field transition
with a millisecond timestamp, so that toggling a stereo pair in the Home app
produces an ordered trace of which device changed what, and when.

Two input paths run at once:

  * a passive listener joined to 224.0.0.251:5353, which sees the unsolicited
    announcements devices send when their records actually change (this is what
    catches the exact moment, including TTL=0 goodbyes);
  * an active poller issuing legacy-unicast (QU) queries, as a safety net for
    announcements that were missed or coalesced.

Stdlib only. Must run on a host in the same L2 broadcast domain as the
speakers -- mDNS does not cross Tailscale or a routed subnet boundary.

Usage:
    python3 mdns_watch.py                      # watch everything
    python3 mdns_watch.py --filter "Stue oppe" --filter Projektor
    python3 mdns_watch.py --logfile pair.log
"""

from __future__ import annotations

import argparse
import logging
import socket
import struct
import sys
import time
from typing import NamedTuple

MDNS_GROUP = "224.0.0.251"
MDNS_PORT = 5353
QCLASS_IN_UNICAST = 0x8001  # QCLASS IN with the QU (unicast-response) bit set
QCLASS_MASK = 0x7FFF

TYPE_A = 1
TYPE_PTR = 12
TYPE_TXT = 16
TYPE_SRV = 33
TYPE_ANY = 255

SERVICE_TYPES = ("_airplay._tcp.local", "_raop._tcp.local")
SERVICE_SUFFIXES = tuple("." + s for s in SERVICE_TYPES)

DEFAULT_POLL_INTERVAL_S = 5.0
RECV_BUFFER_BYTES = 9000
SELECT_TIMEOUT_S = 0.25
MAX_NAME_LABELS = 128
COMPRESSION_MASK = 0xC0
COMPRESSION_OFFSET_MASK = 0x3FFF
GOODBYE_TTL = 0

# Fields that carry grouping or identity state. Changes to these are logged at
# a higher prominence than the routine churn of osvers/srcvers/etc.
GROUPING_FIELDS = frozenset(
    {
        "tsid", "gpn", "gid", "pgid", "pgcgl", "igl", "gcgl", "tsm",
        "psgid", "psgsz", "psgtp", "pgm", "pgmid",
        "pi", "psi", "fex", "flags", "sf", "features", "ft",
        "hkid", "hgid", "hmid", "am", "model",
    }
)

# airplay2-rs AIRPLAY_2_SPEC.md section 2.5.
FLAG_BITS = {
    0: "ProblemDetected",
    1: "NotConfigured",
    2: "AudioCableAttached",
    3: "PINRequired",
    6: "SupportsAirPlayFromCloud",
    7: "PasswordRequired",
    9: "OneTimePairingRequired",
    10: "DeviceWasSetupForHKAccessControl",
    11: "DeviceSupportsRelay",
    12: "SilentPrimary",
    13: "TightSyncIsGroupLeader",
    14: "TightSyncBuddyNotReachable",
    15: "IsAppleMusicSubscriber",
    16: "CloudLibraryIsOn",
    17: "ReceiverSessionIsActive",
}

# airplay2-rs AIRPLAY_2_SPEC.md section 2.2 (upper word only; the bits that
# matter for grouping all live above 32).
FEATURE_BITS_HIGH = {
    38: "SupportsUnifiedMediaControl",
    40: "SupportsBufferedAudio",
    41: "SupportsPTP",
    43: "SupportsSystemPairing",
    46: "SupportsHKPairingAndAccessControl",
    48: "SupportsTransientPairing",
    50: "MetadataFeatures_4",
    51: "SupportsUnifiedPairSetupAndMFi",
    52: "SupportsSetPeersExtendedMessage",
    54: "SupportsAPSync",
}

logger = logging.getLogger("mdns_watch")


class Record(NamedTuple):
    """One resource record, with the message it came from for name decompression."""

    name: str
    rtype: int
    ttl: int
    rdata: bytes
    message: bytes
    rdata_offset: int


def encode_name(name: str) -> bytes:
    encoded = b""
    for label in name.split("."):
        if label:
            encoded += bytes([len(label)]) + label.encode("utf-8")
    return encoded + b"\x00"


def parse_name(data: bytes, offset: int) -> tuple[str, int]:
    """Decodes a DNS name, following compression pointers into `data`.

    Returns the name and the offset just past the name in the original stream
    (which, when a pointer was followed, is not where decoding ended).
    """
    labels: list[str] = []
    jumped = False
    end = offset
    for _ in range(MAX_NAME_LABELS):
        if offset >= len(data):
            break
        length = data[offset]
        if length == 0:
            offset += 1
            if not jumped:
                end = offset
            break
        if length & COMPRESSION_MASK == COMPRESSION_MASK:
            if offset + 2 > len(data):
                break
            pointer = struct.unpack("!H", data[offset : offset + 2])[0] & COMPRESSION_OFFSET_MASK
            if not jumped:
                end = offset + 2
            offset = pointer
            jumped = True
            continue
        offset += 1
        labels.append(data[offset : offset + length].decode("utf-8", "replace"))
        offset += length
        if not jumped:
            end = offset
    return ".".join(labels), end


def parse_records(data: bytes) -> list[Record]:
    try:
        _, _, qdcount, ancount, nscount, arcount = struct.unpack("!HHHHHH", data[:12])
    except struct.error:
        return []
    offset = 12
    for _ in range(qdcount):
        _, offset = parse_name(data, offset)
        offset += 4
    records: list[Record] = []
    for _ in range(ancount + nscount + arcount):
        if offset >= len(data):
            break
        name, offset = parse_name(data, offset)
        if offset + 10 > len(data):
            break
        rtype, _rclass, ttl, rdlength = struct.unpack("!HHIH", data[offset : offset + 10])
        offset += 10
        records.append(Record(name, rtype, ttl, data[offset : offset + rdlength], data, offset))
        offset += rdlength
    return records


def decode_txt(rdata: bytes) -> dict[str, str]:
    entries: dict[str, str] = {}
    pos = 0
    while pos < len(rdata):
        length = rdata[pos]
        pos += 1
        item = rdata[pos : pos + length].decode("utf-8", "replace")
        pos += length
        key, _, value = item.partition("=")
        if key:
            entries[key] = value
    return entries


def describe_flags(value: str) -> str:
    try:
        bits = int(value, 16)
    except ValueError:
        return ""
    names = [FLAG_BITS.get(i, f"bit{i}") for i in range(32) if bits >> i & 1]
    return ", ".join(names)


def describe_features(value: str) -> str:
    parts = value.split(",")
    if len(parts) != 2:
        return ""
    try:
        high = int(parts[1], 16)
    except ValueError:
        return ""
    names = [FEATURE_BITS_HIGH.get(32 + i, f"bit{32 + i}") for i in range(32) if high >> i & 1]
    return ", ".join(names)


def describe(key: str, value: str) -> str:
    if key in ("flags", "sf"):
        return describe_flags(value)
    if key in ("features", "ft"):
        return describe_features(value)
    return ""


class MdnsWatcher:
    """Tracks TXT state per service instance and logs transitions."""

    def __init__(self, filters: list[str], poll_interval_s: float, all_services: bool) -> None:
        self.filters = filters
        self.poll_interval_s = poll_interval_s
        self.all_services = all_services
        self.txt_state: dict[str, dict[str, str]] = {}
        self.addr_state: dict[str, str] = {}
        self.instances: set[str] = set()
        self.listen_socket = self._open_listen_socket()
        self.query_socket = self._open_query_socket()

    def _open_listen_socket(self) -> socket.socket | None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            try:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
            except OSError:
                pass
        try:
            sock.bind(("", MDNS_PORT))
        except OSError as exc:
            logger.warning("Cannot bind %d (%s); running poll-only, transitions may be coarser",
                           MDNS_PORT, exc)
            sock.close()
            return None
        membership = struct.pack("4sl", socket.inet_aton(MDNS_GROUP), socket.INADDR_ANY)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, membership)
        sock.setblocking(False)
        logger.info("Passive listener joined %s:%d", MDNS_GROUP, MDNS_PORT)
        return sock

    def _open_query_socket(self) -> socket.socket:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
        sock.bind(("0.0.0.0", 0))
        sock.setblocking(False)
        return sock

    def _matches(self, instance: str) -> bool:
        if not self.all_services and not instance.endswith(SERVICE_SUFFIXES):
            return False
        if not self.filters:
            return True
        return any(f.lower() in instance.lower() for f in self.filters)

    def send_queries(self) -> None:
        targets = [encode_name(s) for s in SERVICE_TYPES]
        queries = [
            struct.pack("!HHHHHH", 0, 0, 1, 0, 0, 0) + name + struct.pack("!HH", TYPE_PTR, QCLASS_IN_UNICAST)
            for name in targets
        ]
        queries += [
            struct.pack("!HHHHHH", 0, 0, 1, 0, 0, 0)
            + encode_name(instance)
            + struct.pack("!HH", TYPE_ANY, QCLASS_IN_UNICAST)
            for instance in sorted(self.instances)
            if self._matches(instance)
        ]
        for packet in queries:
            try:
                self.query_socket.sendto(packet, (MDNS_GROUP, MDNS_PORT))
            except OSError as exc:
                logger.error("Query send failed: %s", exc)

    def ingest(self, payload: bytes) -> None:
        for record in parse_records(payload):
            if record.rtype == TYPE_PTR and record.name in SERVICE_TYPES:
                instance, _ = parse_name(record.message, record.rdata_offset)
                if instance:
                    self.instances.add(instance)
            elif record.rtype == TYPE_A and len(record.rdata) == 4:
                self.addr_state[record.name] = socket.inet_ntoa(record.rdata)
            elif record.rtype == TYPE_TXT:
                if not self._matches(record.name):
                    continue
                self.instances.add(record.name)
                if record.ttl == GOODBYE_TTL:
                    self._log_goodbye(record.name)
                    continue
                self._apply_txt(record.name, decode_txt(record.rdata))

    def _log_goodbye(self, instance: str) -> None:
        logger.warning("GOODBYE  %s  (TTL=0, record withdrawn)", short(instance))
        self.txt_state.pop(instance, None)

    def _apply_txt(self, instance: str, new: dict[str, str]) -> None:
        old = self.txt_state.get(instance)
        if old is None:
            self.txt_state[instance] = new
            group = {k: v for k, v in new.items() if k in GROUPING_FIELDS}
            logger.info("BASELINE %s  %s", short(instance), format_fields(group))
            return
        if old == new:
            return
        for key in sorted(set(old) | set(new)):
            before = old.get(key)
            after = new.get(key)
            if before == after:
                continue
            mark = "*" if key in GROUPING_FIELDS else " "
            if before is None:
                logger.info("%s ADD     %s  %s=%s%s", mark, short(instance), key, after,
                            annotate(key, after))
            elif after is None:
                logger.info("%s DROP    %s  %s (was %s)", mark, short(instance), key, before)
            else:
                logger.info("%s CHANGE  %s  %s: %s -> %s%s", mark, short(instance), key,
                            before, after, annotate(key, after))
        self.txt_state[instance] = new

    def run(self) -> None:
        import select

        self.send_queries()
        next_poll = time.monotonic() + self.poll_interval_s
        sockets = [s for s in (self.listen_socket, self.query_socket) if s is not None]
        while True:
            readable, _, _ = select.select(sockets, [], [], SELECT_TIMEOUT_S)
            for sock in readable:
                try:
                    payload, _ = sock.recvfrom(RECV_BUFFER_BYTES)
                except OSError:
                    continue
                self.ingest(payload)
            if time.monotonic() >= next_poll:
                self.send_queries()
                next_poll = time.monotonic() + self.poll_interval_s


def short(instance: str) -> str:
    name = instance
    for suffix in ("._airplay._tcp.local", "._raop._tcp.local"):
        if name.endswith(suffix):
            kind = "airplay" if "airplay" in suffix else "raop"
            return f"{name[: -len(suffix)]} [{kind}]".ljust(34)
    return name.ljust(34)


def annotate(key: str, value: str | None) -> str:
    if value is None:
        return ""
    text = describe(key, value)
    return f"   ({text})" if text else ""


def format_fields(fields: dict[str, str]) -> str:
    if not fields:
        return "(no grouping fields)"
    return " ".join(f"{k}={v}" for k, v in sorted(fields.items()))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--filter", action="append", default=[],
                        help="only watch instances containing this substring (repeatable)")
    parser.add_argument("--interval", type=float, default=DEFAULT_POLL_INTERVAL_S,
                        help=f"active poll interval in seconds (default {DEFAULT_POLL_INTERVAL_S})")
    parser.add_argument("--all-services", action="store_true",
                        help="also track non-AirPlay services (_companion-link, _device-info, ...)")
    parser.add_argument("--logfile", help="also append the trace to this file")
    args = parser.parse_args()

    handlers: list[logging.Handler] = [logging.StreamHandler(sys.stdout)]
    if args.logfile:
        handlers.append(logging.FileHandler(args.logfile))
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s.%(msecs)03d  %(message)s",
        datefmt="%H:%M:%S",
        handlers=handlers,
    )

    watcher = MdnsWatcher(args.filter, args.interval, args.all_services)
    logger.info("Watching %s. Toggle the pair in the Home app now. Ctrl-C to stop.",
                ", ".join(args.filter) if args.filter else "all AirPlay instances")
    try:
        watcher.run()
    except KeyboardInterrupt:
        logger.info("Stopped.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
