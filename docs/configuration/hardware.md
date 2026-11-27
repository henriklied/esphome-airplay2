# Boards and pins

Two board classes are supported, each with its own memory profile.

## Supported boards

| Board class | Part | PSRAM | PCM ring |
|---|---|---|---|
| ESP32-S3 (primary) | ESP32-S3-WROOM-1-N8R8 | 8 MB octal | 1000 frames, 64 KiB internal reserve |
| ESP32 | ESP32-WROVER class | 4 MB quad | 200 frames, 32 KiB internal reserve |

The profile is selected at build time from the target variant. There is nothing to configure. The S3 is the board the component is developed and measured on, and it has the headroom to ride out a Wi-Fi hiccup that the plain ESP32 does not.

!!! warning "PSRAM is required"

    PSRAM is not optional on either class. Set the `psram:` block that matches your board (octal or quad).

The DAC is expected to be a PCM5100-class I2S part.

## Sonocotta Amped-ESP32-S3

The reference board: ESP32-S3-WROOM-1-N8R8 (8 MB flash, 8 MB octal PSRAM) with an onboard PCM5100 DAC and a TPA311x amplifier.

| Signal | Option | GPIO |
|---|---|---|
| I2S bit clock | `i2s_bclk_pin` | 14 |
| I2S word select | `i2s_lrclk_pin` | 15 |
| I2S data | `i2s_dout_pin` | 16 |
| Amplifier enable | `amp_enable_pin` | 17 |

```yaml
esp32:
  board: esp32-s3-devkitc-1
  variant: esp32s3
  cpu_frequency: 240MHz
  flash_size: 8MB
  flash_mode: dio
  flash_frequency: 80MHz
  framework:
    type: esp-idf
    version: recommended
  toolchain: esp-idf
  partitions: partitions.csv

psram:
  mode: octal
  speed: 80MHz
```

Keep `flash_mode: dio`. `opi` describes the PSRAM, and on the flash it produces a board that does not boot. Use the repository's `partitions.csv`, see [Installation](../installation.md#partition-table).

## Generic ESP32 + PCM5102A

A WROVER-class ESP32 and a PCM5102A I2S DAC breakout. There is no amplifier: use line out into powered speakers or an existing amp, and leave `amp_enable_pin` unset.

| Signal | Option | GPIO |
|---|---|---|
| I2S bit clock | `i2s_bclk_pin` | 26 |
| I2S word select | `i2s_lrclk_pin` | 25 |
| I2S data | `i2s_dout_pin` | 22 |
| Amplifier enable | `amp_enable_pin` | not used |

Wire the PCM5102A with SCK to GND (it generates its own clock), FLT, DEMP and XSMT per the breakout's defaults, and VIN to 3V3.

```yaml
esp32:
  board: esp-wrover-kit
  variant: esp32
  cpu_frequency: 240MHz
  framework:
    type: esp-idf
    version: recommended
  toolchain: esp-idf

psram:
  mode: quad
  speed: 40MHz
```

On a 4 MB board this builds within ESPHome's default partition table. The smaller memory profile gives less tolerance for Wi-Fi hiccups, which shows up as occasional concealment on a congested 2.4 GHz network.

## Other boards

Other ESP32 and ESP32-S3 boards can work. The memory values are tuned for the two parts above.

- Pick the `esp32` variant and `psram:` mode that match your board.
- Set `i2s_mclk_pin` if your DAC needs a master clock and does not self-strap.
- Set `amp_enable_inverted: true` if the amplifier's enable line is active low.

## Pin rules

- The I2S and amp pins are registered with ESPHome's GPIO conflict detection. Reusing one elsewhere, for a button for example, fails validation.
- On the S3, avoid the strapping pins 0, 3, 45 and 46 for buttons and other inputs.
- The receiver needs the I2S bus. `i2s_audio` and `sendspin` in the same config fail validation.

## Examples

Each example in the repository is validated and compiled.

| File | What it shows |
|---|---|
| [`amped-s3.yaml`](https://github.com/henriklied/esphome-airplay2/blob/main/examples/amped-s3.yaml) | The reference board. |
| [`generic-esp32.yaml`](https://github.com/henriklied/esphome-airplay2/blob/main/examples/generic-esp32.yaml) | WROVER ESP32 with a PCM5102A, line out. |
| [`dsp-speaker-correction.yaml`](https://github.com/henriklied/esphome-airplay2/blob/main/examples/dsp-speaker-correction.yaml) | A filter cascade with Home Assistant sliders. |
| [`buttons.yaml`](https://github.com/henriklied/esphome-airplay2/blob/main/examples/buttons.yaml) | Play/pause and volume buttons on GPIO 4, 5 and 6. |
| [`display.yaml`](https://github.com/henriklied/esphome-airplay2/blob/main/examples/display.yaml) | A 20x4 character LCD on I2C (SDA 8, SCL 9) showing title, state, volume and PTP lock. |
| [`stereo-pair-primary.yaml`](https://github.com/henriklied/esphome-airplay2/blob/main/examples/stereo-pair-primary.yaml) | The primary half of a `pair:`. |
| [`stereo-pair-secondary.yaml`](https://github.com/henriklied/esphome-airplay2/blob/main/examples/stereo-pair-secondary.yaml) | The secondary half of a `pair:`. |
| [`secrets.example.yaml`](https://github.com/henriklied/esphome-airplay2/blob/main/examples/secrets.example.yaml) | The secrets the examples expect. |
