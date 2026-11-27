# Dynamic stages

Eight optional stages under `dsp:` that make a small speaker sound larger than its static EQ allows.

Each stage is off until you configure it, and a stage that is off costs nothing per sample.

## Order

```text
preamp -> filters -> loudness -> + bass enhancer harmonics -> stereo width + ambience
       -> crosstalk cancel -> bass protection -> bass limiter -> limiter
```

The bass enhancer reads the signal after the preamp but before `filters`, so a protective high-pass
cannot starve it of the bass it stands in for. Its harmonics are added after loudness.

## Example

```yaml
airplay_receiver:
  id: airplay
  dsp:
    loudness:
      frequency: 120Hz
      max_boost: 8dB
      range: 30dB
    bass_enhancer:
      frequency: 80Hz
      amount: 1.0
    stereo_width:
      width: 1.4
      frequency: 300Hz
      centre_lock: 0.8
    ambience:
      amount: 0.15
      decay: 400ms
    crosstalk_cancel:
      amount: 0.6
      delay: 60us
    bass_protection:
      frequency: 70Hz
    bass_limiter:
      frequency: 120Hz
      threshold: -6dB
      release: 200ms
    limiter:
      threshold: -1dB
      release: 100ms
```

## Loudness

A low shelf whose lift follows the playback volume. It is flat at full volume and reaches
`max_boost` once the volume is `range` below full. This compensates for the ear losing bass at low
level, and keeps extra bass away from the limiter where headroom is scarcest.

| Parameter | Description | Default |
|---|---|---|
| `max_boost` | Required. Lift at or below `range` of attenuation, 0 to 15 dB. `0` disables. | |
| `frequency` | Shelf corner. | `100Hz` |
| `range` | Attenuation at which the full lift is reached, 1 to 60 dB. | `30dB` |

## Bass enhancer

Replaces bass below `frequency` (the speaker's lower limit) with its 2nd and 3rd harmonics, which the
ear reads as the missing fundamental. The harmonics track the bass level linearly; they are not
distortion that grows with drive.

| Parameter | Description | Default |
|---|---|---|
| `frequency` | Upper edge of the band to synthesize from, 30 to 300 Hz. | `90Hz` |
| `amount` | Harmonic level, 0 to 3. `1.0` is roughly equal level. `0` disables. | `1.0` |

## Stereo width

Mid/side width above `frequency`. `1.0` is unchanged, `0` is mono above the corner. Bass keeps its
original width.

| Parameter | Description | Default |
|---|---|---|
| `width` | Required. 0 to 3. | |
| `frequency` | Width corner. Ambience uses it too. | `300Hz` |
| `centre_lock` | 0 to 1. See below. | `0` |

At `centre_lock: 0` the stage is plain mid/side, which also moves panned and centred sources. Toward
1 it tracks the dominant source (principal component, over about 30 ms) and widens only what is
orthogonal to it. Vocals and panned instruments keep their place and level. Diffuse sound has no
dominant source and still widens fully.

## Ambience

Room-fill reverb: a damped 4-line feedback delay network fed only by the ambient part of the mix
above the width corner, with decorrelated left and right tails. A dry mono source feeds it nothing.

| Parameter | Description | Default |
|---|---|---|
| `amount` | Required. Wet level, 0 to 1. 0.1 to 0.3 is subtle. | |
| `decay` | Time for the tail to fall 60 dB, 100 to 3000 ms. | `400ms` |
| `damping` | Low-pass inside the loop, 500 to 15000 Hz. Lower is darker. | `5000Hz` |

!!! note

    The 38 KB delay buffer is allocated from PSRAM the first time ambience is enabled, and never
    freed. If there is no PSRAM for it, ambience stays off and the log says
    `Ambience disabled: no PSRAM for the reverb`. There is no fallback to internal RAM: on hardware,
    38 KB of internal RAM starved the decoder and then Wi-Fi.

## Crosstalk cancel

Recursive crosstalk cancellation (RACE). Each channel subtracts a delayed, band-limited, attenuated
copy of the other channel's output, so the sound from each driver is cancelled at the far ear and
the image extends past the cabinet.

| Parameter | Description | Default |
|---|---|---|
| `amount` | Required. Depth, 0 to 0.95. Below 1 keeps the recursion decaying. | |
| `delay` | Extra path from a driver to the far ear, 10 to 300 us. | `60us` |
| `low_frequency` | Lower band edge. Bass is left alone. | `250Hz` |
| `high_frequency` | Upper band edge. | `5000Hz` |

In band it lifts side by up to 1/(1-amount) and lowers mid by up to 1/(1+amount). Pair it with a
lower `stereo_width`.

## Bass protection

A 4th-order Butterworth high-pass placed after every stage that adds bass. A loudness or shelf lift
below a `filters` high-pass cancels it (a +13 dB loudness shelf undoes an 80 Hz 2nd-order high-pass at
50 Hz), and the deepest notes then overdrive small drivers and their supply. Nothing after this
stage can add them back.

| Parameter | Description | Default |
|---|---|---|
| `frequency` | Required. The speaker's real lower limit, 20 to 200 Hz. | |

## Bass limiter

Splits at `frequency` (Linkwitz-Riley, so the bands sum back flat), limits the lows to `threshold` on
their own and delays the highs to match. Bass peaks no longer duck the whole mix.

| Parameter | Description | Default |
|---|---|---|
| `enabled` | Turn the stage on or off. | `true` |
| `frequency` | Crossover, 40 to 400 Hz. | `120Hz` |
| `threshold` | Ceiling for the bass band, -30 to 0 dB. | `-6dB` |
| `release` | Gain recovery, 20 to 2000 ms. | `200ms` |

## Limiter

Stereo-linked look-ahead peak limiter, last in the chain. Peaks never exceed `threshold`. With it
enabled, boosts no longer need a matching negative `preamp`, and the boot headroom warning is
skipped.

| Parameter | Description | Default |
|---|---|---|
| `enabled` | Turn the stage on or off. | `true` |
| `threshold` | Ceiling, -24 to 0 dBFS. | `-1dB` |
| `release` | Gain recovery, 10 to 2000 ms. | `100ms` |

## Delay and sync

The limiter and the bass limiter each add 1.5 ms of look-ahead delay. The playout clock includes it,
so AirPlay sync holds.

## CPU and headroom

- The DSP runs at priority 9 on the core the decoder shares, so every stage takes time from AAC
  decode. Watch `pk=` in the `playout:` log line when you add one.
- A prior Geneva measurement put the full output DSP at about a third of the audio core.
- Loudness and the bass enhancer add level that the boot headroom check does not count.
- If the limiter's gain reduction sits at several dB while the volume is low, the loudness lift or
  the enhancer is set too hot.

## Next

Every stage has a runtime setter, so you can tune it from a Home Assistant slider. See
[Tuning from Home Assistant](runtime-tuning.md).
