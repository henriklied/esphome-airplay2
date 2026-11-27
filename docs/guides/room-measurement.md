# Measure a room

Measure a speaker in its room with a phone, and fit the board's EQ to the result.

`tools/measure` plays a sweep through the board, takes a phone recording of it, and writes a `dsp:` block. No measurement microphone is needed. An uncalibrated phone is trusted for cuts below about 10 kHz and for small boosts only, and the fit is limited accordingly.

!!! note "What you need"

    - [uv](https://docs.astral.sh/uv/) and `ffmpeg` on your `PATH`. The tool decodes the recording with ffmpeg.
    - A Mac with the Music app, to play the sweep to the board.
    - An iPhone with Voice Memos, to record it.
    - A way to bypass the board's DSP from Home Assistant, such as the **DSP enabled** switch in [examples/dsp-speaker-correction.yaml](https://github.com/henriklied/esphome-airplay2/blob/main/examples/dsp-speaker-correction.yaml).

Run every command from `tools/measure` in a checkout of the repository.

## 1. Make the sweep file

```bash
cd tools/measure
uv run airplay-measure sweep sweep.wav
```

The file holds 3 s of silence, then five 6 s sweeps from 30 Hz to 20 kHz, each followed by 6 s of silence: 63 s in all, at -15 dBFS. Each sweep becomes one microphone position.

To measure one speaker of a pair, put the sweep on one channel only:

```bash
uv run airplay-measure sweep sweep-left.wav --channel left
```

!!! warning

    `--repeats`, `--sweep-seconds` and `--gap-seconds` change the sweep plan. They go before the subcommand, and `analyze` must be given the same values the file was made with.

## 2. Set up the board

1. Turn the board's **DSP** switch off in Home Assistant, so the measurement is the bare speaker in the room.
2. Keep the volume at a normal listening level. With the DSP off there is no high-pass in front of the woofer.

## 3. Play and record

1. In Music on the Mac, turn off Sound Check and the equalizer, and pick the board as the AirPlay output.
2. On the iPhone, set Voice Memos to lossless: Settings > Apps > Voice Memos > Audio Quality: Lossless.
3. Hold the phone at ear height at the listening position, bottom microphone towards the speaker. Start recording.
4. Play the file:

    ```bash
    open -a Music sweep.wav
    ```

5. In each 6 s gap, move the phone about 30 cm: left, right, forward, back.
6. Stop the recording after the last sweep and copy it to the computer.

!!! note "Why Music on a Mac"

    Music streams buffered AAC, which retransmits lost packets. The realtime path (system audio, Spotify) conceals lost packets instead, and concealment would corrupt the sweep.

## 4. Analyze

```bash
uv run airplay-measure analyze recording.m4a --high-pass 50 --out living-room/
```

It writes four files to `--out` (default `measurement/`):

| File | Contents |
|---|---|
| `eq.yaml` | A `dsp:` block for the board config |
| `ParametricEQ.txt` | The same filters in Equalizer APO / REW format |
| `response.png` | The plot described below |
| `report.json` | Every number behind the plot, plus the filters |

Turn the DSP switch back on when you are done.

### Options

| Option | Default | Effect |
|---|---|---|
| `--high-pass` | 50 | Protective high-pass kept in front of the fit, in Hz. `0` for none. Nothing is boosted below the speaker's own roll-off. |
| `--fit-low` / `--fit-high` | 40 / 8000 | Range the fit corrects, in Hz. |
| `--max-bands` | 7 | Most bands to fit, on top of the high-pass. |
| `--tilt` | -0.8 | Target slope in dB per octave, a common in-room target. |
| `--slots` | none | Fill a fixed band layout instead of free bands. See below. |
| `--target-recording` | none | Match another speaker instead of the tilt. See below. |

## 5. Read the result

The tool logs a summary line and one line per filter:

```text
airplay-measure: 5 positions, fit 55-8000 Hz, deviation 4.1 -> 1.6 dB RMS
```

- **positions**: how many sweeps it found. If it warns `found 3 of 5 sweeps`, the rest were too quiet or cut off. Record again, louder or closer.
- **fit**: the range it corrected. The low end is the higher of `--fit-low` and just above the speaker's measured roll-off.
- **deviation**: how far the response is from the target, in shape, before and after the EQ.

In `response.png`:

- grey lines: each position;
- black: their average, which is what the fit uses;
- green dashed: the target;
- blue: the predicted response with the EQ;
- shaded areas: outside the fit range, left alone.

The fit prefers cuts. A peak in a position-averaged measurement is real energy, while a dip is often a cancellation that a boost only feeds. Boosts are capped at 4 dB and kept broad, cuts go to 12 dB, and a band is kept only if it lowers the error noticeably. The preamp is set in 0.5 dB steps so the whole cascade stays at or below 0 dB.

## 6. Put the filters on the board

Paste `eq.yaml` into the `airplay_receiver:` block and flash. It holds the high-pass and up to seven bands, which fits the eight-filter limit.

To apply it without a flash on a board with the text entity from [Tuning from Home Assistant](../configuration/runtime-tuning.md), write the same filters in the text form, one section per `;`:

```text
hp,50,0.71,0;pk,120,4.00,-6.5;hs,3000,0.70,-1.5
```

The text form carries filters only. Set the preamp separately. The text format is in [Text formats](../reference/text-formats.md).

## Fit to your sliders

`--slots layout.json` fills a fixed band layout, such as the Home Assistant sliders a board config exposes, so you can dial the result in by hand without a flash. Each slot is used at most once. For the sliders in the speaker-correction example (bass lift at 150 Hz, treble tilt at 6 kHz, high-pass at 80 Hz):

```json
[
  {"name": "bass", "type": "low_shelf", "min_hz": 150, "max_hz": 150,
   "min_q": 0.7, "max_q": 0.7, "min_gain_db": -6, "max_gain_db": 9},
  {"name": "treble", "type": "high_shelf", "min_hz": 6000, "max_hz": 6000,
   "min_q": 0.7, "max_q": 0.7, "min_gain_db": -9, "max_gain_db": 6}
]
```

```bash
uv run airplay-measure analyze recording.m4a --high-pass 80 --slots layout.json
```

`type` is a `dsp:` filter type. Gains are also capped by the fit's own limits (4 dB boost, 12 dB cut). The log and `report.json` give each slot's value; a slot the fit did not use reads `gain_db: 0.0`.

## Match another speaker

`--target-recording reference.m4a` uses another speaker's response as the target instead of the tilt. Record a speaker you like in the same spot, with the same phone and positions. Its balance, smoothed to 1/3 octave, becomes the target, and the phone's own response cancels out because both recordings carry it.
