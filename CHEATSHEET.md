# AirPlay 2 Receiver — ESPHome Component CHEATSHEET

Quick orientation for an AI agent or engineer who has just been handed this package.

## What this is
A port of [`rbouteiller/airplay-esp32`](https://github.com/rbouteiller/airplay-esp32) into a single
ESPHome **external component** named `airplay_receiver`. It makes an ESP32-S3 (with PSRAM) appear as an
AirPlay 2 audio destination to iOS/macOS/Home Assistant, decoding ALAC/AAC and playing over I2S to a
PCM5100-class DAC. AirPlay 1 (RAOP) is intentionally **not** ported.

Target hardware reference: **Sonocotta Amped-ESP32-S3** (ESP32-S3-WROOM-1-N8R8, octal PSRAM,
onboard PCM5100 + TPA311x). A WROVER-class plain ESP32 with a PCM5102A breakout also works, on a
smaller memory profile selected automatically from the build target.

Licensed **Non-Commercial**, inherited from upstream — see `LICENSE` and `THIRD-PARTY-NOTICES.md`
before adding or relicensing anything.

## Drop it in
```yaml
external_components:
  - source: github://henriklied/esphome-airplay2
    components: [airplay_receiver]

airplay_receiver:
  id: airplay_1
  name: "AirPlay2"
  i2s_bclk_pin: 26
  i2s_lrclk_pin: 25
  i2s_dout_pin: 22
  amp_enable_pin: 13
```
The **component is the media_player** — it inherits `media_player::MediaPlayer` and self-registers, so
Home Assistant gets the device with volume, play/pause/stop, and transport state. **Only configure
`airplay_receiver:`.** There is no `media_player: - platform: airplay_receiver` entry point; a config
that adds one fails validation (`Platform not found`), which prevents two RTSP servers / two I2S claims.

## YAML reference (`airplay_receiver:` block)
| key | default | meaning |
|---|---|---|
| `id` | — | component id |
| `name` | `"AirPlay2"` | _airplay._tcp / _raop._tcp friendly name (media_player entity name) |
| `buffer_size` | `1000000` | internal buffer size |
| `i2s_bclk_pin` / `i2s_lrclk_pin` / `i2s_dout_pin` | `-1` | I2S to the DAC. `-1` = unconfigured (silence) until set. Validator: `pins.internal_gpio_output_pin_number`. |
| `i2s_mclk_pin` | `-1` | optional PCM5100 MCLK/SCK (`-1` = unused) |
| `amp_enable_pin` | `-1` | amp unmute GPIO line. `-1` = unconfigured. |
| `amp_enable_inverted` | `false` | invert the amp-enable polarity |
| `amp_idle_timeout` | `60` | seconds of silence before the amp-enable line de-asserts (mutes). `0` = never. |
| `sample_rate` | `44100` | output sample rate |
| `audio_channel_mode` | `"stereo"` | `stereo \| mono \| left \| right` (AuMONO modes) |
| `dsp` | — | output biquad cascade, see below |

### `dsp:` — output EQ (`audio/audio_dsp.{h,cpp}`)

Direct AirPlay never passes through Music Assistant, so MA's per-player EQ, high-pass and
limiter do not apply. `dsp:` runs the speaker correction on the board instead.

| key | default | meaning |
|---|---|---|
| `enabled` | `true` | `false` bypasses the stage bit-exactly (a true A/B) |
| `preamp` | `"0dB"` | broadband gain ahead of the cascade; negative buys headroom for a boost |
| `filters` | `[]` | up to 8 biquad sections, applied in order |

Each entry in `filters`:

| key | default | meaning |
|---|---|---|
| `type` | required | `low_shelf \| high_shelf \| high_pass \| low_pass \| peaking \| notch` |
| `frequency` | required | corner/centre frequency (`80Hz`, `1.5kHz`); validated against `sample_rate` |
| `q` | `0.7071` | Butterworth by default. Shelves use the Q form, matching MA's DSP. |
| `gain` | `"0dB"` | shelf/peaking gain. Rejected at validation on `high_pass`/`low_pass`/`notch`. |

```yaml
airplay_receiver:
  id: airplay
  dsp:
    preamp: -4dB          # against the +4 dB shelf below
    filters:
      - type: high_pass   # [0]
        frequency: 80Hz
        q: 0.7
      - type: low_shelf   # [1]
        frequency: 150Hz
        q: 0.7
        gain: 4dB
```

**A positive `gain` without a matching negative `preamp` clips at high volume.** The
component logs a warning at boot naming the preamp it wants; the stage clamps rather than
wrapping, so the symptom is distortion on peaks, not noise.

Runtime tuning, for a `number` entity or any lambda (main loop only — these recompute
coefficients). The index is positional into `filters`, so reordering the list repoints
every caller:

```cpp
id(airplay).set_dsp_filter_frequency(0, 90.0f);
id(airplay).set_dsp_filter_gain(1, 3.0f);
id(airplay).set_dsp_filter_q(1, 0.8f);
id(airplay).set_dsp_preamp(-3.0f);
id(airplay).set_dsp_enabled(false);
```

## Board pin reference
- **Amped-ESP32-S3:** BCLK=GPIO14, LRCLK=GPIO15, DOUT=GPIO16, amp-enable=GPIO17 (PCM5100/5122 + TPA311x).
- **Generic ESP32 + PCM5102A:** BCLK=GPIO26, LRCLK=GPIO25, DOUT=GPIO22, no amp-enable (line out).

`examples/` carries a validated, compiled config for each: `amped-s3.yaml`, `generic-esp32.yaml`,
`dsp-speaker-correction.yaml`, `buttons.yaml`, `display.yaml`.

## Build / verify on this host
```bash
cd /path/to/esphome-airplay2 && source .venv/bin/activate
esphome config config.gate.yaml            # schema validation   (expect: "Configuration is valid!")
esphome compile config.gate.yaml           # full C++ build      (expect: "Successfully compiled program.")
```
Requirements: ESPHome ≥ **2025.1.0** (verified against 2026.8.2), `framework: {type: esp-idf}`, ESP32-S3.
Flash config: `flash_mode: dio` for the N8R8 (quad flash + octal PSRAM — `opi` describes the PSRAM
and produces a board that does not boot if set on the flash); `partitions.csv` (8MB OTA layout) is
in the repository root, and the examples reference it as `../partitions.csv`.

To validate the examples, which point at `github://` rather than the local tree, rewrite the source
into `.check/` (gitignored) and build from there:

```bash
mkdir -p .check && cp examples/secrets.example.yaml .check/secrets.yaml   # or write your own
for f in examples/*.yaml; do
  sed 's|source: github://henriklied/esphome-airplay2|source: {type: local, path: ../components}|' \
    "$f" > ".check/$(basename "$f")"
done
cd .check && esphome compile amped-s3.yaml
```

## Key files (where the action is)
- `__init__.py` — DOMAIN/CONFIG_SCHEMA (pins validated via `pins.internal_gpio_*`), `to_code`,
  `register_media_player`, `_add_memory_policy_flags()` (emits `-DAIRPLAY_PLATFORM_*`),
  `_add_lwip_requirements()` (socket count + UDP receive mbox depth),
  `_register_recursive_sources()`, `CONFLICTS_WITH=["sendspin","i2s_audio"]`.
- `FIELD-NOTES.md` — what hardware taught us: five distinct faults that look identical from
  outside, how to read the 1Hz `playout:` / `resend:` / `rxpath:` telemetry, and the dead-ends
  list. Read before debugging any artefact.
- `airplay_receiver.{h,cpp}` — the component class (`: cg.Component, media_player::MediaPlayer`),
  setup/loop, transport-event handling (incl. the METADATA → track-title wiring), media_player callbacks.
- `transport/` — RTSP server/control + Bonjour/mDNS glue + binary-plist + sockets. The sender's
  timing/control ports are captured in SETUP; PTP is started at SETPEERS **and** belt-and-braces at
  stream SETUP / RECORD.
- `crypto/` — HomeKit pairing (SRP-6a + Ed25519 + ChaCha20-Poly1305) + audio decrypt.
- `timing/` — PTP one-sample clock servo + NTP client (`ptp_clock`, `ntp_clock`, `audio_timing`).
- `decoder/` — ALAC/AAC decode via `espressif/esp_audio_codec` (managed component; impl headers resolve from the managed include dirs, do not create local shadow copies).
- `audio/` — timeline/engine v2, decode worker, I2S output, and the **control surface** in `audio_control.{h,cpp}`.
- `audio/audio_dsp.{h,cpp}` — the output biquad cascade (RBJ coefficients). Last stage in
  `playback_task`, after volume and channel mode. Coefficients are double-buffered and published
  by flipping one index, so `audio_dsp_set_*()` (trig, main loop only) never blocks the playback
  task; only `audio_dsp_process()` / `audio_dsp_reset()` are realtime-safe.
- `tests/test_biquad_response.py` — evaluates each filter shape on the unit circle against its
  design points, and checks the shipped cascades never exceed 0 dBFS. `uv run tests/test_biquad_response.py`.
- `allocator.{h,cpp}` — `airplay_alloc/calloc/free`, per-platform memory policy selected by `-DAIRPLAY_PLATFORM_*`.
- `platform/{esp32,esp32s3}/config.h` — reference tuning values.

## Agent-to-agent contract (do not break this)
`airplay_receiver.cpp` / the media_player glue drive the audio engine through **`audio_control.h`**. Keep these eight signatures exact (declared `extern "C"`, global scope — which is also what makes them reachable from a YAML lambda):
```c
bool airplay_audio_set_volume(uint8_t volume);   // 0-100
bool airplay_audio_play(void);
bool airplay_audio_pause(void);
bool airplay_audio_stop(void);
bool airplay_audio_is_playing(void);
int  airplay_audio_get_volume(void);
const char *airplay_audio_get_track_title(void);
void airplay_audio_set_track_title(const char *title);
```
The RTSP `TRANSPORT_EVENT_METADATA` handler calls `airplay_audio_set_track_title()` (this ESPHome
media_player has no title/artist fields, so the title is held here and logged).

## Pitfalls / gotchas (already handled, don't regress)
- **`api:` must not enable `encryption:`.** It pulls `esphome/noise-c`, which brings its own
  `libsodium`; the pairing crypto needs `espressif/libsodium`, and the IDF component manager
  refuses to choose ("Requirement espressif__libsodium and requirement libsodium are both added as
  project_managed_components"). It fails at CMake configure, before compilation. A plain `api:`
  block works and is what every example uses.
- **lwIP sizing is set by the component, not the board YAML.** `_add_lwip_requirements()` in
  `__init__.py` sets `CONFIG_LWIP_MAX_SOCKETS=24` and `CONFIG_LWIP_UDP_RECVMBOX_SIZE=32`. The IDF
  defaults (10 and 6) are both too small and **both fail silently** — the RTSP socket survives, so
  metadata and transport look healthy while audio is absent or riddled with concealment. The mbox
  default alone cost 931 concealment events in 75s. A board file setting these would override the
  component's values; don't.
- **`SO_RCVBUF` on the audio socket does nothing.** `CONFIG_LWIP_SO_RCVBUF` is off by default, so
  lwIP compiles the option out and `setsockopt()` returns `ENOPROTOOPT`. The 128KB request in
  `socket_utils_bind_udp()` is a no-op and its return value is unchecked. Queue depth comes from
  `CONFIG_LWIP_UDP_RECVMBOX_SIZE` (a slot count), not from bytes.
- **A superseded RTSP slot must not emit `TRANSPORT_EVENT_DISCONNECTED`.** `client_task()`'s
  cleanup guards on `slot->should_stop`. Without it, a wifi roam makes the dying old slot call
  `audio_receiver_stop()` on the session that just replaced it — sender connected, metadata
  flowing, silence.
- **ESPHome excludes `esp_driver_i2s` by default** — `__init__.py` must call `include_builtin_idf_component("esp_driver_i2s")` or `audio_output.cpp`'s `#include <driver/i2s_std.h>` fails.
- **Don't hijack ESPHome's mDNS.** Services must register via ESPHome's `mdns_service` API; never call `mdns_init()/mdns_hostname_set()` directly (it makes ESPHome's MDNSComponent fail). `_airplay` + `_raop` are both advertised.
- **`_raop._tcp` must be a valid AirPlay 2 advert or the client can't open the session.** Instance name must be `<MAC uppercase hex, no colons>@<device name>` (e.g. `DCB4D900A47C@Boston X90`) so the client correlates it to the `_airplay` device it paired with; TXT must be the dual-mode set with `vv=2` (else it reads as AirPlay 1), `et=0,1,3,5`, `cn=0,1,2,3`, `ft=<features>`, `da=true`, `vn=65537`, `md=0,2` (text+progress, no artwork). The `_airplay._tcp` record stays on the bare device name.
- **Component must run AFTER the network stack** — `get_setup_priority()` returns `setup_priority::AFTER_CONNECTION` (default `DATA` runs before lwIP/esp_netif).
- **`platform/*/config.h` are two levels deep and ESPHome doesn't copy them.** Per-platform tuning is selected at compile time from the `-DAIRPLAY_PLATFORM_ESP32S3` / `-DAIRPLAY_PLATFORM_ESP32` macro emitted by `_add_memory_policy_flags()` — do NOT add logic that depends on the config.h files being copied.
- **Single entry point only.** The component is the media_player. There is no `media_player: - platform: airplay_receiver` — a config with both fails at validation (the platform file was removed to prevent two RTSP servers / two I2S claims).
- **`CONFLICTS_WITH=["sendspin","i2s_audio"]`** — AirPlay, the vendor Sendspin media firmware, and any `i2s_audio` speaker all want `I2S_NUM_0`; a config with them fails at validation (not runtime). `audio_output.cpp` defaults to `I2S_NUM_1` on ESP32-S3 to avoid the clash; `AudioOutputConfig.i2s_port`/`AIRPLAY_I2S_PORT` overrides it.
- **PTP must be running.** Started in `SETPEERS` and (belt-and-braces) at stream SETUP / RECORD. Keep both, or you get silence. The sender's `client_timing_port` is captured from the RTSP Transport header so the NTP fallback is wired too.
- All I2S/amp pins use `pins.internal_gpio_output_pin_number` (validated, cross-component GPIO conflict detection); `-1` sentinel means "unused".

## Current status
The component builds cleanly (`esphome config` valid; `esphome compile` exit 0, no warnings) against
ESPHome 2026.8.2 on ESP32-S3, at roughly 33% RAM and 28% flash. Every config in `examples/` is
validated and compiled.

Multi-room works well, including two boards as a stereo pair (`audio_channel_mode: left` / `right`).
Note that the local-anchor fallback gives up group sync by design: a board that loses its network
clock keeps playing on its own clock instead of going silent, and drifts from the group until it
re-locks. That is the trade, not a bug.

See `components/airplay_receiver/UPSTREAMING.md` for the PR checklist and the licensing blocker.
