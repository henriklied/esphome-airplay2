#pragma once
// Text form of the stereo-pair / group role, so Home Assistant can change it
// without a reflash. Stored in NVS by the component and applied at boot (the
// pair identity, its mDNS record and the second RTSP port are set up once).
//
//   standalone
//   role=primary;peer=10.0.40.146;channel=stereo;name=Kjøkkenet
//   role=secondary;peer=10.0.40.90;clock=auto;delay=0;standalone=1
//
// Keys: role (standalone|primary|secondary), peer (dotted IPv4; required for a
// primary, the only primary accepted for a secondary), port, channel
// (left|right|mono|stereo, what a primary forwards), clock (auto|ptp|relay),
// delay (secondary playout offset in microseconds), name (a primary's group
// destination; no `;`), standalone (0|1, a secondary keeps its own destination).
// Missing keys take the defaults below, which match the YAML schema.
//
// Pure C++ apart from the enums in audio_peer.h, so the host tests build it.
#include <cstddef>
#include <cstdint>

#include "audio_peer.h"

namespace esphome {
namespace airplay_receiver {

constexpr size_t GROUP_SPEC_MAX_LEN = 200;
constexpr size_t GROUP_NAME_MAX_LEN = 63;
constexpr uint16_t GROUP_DEFAULT_PORT = 7011;

struct GroupSpec {
  PeerRole role = PeerRole::STANDALONE;
  char peer[16] = "";
  uint16_t port = GROUP_DEFAULT_PORT;
  PeerForward channel = PeerForward::RIGHT;
  PeerClockMode clock = PeerClockMode::AUTO;
  int32_t delay_us = 0;
  char name[GROUP_NAME_MAX_LEN + 1] = "";
  bool standalone = false;
};

/**
 * Parse `text` into `out`. Nothing is written unless the whole text is valid.
 * @return false on an unknown key or value, a malformed IPv4, a primary
 *         without a peer, a name over 63 bytes, or text over GROUP_SPEC_MAX_LEN.
 */
bool group_spec_parse(const char *text, GroupSpec *out);

/// Write `spec` as text (only non-default keys). Returns the length, or 0 if
/// `capacity` is too small.
size_t group_spec_format(const GroupSpec &spec, char *out, size_t capacity);

}  // namespace airplay_receiver
}  // namespace esphome
