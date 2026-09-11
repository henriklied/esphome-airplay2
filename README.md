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

Multi-room works well. Two boards play as a stereo pair with `audio_channel_mode: left` and
`right`, and a group of boards stays in sync off the shared PTP clock.

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
* **sample_rate** (Optional, int): Output sample rate (default: 44100);
* **audio_channel_mode** (Optional, string): `stereo`, `mono`, `left` or `right`. Use `left`/`right` for a stereo pair of boards (default: stereo);
* **buffer_size** (Optional, int): Internal buffer size in bytes (default: 1000000);
* **dsp** (Optional): Output filter cascade, see [Output DSP](#output-dsp).

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

These recompute coefficients and must run on the main loop, which is exactly where a YAML lambda
runs. See [examples/dsp-speaker-correction.yaml](examples/dsp-speaker-correction.yaml) for the whole
set wired up as Home Assistant sliders.

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
