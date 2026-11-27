# ESPHome AirPlay 2 Receiver
[![License](https://img.shields.io/badge/license-Non--Commercial-blue?style=for-the-badge)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5+-red?style=for-the-badge)](https://docs.espressif.com/projects/esp-idf/)
[![ESPHome](https://img.shields.io/badge/ESPHome-2025.1.0+-1ABC9C.svg?style=for-the-badge)](https://esphome.io/)

Custom [esphome](https://esphome.io/) component that turns an ESP32 into an AirPlay 2 speaker.

It advertises `_airplay._tcp`, runs the HomeKit pairing handshake, decodes ALAC and AAC, and plays
out over I2S to an external DAC. The board shows up in Control Center next to the HomePods, and in
Home Assistant as a `media_player`.

## More info

- [Supported hardware](#supported-hardware)
- [Base configuration](#base-configuration)
  - [Requirements](#requirements)
  - [Adding a component](#adding-a-component)
  - [Configuration](#configuration)
  - [Output DSP](#output-dsp)
  - [Runtime tuning](#runtime-tuning)
- [Entities](#entities)
- [Diagnostics](#diagnostics)
- [Examples](examples)
  - [Amped-ESP32-S3](examples/amped-s3.yaml)
  - [Generic ESP32 + PCM5102A](examples/generic-esp32.yaml)
  - [Speaker correction](examples/dsp-speaker-correction.yaml)
  - [Physical buttons](examples/buttons.yaml)
  - [Character LCD](examples/display.yaml)
- [What is not implemented](#what-is-not-implemented)
- [Credits](#credits)
- [License](#license)

## Supported hardware

| Board class        | Part                            | PSRAM       | PCM ring |
|--------------------|---------------------------------|-------------|----------|
| ESP32-S3 (primary) | ESP32-S3-WROOM-1-N8R8           | 8 MB octal  | 1000 frames, 64 KiB internal reserve |
| ESP32              | ESP32-WROVER class              | 4 MB quad   | 200 frames, 32 KiB internal reserve |

The profile is selected at build time from the target variant, so there is nothing to configure. The
S3 is the board this is developed and measured on, and it has the headroom to ride out a wifi hiccup
that the plain ESP32 does not.

PSRAM is not optional on either. The DAC is expected to be a PCM5100-class I2S part.

Multi-room works well, and a group of boards stays in sync off the shared PTP clock. Two boards
can be a stereo pair two ways: as two AirPlay destinations with `audio_channel_mode: left` and
`right`, selected together, or as **one** destination with [`pair:`](#stereo-pair), where the
second board plays what the first forwards.

## Base configuration

### Requirements

* **Board**: esp32, esp32s3, with PSRAM;
* **Framework**: esp-idf (the Arduino framework is not supported);
* **ESPHome**: 2025.1.0 or newer;
* **`api:` without `encryption:`**. See below.

`api:` must not enable `encryption:`. It pulls in `esphome/noise-c`, which ships its own
`libsodium`. The pairing crypto here uses `espressif/libsodium`, and the IDF component manager
refuses to choose between them:

```
ERROR: Cannot process component requirements. Requirement espressif__libsodium
and requirement libsodium are both added as "project_managed_components".
Can't decide which one to pick.
```

It breaks at CMake configure, before anything compiles. The examples all use a plain `api:` block.

### Adding a component

```yaml
external_components:
  - source: github://henriklied/esphome-airplay2
    components: [airplay_receiver]
```

### Configuration

```yaml
airplay_receiver:
  id: airplay
  name: "Living Room"
  i2s_bclk_pin: 14
  i2s_lrclk_pin: 15
  i2s_dout_pin: 16
  amp_enable_pin: 17
  sample_rate: 44100
  amp_idle_timeout: 60
```

* **id** (Optional, string): Component ID. Needed for lambdas;
* **name** (Optional, string): The name senders see in Control Center, and the `media_player` entity name;
* **i2s_bclk_pin** (Optional, pin): I2S bit clock to the DAC (default: unused);
* **i2s_lrclk_pin** (Optional, pin): I2S word select to the DAC (default: unused);
* **i2s_dout_pin** (Optional, pin): I2S data to the DAC (default: unused);
* **i2s_mclk_pin** (Optional, pin): Master clock, for DACs that do not self-strap (default: unused);
* **amp_enable_pin** (Optional, pin): Amplifier enable/unmute line (default: unused);
* **amp_enable_inverted** (Optional, bool): True if the amp-enable line is active-LOW (default: false);
* **amp_idle_timeout** (Optional, int): Seconds of silence before the amp line is de-asserted. `0` keeps the amp on (default: 60);
* **initial_volume** (Optional, percentage): Start volume until a sender has set one. Senders send a
  volume only when the slider moves, never on connect, so after that the board keeps the last sender
  volume across reboots (not mute) and starts there (default: 20%);
* **sample_rate** (Optional, int): Output sample rate (default: 44100);
* **audio_channel_mode** (Optional, string): `stereo`, `mono`, `left` or `right`. Use `left`/`right` for a stereo pair of boards (default: stereo);
* **buffer_size** (Optional, int): Internal buffer size in bytes (default: 1000000);
* **dsp** (Optional): Output filter cascade and dynamic stages, see [Output DSP](#output-dsp).
* **pair** (Optional): Stereo-pair role, see [Stereo pair](#stereo-pair).

Leaving the three I2S pins unset is valid. You get a receiver that pairs and plays silently, which is
occasionally useful for testing pairing on a board with no DAC attached.

The component **is** the media_player. There is no `media_player: - platform: airplay_receiver`, and
a config that adds one fails validation. That is deliberate: two instances would mean two RTSP
servers on port 7000 and two claims on the same I2S peripheral.

### Output DSP

A direct AirPlay stream goes from the phone to the board. It never passes through Music Assistant, so
MA's per-player EQ, high-pass and limiter never apply. `dsp:` runs the speaker correction here
instead, as a cascade of up to eight biquads.

```yaml
airplay_receiver:
  id: airplay
  dsp:
    preamp: -4dB
    filters:
      - type: high_pass
        frequency: 80Hz
        q: 0.7
      - type: low_shelf
        frequency: 150Hz
        q: 0.7
        gain: 4dB
```

* **enabled** (Optional, bool): `false` bypasses the whole stage bit-exactly, which makes an honest A/B (default: true);
* **preamp** (Optional, decibel): Broadband gain ahead of the cascade (default: 0dB);
* **filters** (Optional, list): Up to 8 sections, applied in order.

Each filter:

* **type** (Required, string): `low_shelf`, `high_shelf`, `high_pass`, `low_pass`, `peaking` or `notch`;
* **frequency** (Required, frequency): Corner or centre frequency. Validated against `sample_rate`;
* **q** (Optional, float): Butterworth (0.7071) by default. Shelves use the Q form, matching Music Assistant;
* **gain** (Optional, decibel): Shelf and peaking gain. Rejected at validation on `high_pass`, `low_pass` and `notch`, which have no gain term (default: 0dB).

A positive `gain` without a matching negative `preamp` clips at high volume. The component logs the
preamp it wants at boot. The stage clamps instead of wrapping, so what you hear is distortion on
peaks, not noise.

### Dynamic stages

Eight optional stages make a small speaker sound larger than its static EQ allows. Each is off until
configured. Order: preamp, `filters`, loudness, bass enhancer, stereo width, ambience,
crosstalk cancellation, bass protection, bass limiter, limiter.

```yaml
  dsp:
    loudness:
      frequency: 120Hz
      max_boost: 8dB
      range: 30dB
    bass_enhancer:
      frequency: 80Hz
      amount: 1.0
    stereo_width:
      width: 1.4
      frequency: 300Hz
      centre_lock: 0.8
    ambience:
      amount: 0.15
      decay: 400ms
    crosstalk_cancel:
      amount: 0.6
      delay: 60us
    bass_protection:
      frequency: 70Hz
    bass_limiter:
      frequency: 120Hz
      threshold: -6dB
      release: 200ms
    limiter:
      threshold: -1dB
      release: 100ms
```

* **loudness**: Low shelf whose lift follows the playback volume: flat at full volume, `max_boost`
  once the volume is `range` below it. Compensates for the ear losing bass at low level.
* **bass_enhancer**: Replaces bass below `frequency` (the speaker's lower limit) with its 2nd and 3rd
  harmonics, which the ear reads as the missing fundamental. `amount` 1.0 is roughly equal level.
  The harmonics track the bass level linearly; they are not distortion that grows with drive.
* **stereo_width**: Width above `frequency`. 1.0 is unchanged, 0 is mono; bass keeps its width.
  `centre_lock` (0-1) tracks the dominant source (principal component, ~30 ms) and widens only what is
  orthogonal to it, so panned and centred sources keep their place and level; diffuse sound, which has
  no dominant source, still widens fully. At 0 it is plain mid/side.
* **ambience**: Room-fill reverb: a damped 4-line feedback delay network fed only by the ambient
  component above the width corner, with decorrelated left/right tails. A dry mono source feeds it
  nothing. `amount` is the wet level (0.1-0.3 is subtle), `decay` the 60 dB decay time, `damping`
  (default 5000Hz) the in-loop low-pass. The 38 KB delay buffer is allocated on first enable.
* **crosstalk_cancel**: Recursive crosstalk cancellation (RACE). Each channel subtracts a delayed,
  band-limited (`low_frequency` 250Hz to `high_frequency` 5000Hz), attenuated copy of the other
  channel's output, so the sound from each driver is cancelled at the far ear and the image extends
  past the cabinet. `delay` is the extra path from a driver to the far ear (10-300us); `amount`
  (0-0.95) sets the depth. In band it lifts side by up to 1/(1-amount) and lowers mid by up to
  1/(1+amount), so pair it with a lower `stereo_width`.
* **bass_protection**: 4th-order Butterworth high-pass after every stage that adds bass. A loudness or
  shelf lift below a `filters` high-pass cancels it (a +13 dB loudness shelf undoes an 80 Hz 2nd-order
  high-pass at 50 Hz), and the deepest notes then overdrive small drivers and their supply. Set
  `frequency` at the speaker's real lower limit.
* **bass_limiter**: Splits at `frequency` (Linkwitz-Riley, so the bands sum back flat), limits the
  lows to `threshold` on their own and delays the highs to match. Bass peaks no longer duck the whole
  mix. Adds another 1.5 ms, also included in the playout clock.
* **limiter**: Stereo-linked look-ahead peak limiter, last in the chain. Peaks never exceed
  `threshold`. Adds 1.5 ms of delay, which the playout clock includes, so AirPlay sync holds. With it
  enabled, boosts no longer need a matching negative `preamp`.

Runtime setters: `set_dsp_loudness_boost(db)`, `set_dsp_bass_enhancer_amount(x)`,
`set_dsp_stereo_width_amount(x)`, `set_dsp_centre_lock(x)`, `set_dsp_ambience_amount(x)`,
`set_dsp_ambience_decay(ms)`, `set_dsp_crosstalk_amount(x)`, `set_dsp_crosstalk_delay(us)`,
`set_dsp_bass_limiter_threshold(db)`, `set_dsp_bass_protection(hz)`, `set_dsp_limiter_enabled(bool)`, `set_dsp_limiter_threshold(db)`.
`get_dsp_loudness_now()` returns the lift in effect; `take_dsp_limiter_reduction()` returns the
deepest gain reduction since the last call (`take_dsp_bass_limiter_reduction()` for the bass band).

### Runtime tuning

Every DSP parameter can be changed from a lambda, so a `number` entity can drive it and you can tune
by ear in the room instead of spending an OTA cycle per change. The index is positional into
`filters:`, so reordering that list repoints every caller.

```cpp
id(airplay).set_dsp_filter_frequency(0, 90.0f);
id(airplay).set_dsp_filter_gain(1, 3.0f);
id(airplay).set_dsp_filter_q(1, 0.8f);
id(airplay).set_dsp_preamp(-3.0f);
id(airplay).set_dsp_enabled(false);
```

To change the type or number of filters at runtime, replace the whole cascade from text instead.
One section per `;`, fields `type,frequency_hz,q,gain_db`, types `pk ls hs hp lp no`; `flat` is an
empty cascade. Eight sections fit in a 255-character `text` entity, which also gives you restore
across reboots:

```yaml
text:
  - platform: template
    name: "EQ filters"
    id: dsp_filters
    mode: text
    max_length: 255
    optimistic: true
    restore_value: true
    initial_value: ""          # empty: keep the YAML filters
    on_value:
      then:
        - lambda: |-
            if (!id(airplay).set_dsp_filters_spec(x)) {   // refused: show what runs
              id(dsp_filters).publish_state(id(airplay).get_dsp_filters_spec());
            }
esphome:
  on_boot:
    priority: -100             # after setup(), so the restored text wins over YAML
    then:
      - lambda: |-
          if (id(dsp_filters).state.empty() ||
              !id(airplay).set_dsp_filters_spec(id(dsp_filters).state)) {
            id(dsp_filters).publish_state(id(airplay).get_dsp_filters_spec());
          }
```

The positional setters still work, but they address `dsp_filters_` by index, which a text edit can
reorder; use one style per board.

**Switching an existing board to the text form.** If your HA sliders call the positional setters
with `restore_value: true`, the values you tuned in HA live only in those entities. When the
reflashed board stops reporting them, Home Assistant removes the entities and the values are gone.
The board then plays the `dsp: filters:` block from YAML until the text is set. So, before you
reflash:

1. Read the current values off the HA sliders (or the cascade the component logs at boot).
2. Write them into the `dsp: filters:` block, in the same order.
3. Remove the slider entities from your YAML, add the `text` entity above, and flash.

The text starts empty, so the board keeps playing the YAML filters, now with your tuned values.

These recompute coefficients and must run on the main loop, which is exactly where a YAML lambda
runs. See [examples/dsp-speaker-correction.yaml](examples/dsp-speaker-correction.yaml) for the whole
set wired up as Home Assistant sliders.

## Stereo pair

`pair:` makes two boards one AirPlay destination. The **primary** is an ordinary receiver: it
advertises, decodes, plays one channel, and forwards the other over UDP to the **secondary**, which
runs no AirPlay at all. Volume, pause, seek and track changes follow the primary.

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

* **role** (Required, string): `primary` or `secondary`;
* **peer** (Optional, IPv4): Primary: the secondary's address (required). Secondary: the only
  primary to accept (default: the first that speaks);
* **port** (Optional, int): UDP port for audio and state; the clock exchange uses the one above it
  (default: 7011);
* **channel** (Optional, string, primary): What the secondary plays: `left`, `right`, `mono`, or
  `stereo` to send both, for stereo-dependent DSP on the secondary (default: right);
* **clock** (Optional, string, secondary): `ptp` plays against the secondary's own PTP lock to the
  sender, `relay` against the primary's network time measured over the link, `auto` uses PTP when
  it is locked to the right master and otherwise the relay (default: auto);
* **delay** (Optional, time, secondary): Plays this much later, up to 100 ms, for output paths that
  differ acoustically. Each board's own DAC and DSP latency is already accounted for (default: 0us).
* **name** (Optional, string, primary): Advertise the pair as a destination of its own, under this
  name, and forward only sessions on it. The board's own `name` then plays on this board alone
  (default: none, every session is forwarded);
* **standalone** (Optional, bool, secondary): Keep this board's own AirPlay destination and play the
  primary's only while it forwards. Pairs with a primary `name:` (default: false).

With both set, one board pair gives three picker entries: each board on its own, and both together.
A new forwarded session takes the secondary over from a local session. A local session started on
the secondary yields it until the primary's next forwarded session. Only one session plays per
board, and the newest supersedes. The pair destination has its own deviceid (the Wi-Fi MAC with
the locally-administered bit flipped), pairing key, `pi` and port (7001), so senders pair with it
separately. Start volume is kept per destination.

Only the primary decodes. It sends 16-bit PCM, about 0.7 Mbit/s for one channel at 44.1 kHz. Give the secondary a fixed address, and set `wifi: power_save_mode: none` on both,
because modem sleep delays the clock exchange unevenly. The secondary's `media_player` entity
mirrors the pair's state and ignores commands.

The secondary joins a stream already playing, and rejoins after a dropout, at the correct sample,
but it only receives audio decoded after it joined, so it is silent for up to the length of the
primary's buffer (about 4.5 s on a buffered stream). If it loses the link or its clock, it goes
quiet and the primary keeps playing.

**Status: running on two boards since November 2026.** Output skew between the speakers is not
measured yet. On the boards tested, the secondary never received the sender's PTP (it reaches only
the board the session is on), so `clock: auto` runs on the relay. See
[STEREO-PAIR-PLAN.md](STEREO-PAIR-PLAN.md). Complete configs:
[examples/stereo-pair-primary.yaml](examples/stereo-pair-primary.yaml) and
[examples/stereo-pair-secondary.yaml](examples/stereo-pair-secondary.yaml).

Both boards log a `peer tx:` / `peer rx:` line every 5 s while audio flows: blocks and datagrams,
loss, frames that arrived incomplete or late, the clock in use, and the relay's round trip and rate.

### Changing the role at runtime

`set_group_spec(text)` stores a role in NVS that overrides `pair:` from the next boot, and reboots
the board a second later to apply it (the pair identity, mDNS record and second RTSP port are set up
once). `get_group_spec()` returns the running role. Text form:

```
standalone
role=primary;peer=10.0.40.146;channel=stereo;name=Kitchen
role=secondary;peer=10.0.40.90;clock=auto;delay=0;standalone=1
yaml        # forget the stored role, use pair: again
```

Wire it to a template `text` entity with `lambda: return id(airplay).get_group_spec();` and a
`set_action` that calls `set_group_spec(x)`. Invalid text is refused without a reboot.

## Entities

The component registers one `media_player` supporting play, pause, stop, volume set, volume step and
mute. ESPHome's `media_player` has no title field, so track metadata from the sender is held in the
audio layer instead:

```cpp
const char *title = airplay_audio_get_track_title();
```

Browse, play-media and announce are deliberately cleared from the feature flags. This is a receiver.
It plays what a sender pushes to it, and cannot be told to go and fetch a URL.

## Diagnostics

An AirPlay fault almost always looks the same from outside: the sender is connected, metadata and
transport work, and no audio comes out. The RTSP control socket is independent of the audio path, so
that symptom narrows nothing down on its own. These accessors separate the causes:

```cpp
id(airplay).diag_is_stalled();          // sender says play, scheduler renders silence
id(airplay).diag_sched_state();         // named scheduler state
id(airplay).diag_wait_reason();         // why it is not playing
id(airplay).diag_ptp_locked();          // clock lock. no lock means no timeline
id(airplay).diag_ptp_rejected();        // SYNC dropped by the master filter
id(airplay).diag_ptp_socket_rebuilds(); // multicast re-joins by the watchdog
id(airplay).diag_ptp_quiet_event_ms();  // age of the last datagram, per socket
id(airplay).diag_ptp_quiet_general_ms();
id(airplay).diag_holes();               // concealed packets
id(airplay).diag_underruns();           // I2S underruns
id(airplay).diag_peer_link_up();        // pair secondary: hearing the primary
id(airplay).diag_peer_clock();          // pair secondary: "ptp", "relay" or "none"
```

Every one is a rarely-changing state or a counter that only moves during a fault, so they are safe on
`template` sensors. Home Assistant writes a recorder row on change: a healthy board costs nothing,
and an incident leaves a timestamped trail. Keep continuously-varying values (playout error, drift,
buffer depth) out of Home Assistant. Those belong in the log stream.

`diag_is_stalled()` goes true for about four seconds on every normal connect while the engine
prerolls, so any automation on it wants `for: 00:00:30`.

`diag_ptp_locked()` is the one to watch in a group. A board that loses its network clock keeps
playing on the local clock instead of going silent. That is the right trade for a single speaker,
and it gives up group sync while it lasts: in a pair, one board drifts until it re-locks.

[components/airplay_receiver/FIELD-NOTES.md](components/airplay_receiver/FIELD-NOTES.md) is worth
reading before you debug anything. It is what hardware taught us: five distinct faults that all
present identically, how to read the telemetry, and a list of dead ends nobody needs to walk down
twice.

## What is not implemented

* **AirPlay 1 / RAOP.** RSA auth, the FairPlay handshake and AES-CBC audio encryption are
  deliberately not ported. If you only ever use AirPlay 2, this is the component you want;
* **Bluetooth A2DP**, SPDIF and USB outputs.

## Credits

* Thanks to [@rbouteiller](https://github.com/rbouteiller) for
  [airplay-esp32](https://github.com/rbouteiller/airplay-esp32). This component is a port of it, and
  the protocol logic, pairing crypto, timing engine and audio pipeline all originate there;
* Thanks to [@dbry](https://github.com/dbry) for the
  [sinc resampler](https://github.com/dbry/audio-resampler);
* Thanks to all [ESPHome](https://github.com/esphome/esphome) contributors.

## License

**Non-Commercial.** Personal and hobby use, modification and redistribution are permitted; commercial
use requires written permission from the upstream author. Those terms are inherited from
[rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32) and cannot be relaxed here,
because a derivative work cannot grant rights its source did not grant.

It also means this component cannot be merged into ESPHome, which is permissively licensed. See
[LICENSE](LICENSE), [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and
[components/airplay_receiver/UPSTREAMING.md](components/airplay_receiver/UPSTREAMING.md).
