# One AirPlay destination for two Sonocotta boards

Proposed implementation for two Sonocotta Amped ESP32-S3 boards on the same LAN. Selecting the pair
once in AirPlay should play the left speaker on one board and the right speaker on the other, with one
volume control and synchronized playback.

The October 7 hardware experiment advertised an Apple persistent group and got one picker entry, but
iOS could not play to it. The sender attempted a type-130 MediaRemote handoff to the group leader.
The local `group-experiment` commit `e0f40fd` records that approach. In the design below, AirPlay sees
one ordinary receiver and the two boards handle audio distribution and synchronization themselves.
[Stereo-pair research](STEREO-PAIR-RESEARCH.md#5-the-other-architecture-the-sonos-model)

## Firmware roles

Use the existing `airplay_receiver:` configuration entry point, with the role selected in YAML.

| Role | Behaviour |
| --- | --- |
| Standalone (default) | Advertises and plays AirPlay with the current channel-mode and DSP options. |
| Pair primary | Advertises `_airplay._tcp` and `_raop._tcp` under the pair name. Decodes the stereo stream once, plays one channel locally, and forwards the other to the configured secondary. |
| Pair secondary | Receives the forwarded channel and plays it at the scheduled time. Starts only the peer link and playout path, with AirPlay discovery, RTSP and decoding disabled. |

## Implementation sequence

1. **Forward decoded samples.** Tap PCM where decoded blocks enter the playback timeline, before
   local channel selection and ahead of their playout deadline. Keep the primary's normal render
   path. Send the secondary channel through a bounded PSRAM queue drained by a separate network
   task, so a stalled link cannot block decoding or I2S. Start with a fixed peer and timestamped
   16-bit mono PCM datagrams. Carry a session generation, sample position, sample rate and sequence
   number so the secondary can place packets and detect gaps.
2. **Schedule both outputs.** For the first prototype, have both boards listen to the AirPlay 2
   sender's PTP clock. Forward the clock identity and stream anchor to the secondary. Use the
   existing clock map and scheduler to place samples on one presentation timeline, including
   enough lead time for the peer buffer. Measure actual DAC output skew and account for each board's
   output and DSP delay. Use the measurements to decide whether the sender clock is sufficient or
   whether the peer link needs its own clock synchronization. Start with per-channel EQ for the
   timing tests. Stereo-dependent DSP needs access to both channels before the split.
3. **Synchronize playback state.** Send volume and mute updates, pause and resume, and stream resets
   for seeks, rate changes and teardown. Flush both queues on a generation change. If the secondary
   loses the link or timing lock, mute it and let the primary keep playing. Have the secondary
   rejoin at a future scheduled sample once it has a fresh anchor and enough buffered audio.
4. **Expose the roles in ESPHome.** Add configuration and validation for the peer and channel
   assignment. Document examples for the two boards. Keep allocations and network tasks scoped to
   the active role, so a standalone board pays only for its existing playback path.

Relevant code lives in [audio receiver](components/airplay_receiver/audio/audio_receiver.cpp),
[clock mapping](components/airplay_receiver/audio/audio_clock_map.h),
[playout scheduler](components/airplay_receiver/audio/audio_scheduler.cpp), and
[mDNS discovery](components/airplay_receiver/transport/mdns_airplay.cpp).

## Resource budget and effort

- **Flash and PSRAM.** The November 21 Geneva build was 1.24 MiB in a 3.75 MiB OTA slot. The target
  has 8 MiB of PSRAM. A 250–500 ms mono PCM queue needs about 23–47 KiB at 48 kHz, before headers and
  bookkeeping. The stream adds about 0.77 Mbit/s of payload plus network overhead.
- **CPU.** The current stereo AAC decoder takes about 3 ms per 23 ms frame, and a prior Geneva
  measurement put the full output DSP at about a third of the audio core. Forwarding PCM adds a
  copy and packet handling. Measure CPU load and I2S underruns with the link active. [AAC benchmark and DSP telemetry](components/airplay_receiver/FIELD-NOTES.md)
- **Internal RAM.** The AAC decoder needs about 51 KB of internal RAM to open. Check free internal
  heap and the largest free block during pairing, stream startup and reconnects. Keep the peer
  queues in PSRAM and track their high-water marks during playback. [Decoder allocation](components/airplay_receiver/audio/audio_decode_worker.cpp)

Estimate 2–4 engineering days for a fixed-peer prototype and 1–3 weeks total for everyday use,
including hardware testing. The main uncertainty is holding stereo alignment through Wi-Fi jitter,
clock loss and reconnects. Relay CPU and memory measurements are still pending.

## Hardware verification

- Check that iPhone and Mac each show one AirPlay entry and that selecting it plays both boards.
  Play a left/right identification track to verify channel assignment.
- Capture both outputs simultaneously. Use under 1 ms of output skew during steady playback as the
  initial target. Check for drift over a long session and measure alignment again after pause,
  resume, seek, and secondary Wi-Fi disconnect and rejoin.
- Exercise buffered AAC and realtime ALAC on the pair, including volume and mute. Record peer queue
  occupancy, late or dropped packets, I2S underruns, and minimum free internal heap and largest
  free block. Run the existing standalone config and compile gate and check standalone playback
  on hardware.

## Extension to six-board 5.1

The primary would accept a 48 kHz 5.1 AirPlay stream and play one channel locally, for example
front left. Five secondaries would play front right, centre, surround left, surround right and
LFE. Give each board an explicit channel assignment and use the same presentation timestamps and
session state across all six. Verify the channel mapping with a six-channel identification track.
For TV use, also measure lip sync and support individual speaker level and delay trims.

Current `main` supports stereo AAC. The `parked/surround-5.1` tag has the earlier AirPlay 5.1
negotiation and FDK decoder. The hardware test received 5.1 AAC from Apple TV and rendered a stereo
downmix. A 5.1 primary would need to keep the six decoded channels and route them individually.
[Decoder history and Apple TV measurements](components/airplay_receiver/FIELD-NOTES.md#aac-decoder-fdk-and-what-airplay-surround-looks-like-2026-10-06)

Five unicast mono streams at 48 kHz and 16 bits total 3.84 Mbit/s of outgoing payload. Half a second
of queued audio per stream takes about 234 KiB of PSRAM on the primary. The Wi-Fi access point has
to send each stream back down to its destination when the boards are all on Wi-Fi, in addition to
carrying the incoming AirPlay stream. Measure packet lateness and airtime with all five peers active.

The earlier Apple TV run averaged 15.2–16.3 ms per 21.3 ms AAC frame and peaked at 28 ms. It used
both cores and once failed to start its decode worker with roughly 30–35 KB of internal RAM free.
First exercise five peers with synthetic PCM to establish the network and timing budget. Then run
live 5.1 AAC decode alongside all five streams and measure decode backlog, minimum internal heap,
output skew and underruns. Flash and PCM buffer sizes look manageable. CPU, internal heap and
Wi-Fi timing still need hardware measurements before relying on one S3 for the entire group.
[Apple TV test](components/airplay_receiver/FIELD-NOTES.md#apple-tv-51-plays-2026-10-06-night-measured)
