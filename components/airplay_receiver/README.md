# airplay_receiver

An ESPHome component that turns an **ESP32 / ESP32-S3** into an **AirPlay 2
receiver**. It advertises `_airplay._tcp` (port 7000), runs the HomeKit pairing
handshake, and streams AirPlay 2 audio out over I2S to an external DAC.

> **AirPlay 2 only.** The AirPlay 1 / RAOP path (RSA auth, FairPlay handshake,
> AES-CBC audio encryption) is deliberately **not** ported. If you only ever
> use AirPlay 2, this is the component you want. See
> [`UPSTREAMING.md`](UPSTREAMING.md) for details and the licensing blocker.

## Target parts

Two board classes are supported, each with its own tuned memory profile:

| Board class        | Part                             | PSRAM   | PCM ring profile |
| ------------------ | -------------------------------- | ------- | ---------------- |
| ESP32-S3 (primary) | `ESP32-S3-WROOM-1-N8R8`          | 8 MiB octal | 1000-frame ring, 64 KiB internal reserve |
| generic ESP32      | `ESP32-WROVER-N8R8` / 4 MiB PSRAM | 4 MiB   | 200-frame ring, 32 KiB internal reserve |

The tuning is selected at build time by the target variant (see "Memory policy"
below). Any other ESP32 / ESP32-S3 board is possible in principle; the values
are tuned for these parts.

> **Board-specific firmware.** Choose the `esp32`/`esp32s3` variant and the
> `psram:` block that match your exact board (e.g. octal vs quad PSRAM). The
> example below targets the **Amped-ESP32** (ESP32-S3, octal PSRAM).

## What is implemented

End-to-end AirPlay 2:

* **Crypto (`crypto/`)** — HomeKit pairing: **SRP-6a 3072-bit pair-setup**,
  **Ed25519 + X25519 + ChaCha20-Poly1305 pair-verify**, and **ChaCha20-Poly1305
  audio-key** setup/decrypt. `airplay_receiver.h` wires the crypto module into
  the transport. (AirPlay 1 RSA/AES-CBC not ported.)
* **Transport / control (`transport/`)** — `_airplay._tcp` mDNS advertisement
  and an **RTSP server on port 7000** serving
  `ANNOUNCE / SETUP / RECORD / PAIR-SETUP / PAIR-VERIFY / GET_PARAMETER /
  SET_PARAMETER / TEARDOWN / FLUSH / OPTIONS / GET / POST / PAUSE / SETPEERS`.
* **Audio engine (`audio/`)** — RTP receive (realtime + buffered), `CryptoModule`
  decrypt, **ALAC / AAC decode via `espressif/esp_audio_codec`**, PTP/NTP clock,
  playout timing engine (audio_timeline v2), and an **I2S PCM5100 DAC + amp-enable**
  output backend.
* **Allocator (`allocator.h` / `allocator.cpp`)** — a single choke point that
  routes every allocation through `airplay_alloc` / `airplay_calloc` /
  `airplay_free` enforcing the internal-DRAM-vs-PSRAM policy.

## Memory policy

`__init__.py::to_code` reads the target variant and emits a build flag:

* `-DAIRPLAY_PLATFORM_ESP32S3` (or `-DAIRPLAY_PLATFORM_ESP32`), and
* `-DUSE_AIRPLAY_HEAP_TRACE`, enabling the allocation reporters.

The per-platform knobs (`AIRPLAY_RING_FRAMES`, `AIRPLAY_ALWAYS_INTERNAL_BYTES`,
`AIRPLAY_INTERNAL_RESERVE_BYTES`, per-task stack sizes, core affinity,
`AIRPLAY_DECODER_IN_PSRAM`, `AIRPLAY_BT_ENABLED`) are defined inline in
**`allocator.h`** selected by that macro. `allocator.h` is included by every
module in the component, so the correct profile genuinely applies on the target
hardware. The two-level `platform/esp32s3/config.h` / `platform/esp32/config.h`
headers are the *canonical reference* for a native ESP-IDF build; ESPHome's
`external_components` copy does not bring them into the generated tree, so
`allocator.h` carries the same values inlined.

* `realtime == true` -> **internal DRAM** (fast, cacheable, DMA-safe).
* `realtime == false` -> **PSRAM-first**, but any request
  `<= AIRPLAY_ALWAYS_INTERNAL_BYTES` (default **1024**) stays in internal DRAM,
  mirroring `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`.

## Configuration

The YAML schema, the `dsp:` cascade, the runtime-tuning API, the diagnostics accessors and five
working example configs are documented in the [repository README](../../README.md). This file covers
what is specific to the component's internals.

Audio output is unconfigured until you supply `i2s_bclk_pin`, `i2s_lrclk_pin` and `i2s_dout_pin`.
A receiver without them pairs and plays silently, which is occasionally useful for testing pairing
on a board with no DAC attached, and confusing if you did not mean it.

> **Requires ESP-IDF.** This component is `only_on_esp32` and
> `only_with_framework("esp-idf")`; the Arduino framework is not supported.

## Where the logic comes from

The upstream protocol/audio implementation being ported is
[`rbouteiller/airplay-esp32`](https://github.com/rbouteiller/airplay-esp32)
(Espressif ESP-IDF project). The code is ported into `esphome::airplay_receiver`
and its heap is routed through the centralized allocator. The memory constants
(`MAX_RING_BUFFER_FRAMES`, the `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024`
tuning, per-task stack sizes and core-affinity choices) were read directly from
that repository.

## Status

* **Implemented:** HomeKit pairing + ChaCha20-Poly1305 audio crypto, RTSP
  control plane + mDNS, RTP audio engine (ALAC/AAC decode), playout timing,
  the output biquad cascade, I2S PCM5100 output + amp enable, centralized
  allocator + platform profiles.
* **Not implemented:** AirPlay 1 / RAOP (RSA auth, FairPlay handshake, AES-CBC
  audio encryption), Bluetooth A2DP, SPDIF/USB outputs.
* **Known caveat:** the two board classes use *different* memory profiles and
  PSRAM wiring — pick the `esp32`/`esp32s3` variant and `psram:` block that
  match your actual board.
* **Multi-room works**, including two boards as a stereo pair via
  `audio_channel_mode: left` / `right`. The local-anchor fallback gives up
  group sync by design: a board that loses its network clock keeps playing on
  its own clock instead of going silent, and drifts from the group until it
  re-locks. That is the trade, not a bug.

> **Licensing note:** the upstream logic is **Non-Commercial** licensed, and so
> is this repository — see [`../../LICENSE`](../../LICENSE) and
> [`UPSTREAMING.md`](UPSTREAMING.md). It cannot be relicensed permissively here,
> which is also why it cannot be merged into ESPHome.
