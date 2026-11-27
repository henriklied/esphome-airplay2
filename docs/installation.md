# Installation

Add the component to an ESPHome device config and set up the board for it.

## Requirements

| Requirement | Value |
|---|---|
| Board | ESP32-S3 or ESP32, with PSRAM. PSRAM is not optional. |
| Framework | `esp-idf`. Arduino is not supported. |
| ESPHome | 2025.1.0 or newer. Verified against 2026.8.2. |
| DAC | A PCM5100-class I2S DAC. |
| API | `api:` without `encryption:`. |

!!! warning "No API encryption"

    `api: encryption:` pulls in `esphome/noise-c`, which ships its own `libsodium`. The pairing crypto here uses `espressif/libsodium`, and the IDF component manager refuses to pick one. The build stops at CMake configure, before anything compiles:

    ```
    ERROR: Cannot process component requirements. Requirement espressif__libsodium
    and requirement libsodium are both added as "project_managed_components".
    Can't decide which one to pick.
    ```

    Use a plain `api:` block. All examples do.

## Add the component

Add the repository as an external component:

```yaml
external_components:
  - source: github://henriklied/esphome-airplay2
    components: [airplay_receiver]
```

Then add an `airplay_receiver:` block. The [Quick start](quick-start.md) has a complete config, and [Configuration](configuration/index.md) lists every base option.

!!! note "Branches"

    Without a ref, ESPHome fetches the repository's default branch. To build from a specific branch, tag or commit, append it: `github://henriklied/esphome-airplay2@<ref>`.

## Set up the ESP32 block

The component checks at validation that the target is an ESP32 and the framework is `esp-idf`. For the reference board, an ESP32-S3-WROOM-1-N8R8:

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

!!! warning "Flash mode"

    The N8R8 has quad flash and octal PSRAM. `opi` describes the PSRAM, not the flash. Setting `flash_mode: opi` produces a board that does not boot. Use `dio`.

For a plain ESP32 board, see [Boards and pins](configuration/hardware.md).

## Partition table

On an 8 MB board, use the repository's `partitions.csv` (an 8 MB OTA layout). The default ESPHome layout leaves too little room for two OTA slots once the codec and crypto libraries are linked in.

1. Download [`partitions.csv`](https://github.com/henriklied/esphome-airplay2/blob/main/partitions.csv) from the repository root.
2. Put it next to your device YAML.
3. Point `esp32: partitions:` at it.

The examples reference it as `../partitions.csv` because they live one folder down in the repository.

The generic ESP32 example on a 4 MB board needs no custom table: it builds to 1.11 MB, 60% of the 1.79 MB OTA slot in ESPHome's default layout.

## Components that conflict

The receiver needs the I2S bus. A config that also has `sendspin` or an `i2s_audio` component fails validation.

Do not add `media_player: - platform: airplay_receiver`. It does not exist, and validation fails with `Platform not found`. Two instances would mean two RTSP servers on port 7000 and two claims on the same I2S peripheral.

## Settings the component makes for you

The component sets `CONFIG_LWIP_MAX_SOCKETS` to 24 and `CONFIG_LWIP_UDP_RECVMBOX_SIZE` to 32, and re-enables ESP-IDF's I2S driver. The IDF defaults for both lwIP values are too small for a realtime receiver, and both fail silently: metadata and transport work while audio is missing or full of concealment.

!!! warning

    Do not set these two lwIP options in your board YAML. A value there overrides the component's.

## Validate and build

```bash
esphome config my-speaker.yaml
esphome compile my-speaker.yaml
```

`esphome config` should end with `Configuration is valid!`, and `esphome compile` with `Successfully compiled program.`
