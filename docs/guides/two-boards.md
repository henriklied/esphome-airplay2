# Set up two boards as a pair

Make two boards play left and right of one stereo picture.

There are two ways to do it:

| | Two destinations | One destination (`pair:`) |
|---|---|---|
| Picker entries | Two, selected together | One |
| Config | `audio_channel_mode: left` on one, `right` on the other | `pair:` on both |
| Decoding | Each board decodes its own stream | The primary decodes, the secondary plays what it forwards |
| Sync | Shared PTP clock from the sender | Primary's clock, relayed over the link (or PTP) |

The two-destination way is plain multi-room with a channel set on each board. The rest of this page covers `pair:`.

!!! warning "Status"

    `pair:` runs on two boards. Output skew between the two speakers has not been measured, because that needs one microphone hearing both.

!!! note "Before you start"

    - Two boards, each flashed and working on its own.
    - A fixed address for the secondary: a DHCP reservation or `manual_ip:` in its `wifi:` block.
    - Both boards on the same subnet.

## 1. Configure the primary

Start from [examples/stereo-pair-primary.yaml](https://github.com/henriklied/esphome-airplay2/blob/main/examples/stereo-pair-primary.yaml). The primary is an ordinary receiver: it advertises, decodes, plays one channel and forwards the other.

```yaml
wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  power_save_mode: none

airplay_receiver:
  id: airplay
  name: "Living Room"
  audio_channel_mode: left
  pair:
    role: primary
    peer: 192.168.1.51      # the secondary's address
    channel: right          # what the secondary plays
```

`peer:` is required on a primary. If `channel:` equals `audio_channel_mode`, the build warns that the other side is left unplayed.

## 2. Configure the secondary

Start from [examples/stereo-pair-secondary.yaml](https://github.com/henriklied/esphome-airplay2/blob/main/examples/stereo-pair-secondary.yaml). The secondary runs no AirPlay discovery, RTSP or decoding.

```yaml
wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  power_save_mode: none
  manual_ip:
    static_ip: 192.168.1.51
    gateway: 192.168.1.1
    subnet: 255.255.255.0

airplay_receiver:
  id: airplay
  name: "Living Room Right"   # entity name only, senders never see it
  audio_channel_mode: stereo  # a one-channel forward arrives on both outputs
  pair:
    role: secondary
    peer: 192.168.1.50        # accept audio only from this primary
    clock: auto
    delay: 0us
```

Set `wifi: power_save_mode: none` on both boards. Modem sleep delays incoming frames unevenly, and the clock exchange reads that as clock error.

Leave `clock: auto`. It uses the secondary's own PTP lock when that is locked to the right master, and otherwise the primary's clock relayed over the link. On the first hardware run the secondary never received the sender's PTP (the phone's PTP reached only the session's receiver), so `auto` ran on the relay.

Use `delay:` (up to 100 ms) only if the two output paths differ acoustically. Each board's own DAC and DSP latency is already accounted for.

Role-specific keys are rejected on the wrong role: `clock:` on a primary, or `channel:` on a secondary, fails validation.

## 3. Flash both boards

```bash
esphome run living-room-left.yaml
esphome run living-room-right.yaml
```

If your computer cannot reach the boards by their `.local` names, for example because they are on another subnet or VLAN and mDNS is not forwarded, flash by IP:

```bash
esphome run living-room-left.yaml --device 192.168.1.50
esphome run living-room-right.yaml --device 192.168.1.51
```

!!! warning

    OTA fails while a stream is playing (`Device closed connection without responding`). Stop playback first.

## 4. Check that it works

1. Open the AirPlay picker on a phone. There should be one entry, "Living Room".
2. Play a left/right identification track. Left should come from the primary, right from the secondary.
3. Change the volume and pause. Both boards follow the primary.
4. Watch the logs. While audio flows, each board logs a line every 5 s:

    ```text
    peer tx: blocks=.. datagrams=.. send_err=.. ring_full=.. queue=.. hw=.. states=.. pings=.. anchor=..
    peer rx: datagrams=.. lost=.. frames=.. incomplete=.. stale=.. ... link=1 clock=relay ...
    ```

    On the secondary, `link=1` means it hears the primary, and `clock=` names the clock in use. `lost`, `incomplete` and `stale` should stay near zero.

5. To see the same in Home Assistant, put `diag_peer_link_up()` and `diag_peer_clock()` on template sensors. See [Entities](../configuration/entities.md).

### What to expect

- When you start a stream, or after a dropout, the secondary joins at the correct sample but is silent for up to the length of the primary's buffer, about 4.5 s on a buffered stream. It only receives audio decoded after it joined.
- If the secondary loses the link or its clock, it goes quiet and the primary keeps playing.
- The secondary's `media_player` entity mirrors the pair's state and ignores commands. Control the primary.

## Three destinations from one pair

Add `name:` on the primary and `standalone: true` on the secondary to get three picker entries: each board alone, and both together.

```yaml
# Primary
  pair:
    role: primary
    peer: 192.168.1.51
    channel: right
    name: "Living Room Pair"

# Secondary
  pair:
    role: secondary
    peer: 192.168.1.50
    standalone: true
```

The pair destination has its own pairing key and port (7001), so senders pair with it separately. Only one session plays per board, and the newest supersedes. A new pair session takes the secondary over from a local one.

## Change roles without a flash

`set_group_spec()` stores a role that overrides `pair:` from the next boot, then reboots the board. Wire it to a `text` entity to change roles from Home Assistant. The text form and an example entity are in [Stereo pair and groups](../configuration/stereo-pair.md) and [Text formats](../reference/text-formats.md).
