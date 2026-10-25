# aac_bench

Throwaway firmware component: decodes three stereo clips (44.1k and 48k from
Apple's encoder, 44.1k from ffmpeg's) with FDK and with `esp_audio_codec`, each
in a fresh task on the audio core, 20 s after boot and every 30 s after. Logs
time per frame, stack high-water mark, heap taken by open, and the
`esp_audio_codec` output's SNR against FDK's after alignment. Results are in
`components/airplay_receiver/FIELD-NOTES.md`, "Stereo AAC: esp_audio_codec
against FDK".

1. `./make_clips.sh`
2. Copy a board's AirPlay config and append:
   ```yaml
   external_components:
     - source: <absolute path to tools/aac_bench/components>
       components: [aac_bench]
   aac_bench:
   ```
3. `esphome run` it, read `esphome logs`, then flash the normal config back.

Never ship it: it occupies the audio core for several seconds of every 30.
