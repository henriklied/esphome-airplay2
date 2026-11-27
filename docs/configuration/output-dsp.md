# Output DSP

`dsp:` runs speaker correction on the board as a cascade of up to eight biquad filters.

A direct AirPlay stream goes from the phone to the board. It never passes through Music Assistant,
so MA's per-player EQ, high-pass and limiter never apply. If the speaker needs correction, it has to
happen here.

## Configure the cascade

```yaml
airplay_receiver:
  id: airplay
  dsp:
    preamp: -4dB
    filters:
      - type: high_pass      # [0]
        frequency: 80Hz
        q: 0.7
      - type: low_shelf      # [1]
        frequency: 150Hz
        q: 0.7
        gain: 4dB
      - type: high_shelf     # [2]
        frequency: 6kHz
        q: 0.7
        gain: -2dB
```

This is the starting point from `examples/dsp-speaker-correction.yaml`, for a small sealed two-way:
keep the woofer out of the bottom octave, lift the bass a little, and take the edge off a bright
tweeter.

## Options

| Parameter | Description | Default |
|---|---|---|
| `enabled` | `false` bypasses the whole stage bit-exactly, preamp included. Use it for an honest A/B. | `true` |
| `preamp` | Broadband gain ahead of the cascade, in dB. Negative values buy back the headroom a boost spends. | `0dB` |
| `filters` | Up to 8 sections, applied in list order. | `[]` |

Each entry in `filters`:

| Parameter | Description | Default |
|---|---|---|
| `type` | Required. One of the types below. | |
| `frequency` | Required. Corner or centre frequency, e.g. `80Hz`, `1.5kHz`. | |
| `q` | Width or slope, 0.05 to 20. Shelves use the Q form, matching Music Assistant. | `0.7071` (Butterworth) |
| `gain` | Shelf and peaking gain, in dB. | `0dB` |

## Filter types

| `type` | Text code | Takes `gain` | Typical use |
|---|---|---|---|
| `high_pass` | `hp` | No | Keep cone excursion out of the range the speaker cannot play |
| `low_pass` | `lp` | No | Roll off the top end |
| `low_shelf` | `ls` | Yes | Lift or cut the bass |
| `high_shelf` | `hs` | Yes | Lift or cut the treble |
| `peaking` | `pk` | Yes | Fix a single peak or dip |
| `notch` | `no` | No | Remove a narrow resonance |

The text code is what the [text form](../reference/text-formats.md) uses.

!!! warning

    A `gain:` on `high_pass`, `low_pass` or `notch` fails validation. Those shapes have no gain term.

## Order and the filter index

Filters run in the order you list them. The position of each one is also its index: the runtime
setters in [Tuning from Home Assistant](runtime-tuning.md) address filters by that index. Reorder
the list and every lambda that calls `set_dsp_filter_*()` points at a different filter.

The board logs the cascade at `INFO` on boot, with the index of each section, so you can check what
runs without raising the log level.

The DSP stage sits last in the playback path, after volume and channel mode. Filtering after the
volume attenuation gives a boost room to work instead of clipping at high volume.

## Headroom

A positive `gain` without a matching negative `preamp` clips at high volume. The stage clamps
instead of wrapping, so what you hear is distortion on peaks, not noise.

At boot, and each time the cascade is replaced from text, the component adds up the positive filter
gains. If that sum plus `preamp` is above 0 dB, it logs a warning naming the preamp that would stay
clear:

```text
DSP boosts up to +4.0 dB with preamp +0.0 dB: peaks above -4.0 dBFS will clip. Set dsp.preamp to -4.0 dB to stay clear.
```

!!! note

    The check counts only `filters` gains. It does not count the loudness or bass enhancer stages,
    and it is skipped when the `limiter` is enabled, because the limiter holds peaks under its
    ceiling. See [Dynamic stages](dynamic-stages.md).

## Limits

- At most 8 filters. A longer list fails validation.
- `frequency` must be at or below 0.49 times `sample_rate` (21609 Hz at 44.1 kHz). Validation
  rejects anything higher.
- `q` from 0.05 to 20.
- A section that cannot be designed at the running sample rate is skipped, with a warning in the
  log. The rest of the cascade still runs.

## Bypass and CPU

`enabled: false`, or `set_dsp_enabled(false)` at runtime, passes the signal through unchanged.
Turning it back on clears the filter state first, so it does not thump.

Eight biquads cost about 1% of one core at 44.1 kHz stereo. The dynamic stages cost more. The
`playout:` log line reports DSP cost as `dsp=` (average over the second) and `pk=` (worst block),
as a share of one core. Watch `pk` when you add stages.

## Next

- Add loudness, bass enhancement, width and limiting: [Dynamic stages](dynamic-stages.md).
- Tune the cascade by ear from Home Assistant: [Tuning from Home Assistant](runtime-tuning.md).
- Walk through a correction: [Correct a speaker by ear](../guides/speaker-correction.md) and
  [Measure a room](../guides/room-measurement.md).
