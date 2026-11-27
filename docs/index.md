# ESPHome AirPlay 2 receiver

`airplay_receiver` is an ESPHome external component that turns an ESP32-S3 or ESP32 with PSRAM into an AirPlay 2 speaker. It advertises `_airplay._tcp`, runs the HomeKit pairing handshake, decodes ALAC and AAC, and plays out over I2S to an external PCM5100-class DAC. The board shows up in Control Center next to your HomePods, and in Home Assistant as a `media_player`.

The protocol logic is a port of [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32). See [Credits and license](reference/credits-license.md).

## The parts

### The component

You configure one block, `airplay_receiver:`. It is the AirPlay receiver and the `media_player` entity at the same time. There is no separate `media_player:` platform. See [Configuration](configuration/index.md) and [Boards and pins](configuration/hardware.md).

### Home Assistant entities

The component registers one `media_player` with play, pause, stop, volume set, volume step and mute. Everything else you add yourself as template entities that call the component from a lambda: DSP sliders, an EQ text field, a group role selector, diagnostic sensors. See [Entities](configuration/entities.md).

### Optional extras

- [Output DSP](configuration/output-dsp.md): up to eight biquad filters for speaker correction, run on the board. Direct AirPlay never passes through Music Assistant, so its per-player DSP does not apply.
- [Dynamic stages](configuration/dynamic-stages.md): loudness, bass enhancer, stereo width, ambience, crosstalk cancellation, bass protection and two limiters.
- [Tuning from Home Assistant](configuration/runtime-tuning.md): change DSP values and the whole filter layout without reflashing.
- [Stereo pair and groups](configuration/stereo-pair.md): two boards as one AirPlay destination, or as three (each board alone, and both together). The role can be changed at runtime.

## What works and what doesn't

Works:

- AirPlay 2 from iOS and macOS senders.
- Multi-room: a group of boards stays in sync off the shared PTP clock.
- A stereo pair, either as two destinations with `audio_channel_mode: left` and `right`, or as one destination with `pair:`.

Not implemented:

- AirPlay 1 (RAOP).
- Bluetooth A2DP, SPDIF and USB outputs.
- Play-media, browse and announce. The board plays what a sender pushes to it and cannot fetch a URL.

Also note: the Arduino framework is not supported, and `api:` cannot use `encryption:`. Details in [Limitations](help/limitations.md).

Start with [Installation](installation.md).
