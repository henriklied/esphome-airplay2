# YAML reference

Every option the `airplay_receiver:` block accepts, as validated by the component's schema. For explanations and examples, follow the links to the configuration pages.

Units follow ESPHome conventions: frequencies as `80Hz` or `1.5kHz`, gains as `-4dB`, times as `400ms` or `60us`, percentages as `20%`.

## Top level

| Option | Type | Default | Notes |
|---|---|---|---|
| `id` | ID | generated | Needed for lambdas. |
| `name` | string | `AirPlay2` if empty | Name in Control Center and of the `media_player` entity. |
| `i2s_bclk_pin` | GPIO number | unused | Output-capable internal GPIO. |
| `i2s_lrclk_pin` | GPIO number | unused | Output-capable internal GPIO. |
| `i2s_dout_pin` | GPIO number | unused | Output-capable internal GPIO. |
| `i2s_mclk_pin` | GPIO number | unused | For DACs that do not self-strap. |
| `amp_enable_pin` | GPIO number | unused | Amplifier enable or unmute line. |
| `amp_enable_inverted` | boolean | `false` | `true` for an active-low enable line. |
| `amp_idle_timeout` | integer (seconds) | `60` | `0` disables the power-down. |
| `initial_volume` | percentage | `20%` | Used until a sender sets a volume. |
| `sample_rate` | integer (Hz) | `44100` | Also sets the frequency limit for `dsp: filters:`. |
| `audio_channel_mode` | enum | `stereo` | `stereo`, `mono`, `left`, `right`. |
| `buffer_size` | integer (bytes) | `1000000` | Internal buffer size. |
| `dsp` | block | none | See [dsp](#dsp). |
| `pair` | block | none | See [pair](#pair). |

Inherited from ESPHome's `media_player` and entity schemas: `icon`, `internal`, `disabled_by_default`, `entity_category`, `device_id`, `setup_priority`, and the triggers `on_state`, `on_idle`, `on_play`, `on_pause`, `on_announcement`, `on_turn_on`, `on_turn_off`.

Explained in [Configuration](../configuration/index.md).

## dsp

| Option | Type | Default | Notes |
|---|---|---|---|
| `enabled` | boolean | `true` | `false` bypasses the stage bit-exactly. |
| `preamp` | decibel | `0dB` | Broadband gain ahead of the filters. |
| `filters` | list | `[]` | At most 8 entries, applied in order. |
| `loudness` | block | off | See below. |
| `bass_enhancer` | block | off | See below. |
| `stereo_width` | block | off | See below. |
| `ambience` | block | off | See below. |
| `crosstalk_cancel` | block | off | See below. |
| `bass_protection` | block | off | See below. |
| `bass_limiter` | block | off | See below. |
| `limiter` | block | off | See below. |

Processing order: preamp, `filters`, loudness, bass enhancer, stereo width, ambience, crosstalk cancellation, bass protection, bass limiter, limiter. Explained in [Output DSP](../configuration/output-dsp.md) and [Dynamic stages](../configuration/dynamic-stages.md).

### dsp: filters

| Option | Type | Default | Notes |
|---|---|---|---|
| `type` | enum | required | `low_shelf`, `high_shelf`, `high_pass`, `low_pass`, `peaking`, `notch`. |
| `frequency` | frequency | required | Must not exceed 0.49 × `sample_rate` (21609 Hz at 44100). |
| `q` | float, 0.05 to 20 | `0.7071` | Butterworth. Shelves use the Q form. |
| `gain` | decibel | `0dB` | Only on `low_shelf`, `high_shelf` and `peaking`. A non-zero gain on the other types fails validation. |

### dsp: loudness

| Option | Type | Default | Notes |
|---|---|---|---|
| `frequency` | frequency | `100Hz` | Shelf corner. |
| `max_boost` | decibel, 0 to 15 | required | Lift once the volume is `range` below full. |
| `range` | decibel, 1 to 60 | `30dB` | Volume span over which the lift grows. |

### dsp: bass_enhancer

| Option | Type | Default | Notes |
|---|---|---|---|
| `frequency` | frequency, 30 to 300 Hz | `90Hz` | The speaker's lower limit. |
| `amount` | float, 0 to 3 | `1.0` | 1.0 is roughly equal level. |

### dsp: stereo_width

| Option | Type | Default | Notes |
|---|---|---|---|
| `width` | float, 0 to 3 | required | 1.0 unchanged, 0 mono. |
| `frequency` | frequency | `300Hz` | Bass below keeps its width. |
| `centre_lock` | float, 0 to 1 | `0` | 0 is plain mid/side. |

### dsp: ambience

| Option | Type | Default | Notes |
|---|---|---|---|
| `amount` | float, 0 to 1 | required | Wet level. |
| `decay` | time, 100 to 3000 ms | `400ms` | 60 dB decay time. |
| `damping` | frequency, 500 to 15000 Hz | `5000Hz` | In-loop low-pass. |

### dsp: crosstalk_cancel

| Option | Type | Default | Notes |
|---|---|---|---|
| `amount` | float, 0 to 0.95 | required | Depth. |
| `delay` | time, 10 to 300 us | `60us` | Extra path from a driver to the far ear. |
| `low_frequency` | frequency | `250Hz` | Lower band edge. |
| `high_frequency` | frequency | `5000Hz` | Upper band edge. |

### dsp: bass_protection

| Option | Type | Default | Notes |
|---|---|---|---|
| `frequency` | frequency, 20 to 200 Hz | required | 4th-order Butterworth high-pass. |

### dsp: bass_limiter

| Option | Type | Default | Notes |
|---|---|---|---|
| `enabled` | boolean | `true` | |
| `frequency` | frequency, 40 to 400 Hz | `120Hz` | Split frequency. |
| `threshold` | decibel, -30 to 0 | `-6dB` | |
| `release` | time, 20 to 2000 ms | `200ms` | |

Adds 1.5 ms of delay, included in the playout clock.

### dsp: limiter

| Option | Type | Default | Notes |
|---|---|---|---|
| `enabled` | boolean | `true` | |
| `threshold` | decibel, -24 to 0 | `-1dB` | |
| `release` | time, 10 to 2000 ms | `100ms` | |

Adds 1.5 ms of delay, included in the playout clock.

## pair

| Option | Type | Default | Role | Notes |
|---|---|---|---|---|
| `role` | enum | required | both | `primary` or `secondary`. |
| `peer` | IPv4 address | none | both | Primary: the secondary's address, required. Secondary: the only primary to accept; unset accepts the first that speaks. |
| `port` | port, up to 65534 | `7011` | both | UDP port for audio and state. The clock exchange uses the port above it. |
| `channel` | enum | `right` | primary | What the secondary plays: `left`, `right`, `mono`, `stereo`. |
| `name` | string, 1 to 63 characters | none | primary | Advertise the pair as its own destination under this name, and forward only its sessions. |
| `clock` | enum | `auto` | secondary | `auto`, `ptp`, `relay`. |
| `delay` | time, up to 100 ms | `0us` | secondary | Play this much later than the primary. |
| `standalone` | boolean | `false` | secondary | Keep this board's own destination, and play the primary's only while it forwards. |

Explained in [Stereo pair and groups](../configuration/stereo-pair.md). The same settings can be stored at runtime with `set_group_spec()`, which overrides `pair:` from the next boot; see [Text formats](text-formats.md).

## Validation rules

Beyond types and ranges, a config fails validation when:

- the target is not an ESP32, or the framework is not `esp-idf`;
- it also contains `sendspin` or `i2s_audio`;
- `dsp: filters:` has more than 8 entries;
- a `high_pass`, `low_pass` or `notch` filter has a non-zero `gain`;
- a filter `frequency` is above 0.49 × `sample_rate`;
- `pair: role: primary` has no `peer`;
- a primary sets `clock`, `delay` or `standalone`, or a secondary sets `channel` or `name`;
- a pin is used by another component.

It validates with a warning when a pair primary forwards `left` or `right` and its own `audio_channel_mode` is the same side. Set `audio_channel_mode` to the other side.

## Build settings the component adds

You do not set these. They are listed so you do not override them by accident.

| Setting | Value |
|---|---|
| `CONFIG_LWIP_MAX_SOCKETS` | `24` |
| `CONFIG_LWIP_UDP_RECVMBOX_SIZE` | `32` |
| ESP-IDF I2S driver (`esp_driver_i2s`) | included |
| Managed components | `espressif/libsodium ^1.0.21`, `espressif/esp_audio_codec ^2.5.0` |
| Memory profile flag | `AIRPLAY_PLATFORM_ESP32S3` or `AIRPLAY_PLATFORM_ESP32`, from the variant |

The component also loads `media_player` and `mdns`, and depends on `network`.
