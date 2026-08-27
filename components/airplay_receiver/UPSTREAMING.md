# Upstreaming to ESPHome — status & blockers

This document is honest, planning-oriented notes about what it would take to get
`airplay_receiver` merged into the
[esphome/esphome](https://github.com/esphome/esphome) monorepo, and why it is
**likely blocked today**.

## The licensing blocker (the big one)

The protocol / algorithm logic being ported — the RTSP + FairPlay + HIPairing
crypto, the ALAC/AAC decode, the playout timing engine and the audio pipeline —
comes from
[`rbouteiller/airplay-esp32`](https://github.com/rbouteiller/airplay-esp32).

That repository is licenced under a **Non-Commercial License**:

> *"...to use, copy, modify, and distribute the Software for non-commercial
> purposes only..."* — `rbouteiller/airplay-esp32` LICENSE (Copyright (c) 2026
> Remi Bouteiller)

ESPHome is **permissively licensed** (MIT-style, Apache-2.0 for the ESPHome
core; components generally follow the same permissive model). Mixing a
non-commercial source with a permissive project is a **hard blocker**: the
non-commercial terms cannot be reconciled with ESPHome's permissive
redistribution. Shipping this component with copied upstream logic would put a
non-commercial obligation on every ESPHome user.

**Options:**

1. **Clean-room reimplementation** of the protocol/algorithm logic — i.e. write
   the RTSP handlers, the crypto, the decoder glue and the timing engine from
   the *public protocol descriptions* (AirPlay 2 spec, RFCs, Apple's published
   pairing documentation) without deriving from or copying the non-commercial
   source. The memory constants and *factual* configuration values are not
   creative expression and are fine to match; the *code* is not.
2. **Keep it as a self-hosted external_components** (the current layout) and
   never upstream it. This is the path that keeps the non-commercial terms
   intact for this codebase.
3. Obtain written **commercial permission** from the upstream author. This is
   the upstream author's call; contact `bouteiller.remi@gmail.com` for
   commercial licensing.

This document assumes the project continues under option 2 unless/until a
clean-room reimplementation (option 1) is done.

## What an ESPHome PR would need (checklist)

Assuming the licensing issue is resolved (clean-room), the following is the
minimum a reviewable ESPHome PR requires. This repo is **not yet in that
shape** — the component lives under `components/airplay_receiver/` in a
self-hosted tree and the protocol logic is not ported.

- [ ] Move the component into `esphome/components/airplay_receiver/` and add
      `__init__.py` with:
      * the `DOMAIN` set correctly,
      * a `CONFIG_SCHEMA` (currently present, with `cv.only_on_esp32` +
        `cv.only_with_framework("esp-idf")`),
      * a `to_code` (currently present),
      * `DEPENDENCIES = ["network"]` (currently present),
      * `AUTO_LOAD` for the audio / network pieces it needs,
      * a `CODEOWNERS` entry.
- [ ] Ship `tests/components/airplay_receiver/test.esp32-s3-idf.yaml` (a
      minimal build config) — see [`pr/tests/components/airplay_receiver/`](pr/tests/components/airplay_receiver/).
- [ ] Reach **clang-format** compliance on every `.cpp`/`.h` (the repo uses
      `.clang-format`; run `clang-format -i` and confirm `git clang-format` is
      clean).
- [ ] Implement the protocol logic (RTSP/FairPlay, HIPairing crypto, decoder,
      timing, audio output) as separate slices; this repo's module skeletons are
      the planned structure.
- [ ] Confirm the full `esphome compile` builds for `esp32-s3-idf` (and keep
      `esphome config` + `esphome compile --only-generate` green).
- [ ] Wire up the managed components (`espressif/esp_audio_codec`,
      `espressif/mdns`, `espressif/libsodium`, `cmake_utilities`) and note the
      `mbedtls` usage.
- [ ] Add an ESPHome-doc entry / example under `docs/`.

## Status

- [x] Memory-policy foundation (allocator, platform profiles, codegen wiring).
- [x] Compilable module skeletons.
- [x] AirPlay 2 HomeKit pairing + ChaCha20-Poly1305 audio crypto slice (`crypto/`): SRP-6a 3072-bit pair-setup, Ed25519 + X25519 + ChaCha20-Poly1305 pair-verify, ChaCha20-Poly1305 audio key setup/decrypt. AirPlay 1 (RSA auth, AES-CBC) deliberately not ported.
- [x] libsodium managed component wired (`add_idf_component`) + mbedtls (built-in IDF) — full `esphome compile` is green.
- [ ] Protocol logic (RTSP, FairPlay, decoder, timing, audio pipeline).
- [ ] Clean-room license approval or upstream-author permission.

> This document does **not** claim the component is production-ready or
> upstreamable today. It states the blocker plainly and lists what a future PR
> would require.
