# airplay-measure

Measure a speaker in its room with a phone, and fit the board's parametric EQ
to the result. No measurement microphone needed; an uncalibrated phone is
trusted for cuts below ~10 kHz and for small boosts only.

1. `uv run airplay-measure sweep sweep.wav` -- five 6 s sweeps, 6 s apart (63 s).
2. Turn the board's **DSP** switch off in Home Assistant, so the measurement is
   the bare speaker in the room. Keep the volume at a normal listening level:
   with the DSP off there is no high-pass in front of the woofer.
3. In Music on a Mac: disable Sound Check and the equalizer, pick the board as
   the AirPlay output, then `open -a Music sweep.wav`. Music streams buffered
   AAC, which retransmits; the realtime path (system audio, Spotify) conceals
   lost packets and would corrupt the sweep.
4. Record on an iPhone with Voice Memos (Settings > Apps > Voice Memos >
   Audio Quality: Lossless), the bottom microphone towards the speaker, at
   ear height at the listening position. Start recording before playback.
   In each 6 s gap, move the phone ~30 cm (left, right, forward, back).
5. `uv run airplay-measure analyze recording.m4a --high-pass 50 --out geneva/`
   writes `response.png`, `eq.yaml` (a `dsp:` block), `ParametricEQ.txt`
   (Equalizer APO / REW format) and `report.json`.
6. Turn the DSP switch back on.

`--high-pass` is the protective filter that stays in front of the fit; nothing
is boosted below the speaker's own roll-off. `--tilt` sets the target slope
(default -0.8 dB/octave, a common in-room target). Tests: `uv run --group dev pytest`.
