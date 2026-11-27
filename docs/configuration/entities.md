# Entities

The component creates one entity, a `media_player`. Everything else is a template entity you add in
YAML that calls the component from a lambda.

## Media player

The component is the media player. There is no `media_player: - platform: airplay_receiver`, and a
config that adds one fails validation.

| Feature | Supported |
|---|---|
| Play, pause, stop | Yes |
| Volume set, volume step, mute | Yes |
| Browse media, play media, announce | No |

The board is a receiver. It plays what a sender pushes to it, and cannot be told to fetch a URL. The
entity name is the `name:` senders see in Control Center.

On a pair secondary the entity mirrors the pair's state and ignores commands. See
[Stereo pair and groups](stereo-pair.md).

### Track title

ESPHome's `media_player` has no title field, so the audio layer holds the title the sender pushes:

```cpp
const char *title = airplay_audio_get_track_title();
```

`examples/display.yaml` shows it on a character LCD.

## Entities in the examples

| Example | Entities |
|---|---|
| `dsp-speaker-correction.yaml` | `number`: DSP high-pass, DSP bass lift, DSP treble tilt, DSP preamp. `switch`: DSP enabled. All `entity_category: config`. |
| `buttons.yaml` | `binary_sensor`: play/pause, volume up, volume down buttons on GPIO. |
| `display.yaml` | None in Home Assistant. An LCD shows title, state, volume, PTP lock and `holes`. |

The DSP entities and the `text` entities for the EQ and the group role are covered in
[Tuning from Home Assistant](runtime-tuning.md) and [Stereo pair and groups](stereo-pair.md).

## Diagnostics

An AirPlay fault almost always looks the same from outside: the sender is connected, metadata and
transport work, and no audio comes out. These accessors separate the causes:

| Accessor | Returns |
|---|---|
| `diag_is_stalled()` | The sender says play and the scheduler renders silence |
| `diag_sched_state()` | Named scheduler state |
| `diag_wait_reason()` | Why it is not playing |
| `diag_ptp_locked()` | Clock lock. No lock means no timeline |
| `diag_ptp_rejected()` | SYNC dropped by the master filter |
| `diag_ptp_socket_rebuilds()` | Multicast re-joins by the watchdog |
| `diag_ptp_quiet_event_ms()` | Age of the last datagram on the PTP event socket |
| `diag_ptp_quiet_general_ms()` | Same, general socket |
| `diag_holes()` | Concealed packets |
| `diag_underruns()` | I2S underruns |
| `diag_peer_link_up()` | Pair secondary: hearing the primary |
| `diag_peer_clock()` | Pair secondary: `ptp`, `relay` or `none` |

Each is a rarely-changing state or a counter that only moves during a fault, so they are safe on
`template` entities. Home Assistant writes a recorder row on change: a healthy board costs nothing,
and an incident leaves a timestamped trail.

```yaml
binary_sensor:
  - platform: template
    name: "AirPlay stalled"
    entity_category: diagnostic
    lambda: return id(airplay).diag_is_stalled();
  - platform: template
    name: "AirPlay PTP locked"
    entity_category: diagnostic
    lambda: return id(airplay).diag_ptp_locked();

sensor:
  - platform: template
    name: "AirPlay holes"
    entity_category: diagnostic
    accuracy_decimals: 0
    lambda: return id(airplay).diag_holes();
  - platform: template
    name: "AirPlay underruns"
    entity_category: diagnostic
    accuracy_decimals: 0
    lambda: return id(airplay).diag_underruns();

text_sensor:
  - platform: template
    name: "AirPlay wait reason"
    entity_category: diagnostic
    lambda: return id(airplay).diag_wait_reason();
```

!!! warning

    `diag_is_stalled()` goes true for about four seconds on every normal connect while the engine
    prerolls. Give any automation on it `for: "00:00:30"`.

`diag_ptp_locked()` is the one to watch in a group. A board that loses its network clock keeps
playing on its local clock instead of going silent. That is the right trade for a single speaker, but
in a pair one board drifts until it re-locks.

## Keep 1 Hz telemetry out of Home Assistant

!!! danger

    Never put continuously-varying values in Home Assistant: playout error, drift, buffer
    depth. Each sample would be a recorder row. Those values belong in the log stream.

The board logs three lines a second at `INFO` (`playout:`, `resend:`, `rxpath:`), which carry drift,
buffer depth, concealment, underruns and DSP cost. Read them with `esphome logs`. `logger: level: WARN` silences them without removing the
counters. How to read them: `components/airplay_receiver/FIELD-NOTES.md` and
[Troubleshooting](../help/troubleshooting.md).
