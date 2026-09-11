# AGENTS.md — read this first

You are working on `airplay_receiver`, an ESPHome external component that makes an ESP32 an
AirPlay 2 receiver. The component lives in `components/airplay_receiver/`.

## Start here
1. **`CHEATSHEET.md`** — YAML reference, board pins, the `airplay_audio_*` hook contract,
   build/verify commands, and the pitfalls that are already handled.
2. **`components/airplay_receiver/FIELD-NOTES.md`** — before touching the audio or transport path,
   and before debugging any artefact. It is what hardware taught us: five distinct faults that all
   present as "connected, metadata fine, no audio", how to read the 1 Hz telemetry, and a dead-ends
   list that includes one confidently-wrong conclusion a previous pass reached and committed.
3. **`README.md`** — the user-facing documentation. If you change the schema or the public API,
   change it there too.
4. `examples/` — five working configs, each validated and compiled. `examples/amped-s3.yaml` is the
   reference board.

## Non-negotiable rules
- **Single entry point:** the component is the media_player. Only configure `airplay_receiver:`.
  There is NO `media_player: - platform: airplay_receiver` — it was removed so a config with both
  fails validation (two RTSP servers on :7000, two I2S claims). Don't reintroduce a separate
  media_player platform file.
- **mDNS:** register services only via ESPHome's `mdns_service` API. Never call `mdns_init()` /
  `mdns_hostname_set()`.
- **Import alias:** `from esphome.components import media_player as media_player_mod` (the component
  lives in the airplay_receiver namespace; importing the platform by the bare name would shadow the
  core module).
- **Build flags:** keep `_add_memory_policy_flags()` emitting
  `-DAIRPLAY_PLATFORM_ESP32S3`/`-DAIRPLAY_PLATFORM_ESP32` and keep `_register_recursive_sources()` —
  the per-platform tuning and subdirectory .cpp collection depend on them.
- **lwIP sizing belongs to the component:** keep `_add_lwip_requirements()` setting
  `CONFIG_LWIP_MAX_SOCKETS` and `CONFIG_LWIP_UDP_RECVMBOX_SIZE`. Both IDF defaults are too small for
  a realtime receiver and **both fail silently** — the RTSP socket survives, so metadata and
  transport keep working while audio is absent or full of concealment. Do not move these back into
  board YAML: that is where they were, and it meant the component mis-performed for anyone who
  dropped it in without knowing. See `components/airplay_receiver/FIELD-NOTES.md`.
- **Superseded RTSP slots must not emit `TRANSPORT_EVENT_DISCONNECTED`.** `client_task()`'s cleanup
  checks `slot->should_stop` for exactly this. Removing that guard makes every wifi roam kill the
  audio of the session that replaced it, while leaving the control channel healthy.
- **`audio_control.h`** is the public audio driver surface. Keep the eight `airplay_audio_*`
  signatures unchanged.
- **PTP / timing:** the clock starts on the first `loop()` pass with the network up
  (`start_ptp_when_network_up_()`), and `ensure_ptp_started()` still runs at stream SETUP + RECORD
  and in SETPEERS. Keep all of these or you get silence.
- **Verify before declaring success:** `esphome config config.gate.yaml` (valid) then
  `esphome compile config.gate.yaml` (exit 0). Only report "done" after both pass.
- Do not edit anything outside this repository unless explicitly asked.

## Licensing — read before adding code
The protocol logic is a port of `rbouteiller/airplay-esp32`, which is **Non-Commercial** licensed,
and this repository inherits those terms (`LICENSE`). Consequences that affect what you may do:

- Do not add an SPDX header, a `license:` field, or any text claiming MIT/Apache-2.0.
- Do not prepare an ESPHome monorepo PR as if the licence were settled;
  `components/airplay_receiver/UPSTREAMING.md` records the blocker and the three ways out.
- Third-party code vendored into a source file keeps its own notice. See `THIRD-PARTY-NOTICES.md`,
  and `audio/audio_resample.cpp` for the pattern.

## Environment
- ESPHome ≥ **2025.1.0**, verified against **2026.8.2**. Framework `esp-idf`, ESP-IDF 5.5,
  ESP32-S3 with `flash_mode: dio`.
- `api:` must not use `encryption:` — it pulls `esphome/noise-c` and its own `libsodium`, which
  collides with `espressif/libsodium` at CMake configure. See README.
