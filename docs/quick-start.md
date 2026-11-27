# Quick start

Flash a Sonocotta Amped-ESP32-S3 and play to it from an iPhone or Mac. The config below is the repository's `examples/amped-s3.yaml`.

!!! note "Before you start"

    You need ESPHome 2025.1.0 or newer, an Amped-ESP32-S3 (or another board from [Boards and pins](configuration/hardware.md)), and a USB cable for the first flash. Read [Installation](installation.md) for the requirements.

## Steps

1. Create a folder for the device and put `partitions.csv` from the repository root in it.
2. Create `secrets.yaml` in the same folder:

    ```yaml
    wifi_ssid: "your-ssid"
    wifi_password: "your-wifi-password"
    ota_password: "your-ota-password"
    ```

3. Create `airplay-living-room.yaml`:

    ```yaml
    esphome:
      name: airplay-living-room
      friendly_name: Living Room
      min_version: 2025.1.0

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

    wifi:
      ssid: !secret wifi_ssid
      password: !secret wifi_password

    api:  # no encryption: it breaks the build

    ota:
      - platform: esphome
        password: !secret ota_password

    logger:
      level: INFO

    external_components:
      - source: github://henriklied/esphome-airplay2
        components: [airplay_receiver]

    airplay_receiver:
      id: airplay
      name: "Living Room"
      i2s_bclk_pin: 14
      i2s_lrclk_pin: 15
      i2s_dout_pin: 16
      amp_enable_pin: 17
      sample_rate: 44100
      amp_idle_timeout: 60
    ```

4. Validate, build and flash:

    ```bash
    esphome config airplay-living-room.yaml
    esphome run airplay-living-room.yaml
    ```

5. Open Control Center on the iPhone or Mac and pick "Living Room" as the output. The board appears within a few seconds of boot.
6. Add the device to Home Assistant through the ESPHome integration. You get one `media_player` entity named after `name:`.

## What to expect

- The first start plays at 20% volume (`initial_volume`). Once a sender has moved the volume slider, the board keeps that volume across reboots.
- The amplifier is switched off after 60 seconds without audio (`amp_idle_timeout`). The PCM5100 idles near silent, but the TPA311x hisses audibly in a quiet room.
- `name:` is the name senders see and the entity name in Home Assistant.

## If there is no sound

- Watch `esphome logs`: at `level: INFO` the receiver logs a playout line every second while it plays.
- Check that all three I2S pins are set. Without them the receiver pairs and plays silently.
- See [Troubleshooting](help/troubleshooting.md).

## Next steps

- [Configuration](configuration/index.md): all base options.
- [Output DSP](configuration/output-dsp.md): correct the speaker on the board.
- [Stereo pair and groups](configuration/stereo-pair.md): play one destination on two boards.
- [Entities](configuration/entities.md): DSP controls and diagnostics in Home Assistant.
