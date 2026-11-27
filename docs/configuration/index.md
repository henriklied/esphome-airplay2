# Configuration

All configuration goes in one `airplay_receiver:` block. This page covers the base options. The full schema, with types and ranges, is in the [YAML reference](../reference/yaml.md).

## Example

```yaml
airplay_receiver:
  id: airplay
  name: "Living Room"
  i2s_bclk_pin: 14
  i2s_lrclk_pin: 15
  i2s_dout_pin: 16
  amp_enable_pin: 17
  sample_rate: 44100
  amp_idle_timeout: 60
  initial_volume: 20%
  audio_channel_mode: stereo
```

## Options

| Parameter | Description | Default |
|---|---|---|
| `id` | Component ID. Needed to call the component from a lambda. | generated |
| `name` | The name senders see in Control Center, and the `media_player` entity name. | `AirPlay2` if empty |
| `i2s_bclk_pin` | I2S bit clock to the DAC. | unused |
| `i2s_lrclk_pin` | I2S word select to the DAC. | unused |
| `i2s_dout_pin` | I2S data to the DAC. | unused |
| `i2s_mclk_pin` | Master clock, for DACs that do not self-strap. | unused |
| `amp_enable_pin` | Amplifier enable or unmute line. | unused |
| `amp_enable_inverted` | Set `true` if the amp-enable line is active low. | `false` |
| `amp_idle_timeout` | Seconds without audio before the amp-enable line is de-asserted. `0` keeps the amp on for the whole session. | `60` |
| `initial_volume` | Start volume until a sender has set one. | `20%` |
| `sample_rate` | Output sample rate in Hz. | `44100` |
| `audio_channel_mode` | `stereo`, `mono`, `left` or `right`. | `stereo` |
| `buffer_size` | Internal buffer size in bytes. | `1000000` |
| `dsp` | Output filters and dynamic stages. See [Output DSP](output-dsp.md) and [Dynamic stages](dynamic-stages.md). | none |
| `pair` | Stereo-pair role. See [Stereo pair and groups](stereo-pair.md). | none |

The block also accepts the standard ESPHome entity options (`icon`, `internal`, `disabled_by_default`, `entity_category`) and the `media_player` triggers such as `on_state`, `on_play` and `on_pause`.

## Pins

Pins are GPIO numbers. They go through ESPHome's pin registry, so a pin used twice, or one that cannot drive an output, fails validation. Pin tables for the supported boards are in [Boards and pins](hardware.md).

!!! note "No pins, no sound"

    Leaving the three I2S pins unset is valid. The receiver then pairs and plays silently. That is useful for testing pairing on a board with no DAC, and confusing if you did not mean it.

## Volume at boot

Senders send a volume only when the slider moves, never on connect. Until a sender has set one, the board starts at `initial_volume`. After that it keeps the last sender volume across reboots (mute is not kept) and starts there.

In a stereo pair with a pair destination, start volume is kept per destination.

## Channel mode

`audio_channel_mode` picks what reaches the DAC:

- `stereo`: the normal left and right mix.
- `mono`: the (L+R)/2 downmix on both outputs.
- `left` or `right`: that source channel on both outputs. Use these for two boards as a stereo pair that you select together in the picker.

With `pair:`, the primary plays its own `audio_channel_mode` and forwards the other channel. See [Stereo pair and groups](stereo-pair.md).

## Amplifier enable

If your board has an amplifier with an enable or mute line, set `amp_enable_pin`. The component asserts it with the first audible frame and de-asserts it after `amp_idle_timeout` seconds of silence or of being stopped, so a connect switches it at most once. Use `amp_enable_inverted: true` for an active-low line. Line-out builds without an amplifier leave the pin unset.

## One entry point

The component is the `media_player`. Do not add `media_player: - platform: airplay_receiver`; there is no such platform and validation fails. Use `id(airplay)` in lambdas and `media_player.*` actions with the component's `id`.
