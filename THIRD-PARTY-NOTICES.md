# Third-party notices

This repository as a whole is distributed under the terms in [LICENSE](LICENSE). The components
listed here carry their own terms in addition.

## Vendored into this repository

### rbouteiller/airplay-esp32

The AirPlay 2 protocol logic — HomeKit pairing and audio crypto (`crypto/`), the RTSP control plane
and mDNS advertisement (`transport/`), the RTP/decode/timing audio engine (`audio/`, `decoder/`,
`timing/`) — is a port of <https://github.com/rbouteiller/airplay-esp32>, Copyright (c) 2026
Remi Bouteiller, under the Non-Commercial License reproduced in [LICENSE](LICENSE). This is the
term that governs the repository as a whole.

AirPlay 1 / RAOP (RSA auth, the FairPlay handshake, AES-CBC audio encryption), Bluetooth A2DP and
the SPDIF/USB outputs were not ported.

### David Bryant's sinc resampler

`components/airplay_receiver/audio/audio_resample.cpp` vendors the fixed-ratio sinc resampler from
<https://github.com/dbry/audio-resampler>, Copyright (c) 2006 - 2025 David Bryant, under the
3-clause BSD license in [licenses/audio-resampler.txt](licenses/audio-resampler.txt).

It sits in a file-local anonymous namespace so the translation unit stays self-contained. Its
`malloc`/`calloc`/`free` are remapped to the component's `airplay_*` allocator wrappers so the
memory policy applies; `ENABLE_THREADS` and `ENABLE_EXTRAPOLATION` are left undefined, so the
multithreading and extrapolation paths are compiled out.

## Fetched at build time

These are pulled by the ESP-IDF component manager from `__init__.py` and are not redistributed here.
Their licenses apply to the resulting firmware.

| Component | Source | License |
|---|---|---|
| `espressif/libsodium` | Espressif Component Registry | ISC |
| `espressif/esp_audio_codec` | Espressif Component Registry | Espressif Modified MIT (Espressif products only) |
| `espressif/mdns` | Espressif Component Registry | Apache-2.0 |
| `mbedtls` | built into ESP-IDF | Apache-2.0 |

`espressif/esp_audio_codec` ships under Espressif's Modified MIT license, which permits
use only on Espressif hardware. That is not a constraint in practice here — the component only
builds for ESP32 targets — but it is one more reason this tree cannot be relicensed permissively.
