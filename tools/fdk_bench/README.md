# fdk_bench

Throwaway firmware component: decodes three embedded clips (stereo, 5.1 from
Apple's encoder, 5.1 from ffmpeg's) on the audio core, 20 s after boot and every
30 s after. Each clip is decoded on one core and on two
(`aac_fdk_set_parallel`), and the two outputs are compared by hash; 5.1 clips
are also decoded with no downmix. Parse and downmix time come from
`-Wl,--wrap` on FDK's cross-file calls; the filterbank is not timed on its own,
since `aac_fdk_parallel.cpp` wraps it. Results are in
`components/airplay_receiver/FIELD-NOTES.md`, "Where FDK's decode time goes"
and "Filterbank on two cores".

1. `./make_clips.sh`
2. Copy a board's AirPlay config and append:
   ```yaml
   external_components:
     - source: <absolute path to tools/fdk_bench/components>
       components: [fdk_bench]
   fdk_bench:
   ```
3. `esphome run` it, read `esphome logs`, then flash the normal config back.

Never ship it: it occupies the audio core for ~20 s of every 30.
