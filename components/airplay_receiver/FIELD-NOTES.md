# Field notes -- what hardware taught us

Findings from running this component on Amped-ESP32-S3 boards (Boston X90 pair,
Geneva Model M) against iOS and macOS senders. Everything here was measured on
hardware; where something is inference it says so.

**Read this before debugging audio artefacts.** Several obvious-looking leads
are dead ends and are listed as such at the bottom. The original long-form
write-up lives in the deployment repo at `esphome/boston-x90/AIRPLAY-FINDINGS.md`
(h.lied.dev); this file is the component-local subset that matters to anyone
working on the component itself.

## The three failures that look identical from outside

All three present as **"the sender is connected, metadata and transport
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

### 3. The sender's PTP grandmaster stopped sending

Measured 2026-09-08 on the Geneva board. Symptom was identical to the two
above, and the board was innocent: an iPhone that had left the network and
come back was no longer sending PTP `SYNC` at all. Reflashing and rebooting the
*board* changed nothing; **hard-rebooting the phone fixed it immediately.**

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

**Diagnosing it takes one line.** `Anchor set: ... lead=N ms ptp_locked=0`:

- `ptp_locked=0` on every anchor is the fault.
- `lead` normally sits at -200 to -800 ms (the sender pre-buffers, so the anchor
  is slightly old). A `lead` of **minutes or days**, in either direction, means
  `ptp_clock_get_offset_ns()` is returning exactly 0 -- i.e. PTP has never
  produced a single accepted sample this session. It is not a timing error to
  be chased; it is the absence of a clock.
- The `unlocked: sync=.. announce=.. rejected=..` line (see below) then says
  whose fault it is: all zero means nothing is arriving and the **sender** is
  the suspect; `rejected` climbing means packets arrive but the master filter
  drops them.

Do not reach for the board first. Confirm the sender is still a grandmaster.

### Why `START STALL` cannot save you here

`START STALL` is the watchdog meant to report exactly this class of wedge, and
it is structurally blind to failures 3 and to any other no-anchor case: it is
armed **inside `audio_engine_v2_set_anchor()`**, which is only reached once a
clock is locked. No lock means no anchor means the watchdog is never armed, so
the one failure it exists to report is the one it can never see. The
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

A fourth line appears only when the clock is in trouble:

```
unlocked: sync=N followup=N announce=N rejected=N samples=N quiet=N ms rebuilds=N master=<id>
```

Emitted at INFO, every 5 s, but **budgeted**: pinning a master (which a session
does on its first anchor) grants six reports and nothing else refills them. A
session that locks normally spends none; a session that never locks leaves ~30 s
of evidence and then goes quiet, so an idle board never accumulates log. Read it
as: `sync`/`announce` both 0 -> nothing is arriving, suspect the sender or the
multicast path; `rejected` climbing -> traffic arrives but the master filter is
pinned to the wrong clock; `quiet` -> ms since *any* PTP datagram; `rebuilds` ->
how many times the receive path was rebuilt (see below).

`ptp_task()` rebuilds both sockets, re-issuing their `IP_ADD_MEMBERSHIP` join,
after `PTP_RX_SILENCE_TIMEOUT_MS` (30 s) with no datagram of any kind. This
recovers a join that a switch pruned or a roam lost -- it cannot recover a
sender that has stopped transmitting, which is why the counters matter more
than the rebuild does.

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
