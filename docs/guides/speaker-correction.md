# Correct a speaker by ear

Tune the board's EQ from Home Assistant while music plays, then copy the values into YAML.

A direct AirPlay stream goes from the phone to the board and never passes through Music Assistant, so MA's per-player EQ, high-pass and limiter never apply. If the speaker needs correction, it has to run on the board.

!!! note "Before you start"

    - A board running the component, adopted in Home Assistant.
    - [examples/dsp-speaker-correction.yaml](https://github.com/henriklied/esphome-airplay2/blob/main/examples/dsp-speaker-correction.yaml) as your starting config.
    - Music you know well, played at your normal listening level.

## 1. Flash the example

The example is a starting point for a small sealed two-way bookshelf speaker. It has three filters and a preamp:

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

It also exposes five controls in Home Assistant, all under the device's configuration entities:

| Entity | Drives | Range |
|---|---|---|
| DSP high-pass | filter `[0]` frequency | 20-200 Hz |
| DSP bass lift | filter `[1]` gain | -6 to +9 dB |
| DSP treble tilt | filter `[2]` gain | -9 to +6 dB |
| DSP preamp | preamp | -12 to 0 dB |
| DSP enabled | bypass switch | on/off |

Copy your board's pins and Wi-Fi settings into it, then flash.

!!! warning

    The number entities address filters by position. `set_dsp_filter_gain(1, x)` means "the second entry in `filters:`". If you reorder that list, every control points at the wrong filter.

## 2. Tune while listening

1. Start playback from your phone and sit where you normally listen.
2. Turn **DSP enabled** off and on a few times. Bypass is bit-exact, so this is a real A/B.
3. Raise **DSP high-pass** until the bass stops sounding strained at your loudest normal level, then back off a step. This keeps cone excursion out of the range the speaker cannot play.
4. Adjust **DSP bass lift** to put back warmth the high-pass took.
5. Adjust **DSP treble tilt** if the tweeter sounds bright or dull.
6. Change one control at a time. Each change takes effect immediately, with no OTA cycle.

The values persist across reboots: each number entity has `restore_value: true`, and the restored value is applied at boot.

## 3. Keep the preamp ahead of the boosts

A positive gain without a matching negative preamp clips at high volume. The stage clamps rather than wrapping, so you hear distortion on peaks, not noise.

1. Add up the positive gains (in the example, bass lift).
2. Set **DSP preamp** to at least that much negative.
3. Check the log. The component warns when the boosts exceed the preamp and names the value it wants:

```text
DSP boosts up to +6.0 dB with preamp -4.0 dB: peaks above -2.0 dBFS will clip. Set dsp.preamp to -6.0 dB to stay clear.
```

If you enable the `limiter` stage, boosts no longer need a matching negative preamp, and this warning is not printed. See [Dynamic stages](../configuration/dynamic-stages.md).

## 4. Copy the values back to YAML

The tuned values live in the Home Assistant entities. Write them into the config so that a fresh flash, or a later change of controls, starts from them.

1. Read each value off its entity, or from the cascade the component logs at boot.
2. Write them into `dsp:` in the same order: high-pass frequency into `filters[0].frequency`, bass lift into `filters[1].gain`, treble tilt into `filters[2].gain`, preamp into `preamp`.
3. Set each number's `initial_value` to the same value.
4. Flash.

!!! warning "Before removing the sliders"

    If you remove the number entities from YAML, Home Assistant removes them when the board stops reporting them, and the tuned values go with them. The board then plays whatever `dsp: filters:` says. Copy the values first.

## Next steps

- To change filter types or the number of bands without a flash, replace the sliders with the text entity described in [Tuning from Home Assistant](../configuration/runtime-tuning.md). Do the copy in step 4 first: the text starts empty, so the board keeps playing the YAML filters until you set it.
- To start from a measurement instead of by ear, see [Measure a room](room-measurement.md).
- Filter types, Q and validation rules: [Output DSP](../configuration/output-dsp.md).
