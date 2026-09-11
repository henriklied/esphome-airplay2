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
self-hosted tree. AirPlay 2 is implemented end-to-end; AirPlay 1 (RSA auth,
FairPlay handshake, AES-CBC) is deliberately not ported.

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
- [x] Implement the protocol logic (RTSP control plane, HIPairing crypto,
      decoder, timing, audio output) as the ported slices in `transport/`,
      `crypto/`, `decoder/`, `timing/` and `audio/`. AirPlay 1 (RSA auth,
      FairPlay handshake, AES-CBC) is intentionally not ported.
- [ ] Confirm the full `esphome compile` builds for `esp32-s3-idf` (and keep
      `esphome config` + `esphome compile --only-generate` green).
- [ ] Wire up the managed components (`espressif/esp_audio_codec`,
      `espressif/mdns`, `espressif/libsodium`, `cmake_utilities`) and note the
      `mbedtls` usage.
- [ ] Add an ESPHome-doc entry / example under `docs/`.

## Status

- [x] Memory-policy foundation (allocator, platform profiles, codegen wiring).
- [x] AirPlay 2 HomeKit pairing + ChaCha20-Poly1305 audio crypto slice (`crypto/`): SRP-6a 3072-bit pair-setup, Ed25519 + X25519 + ChaCha20-Poly1305 pair-verify, ChaCha20-Poly1305 audio key setup/decrypt.
- [x] Transport/control (`transport/`): `_airplay._tcp` mDNS + RTSP server on port 7000 (ANNOUNCE/SETUP/RECORD/PAIR-SETUP/PAIR-VERIFY/SET_PARAMETER/GET_PARAMETER/TEARDOWN/FLUSH + OPTIONS/GET/POST/PAUSE/SETRATEANCHORTIME/SETPEERS).
- [x] Audio engine (`audio/`): RTP receive (realtime + buffered), CryptoModule decrypt, ALAC/AAC decode via `esp_audio_codec`, PTP/NTP clocks, playout timing, I2S PCM5100 DAC + amp-enable output.
- [x] libsodium managed component wired (`add_idf_component`) + mbedtls (built-in IDF) — full `esphome compile` is green.
- [x] AirPlay 2 only: AirPlay 1 (RSA auth, FairPlay handshake, AES-CBC) deliberately not ported.
- [ ] Clean-room license approval or upstream-author permission.
- [ ] **Justify the global sdkconfig writes to reviewers.** `_add_lwip_requirements()` raises
  `CONFIG_LWIP_MAX_SOCKETS` (10 → 24) and `CONFIG_LWIP_UDP_RECVMBOX_SIZE` (6 → 32), which are
  firmware-wide, not component-local, and cost RAM every other component also pays for. Both are
  load-bearing — the defaults fail silently and the mbox default alone cost 931 concealment events
  in 75s on hardware (see `FIELD-NOTES.md`) — but a reviewer will reasonably ask why a media
  component is resizing the IP stack. Have the measurement ready, and expect to argue mbox depth
  as a documented prerequisite rather than a silent override if they push back.

> This document does **not** claim the component is production-ready or
> upstreamable today. It states the blocker plainly and lists what a future PR
> would require.
