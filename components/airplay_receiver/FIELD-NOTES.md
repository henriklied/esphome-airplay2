# Field notes -- what hardware taught us

Findings from running this component on Amped-ESP32-S3 boards (Boston X90 pair,
Geneva Model M) against iOS and macOS senders. Everything here was measured on
hardware; where something is inference it says so.

**Read this before debugging audio artefacts.** Several obvious-looking leads
are dead ends and are listed as such at the bottom. This file is the subset of a
longer private deployment log that matters to anyone working on the component
itself; where a finding depends on evidence not reproduced here, it says so.

## The five failures that look identical from outside

All five present as **"the sender is connected, metadata and transport
work, and no audio comes out."** That symptom does not narrow anything down --
it is the default appearance of every serious bug in this component, because the
RTSP control socket is independent of the audio path. Do not treat it as a
clue.

### 1. The UDP receive queue was six packets deep

`CONFIG_LWIP_UDP_RECVMBOX_SIZE` is a **slot count, not a byte budget**, and the
IDF default is 6. ALAC realtime is 352 frames per packet, so 44100/352 = **125
packets/s** -- six slots is about 48ms of headroom on the audio socket.

When it is full, `recv_udp()` (`lwip/src/api/api_msg.c`) calls
`sys_mbox_trypost()`, and on failure deletes the packet and returns. No counter,
no stat, no error -- only a `LWIP_DEBUGF` that is compiled out. The packet was
received off the air, acknowledged at the 802.11 layer, and then discarded
inside the board. It surfaces only as an RTP sequence gap, which is exactly what
a packet lost on air looks like.

Measured on `boston-x90-r`, same source, same network, minutes apart:

| | duration | `holes` | `concealed` | seconds w/ NACKs | `under` |
|---|---|---|---|---|---|
| `CONFIG_LWIP_UDP_RECVMBOX_SIZE=6` | 75s | **931** | 217183 | 72 | 0 |
| `CONFIG_LWIP_UDP_RECVMBOX_SIZE=32` | 80s | **0** | 0 | 0 | 0 |

`__init__.py` now sets this (and `CONFIG_LWIP_MAX_SOCKETS`) in
`_add_lwip_requirements()`. **Keep it there.** It used to live in the board YAML,
which meant the component silently mis-performed for anyone who dropped it in
without knowing. A board file that sets these itself will override the
component's value, so don't.

`SO_RCVBUF` cannot substitute for it: `CONFIG_LWIP_SO_RCVBUF` is off by default,
so lwIP compiles the option out and `setsockopt()` fails with `ENOPROTOOPT`. The
call in `socket_utils_bind_udp()` asking for 128KB is a **silent no-op** -- its
return value is not checked. Either enable the Kconfig or drop the argument; do
not leave it looking effective.

### 2. A superseded RTSP slot tore down its replacement

Reproduce by walking a phone out of AP range mid-stream and back.

The sender's RTSP socket dies without a FIN. There is no `SO_KEEPALIVE` on it,
and the header idle deadline in `client_task()` only applies once `buf_len > 0`,
so the slot's task stays blocked in `recv()` indefinitely -- nothing notices.

When the phone returns, `server_task()` calls `signal_old_client_stop()`, which
shuts the dead socket down **without waiting**, then immediately creates the new
client task. Both now run. The new one completes SETUP/RECORD and starts audio;
the old one wakes, reaches `cleanup:`, and emits `TRANSPORT_EVENT_DISCONNECTED`
-- which calls `audio_receiver_stop()` and `audio_output_stop()` on the session
that just replaced it, and `stop_event_port_task()` closes the event port it
just opened.

`slot->should_stop` distinguishes the cases: set by the server task when the
slot is superseded, clear when the sender itself went away. Only the latter may
emit `DISCONNECTED`. The disconnect log line prints `(superseded)` when the
guard fires.

A clean disconnect never races, which is why ordinary use never showed this and
a roam shows it every time -- the dead socket only wakes once the replacement is
already live.

### 3. The sender's PTP grandmaster stops sending (UNCONFIRMED)

> **Correction, 2026-09-09.** This failure was written up from a 2026-09-08
> incident whose key evidence -- "hard-rebooting the phone fixed it
> immediately" -- the operator has since said did not happen; he never rebooted
> the phone. **No observed incident is known to have been sender-side.** The
> failure mode below is real in the sense that the code permits it and the
> chain is verified, but it has not been witnessed, and failure 4 is the
> better-evidenced explanation for what was seen. Do not reach for "blame the
> sender" on the strength of this section.

A sender that stops sending PTP `SYNC` -- one that left the network and came
back without resuming its grandmaster role -- produces a symptom identical to
the two above, with the board innocent.

The chain, all verified in code, is worth knowing because every link is silent:

1. No `SYNC` reaches `ptp_task()`, so `ptp.sample_count` stays 0 and
   `ptp_clock_is_locked()` never goes true.
2. `audio_receiver_arm_engine_v2_anchor()` returns early on `!locked`, so the
   anchor is **never published** to the engine.
3. `clock_map->valid` stays false.
4. `audio_scheduler_render()` takes the `if (!clock_map->valid)` branch and
   calls `output_silence()`. Every render, forever.

Nothing logs an error anywhere along it. The RTSP session, metadata, the RTP
receive path and the NACK machinery all stay perfectly healthy, because none of
them need the clock.

> **Confirmed, and no longer fatal, 2026-09-09.** Both boards witnessed the same
> master (a Mac, `bac6d397d0d50008`) stop emitting the sync pair for seven
> minutes while its ANNOUNCEs kept arriving -- one board with 3424 SYNC banked
> and frozen, the other cold-booted at `sync=0 followup=0 announce=103`, on
> independent sockets and independent IGMP joins, `rejected=0` throughout. Two
> receivers cannot go deaf identically at once, so this failure is real and
> observed after all; the retraction above applies only to the phone-reboot
> evidence originally cited for it.
>
> Step 4 no longer holds indefinitely: after
> `ENGINE_V2_LOCAL_ANCHOR_AFTER_US` (1 s) with an anchor pending, no lock and
> PCM in the timeline, `audio_receiver_arm_engine_v2_anchor()` publishes the
> anchor against the local instant it arrived instead of the sender's
> timestamp, and the stream plays unsynchronised. This is upstream's
> `SYNC_MODE_NONE` behaviour, which this port had removed. Two details are
> load-bearing: the anchor stays *pending*, so a later lock replaces it with
> the sender's own and restores group sync; and `wait_for_anchor()` must be
> skipped while the fallback is armed, because the sender re-anchors about once
> a second and each of those would otherwise reset the clock map and tear the
> playback back down.

**Diagnosing it takes one line.** `Anchor set: ... lead=N ms ptp_locked=0`:

- `ptp_locked=0` on every anchor is the fault.
- `lead` normally sits at -200 to -800 ms (the sender pre-buffers, so the anchor
  is slightly old). A `lead` of **minutes or days**, in either direction, means
  `ptp_clock_get_offset_ns()` is returning exactly 0 -- i.e. PTP has never
  produced a single accepted sample this session. It is not a timing error to
  be chased; it is the absence of a clock.
- The `unlocked: sync=.. announce=.. rejected=..` line (see below) narrows it
  but does not settle it: all zero means nothing is arriving, which is failure
  3 *or* failure 4 -- `quiet_event`/`quiet_general`/`rebuilds` are what separate
  a dead sender from a deaf socket. `rejected` climbing means packets arrive but
  the master filter drops them, which is neither.

Confirm the sender is still a grandmaster before concluding it is failure 3;
absent that check, failure 4 is the likelier reading.

### 4. The board's own PTP receive path went deaf

Reported 2026-09-09 on the same board: the sender is fine, the board has
stopped hearing it. The chain from step 1 onward is identical to failure 3, and
so is everything the telemetry shows -- `unlocked:`'s `quiet_event`/
`quiet_general` split is the only thing that separates them in a log, and by
hand the tell is that this one **correlates with uptime and clears on a board
power cycle**.

With failure 3's phone-reboot evidence retracted, this is the only one of the
two that any observed incident actually supports.

Two defects in the diagnostics for it, both of the shape "a check that looks
live while being structurally unable to fire":

- **The `unlocked:` budget was refilled inside
  `ptp_clock_set_master_clock_id()`**, which early-returns when the clock id is
  unchanged. A session whose sender reused the previous master got no budget and
  therefore produced no diagnostics at all -- and the fault is invisible without
  them. It appeared to work only because the master happened to change between
  the first two sessions. Refill belongs on `ensure_ptp_started()`, i.e. every
  session start, which is what it should always have keyed on.
- **`note_ptp_packet()` fed one `last_packet_ms` from both ports.** `SYNC`
  arrives only on the event port (319) and `FOLLOW_UP`/`ANNOUNCE` only on the
  general port (320). A deaf event socket alongside a live general socket
  therefore looks healthy to the receive-path watchdog: no `SYNC`, so no
  samples, so no lock and silence -- while the general port keeps resetting the
  30 s timer and the socket rebuild never runs. Nothing clears that state but a
  reboot.

Both timestamps are tracked per socket now; either going quiet triggers the
rebuild, and `unlocked:` carries `quiet_event` and `quiet_general` separately so
the next occurrence names the side that died.

Caveat worth preserving: the deaf-event-socket mechanism is **what the code
permitted**, found by reading it after a reboot fixed what a rebuild had not. It
is not confirmed as what actually happened. The counters were added to settle
that and had not yet caught an occurrence at the time of writing.

### 5. The buffered listener stayed bound to the previous sender's port

Reproduce by playing to the board from one device and then selecting it from a
second **without stopping the first**. Confirmed on hardware 2026-09-09; the
fix is deployed and takeovers work.

`audio_receiver_start_buffered()` skipped the restart whenever the stream was
already running, on the premise that "buffered streams use a fixed port". They
do not: `handle_setup()` calls `alloc_stream_port()` per RTSP connection and
the SETUP response advertises *that* port. So the second sender was told a port
the board was not listening on, its connect went unanswered, and the session sat
at `blocks=0` with the sender's UI showing "playing". Stopping the first device
first worked because TEARDOWN cleared `running`, letting the next SETUP bind --
that workaround is the tell for this failure.

An unchanged port must still skip the restart, or a repeat SETUP from the same
sender drops its own live connection; only a changed port stops and rebinds.
`buffered_start()` now returns `ESP_ERR_INVALID_STATE` rather than success when
asked to serve a port it is not bound to. The realtime path never had this --
it has always restarted unconditionally.

Distinguishing it from failures 3 and 4 takes one field: this one is
`blocks=0` in `stalled:` **with the clock fine**, and the log shows
`(superseded)` shortly before. A PTP wedge is `clock_map=0`. Reading a
`Buffered audio connection closed by peer` line as the *new* sender hanging up
is the trap -- it is the superseded one's socket dying a couple of seconds
late.

### Why `START STALL` cannot save you here

`START STALL` is the watchdog meant to report exactly this class of wedge, and
it is structurally blind to failures 3 and 4, and to any other no-anchor case:
it is armed **inside `audio_engine_v2_set_anchor()`**, which is only reached
once a clock is locked. No lock means no anchor means the watchdog is never
armed, so the one failure it exists to report is the one it can never see. The
`unlocked:` counters exist because of this.

## Reading the telemetry

Three 1Hz lines, all at `ESP_LOGI`. Together ~3 log lines/s; `under=0`
throughout confirms the cost is not audible. `logger: level: WARN` silences them
without removing the counters.

```
playout: raw=.. span=.. filt=.. drift=..ppm trims=../s (N) buffered=N
         concealed=N holes=N (+N) under=N dfail=N qdrop=N edrop=N ins=N
resend:  sent=N recovered=N stale=N unmarked_ok=N unmarked_stale=N
rxpath:  stack=N task=N gap=N
```

- `holes` -- concealment *events*; `(+N)` is new ones this second. The number
  that matters. `concealed` is concealed samples.
- `buffered` -- healthy is ~250 blocks. Dips during loss bursts.
- `drift` / `filt` -- clock health. Healthy is +-40ppm and +-0.05ms. Wild values
  (hundreds of ppm, milliseconds) mean the problem is timing, not loss.
- `under` -- I2S underruns. Non-zero means CPU starvation of the playback task,
  a different problem entirely.
- `ins` -- steady ~125/s resampler insertion. Flat regardless of artefacts; not
  a fault signal.
- `resend:` is **suppressed entirely when every counter is zero**, so no
  `resend:` lines at all is the healthy state -- no sequence gap was detected.

A line that appears only while a stream is wedged, at `ESP_LOGW` so it clears
the `INFO` level the boards run at:

```
stalled: state=S reason=R clock_map=N epoch=N wanted_rtp=N blocks=N silent=N
         starts=N fallbacks=N concealed=N
```

It is the counterpart to `playout:`, which is gated on `AUDIO_SCHED_PLAYING`
and therefore prints nothing during the one fault worth watching. `reason` is
the field that matters, and until this line existed it was reachable only from
`START STALL` -- which, as above, can never fire for a no-anchor wedge.

- `clock_map=0` with `reason=WAIT_CLOCK_MAP` -- no usable clock. A PTP fault;
  read `unlocked:` next.
- `clock_map=1` with a preroll/fallback reason -- the clock is fine and the
  ring is not playable. Nothing to do with PTP; `starts`/`fallbacks` climbing
  says the scheduler is retrying and failing to find contiguous audio.
- It fires only when the sender has asked to play. An idle or paused board is
  silent on purpose and prints nothing.

A fourth line appears only when the clock is in trouble:

```
unlocked: sync=N followup=N announce=N rejected=N samples=N
          quiet_event=N ms quiet_general=N ms rebuilds=N master=<id>
```

Emitted at INFO, every 5 s. A session start grants
`PTP_UNLOCKED_STATUS_BUDGET` (6) reports, and the audio path re-arms reporting
on every render that finds an anchor pending with no clock
(`ptp_clock_notify_playing_unlocked()`, expiring
`PTP_STALL_NOTICE_TTL_MS` after the last call). So a session that locks
normally spends nothing, an idle board logs nothing, and a wedge leaves
evidence for as long as it lasts -- the budget alone used to go dark 21 s into
a seven-minute fault.

Read it as:

- **`followup` is the discriminator.** SYNC arrives only on the event port;
  FOLLOW_UP and ANNOUNCE only on the general port. ANNOUNCE climbing with
  `followup` frozen proves the general socket is fine and no master is emitting
  the sync pair -- the sender. `followup` climbing with `sync` frozen is the
  genuinely deaf event socket, and the only case a rebuild can fix.
- `quiet_event` high with `quiet_general` low is **not** diagnostic by itself:
  ANNOUNCE alone keeps the general timer fed, so a silent master produces it too.
- `rejected` climbing -> traffic arrives but the master filter is pinned to the
  wrong clock. A rebuild cannot help.
- `rebuilds` -> how many times the receive path was rebuilt (see below).

`ptp_task()` rebuilds both sockets, re-issuing their `IP_ADD_MEMBERSHIP` join,
after `PTP_RX_SILENCE_TIMEOUT_MS` (30 s) with no datagram on *either* port.
This recovers a join that a switch pruned or a roam lost -- it cannot recover a
sender that has stopped transmitting, which is why the counters matter more
than the rebuild does.

`PTP <event|general> port receiving again after N socket rebuild(s)` reports a
recovery. The port name is load-bearing and was added because the line lied:
one shared `rebuilds_since_rx` was cleared by a packet on *either* port, so an
ANNOUNCE on a healthy general port announced the recovery of a still-deaf event
socket. It fired 40 times across a fault it was structurally unable to observe.
The counter is per port now, and only a port that was actually quiet when the
rebuild ran is credited with one -- otherwise the healthy port claims a
recovery every 30 s for as long as the other stays deaf.

The clock is started from `AirPlayReceiver::loop()` on the first pass with
`network::is_connected()`, not from the first RTSP session. `setup()` runs at
`AFTER_CONNECTION`, which is after the wifi component's `setup()` but before
association and DHCP, so binding there would fail. Starting at the session
instead cost ~3.5 s of the first play after a reboot and left the receive-path
watchdog stopped between sessions. `ensure_ptp_started()` in the transport is
still called per session: `ptp_clock_init()` is idempotent, and that call is
what re-arms `ptp_clock_notify_session_start()`.

`rxpath:` is what separates loss on air from loss inside the board, which
`holes` alone cannot:

- `stack` -- every UDP datagram that reached lwIP, from `udp.recv`, incremented
  at the top of `udp_input()` before the pcb lookup and before the receive mbox.
  This is what survived the air.
- `task` -- what the receiver actually read off the data socket. Nominal ~125/s.
- `gap` -- `stack - task`, dominated by receive-mbox overflow.

`gap` steady while `holes` is 0 is the non-audio floor: PTP on 319/320 plus
whatever mDNS the segment carries (~30/s on the reference network). The failure
signature is `task` dipping below 125 and then **overshooting** on the next
second while `stack` stays high -- the task draining a backlog after an
overflow. If `stack` falls along with `task`, the packets genuinely never
arrived and only the radio side can help.

Requires `CONFIG_LWIP_STATS`, which the component does **not** set -- it is a
diagnostic, not a requirement. Enable it in the board YAML when investigating;
without it the line compiles out via `#if LWIP_STATS`.

## Output DSP: what the design constraints actually were

`audio/audio_dsp.{h,cpp}` exists because a receiver fed directly by an iPhone has
no server-side EQ in front of it -- whatever the speaker needs has to run here.
Three constraints shaped it, and each one is a real failure if ignored:

- **Coefficient recompute is not realtime-safe.** Designing a biquad needs
  `sin`/`cos`/`pow`. Doing that on the priority-9 playback task starves the DMA
  ring and underruns. So every `audio_dsp_set_*()` runs on the main loop, fills
  the *inactive* one of two coefficient banks, and publishes by flipping a single
  index (release/acquire -- the writer and the playback task are on different
  cores). `audio_dsp_process()` reads the index once per block.
- **Filter state must survive a coefficient swap.** It lives outside the banks
  for exactly this reason. Zeroing it on every edit clicks once per knob turn.
  It *is* zeroed on flush and on session start, where stale state would thump.
- **Order in the chain is a headroom decision, not a response decision.** The
  stage runs last, after volume and channel mode. Response is the same either
  way, but filtering after the volume attenuation means a shelf with positive
  gain has room to boost instead of clipping at high volume.

The stage clamps on output, so a boost without a matching negative `preamp`
distorts peaks rather than wrapping. `warn_if_dsp_clips_()` names the preamp it
wants at boot. There is no limiter.

## The opening of each track was dropped (fixed 2026-10-06, unverified)

A track change is a new stream on a new TCP connection to the same port, and
buffered senders push the opening audio before the anchor that says to play it
now (`lead` ~0 ms). `audio_receiver_start_buffered()` returned early on an
unchanged port, so the previous stream's `discard_all_until_anchor` (after a
flush) or its still-valid anchor (Path B, without one) discarded that audio:
~1 s lost per track, measured on an audiobook. An unchanged-port SETUP with no
live connection now resets as a new stream. Measure it as `start decision`
RTP minus `Anchor set` RTP.

## AAC decoder: FDK, and what AirPlay surround looks like (2026-10-06)

AAC now decodes with Fraunhofer FDK (`decoder/aac_fdk.*`); `esp_audio_codec`'s
AAC decoder is mono/stereo only and AirPlay sends 5.1 / 7.1 as AAC-LC 48 kHz.
ALAC stays on `esp_audio_codec`.

- **Format comes from the RTP SSRC**, per packet (`decoder/aac_format.*`):
  `0x16000000` 44.1k stereo, `0x17000000` 48k stereo, `0x27000000` 5.1,
  `0x28000000` 7.1. A 7-byte ADTS header built from it tells FDK the layout.
  Values and method from shairport-sync 5.0. The previous header was hardcoded
  to 44.1 kHz stereo.
- **Senders learn it from `/info` `supportedFormats.bufferStream`**: bit 39 =
  5.1, bit 40 = 7.1 (HomePods set both). `advertise_surround` sets 22 + 39;
  default off leaves `/info` byte-identical. **That mask breaks Spotify**:
  senders trust `supportedFormats` once present, and without bit 18 (ALAC
  44.1k/16/2, what Spotify's realtime stream uses) they tear down before
  SETUP stream. Add every decodable format before enabling it.
- **Host-verified, not hardware-verified**: `tests/test_aac_fdk.py` decodes
  ffmpeg- and AudioToolbox-encoded 5.1 with a tone per channel; every channel
  lands on the right side, centre and surrounds at -3 dB, LFE dropped.
- **7.1 is ambiguous.** channelConfiguration 7 means front-wide pairs to FDK
  and side pairs to ffmpeg; AudioToolbox instead emits configuration 0 with an
  in-band PCE in the first frame only (`aac_adts_channel_config()` handles the
  PCE case). Not advertised until a real sender's packets are seen -- the
  decoder logs the SSRC and first payload bytes on every format change.
- **Stack: ~49 KB peak** (host), 36 KB of it `CAacDecoder_DecodeFrame` at -Os.
  The decode task went from 6 KB to 56 KB. The realtime RX task (12 KB) must
  never decode AAC with FDK; Apple realtime streams are ALAC.
- **Cost on the S3 is unmeasured.** FDK keeps its state in PSRAM. The decoder
  logs `aac: ... decode avg= max= ... stack free= internal free=` every 1000
  frames; check it on the first stereo and first 5.1 session.

## Clicks on connect (mechanism inferred, not measured)

Selecting the board from iOS produced a run of clicks -- "click click click,
up and down, like the amp turning up and down" -- that Sendspin never made on
the same hardware.

Inside this component, exactly one thing touches the DAC clocks or the amp pin
during a session handshake: `audio_output_flush()`, which purges the DMA ring
by disabling and re-enabling the I2S channel. That stops BCLK/LRCLK, and the
DAC mutes and un-mutes its output stage around a clock dropout. `amp_set()` is
reached only by start/stop/the idle watchdog, none of which fire more than once
here, so elimination points at the flush -- **which is inference from the code,
not a measurement.**

The count fits. A single connect logs a burst of `audio: PAUSED` /
`audio: seek flush` / `start_stream` inside a second or two (four to six flushes
is ordinary), and every one of them purged the ring whether or not it held
anything.

The ring is `I2S_DMA_DESC_NUM * I2S_DMA_FRAME_NUM` = 2048 frames, ~46 ms. A
flush arriving when nothing but silence has gone out for longer than that has
no stale audio to drop, so the click was the entire effect. `playback_task()`
now purges only when `ring_may_hold_audio()` says a tail could still be in
flight; the `cursor_reset()` moved inside that branch, because it is correct
only as the accounting for a purge that actually discarded frames.

This does not touch a mid-playback seek, where the ring really does hold the
old track -- that flush still clicks. Silencing it would need a short volume
ramp around the disable/enable, which has not been written.

## Dead ends -- do not re-investigate

- **`handle_setpeers()` discarding the peer list** (`(void)raw;`) and the
  `SETPEERS: no timing peer (ip/port) available` warning. That is the AirPlay 1
  NTP fallback path; AirPlay 2 uses PTP and it locks fine (`ptp_locked=1`,
  `LOCKED: offset=.. dev=..`). Benign.
- **iOS answering NACKs.** It does not. Measured over 77 consecutive seconds:
  `sent=3-10/s recovered=0 stale=0 unmarked_ok=0 unmarked_stale=0`, every
  sample. The `0x80 0xD5` NACK is the AirPlay 1 / Shairport mechanism; an
  AirPlay 2 realtime stream (`type=96`, PTP-timed) ignores it. A lost packet can
  only be concealed -- which is why the mbox fix above mattered so much.
  (`sent` conflates new requests with retries: `resend_retry_if_due()` re-asks
  every 250ms while a gap is outstanding, so `sent=4/s` is often *one* gap
  retried four times.)
- **`tp=TCP` in the `_raop` TXT record** to request the buffered (`type=103`,
  TCP, loss-immune) stream. The sender still opens `type=96`. Kept because
  advertising the lossy transport was worse, but it is inert.
- **Feature bit 40 `SupportsBufferedAudio`.** Already set in
  `AIRPLAY_FEATURES_HI` (`0x1C340`). Buffered mode is the sender's choice and
  depends on the source app; it cannot be forced from the receiver.
- **Ping RTT as a wifi-health test.** It goes host -> router -> board and the
  host's own link confounds it. It got *worse* on one board after a power-save
  change that measurably helped. Use `playout:` and `rxpath:` instead.
- **Blaming airtime contention for concealment.** An earlier revision concluded
  this from a real A/B (0 holes with one receiver, 2-29/s with two) and declared
  it unfixable in firmware. The measurement was right and the mechanism was
  wrong: a second receiver does not change the first board's packet *rate* --
  the sender unicasts an independent stream to each -- it changes arrival
  *timing*, and a 6-slot queue cannot absorb the resulting bursts. Two boards at
  ~1.4 Mbit/s each is ~2.8 Mbit/s, which is nothing for 802.11n; that
  implausibility was the clue, and it needed no new measurement to spot.

## Log-reading traps

- **The `app:151` ESPHome version banner is re-emitted on every log client
  connect.** It is *not* a reboot indicator. Confirmed against boards that
  demonstrably never rebooted.
- **NACK send/receive logging is `ESP_LOGD`** while boards typically run
  `logger: level: INFO`. Absence of those lines means absence of *logging*, not
  absence of behaviour. This is why the 1Hz counters exist at INFO.
- Do not enable `audio_rt: DEBUG` to investigate loss -- it logs per NACK and
  per stale packet from the audio receive task, which can overflow the 768-byte
  task log buffer and cause the very stutter being measured.
- **ESPHome refuses a per-tag log level more verbose than the global level.**
  `logs: {airplay_ptp: DEBUG}` under `level: INFO` fails config validation, so
  an `ESP_LOGD` diagnostic cannot be switched on selectively -- it needs the
  global level raised, which makes every other tag chatty and reintroduces the
  stutter. Anything that must be readable on a deployed board has to be INFO
  and budgeted, not DEBUG.
- OTA fails while a stream is live (`Device closed connection without
  responding`) -- stop playback first. Flashing reboots the board, which drops
  the AirPlay session; the sender must re-select the speaker before the next
  capture.

## Realtime resume latency (2026-10-06, Spotify on iOS, measured)

A realtime (type 96, ALAC) resume runs `start_stream` -> sync packet (anchor)
-> `FLUSH`, all within ~70 ms. `audio_receiver_seek_flush()` used to drop that
anchor and wait for the next sync packet (~1 s); its gate then sat ~1 s past
the first anchor and discarded that audio, heard as a skip. It now replays the
anchor if it arrived since the stream started and within 500 ms
(`RESUME_ANCHOR_REUSE_WINDOW_US`); log line `seek flush: reusing resume
anchor`. A mid-stream seek never reuses one (one use per stream start).

**The remaining ~2 s from resume to sound is the sender, not the board.**
After the flush the phone sends at exactly 1x, and its first packet is stamped
~1.8 s past the anchor (`latencyMax = 88200`). `start decision` lands within
~15 ms of that packet's due time. Shrinking the 180 ms preroll or the 1 s
fallback timeout (`audio_engine_v2.cpp`) was considered and **would gain
nothing**: no audio exists earlier. `latencyMin = 11025` is not the sender's
working latency.

If resumes regress (silent start, wrong position, `Anchor change detected`
right after a resume), revert the commit "airplay: reuse the resume anchor
across a realtime FLUSH".

## FDK performance (2026-10-06, Geneva, measured)

Stereo AAC 44.1k decode per 1024-sample (23.2 ms) frame, from the decoder's
`decode avg=` log line:

| Build | avg | max | internal free |
|---|---|---|---|
| `-Os`, all FDK memory in PSRAM | 11.6-12.1 ms | 14.4-15.9 ms | 56-62 KB |
| `-O2` + L1 buffers internal | 7.3-7.4 ms | 10.9-12.6 ms | 35-39 KB |

- `-O2` is `esp32: framework: advanced: compiler_optimization: PERF`, set in
  `boston-x90-l-airplay.yaml` only (`sdkconfig_options` is overridden by
  ESPHome). Flash 37.7% -> 44.7%.
- FDK tags allocations by memory section; the pschatzmann port ignored the tag
  and put everything in PSRAM. `-Wl,--wrap=FDKcalloc_L/FDKaalloc_L`
  (`__init__.py`, wrappers at the end of `decoder/aac_fdk.cpp`) puts L1 (2 x
  8 KB for stereo) in internal RAM while 40 KB stays free
  (`INTERNAL_RESERVE_BYTES`). L2 (53 KB) and EXTERN (256 KB) stay in PSRAM.
  Sizes measured on the host by logging `FDKcalloc_L`.
- Internal RAM fell ~23 KB, not 16: `-O2` presumably grows IRAM code, which
  shares SRAM. Not separated. **If WiFi drops or allocations fail**, raise
  `INTERNAL_RESERVE_BYTES` (disables the move, keeps `-O2`) or drop PERF.
- Contribution of each change was not measured separately.
- 5.1 at ~3x stereo work would still be ~22 ms per 23 ms frame: not viable
  without further gains.

### 5.1 decode cost on the board (2026-10-06, measured)

Temporary benchmark firmware (not committed): pink-noise ADTS clips decoded on
the audio core with the board idle, `-O2` + L1 internal, downmix to stereo.

| Clip | avg | max | frame budget | load |
|---|---|---|---|---|
| stereo 44.1k 256 kbps | 5.76 ms | 7.56 ms | 23.2 ms | 24% |
| 5.1 48k 640 kbps | 16.0 ms | 17.8 ms | 21.3 ms | 75% |

5.1 costs 2.8x stereo. Streaming stereo measured 7.4 ms against the idle
5.76 ms (+28%: playback, DSP and network on the same core), so 5.1 while
streaming projects to ~20.5 ms per 21.3 ms frame, ~96% of the core. Not viable
without roughly another 25-30% off the decode.

### Where FDK's decode time goes (2026-10-06, measured)

`tools/fdk_bench/` on the Geneva: the `boston-x90-l-airplay.yaml` build (`-O2`,
L1 internal), the decode task's core, priority and stack, board idle, downmix to
stereo. Phases are timed by `-Wl,--wrap` on FDK's cross-file calls; ~400 frames
per clip, two rounds agreed within 1%. ms per frame:

| Clip | total | read | tools | filterbank | downmix | other |
|---|---|---|---|---|---|---|
| stereo 44.1k 256k, Apple enc. | 6.6 | 2.4 (36%) | 0.2 | 2.4 (37%) | 0.0 | 1.5 (23%) |
| 5.1 48k 640k, Apple enc. | 17.5 | 5.6 (32%) | 0.4 | 7.3 (42%) | 1.5 (9%) | 2.7 (15%) |
| 5.1 48k 640k, ffmpeg enc. | 16.5 | 4.6 (28%) | 0.4 | 7.3 (44%) | 1.5 (9%) | 2.7 (16%) |

read = `CChannelElement_Read` (bitstream + Huffman), tools =
`CChannelElement_Decode`, filterbank = `CBlock_FrequencyToTime` (IMDCT +
overlap-add), downmix = `pcmDmx_ApplyFrame`, other = the remainder.

- **Filterbank: 1.2 ms per channel**, the largest share of 5.1. FDK runs it in a
  per-channel loop after every element is parsed (`aacdecoder.cpp`, the loop
  calling `CBlock_FrequencyToTime`), so channels can be split across cores. The
  shared state is the `pWorkBufferCore1->mdctOutTemp` scratch; a second core
  needs its own. Best case, half the channels on core 0: -3.6 ms.
- **FDK's downmix shows 1.5 ms on the board** (a host run put it at 0.16 ms),
  but it is not removable cost: see the next section.
- **Read is 28-32% and sequential**: element boundaries are only known after
  parsing the previous one. Apple's encoder costs 1 ms more here than ffmpeg's
  at the same bitrate, and its max (23.6 ms) exceeds the 21.3 ms frame.
- Projection, not measured: 17.5 - 3.6 - 1.3 = ~12.6 ms idle (-28%), ~16 ms
  streaming at the +28% measured for stereo, ~76% of core 1, with ~3.6 ms per
  frame (~17%) added to core 0 next to WiFi.
- Stereo is 6.6 ms here against 5.76 ms in the earlier benchmark; that run's
  encoder was not recorded, and the two are not directly comparable.
- The bench needs the decode task's 56 KB stack (16 KB crashed in `dit_fft`)
  and must yield every frame, or IDLE1 starves and the task watchdog resets.

### Filterbank on two cores (2026-10-06, measured)

`decoder/aac_fdk_parallel.cpp`: on multichannel streams, every other
channel's `CBlock_FrequencyToTime` runs on a worker pinned to core 0, and a
wrap of `CAacDecoder_DecodeFrame` waits for it before returning. Link-time
wraps only; the pinned FDK is unmodified. The file's header comment lists the
FDK facts that keep the output identical; a ref bump must re-check them.

Same bench and build as above, ms per frame:

| Clip | 1 core | 2 cores | max, 2 cores |
|---|---|---|---|
| 5.1 640k, Apple enc. | 17.5-17.8 | 14.8-15.1 (-15%) | 18.9-20.3 |
| 5.1 640k, ffmpeg enc. | 16.5-16.8 | 13.8-14.1 (-16%) | 15.3-16.5 |
| stereo 256k | 6.6 | 6.6 (not split) | 9.4-9.9 |

- **Output is bit-identical** to the serial decode: FNV hash over every sample,
  every round, both encoders.
- -2.7 ms of a possible -3.6 ms: the worker's three channels take longer on
  core 0 than on core 1. Handing it the first channel (even calls) rather than
  the second was worth ~0.8 ms; with odd calls the saving was 1.9 ms.
- Deferral only happens in the concealment steady state; a frame with errors
  or fading runs serially.
- Worker: 10 KB stack (peak ~6.3 KB), 8 KB scratch placed like FDK's L1
  buffer, created on the first multichannel frame, freed with the decoder.
- **Replacing FDK's downmix buys nothing.** Decoding 5.1 to six channels with
  no downmix at all measured 15.2 ms against 15.1 ms with it: the 1.5 ms is
  FDK's interleave and PCM conversion, which happen inside the downmix call
  and move elsewhere without it.
- 5.1 while streaming, projected from the stereo overhead (+1.6 ms, or +28%):
  ~16.5-19.5 ms of the 21.3 ms frame. Unmeasured; the next test is a real 5.1
  stream with `advertise_surround`, once the anchor/RTP mismatch below is
  fixed.
- Not split: parsing (28-32%) is sequential.
- Free internal RAM read either ~97 KB or ~72 KB at boot, before any decode, on
  different boots of the same build; worker start/stop did not move it.
- Flashing while the bench decodes can trip the task watchdog on core 1 and
  leave the old image running; retry the upload.

### Buffered port reuse and Mac PTP corrections (2026-10-06 evening)

- **New buffered port per SETUP after a stream TEARDOWN** (`handle_teardown`),
  as shairport-sync does. Verified with Mac Music: every SETUP got a new port,
  the old connection closed, and each connection's first packet RTP sat
  ~0.3 s from its anchor. iPhone Apple Music track changes stay inside one
  connection (FLUSHBUFFERED + new anchor), so they never hit this path. The
  Apple TV, which motivated it, is still untested.
- **A Mac's PTP correctionField is a timescale offset.** Logged raw: Follow_Up
  `preciseOriginTimestamp` = the Mac's awake uptime (`CLOCK_UPTIME_RAW`),
  correctionField = -59806 s; its SETRATEANCHORTIME uses the same uptime under
  its own clock ID. Applying the correction put anchors 16.6 h ahead and Mac
  streams never started, on any subnet. Corrections over 1 s are now ignored;
  Mac Music then played, anchors within a few ms (`lead=-18 ms`, `-4 ms`).
  nqptp applies the correction and files it under the Announce grandmaster,
  so shairport-sync would not have handled this case either.
- **Open: a second RTSP client supersedes a playing one.** Home Assistant
  (Music Assistant, 192.168.0.232) connected after a reflash and the Mac's
  session was dropped silently; Music kept showing "playing".
- **Open: Mac volume.** Reported not working; the test session was the one
  dropped above. `Sender volume %.2f dB` is now logged at INFO to settle it.

### 5.1 attempt with an Apple TV (2026-10-06, measured)

- **120 MHz octal PSRAM** (experimental, temperature-tracked tuning): 5.1
  decode 16.0 -> 15.2 ms, stereo 5.76 -> 5.52 ms (-5%), internal RAM -10 KB.
  Not kept. FDK is compute-bound here, not memory-bound; 5.1 needs decoder
  work (dual-core decode, or an Xtensa-optimised AAC decoder).
- **`supportedFormats` breaks senders even when it lists their format.** With
  bits 18 + 22 + 23 + 39 advertised, the Apple TV (`.175`, AppleTV11,1) tore
  down before stream SETUP, as Spotify did with 22 + 39. Without the key it
  plays AAC 44.1k stereo (`audioFormat = 0x400000`, `audioFormatIndex = 22`) --
  bit 22, which the failing mask included. So the dict itself, not a missing
  bit, is what senders reject. Untested guess: they also need
  `supportedAudioFormatsExtended` (HomePods send it, indices up to 75), which
  the `audioFormatIndex` key in SETUP suggests. Compare a HomePod's full
  `/info` against ours before trying again.

### Apple TV 5.1 over AirPlay (2026-10-06, measured)

- **`supportedFormats.audioStream` unblocked the Apple TV.** HomePods publish
  `audioStream` (0x1440000), `bufferStream`, `screenStream`,
  `lowLatencyAudioStream` and `supportedAudioFormatsExtended`. Adding
  `audioStream` alone (bits 18 + 22 + 24) to our `bufferStream` got the Apple
  TV (`.175`) past setup, and it then **chose 5.1** (`audioFormat =
  0x8000000000`, index 39, packets with SSRC `0x27000000`, FDK sees channel
  configuration 6). `supportedAudioFormatsExtended` was not needed. Spotify
  against this list is untested.
- **`LOUDNESSNORMALIZATION`** (Apple TV, video content, 78-byte bplist) is now
  answered 200 instead of 501. Whether the 501 mattered is unproven: the
  stereo session that stopped after ~2 s also had it rejected.
- **5.1 does not play: the anchor and the packets are on different RTP
  timelines.** Packets are clean (seq +1, RTP +1024, 64-byte opening frames),
  but SETRATEANCHORTIME's rtp is unrelated to them: anchor 16809885 vs packets
  ~1824877526, a different gap in each of four sessions. The scheduler waits
  for the anchor position, the timeline fills with unplayable frames,
  back-pressure stops the TCP reader, and the Apple TV closes the connection.
  Stereo from the same Apple TV has matching anchors. See the 2026-10-06
  shairport-sync notes in AIRPLAY-FINDINGS for how it reconciles the two.
- 5.1 advertising is off again (`advertise_surround: false`); `/info` is then
  identical to the build that plays Spotify and Apple TV stereo.
