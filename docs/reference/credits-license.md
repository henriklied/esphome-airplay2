# Credits and license

## Credits

- [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32) by Remi Bouteiller. This component is a port of it. The protocol logic, pairing crypto, timing engine and audio pipeline all originate there.
- David Bryant's [sinc resampler](https://github.com/dbry/audio-resampler), vendored for sample-rate conversion.
- The [ESPHome](https://github.com/esphome/esphome) contributors.

## License

The repository is distributed under a **Non-Commercial License**, copyright Remi Bouteiller and Henrik Lied (the ESPHome port and modifications). The full text is in [LICENSE](https://github.com/henriklied/esphome-airplay2/blob/main/LICENSE).

| You may | Conditions |
|---|---|
| Use, copy, modify and distribute the software for non-commercial purposes | Include the copyright notice and permission notice in all copies or substantial portions |
| Use it commercially | Only with prior written permission from the copyright holder. For commercial licensing, contact the upstream author at bouteiller.remi@gmail.com |

Commercial use includes selling devices with this firmware and shipping it in a product. The software is provided as is, without warranty.

### Why not an open-source license

The AirPlay 2 protocol logic (HomeKit pairing, the RTSP control plane, the audio pipeline and the playout timing engine) is a port of rbouteiller/airplay-esp32, which is published under this Non-Commercial License. A derivative work cannot grant rights its source did not grant, so this repository carries the same terms.

Consequences:

- It is not OSI-approved open source.
- It cannot be relicensed under MIT or Apache-2.0 without the upstream author's permission.
- It cannot be merged into ESPHome, which is permissively licensed.

## Third-party components

Some parts carry their own terms in addition to the license above. The list is in [THIRD-PARTY-NOTICES.md](https://github.com/henriklied/esphome-airplay2/blob/main/THIRD-PARTY-NOTICES.md).

### Vendored in the repository

| Component | Where | License |
|---|---|---|
| rbouteiller/airplay-esp32 | `crypto/`, `transport/`, `audio/`, `decoder/`, `timing/` | Non-Commercial (governs the repository) |
| David Bryant's sinc resampler | `audio/audio_resample.cpp` | 3-clause BSD, in [licenses/audio-resampler.txt](https://github.com/henriklied/esphome-airplay2/blob/main/licenses/audio-resampler.txt) |

### Fetched at build time

These are pulled by the ESP-IDF component manager and are not redistributed in the repository. Their licenses apply to the firmware you build.

| Component | Source | License |
|---|---|---|
| `espressif/libsodium` | Espressif Component Registry | ISC |
| `espressif/esp_audio_codec` | Espressif Component Registry | Espressif Modified MIT (Espressif products only) |
| `espressif/mdns` | Espressif Component Registry | Apache-2.0 |
| `mbedtls` | built into ESP-IDF | Apache-2.0 |

`esp_audio_codec` may only be used on Espressif hardware. That is no constraint here, since the component only builds for ESP32 targets.
