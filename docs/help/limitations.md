# Limitations

What the component does not do, and approaches that have already been tried and do not work.

## Not implemented

- **AirPlay 1 / RAOP.** RSA auth, the FairPlay handshake and AES-CBC audio encryption are deliberately not ported. The board is an AirPlay 2 receiver only.
- **Bluetooth A2DP, SPDIF and USB outputs.** Output is I2S to an external DAC.
- **Surround.** Main supports stereo AAC. The earlier 5.1 work (FDK decoder, surround negotiation) is parked at the tag `parked/surround-5.1`.
- **Fetching media.** Browse, play-media and announce are cleared from the `media_player` feature flags. The board plays what a sender pushes; it cannot be told to fetch a URL.
- **Track metadata in the entity.** ESPHome's `media_player` has no title field. The title is available from a lambda only.

## Platform constraints

- ESP32 or ESP32-S3 with PSRAM. PSRAM is not optional.
- ESP-IDF framework only. The Arduino framework is not supported.
- `api:` without `encryption:`. See [FAQ](faq.md#why-cant-i-enable-api-encryption).
- It cannot share a config with `sendspin` or an `i2s_audio` speaker. They all want the same I2S peripheral, and validation fails.
- The output DSP holds at most eight biquad filters.
- A plain ESP32 runs a smaller buffer profile and has less headroom for Wi-Fi hiccups than the S3.

## Behaviour to expect

- **One session per board.** The newest connection supersedes the playing one. A second RTSP client, such as Music Assistant connecting, has been seen to drop a playing Mac session silently while Music kept showing "playing".
- **Realtime streams cannot be retransmitted.** iOS ignores resend requests on the realtime stream (Spotify, system audio), so a lost packet can only be concealed.
- **Group sync gives way to playback.** A board that loses its network clock keeps playing on its own clock instead of going silent, and drifts from the group until it re-locks.
- **A mid-playback seek can click.** The output ring is purged, which stops the DAC clock. A volume ramp around it has not been written.
- **OTA fails while a stream is live.** Stop playback first.

### Stereo pair (`pair:`)

- Output skew between the two speakers has not been measured.
- The secondary is silent for up to one primary buffer (about 4.5 s on a buffered stream) when it joins or rejoins, because it only has audio decoded after it joined.
- The secondary's `media_player` mirrors the pair and ignores commands. Forwarding them to the primary is not built.
- Short underrun bursts appear at session start and handover, not in steady playback.
- Advertising an Apple persistent group was tried: it gave one picker entry, but iOS could not play to it. `pair:` makes the pair an ordinary single receiver instead.

## Dead ends

These look promising and have been tested. Do not spend time on them.

| Idea | Result |
|---|---|
| Ask the sender for the buffered (TCP) stream with `tp=TCP` in the advert | The sender still opens the realtime stream. Buffered mode is the sender's choice and depends on the source app. |
| Rely on resend requests (NACKs) to fill losses | iOS does not answer them on AirPlay 2 realtime streams. Measured over 77 s: zero recovered. |
| Raise `SO_RCVBUF` on the audio socket | lwIP compiles the option out by default; the call does nothing. Queue depth comes from `CONFIG_LWIP_UDP_RECVMBOX_SIZE`, which the component sets. |
| Set lwIP sizes in the board YAML | The component sets them. A board file overrides the component's values. |
| Ping round trip as a Wi-Fi health test | The host's own link confounds it. Use the `playout:` and `rxpath:` lines. |
| Blame airtime contention for concealment with two boards | Two boards at about 1.4 Mbit/s each is small for 802.11n. The cause was a 6-slot receive queue that could not absorb arrival bursts. |
| `SETPEERS: no timing peer` warning | That is the AirPlay 1 NTP fallback path. AirPlay 2 uses PTP and locks normally. Benign. |
| `audio_rt: DEBUG` to investigate loss | It logs per NACK from the receive task and can cause the stutter being measured. |
| A per-tag DEBUG log level under global INFO | ESPHome refuses it at validation. |

## License

The component is licensed Non-Commercial. Commercial use, including selling devices with this firmware, needs written permission from the upstream author. For the same reason it cannot be merged into ESPHome. See [Credits and license](../reference/credits-license.md).
