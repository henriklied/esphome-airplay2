# Stereo pair and groups

Two boards can play as a stereo pair in two ways.

- **Two destinations.** Set `audio_channel_mode: left` on one board and `right` on the other, and
  select both in the AirPlay picker. They stay in sync off the shared PTP clock, like any multi-room
  group.
- **One destination with `pair:`.** The senders see one speaker. The second board plays what the
  first forwards.

This page covers `pair:`. For a step-by-step setup, see
[Set up two boards as a pair](../guides/two-boards.md).

## How `pair:` works

The **primary** is an ordinary receiver: it advertises, decodes, plays one channel, and forwards the
other over UDP to the **secondary**. The secondary runs no AirPlay discovery, RTSP or decoding unless
you make it `standalone`. Volume, pause, seek and track changes follow the primary.

Only the primary decodes. It sends 16-bit PCM, about 0.7 Mbit/s for one channel at 44.1 kHz.

```yaml
# Primary: plays left, sends right.
airplay_receiver:
  name: "Living Room"
  audio_channel_mode: left
  pair:
    role: primary
    peer: 192.168.1.51
    channel: right

# Secondary: plays what arrives.
airplay_receiver:
  name: "Living Room Right"
  pair:
    role: secondary
    peer: 192.168.1.50
```

Complete configs: `examples/stereo-pair-primary.yaml` and `examples/stereo-pair-secondary.yaml`.

## Options

| Parameter | Role | Description | Default |
|---|---|---|---|
| `role` | both | Required. `primary` or `secondary`. | |
| `peer` | both | Primary: the secondary's IPv4 address (required). Secondary: the only primary to accept. | Secondary: the first primary that speaks |
| `port` | both | UDP port for audio and state. The clock exchange uses the port above it, so the maximum is 65534. | `7011` |
| `channel` | primary | What the secondary plays: `left`, `right`, `mono`, or `stereo` to send both. | `right` |
| `name` | primary | Advertise the pair as a destination of its own under this name, 1 to 63 characters. See [Three destinations](#three-destinations). | none |
| `clock` | secondary | `ptp`, `relay` or `auto`. See [Clock](#clock). | `auto` |
| `delay` | secondary | Play this much later, up to 100 ms. | `0us` |
| `standalone` | secondary | Keep this board's own AirPlay destination. | `false` |

Validation rejects a primary without `peer:`, and a key set on the role it does not apply to. It warns
when a primary plays the same side it forwards, for example `audio_channel_mode: right` with
`channel: right`.

Use `channel: stereo` when the secondary should get both channels, for example for stereo-dependent
DSP on that board. On the secondary, `audio_channel_mode: stereo` plays a one-channel forward on both
outputs.

## Clock

The secondary has to play each sample at the same moment as the primary.

- `ptp` plays against the secondary's own PTP lock to the sender.
- `relay` plays against the primary's clock, measured over the link with 10 Hz ping exchanges on the
  port above `port`. It also follows the primary when the primary falls back to its local clock.
- `auto` uses PTP when it is locked to the right master, and otherwise the relay.

!!! note

    On the developer's hardware the secondary never received the sender's PTP: the phone's PTP
    reaches only the session's receiver. There `clock: ptp` cannot work and `auto` runs on the relay.

`delay` is for output paths that differ acoustically, such as a DSP stage on one side only. Each
board's own DAC and DSP latency is already accounted for, so leave it at 0 unless you measure a
difference. It can only delay: a board cannot play earlier than its own pipeline allows.

## Network

- Give the secondary a fixed address (DHCP reservation or `manual_ip:`). The primary sends to it by
  IP.
- Set `wifi: power_save_mode: none` on both boards. Modem sleep delays frames until the next beacon,
  unevenly, and the clock exchange reads that as clock error.
- Put both boards on the same subnet.

## Joins, dropouts and the secondary entity

The secondary joins a stream already playing, and rejoins after a dropout, at the correct sample. It
only receives audio decoded after it joined, so it is silent for up to the length of the primary's
buffer (about 4.5 s on a buffered stream).

After 1.5 s without a datagram the secondary goes quiet. The primary keeps playing. The next state
update puts the secondary back at the right sample.

The secondary's `media_player` entity mirrors the pair's state and ignores commands. Control the
primary.

Both boards log a `peer tx:` / `peer rx:` line every 5 s while audio flows: blocks and datagrams,
loss, frames that arrived incomplete or late, the clock in use, and the relay's round trip and rate.

## Three destinations

Set `name:` on the primary and `standalone: true` on the secondary. One pair of boards then gives
three picker entries: each board on its own, and both together.

```yaml
# Primary
airplay_receiver:
  name: "Kitchen Left"
  pair:
    role: primary
    peer: 192.168.1.51
    channel: stereo
    name: "Kitchen"

# Secondary
airplay_receiver:
  name: "Kitchen Right"
  pair:
    role: secondary
    peer: 192.168.1.50
    standalone: true
```

Here both boards play both channels. `audio_channel_mode` is set once at boot and applies to every
session on the board, so a `left`/`right` split also applies when a board plays on its own.

- The primary forwards only sessions on the pair name. Its own `name` plays on this board alone.
- The pair destination has its own deviceid (the Wi-Fi MAC with the locally-administered bit
  flipped), pairing key, `pi` and port 7001, so senders pair with it separately.
- A new forwarded session takes the secondary over from a local session. A local session started on
  the secondary yields it until the primary's next forwarded session.
- Only one session plays per board, and the newest supersedes.
- Start volume is kept per destination.

## Change the role at runtime

`set_group_spec(text)` stores a role in NVS that overrides `pair:` from the next boot.
`get_group_spec()` returns the role the board runs now, in the same form.

```text
standalone
role=primary;peer=10.0.40.146;channel=stereo;name=Kitchen
role=secondary;peer=10.0.40.90;clock=auto;delay=0;standalone=1
yaml
```

`yaml` (or an empty string) forgets the stored role and goes back to `pair:`. Grammar and limits:
[Text formats](../reference/text-formats.md#group-role).

What happens when you set it:

1. Text that does not parse is refused. The call returns `false` and the board does not reboot.
2. Text that matches the running role returns `true` and changes nothing.
3. Anything else is saved, and the board reboots one second later to apply it. The pair identity,
   its mDNS record and the second RTSP port are set up only at boot.

If a stored role fails to parse at boot, the board logs a warning and uses the YAML role.

Wire it to a template `text` entity:

```yaml
text:
  - platform: template
    name: "Group role"
    id: group_role
    mode: text
    max_length: 200
    lambda: return id(airplay).get_group_spec();
    set_action:
      - lambda: id(airplay).set_group_spec(x);
```

!!! warning

    A role change reboots the board, which stops whatever it is playing.

## Status

`pair:` runs on two Amped-ESP32-S3 boards in the developer's house, including three destinations.
The skew between the two speakers has not been measured acoustically. CPU and internal heap with the
link active, and short underrun bursts at session start and handover, are still open. See
`STEREO-PAIR-PLAN.md` and [Limitations](../help/limitations.md).

`diag_ptp_locked()` is the one to watch in a group. A board that loses its network clock keeps
playing on its local clock instead of going silent, and drifts from the other board until it
re-locks. See [Entities](entities.md#diagnostics).
