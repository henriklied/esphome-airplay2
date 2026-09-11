# AGENTS.md — read this first

You are working with an ESPHome external component `airplay_receiver` (an AirPlay 2 receiver).

## Start here
1. Read **`CHEATSHEET.md`** in this directory first — it has the YAML reference, board pins, the
   `airplay_audio_*` hook contract, build/verify commands, and the pitfalls list.
2. Read **`airplay_receiver/FIELD-NOTES.md`** before touching the audio or transport path, and
   before debugging any artefact. It is what hardware taught us: the three bugs that all present as
   "connected, metadata fine, no audio", how to read the 1Hz telemetry, and a dead-ends list that
   includes one confidently-wrong conclusion a previous pass reached and committed.
3. `examples/config-sendspin.yaml` is the drop-in config for the Sendspin board (`flash_mode: dio`,
   `partitions.csv` included); `examples/config-ampled-s3.yaml` for Amped-S3.

## Non-negotiable rules
- **Single entry point:** the component is the media_player. Only configure `airplay_receiver:`. There is
  NO `media_player: - platform: airplay_receiver` — it was removed so a config with both fails validation
  (two RTSP servers on :7000, two I2S claims). Don't reintroduce a separate media_player platform file.
- **mDNS:** register services only via ESPHome's `mdns_service` API. Never call `mdns_init()` / `mdns_hostname_set()`.
- **Import alias:** `from esphome.components import media_player as media_player_mod` (the component lives in
  the airplay_receiver namespace; importing the platform by the bare name would shadow the core module).
- **Build flags:** keep `_add_memory_policy_flags()` emitting `-DAIRPLAY_PLATFORM_ESP32S3`/`-DAIRPLAY_PLATFORM_ESP32` and keep `_register_recursive_sources()` — the per-platform tuning and subdirectory .cpp collection depend on them.
- **lwIP sizing belongs to the component:** keep `_add_lwip_requirements()` setting
  `CONFIG_LWIP_MAX_SOCKETS` and `CONFIG_LWIP_UDP_RECVMBOX_SIZE`. Both IDF defaults are too small for
  a realtime receiver and **both fail silently** — the RTSP socket survives, so metadata and
  transport keep working while audio is absent or full of concealment. Do not move these back into
  board YAML: that is where they were, and it meant the component mis-performed for anyone who
  dropped it in without knowing. See `airplay_receiver/FIELD-NOTES.md`.
- **Superseded RTSP slots must not emit `TRANSPORT_EVENT_DISCONNECTED`.** `client_task()`'s cleanup
  checks `slot->should_stop` for exactly this. Removing that guard makes every wifi roam kill the
  audio of the session that replaced it, while leaving the control channel healthy.
- **audio_control.h** is the public audio driver surface. Keep the seven `airplay_audio_*` signatures unchanged.
- **PTP / timing:** `ensure_ptp_started()` is called at stream SETUP + RECORD (belt-and-braces) and PTP starts in `SETPEERS`. Keep all of these or you get silence.
- **Verify before declaring success:** `source .venv/bin/activate && esphome config config.gate.yaml` (valid) then `esphome compile config.gate.yaml` (exit 0). Only report "done" after both pass.
- Do not edit anything outside this `airplay_receiver` directory unless explicitly asked.

## Environment
- Repo: `airplay2-esphome`, venv: `.venv/bin/activate`, ESPHome ≥ **2025.1.0** (verified 2026.6.5),
  framework `esp-idf`, ESP32-S3, `flash_mode: dio`.
