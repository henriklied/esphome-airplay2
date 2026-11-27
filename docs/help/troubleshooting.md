# Troubleshooting

Find your symptom, then read the log line it points to.

!!! note "Get the logs first"

    Most faults are diagnosed from the board's log, at the default `logger: level: INFO`. Run `esphome logs <config>.yaml` (add `--device <ip>` if the `.local` name does not resolve) and reproduce the fault. For faults that happen unattended, keep a log connection open and save the output.

## Connected, but no audio

**Symptom.** The sender shows the board as playing, track metadata and volume work, and nothing comes out.

**Cause.** This is how almost every serious fault looks, because the RTSP control connection is independent of the audio path. The symptom alone narrows nothing down. Several distinct faults produce it.

**What to do.** Look for the `stalled:` line. It is logged at WARN, once a second, only while the sender has asked to play and the board is rendering silence:

```text
stalled: state=S reason=R clock_map=N epoch=N wanted_rtp=N blocks=N silent=N starts=N fallbacks=N concealed=N
```

| What `stalled:` shows | Meaning | Next |
|---|---|---|
| `clock_map=0`, `reason=WAIT_CLOCK_MAP` | No usable clock (PTP) | [No clock lock](#no-clock-lock) |
| `clock_map=1`, a preroll or fallback reason | Clock is fine, the buffered audio is not playable. `starts`/`fallbacks` climbing means the scheduler keeps retrying | Check Wi-Fi and [dropouts](#dropouts-and-concealment) |
| `blocks=0`, clock fine, `(superseded)` shortly before | A second sender took over and the listener stayed on the old port | [Second sender gets silence](#second-sender-gets-silence) |
| No `stalled:` line at all | The board is idle or paused on purpose, or it is not receiving the session | Check the sender and the picker |

### No clock lock

**Symptom.** `stalled:` shows `clock_map=0`. The `Anchor set:` line shows `ptp_locked=0` on every anchor, and `lead` is minutes or days instead of the usual -200 to -800 ms. A lead that large means PTP has not produced a single accepted sample this session; it is not a timing error to chase.

**Cause.** The board is not getting PTP `SYNC` from the sender. Read the `unlocked:` line, logged every 5 s while the clock is in trouble:

```text
unlocked: sync=N followup=N announce=N rejected=N samples=N quiet_event=N ms quiet_general=N ms rebuilds=N master=<id>
```

`SYNC` arrives on the event port (319), `FOLLOW_UP` and `ANNOUNCE` on the general port (320). `followup` is the field that separates the cases:

| `unlocked:` shows | Cause |
|---|---|
| `announce` climbing, `followup` frozen | The sender stopped emitting the sync pair. Seen with a Mac that stopped for seven minutes while its ANNOUNCEs kept arriving. |
| `followup` climbing, `sync` frozen | The board's event socket went deaf. Correlates with uptime. |
| `rejected` climbing | Packets arrive, but the master filter is pinned to the wrong clock. |
| `samples` climbing, no `LOCKED:` | The sender's timescale jumped under the same clock id. The filter reseeds after about 2 s and logs `Reseeding:`. |
| everything 0 | Nothing arrives. Either of the first two; `quiet_event`, `quiet_general` and `rebuilds` separate them. |

`quiet_event` high with `quiet_general` low is not diagnostic on its own: ANNOUNCE alone keeps the general port's timer fed.

**What to do.**

- After 30 s with no PTP datagram on a port, the board rebuilds its sockets and re-joins the multicast group. `PTP <event|general> port receiving again after N socket rebuild(s)` reports a recovery. This fixes a join that a switch pruned or a roam lost. It cannot fix a sender that stopped transmitting.
- If the event socket stays deaf, power-cycle the board.
- Before concluding the sender stopped, confirm it is still the PTP grandmaster. Without that check, a deaf board socket is the likelier reading.

With the local-anchor fallback, a board with no lock no longer stays silent: after 1 s it plays against its own clock, unsynchronised. A single speaker sounds normal. In a group, that board drifts until it re-locks. `diag_ptp_locked()` is the entity to watch in a group.

### Second sender gets silence

**Symptom.** You play from one device, select the board from a second without stopping the first, and the second shows "playing" with no sound. Stopping the first device first works.

**Cause.** The buffered listener stayed bound to the first sender's port. This is fixed in the component. The tell is `blocks=0` in `stalled:` with the clock fine, and `(superseded)` in the log shortly before. A `Buffered audio connection closed by peer` line here is the old sender's socket closing late, not the new sender hanging up.

**What to do.** Update the component. If it still happens, report it with the log.

### Silence after walking out of Wi-Fi range

**Symptom.** Audio stops after the phone roams away and back, while the control connection stays up.

**Cause.** The phone's old RTSP socket dies without a FIN. When the phone returns, the old session could tear down the new one. The component guards against this; the disconnect line prints `(superseded)` when the guard fires.

**What to do.** Update the component if you are on an old version.

## Dropouts and concealment

**Symptom.** Short gaps, smeared audio or repeated fragments, mostly with Spotify or system audio.

**Cause.** Realtime streams (Spotify, system audio) are not retransmitted: iOS ignores the board's resend requests, so a lost packet can only be concealed. Buffered streams (Apple Music, Music on a Mac) are TCP and retransmit. Loss happens on air, in the board's receive queue, or when the Wi-Fi radio leaves the channel.

**What to do.**

1. Check `holes=N (+N)` on the `playout:` line. `(+N)` is new concealment events this second, the number that matters.
2. Check `udp_drop` on the `rxpath:` line. Non-zero means packets reached the board and were dropped from the audio socket's receive queue. See [lwIP sizing](#lwip-sizing).
3. Check `rssi` on `rxpath:`. On a weak link it has been seen to fall further during stalls.
4. Set `wifi: post_connect_roaming: false` on a stationary speaker. ESPHome 2026.x scans for a better AP 5, 10 and 15 min after each connect. The component suppresses roaming while it plays, but the setting removes the scans entirely.
5. Leave `enable_btm` and `enable_rrm` off. AP steering mid-stream is a dropout.
6. If `under` is non-zero, the playback task is starved of CPU, a different problem from loss. Watch `pk` on `playout:` when adding DSP stages.

## Reading the 1 Hz telemetry

While a stream plays, the board logs up to three lines a second at INFO. `logger: level: WARN` silences them without removing the counters. The cost is not audible.

??? note "Example lines"

    ```text
    playout: raw=.. span=.. filt=.. drift=..ppm trims=../s (N) buffered=N
             concealed=N holes=N (+N) plc=N/N plc_us=N under=N dfail=N qdrop=N
             edrop=N ins=N dsp=N.N% pk=N.N%
    resend:  sent=N recovered=N stale=N unmarked_ok=N unmarked_stale=N
    rxpath:  stack=N task=N gap=N udp_watch=1 udp_ok=N udp_drop=N
             udp_drop_total=N away=N qrun=N rssi=N
    ```

| Field | Healthy | Meaning |
|---|---|---|
| `holes` `(+N)` | 0, no `+` | Concealment events; `(+N)` is new this second |
| `concealed` | 0 | Concealed samples |
| `buffered` | about 250 | Blocks buffered; dips during loss bursts |
| `drift` / `filt` | within ±40 ppm / ±0.05 ms | Clock health. Hundreds of ppm or milliseconds means a timing problem, not loss |
| `under` | 0 | I2S underruns: CPU starvation of playback |
| `ins` | steady about 125/s | Resampler insertion. Not a fault signal |
| `dsp` / `pk` | | DSP cost as a share of one core: average and worst block |
| `resend:` | absent | Printed only when a counter is non-zero. No `resend:` line is the healthy state |
| `task` | about 125/s | Packets read off the audio socket |
| `udp_drop` | 0 | Failed posts to the audio receive queue this second |
| `udp_watch` | 1 | The queue counter is installed. 0 comes with an `Audio UDP queue counters unavailable` warning |
| `away` / `qrun` | small | Longest gap between reads, and longest run of reads that found a packet waiting. Context for `udp_drop` |
| `rssi` | | Last beacon RSSI, 0 if unknown |

`stack` and `gap` appear only with `CONFIG_LWIP_STATS`. A positive `gap` alone does not prove an overflow; use `udp_drop`.

For Home Assistant, put the `diag_*` accessors on template sensors (see [Entities](../configuration/entities.md)). They change only during a fault, so they cost no recorder rows on a healthy board. `diag_is_stalled()` is true for about four seconds on every normal connect, so automations on it need `for: "00:00:30"`. Keep continuously varying values (drift, buffer depth) out of Home Assistant.

## lwIP sizing

**Symptom.** Constant concealment on realtime streams, or missing audio, with the control connection healthy.

**Cause.** The ESP-IDF defaults allow 10 sockets and 6 slots in each UDP receive queue. Six slots is about 48 ms of audio. A full queue drops packets silently. Measured on one board: 931 concealment events in 75 s at 6 slots, none in 80 s at 32.

**What to do.** Nothing, unless you override it. The component sets `CONFIG_LWIP_MAX_SOCKETS=24` and `CONFIG_LWIP_UDP_RECVMBOX_SIZE=32` itself. Remove any `sdkconfig_options` in your board YAML that set these: a board file overrides the component. Raising `SO_RCVBUF` does nothing, because lwIP compiles the option out by default.

## Board not found, or cannot flash

**Symptom.** The board is missing from the AirPlay picker, or `esphome run` cannot find it.

**Cause.** Senders find the board through mDNS (`_airplay._tcp` and `_raop._tcp`), and OTA uses its `.local` name. Neither crosses a subnet or VLAN unless your network forwards mDNS. PTP is a separate multicast group from mDNS, so forwarding mDNS does not carry it.

**What to do.**

- Flash and read logs by IP: `esphome run <config>.yaml --device 192.168.1.50`.
- If senders and boards are on different subnets, the network must forward mDNS for discovery and PTP multicast for the clock.
- OTA fails while a stream is live (`Device closed connection without responding`). Stop playback first, then re-select the speaker after the reboot.

## Build or boot fails

| Symptom | Cause | Fix |
|---|---|---|
| `Requirement espressif__libsodium and requirement libsodium are both added` at CMake configure | `api: encryption:` pulls in a second libsodium | Remove `encryption:` from `api:` |
| `Platform not found` for `media_player` | The component is the media_player | Remove the `media_player: - platform: airplay_receiver` entry |
| Validation error naming `sendspin` or `i2s_audio` | Both want the same I2S peripheral | Remove the other component |
| Board does not boot after flashing (ESP32-S3 N8R8) | `flash_mode: opi` describes the PSRAM, not the flash | Use `flash_mode: dio` |

## Distortion on loud peaks

**Cause.** A positive filter gain without a matching negative `preamp`. The stage clamps, so peaks distort.

**What to do.** Check the boot log for `DSP boosts up to ... Set dsp.preamp to ...` and set that preamp, or enable the `limiter` stage. See [Output DSP](../configuration/output-dsp.md).

## Other symptoms

- **Clicks on a mid-playback seek.** The output ring is purged and the DAC clock stops. Known, not fixed.
- **About 2 s from resume to sound on Spotify.** The sender stamps its first packet about 1.8 s past the anchor. No earlier audio exists to play.
- **A playing Mac session stops when another client connects**, for example Music Assistant. A new RTSP client supersedes the playing one. Open issue.
- **The ESPHome version banner repeats in the log.** It is printed on every log client connect, not on reboot.

!!! warning "Logging traps"

    Do not enable `audio_rt: DEBUG` to investigate loss: it logs per NACK and per stale packet from the audio receive task, which can overflow the task's log buffer and cause the stutter you are measuring. ESPHome also refuses a per-tag level more verbose than the global level, so a single DEBUG tag needs the global level raised. The 1 Hz counters exist at INFO for this reason.
