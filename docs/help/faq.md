# FAQ

## Which boards does it run on?

An ESP32-S3 with 8 MB octal PSRAM (ESP32-S3-WROOM-1-N8R8) is the primary target, and the one it is developed and measured on. The Sonocotta Amped-ESP32-S3 is the reference board. A WROVER-class ESP32 with 4 MB PSRAM also works, on a smaller memory profile chosen automatically at build time; it has less headroom to ride out a Wi-Fi hiccup. PSRAM is required on both. The DAC is expected to be a PCM5100-class I2S part. See [Boards and pins](../configuration/hardware.md).

## Can I use the Arduino framework?

No. It needs `framework: type: esp-idf` and ESPHome 2025.1.0 or newer.

## Why can't I enable API encryption?

`api: encryption:` pulls in `esphome/noise-c`, which brings its own libsodium. The pairing crypto needs `espressif/libsodium`, and the ESP-IDF component manager refuses to pick between the two. The build fails at CMake configure. Use a plain `api:` block.

## Do I add a `media_player:` platform?

No. The component is the media player. A config with `media_player: - platform: airplay_receiver` fails validation, because two instances would mean two RTSP servers on port 7000 and two claims on the same I2S peripheral.

## Does Music Assistant's EQ apply?

Not to a direct AirPlay stream. That goes from the phone to the board and never passes through Music Assistant, so MA's per-player EQ, high-pass and limiter never apply. Use the board's `dsp:` instead. It runs on everything the board plays. See [Output DSP](../configuration/output-dsp.md).

## Does Spotify work?

Yes. Spotify uses the realtime stream (ALAC), which is not retransmitted, so a lost packet is concealed instead of resent. Expect about 2 s from resume to sound: the sender stamps its first packet about 1.8 s past the start point, so there is no earlier audio to play. Apple Music and Music on a Mac use the buffered stream, which is TCP and retransmits. The sender picks the stream type; the board cannot force it.

## Does multi-room work?

Yes. A group of boards stays in sync off the sender's shared PTP clock. If one board loses the clock, it keeps playing on its own clock and drifts from the group until it re-locks. `diag_ptp_locked()` shows which board it is.

## Can two boards be a stereo pair?

Yes, two ways: two destinations with `audio_channel_mode: left` and `right`, selected together, or one destination with `pair:`. See [Set up two boards as a pair](../guides/two-boards.md).

## What sample rate does it play at?

`sample_rate` sets the output rate (default 44100). The output follows the stream's rate for 44.1 kHz and 48 kHz, reclocking before the first audible frame while the amp is still off. Filter frequencies are validated against `sample_rate`.

## Which codecs?

ALAC and stereo AAC. 5.1 support was removed from the main code; it is parked at the tag `parked/surround-5.1`.

## Does it show the track title?

Not in the `media_player` entity, which has no title field in ESPHome. The title is held in the audio layer; read it from a lambda with `airplay_audio_get_track_title()`.

## Can Home Assistant tell it to play a URL?

No. Browse, play-media and announce are cleared from the feature flags. It is a receiver: it plays what a sender pushes to it.

## What volume does it start at?

Senders send a volume only when the slider moves, never on connect. The board keeps the last sender volume across reboots (not mute) and starts there. Before any sender has set one, it uses `initial_volume` (default 20%).

## Why does the ESPHome version banner keep appearing in the log?

It is printed every time a log client connects. It does not mean the board rebooted.

## Can I use it commercially?

Not without written permission from the upstream author. See [Credits and license](../reference/credits-license.md).
