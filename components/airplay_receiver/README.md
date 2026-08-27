# airplay_receiver

An ESPHome component that turns an ESP32 / ESP32-S3 into an **AirPlay 2
receiver**. This repository currently contains the **memory-policy foundation
slice** (centralized allocator + platform profiles + codegen wiring + module
skeletons). The AirPlay 2 protocol/audio logic is **not yet ported** — see
[`UPSTREAMING.md`](UPSTREAMING.md) and the PENDING markers across the skeleton
modules.

## Target part

`ESP32-S3-WROOM-1-N8R8`

| Resource  | Size        |
| --------- | ----------- |
| Flash     | 8 MiB       |
| Octal PSRAM | 8 MiB     |
| Internal SRAM | 512 KiB |

Running on any other ESP32-S3 board is in principle possible; the values below
are tuned for this part.

## What is built (so far)

A centralized memory-policy layer that the rest of the component is expected to
allocate through, so the internal-RAM vs PSRAM decision lives in exactly one
place.

* `allocator.h` / `allocator.cpp`
  * `airplay_alloc(size, realtime)` / `airplay_calloc(n, size, realtime)` /
    `airplay_free(ptr)`
  * `realtime == true`  -> **internal DRAM** (fast, cacheable, DMA-safe).
  * `realtime == false` -> **PSRAM-first**, but any request
    `<= AIRPLAY_ALWAYS_INTERNAL_BYTES` (default **1024**) stays in internal DRAM,
    mirroring `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`.
  * `airplay_internal_free()` / `airplay_internal_largest_block()` /
    `airplay_psram_free()` — heap reports used for `USE_AIRPLAY_HEAP_TRACE`.
  * The `esp_heap_caps_*` path is guarded on `USE_ESP_IDF`; elsewhere it
    behaves as a plain `malloc`/`calloc`/`free` shim.
* `platform/esp32s3/config.h` / `platform/esp32/config.h`
  * Compile-time memory/tuning profiles. `esp32s3` (target):
    `AIRPLAY_RING_FRAMES 1000`, `AIRPLAY_ALWAYS_INTERNAL_BYTES 1024`,
    `AIRPLAY_INTERNAL_RESERVE_BYTES 65536`, the per-task stack sizes
    (`rtsp_client 8192`, `rtsp_server 4096`, `audio_recv 12288`, `audio_ctrl
    4096`, `audio_buffered 4096`, `playback 4096`, `ntp 3072`), core affinity
    (audio core 1, rtsp core 0), `AIRPLAY_DECODER_IN_PSRAM 1` and
    `AIRPLAY_BT_ENABLED 0` (ESP32-S3 has no Classic BT).
* Module **skeletons** (place-holders): `transport/`, `crypto/`, `decoder/`,
  `timing/`, `audio/` each expose a no-op class shell in
  `esphome::airplay_receiver` so the file layout for later slices is already in
  the build.

## How the memory policy is selected

`__init__.py::to_code` reads the target variant and emits

* `-DAIRPLAY_PLATFORM_ESP32S3` (or `-DAIRPLAY_PLATFORM_ESP32`), which picks the
  matching profile in `platform/<variant>/config.h` / the inlined profile
  branch in `allocator.cpp`, and
* `-DUSE_AIRPLAY_HEAP_TRACE` (+ the `defines.h` define), enabling the
  allocation reporters.

The allocator's `#if defined(AIRPLAY_PLATFORM_ESP32S3)` branch carries the same
values as the platform header. This is deliberate: ESPHome's
`external_components` build copies only top-level (and one-level-subdir) source
files, so the two-level `platform/<variant>/config.h` headers are not copied into
the generated tree. The profile headers remain the canonical reference for the
port; a future native-ESP-IDF build can `#include` them directly.

## YAML usage

```yaml
esphome:
  name: airplay2
  min_version: 2024.8.0

esp32:
  board: esp32-s3-devkitc-1
  variant: esp32s3
  framework:
    type: esp-idf
    version: recommended

psram:
  mode: octal
  speed: 80MHz

external_components:
  - source:
      type: local
      path: components
    components:
      - airplay_receiver

airplay_receiver:
  id: airplay_1
  name: "AirPlay2"
  buffer_size: 1000000
```

Options:

| Key           | Type | Default    | Description                          |
| ------------- | ---- | ---------- | ----------------------------------- |
| `name`        | str  | `"AirPlay2"` | Display name of the receiver.     |
| `buffer_size` | int  | `1000000`  | Buffer size (bytes) for the stream. |

## Where the logic comes from

The upstream protocol/audio implementation being ported is
[`rbouteiller/airplay-esp32`](https://github.com/rbouteiller/airplay-esp32)
(Espressif ESP-IDF project). The memory constants mirrored here
(`MAX_RING_BUFFER_FRAMES`, the `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024`
tuning, the per-task stack sizes and core-affinity choices) were read directly
from that repository.

## Status

* **Implemented:** allocator + memory policy, platform profiles, codegen
  wiring, compilable skeleton modules.
* **Pending (later tasks):** RTSP/FairPlay transport, HIPairing crypto
  (Ed25519/SRP/AES & mbedtls/libsodium), the ALAC/AAC decoder, the playout
  timing engine, the PCM ring buffer + audio output backend, and the
  `media_player` surface.

> **Licensing note:** the upstream logic is **Non-Commercial** licensed. Be
> aware of the implications before shipping or upstreaming — see
> [`UPSTREAMING.md`](UPSTREAMING.md).
