# Stereo pairs, groups, and a wireless 2.1 — research notes

Desk research, 2026-09-12. **Nothing here has been verified on hardware.** No third board
exists yet; the existing pair (`boston-x90-{l,r}`) has not been reconfigured. Every claim below is
either quoted from a linked capture or marked as inference.

Written to answer: what would it take to run left, right and a subwoofer as three boards that a
sender treats as one player?

## 1. What a real HomePod stereo pair does on the wire

The short version: **a stereo pair is two ordinary AirPlay receivers plus four mDNS TXT fields.**
There is no leader relay, no special transport, and no channel split on the wire.

### Both members stay individually visible

They keep advertising `_airplay._tcp` separately, each with its own `deviceid`, `pi`, `psi`, `pk`,
`flags`, `features` and hostname. Only Apple's picker collapses them; avahi, owntone and Music
Assistant all see two devices.

### The pair is expressed in TXT

avahi dump of two paired HomePods, from owntone#1413:

```
"tsm=0" "tsid=EAFA36AA-9785-54B2-A537-D9EE2A55CF1C"
"gpn=Büro 2" "gcgl=1" "igl=1"
"gid=EAFA36AA-9785-54B2-A537-D9EE2A55CF1C+0+6276FFFA-04E1-439E-8139-2C906B34E587"   # left
"gid=EAFA36AA-9785-54B2-A537-D9EE2A55CF1C+1+4671EEC3-7E13-4DC7-BEC3-C9805D3AB964"   # right
```

Unpaired, the same two speakers carry unrelated `gid`s and **no `tsid` and no `gpn` at all**. So:

| field | meaning | in a pair |
|---|---|---|
| `tsid` | tight sync UUID | identical on both members; absent when unpaired |
| `gpn` | group public name | identical; this is the name the picker shows ("Büro 2") |
| `gid` | group UUID | shared prefix, then `+<index>+<per-member UUID>` |
| `pgid` | parent group UUID | set when the pair is nested in a larger transient group |
| `igl` / `gcgl` | is / contains group leader | both `1` on a standing pair |
| `tsm` | unexplained; `0` and `1` observed | — |

owntone could not account for the third `+`-separated component and settled on "strip the `gid` at
the first `+`". The `+0+`/`+1+` index is the only candidate for a channel marker in `gid`, and an
earlier dump in the same thread shows `+0+` on **both** members, so it probably is not one.

### Channel assignment is advertised, not negotiated

`fex` / `featuresEx` differs between members: `AMp/StBLNTwQoS54` vs `AMp/StBrNTwQoS54`. The
2026 capture study confirmed by listening test that `StBL` is the physical left and `StBr` the
physical right. `flags`, `features` and `statusFlags` also differ per member, but only the
`StB[L|R]` substring has confirmed semantics.

### The sender streams the full mix to both, twice

The single most useful finding. From the macOS -> HomePod mini capture:

- macOS opens a TCP 7000 control session to **each** member.
- It sends two UDP RTP streams, one per member, **byte-for-byte identical** — RTP header,
  encrypted payload and 24-byte trailer alike. `allPacketsByteIdentical=true`, 1756/1756 packet
  pairs over 14 s, and again 375/375 in a second capture.
- Maximum send-time skew between the paired packets: ~247 µs.
- Payload type `97`, sequence continuous, timestamp +352/packet, i.e. 44.1 kHz at 352 frames per
  packet.
- "On the LAN, no separation of left and right channels into different RTP payload bytes is
  observable."

**Each member decodes the full stereo stream and throws away the channel that is not its own.**

### PTP ran with one member only

Across three captures, PTP on 319/320 appears exclusively between the Mac and `192.168.2.197`
(the left member). The right member received synchronised media without appearing in a single PTP
exchange. Unexplained; the author does not extrapolate it beyond those captures.

## 2. What nobody has worked out

**How a pair is created.** The Home app flow — whatever establishes `tsid` and assigns L/R — is
documented nowhere I could find. owntone's maintainer, on implementing group support:

> Since we do not know the protocol required to setup speaker groups, we will just digest what is
> provided by mDNS.

Every project that handles pairs does exactly that: read the fields, collapse the UI, stream to
each member. owntone's `feature/grouping` branch, Music Assistant's AirPlay provider, and
shairport-sync (which does not support it at all) are all in this position.

**Whether a non-Apple receiver can claim to be in a pair.** I found no published attempt at setting
`tsid`/`gpn`/`gid` on third-party receivers to see whether iOS collapses them. Untried, as far as
the public record goes.

## 3. Consequence for a 2.1 here

The wire behaviour of a genuine stereo pair **is already what this component does**: every receiver
gets the full stereo stream, splits locally, all locked to the sender's PTP clock.
`audio_channel_mode: left | right | mono` is the same mechanism as `StBL`/`StBr`. No audio-path
machinery is missing. What is missing is only the picker collapsing three icons into one.

### Config-only, works today

```yaml
# left board
audio_channel_mode: left
dsp:
  filters: [ {type: high_pass, frequency: 80Hz, q: 0.7071},
             {type: high_pass, frequency: 80Hz, q: 0.7071} ]

# right board: identical, audio_channel_mode: right

# sub board
audio_channel_mode: mono          # (L+R)/2
dsp:
  preamp: -6dB                    # runs after volume, so a fixed trim holds across the range
  filters: [ {type: low_pass, frequency: 80Hz, q: 0.7071},
             {type: low_pass, frequency: 80Hz, q: 0.7071} ]
```

Two cascaded Butterworth sections per side is LR4, which sums flat with both halves in the same
polarity — that sidesteps the polarity invert this component does not have. Two of eight biquads
spent; six left for room correction. Channel mode runs before the DSP cascade
(`audio_output.cpp:258`; DSP is last in `playback_task`), so the sub gets downmix-then-lowpass,
which is the right order.

### The experiment worth running first

Roughly twenty lines in `transport/mdns_airplay.cpp`, which already builds the TXT array: give all
three boards a shared `gid`, `gpn` and `tsid`, with `igl=1` and `gcgl=1`. Then open Control Center
and look.

Two ways it can fail:

- iOS may gate pair display on `model=AudioAccessory*` plus attestation the boards cannot produce.
- A **three**-member pair is not a shape Apple's UI has. Two is the documented case; three is the
  transient group, which carries `gid` but no `gpn`. A folded group entry named by `gpn` is the
  best plausible outcome.

Failure mode is benign — the picker keeps showing three entries — and the result is publishable
either way.

### The one real code gap: per-board output delay

PTP aligns the boards to the sample, but a sub three metres from the mains, plus crossover group
delay, needs millisecond trim and there is no knob. Playout time comes from
`audio_output_get_next_playout_time_ns()` (`audio/audio_output.cpp:664`) and
`timing/audio_timing.cpp:82`, both adding a fixed `OUTPUT_PIPELINE_LATENCY_US` (5000). A signed
config value into both shifts playout.

Implement as **delay-only**: a board cannot play earlier than its pipeline permits, so advancing one
board means delaying the other two.

### Other things to expect

- **Amp.** A TPA311x-class board amp will not drive a woofer; the sub board wants line-out into the
  sub's own plate amp. Set `amp_idle_timeout: 0` there or the sub mutes during quiet passages while
  the mains keep playing.
- **Network.** Three ALAC streams plus three PTP clients off one phone. The UDP mbox fault
  (FIELD-NOTES #1) is fixed in the component, but 3x the bitrate on 2.4 GHz is the likely failure
  point. Wire them if possible.
- **The local-anchor fallback hurts more in 2.1.** A board that loses PTP keeps playing on its own
  clock and drifts. On a stereo pair that is a wandering image; across a crossover it smears the
  whole crossover region. Put `diag_ptp_locked()` on all three with an alert.

## 4. The other architecture (the Sonos model)

Sonos stereo pairs, per the same owntone thread, *remove* both speakers from AirPlay entirely and
expose **one** AirPlay endpoint; the pair's own software distributes and synchronises internally.
AirPlay never learns there are two speakers.

The equivalent here is to stop making all three boards AirPlay receivers and put **one** endpoint in
front of them, fanning out over a transport we control. Sendspin is already that transport in this
tree.

Putting a Linux box in that slot (shairport-sync feeding Music Assistant) is ruled out: it is a
dependency on software this house does not want to run, in the one position where its failures would
take down every speaker at once. The all-ESP32 version is one board terminating AirPlay, decoding to
PCM, and re-distributing to two peers against a clock it derives from the sender's PTP.

That is a large piece of work and should not be started casually:

- It is a new protocol. Sendspin has the right shape, but nothing makes an ESP32 a Sendspin
  *source* — only a sink.
- The leader board carries the AirPlay receive path **and** two outbound streams, on a chip already
  at ~33% RAM and 28% flash.
- It cannot coexist with the AirPlay build on the follower boards
  (`CONFLICTS_WITH: [sendspin]`; both claim I2S_NUM_0).
- The leader becomes a single point of failure for all three speakers.

Weigh that against the twenty-line mDNS experiment in section 3, which might buy the same outcome.
**Order of attack: run the TXT experiment first.** If iOS refuses to collapse three non-Apple
receivers, the fallback is not this relay — it is living with three icons selected together in
Control Center, which already plays correctly and in sync. The relay is only worth building if the
three-icon picker becomes genuinely intolerable.

## 5. Apple TV as the source, and default audio output

Goal: the boards selected automatically when the Apple TV wakes, the way a HomePod pair is.

**This is no longer HomePod-only.** Until tvOS 26 only a HomePod or HomePod mini could be a
permanent audio output. tvOS 26 opened Settings > Video and Audio > Audio Output to any
AirPlay-enabled speaker, and the choice survives a power cycle. Apple's own tvOS guide documents it,
with one carve-out: system alerts and game sounds keep playing through the TV speakers. The
shairport-sync "Advertise as HomePod" thread (open 2022-2025) was built on the older premise and is
moot.

Nothing in the audio path changes for this. It is a routing question, not a protocol one.

**Untested, and the thing to check first:** whether an Apple TV lists this component in that menu at
all. Sonos, Bose and B&O are all Home-app-visible devices; these boards advertise `_airplay._tcp`
and `_raop._tcp` only, with no `_hap._tcp` and no HomeKit accessory (see section 2). If tvOS filters
the Audio Output list by Home membership rather than by plain AirPlay discovery, the boards will not
appear, and no TXT field fixes that.

**The multi-destination question, which may make section 3 load-bearing rather than cosmetic.**
There are user reports that tvOS 26 regressed selecting several AirPlay speakers at once — that
where tvOS 18 would play to multiple destinations, only one outputs at a time afterwards
(discussions.apple.com thread 256218866; unconfirmed, and possibly fixed since). If an Apple TV will
only route to a single AirPlay destination, then three separate icons cannot carry TV audio at all,
and collapsing them into one advertised endpoint stops being a convenience.

**Both questions are answerable today, with the two boards that already exist**, before any third
board is bought:

1. Does the Apple TV list `boston-x90-r` under Video and Audio > Audio Output?
2. If so, can it play to both boards simultaneously, or only one?

**Lip sync.** The component honours the sender's `latencyMin` — 250 ms default on realtime, 0 on
buffered (`transport/transport_module.cpp:1026`) — so the Apple TV has what it needs to delay video
to match. Separately, tvOS 26 drew widespread lip-sync complaints on AirPlay audio output in
general, so budget for that being an Apple-side variable rather than a fault here.

## Sources

- owntone#1413, "Handle Airplay stereo group as one speaker instead of two individual speakers" —
  the paired/unpaired avahi dumps, the `gid` `+`-suffix discussion, the "we do not know the protocol"
  quote, and the note on Sonos pairs:
  <https://github.com/owntone/owntone-server/issues/1413>
- owntone#704, "Airplay 2 - paired receivers support - stereo" — first `tsid`/`gpn` observation, and
  "I check in wireshark that during playing packages (stream) goes to both speakers":
  <https://github.com/owntone/owntone-server/issues/704>
- owntone#1291, "Stereo HomePods show as two devices and switch playback between eachother" — what a
  non-Apple sender gets when it treats pair members as independent:
  <https://github.com/owntone/owntone-server/issues/1291>
- enxi06/airplay2-stereogroup-analysis (2026-06) — macOS to HomePod mini pair captures: identical
  dual streams, `fex` `StBL`/`StBr` left/right confirmation, PTP to one member, `/info` comparison:
  <https://github.com/enxi06/airplay2-stereogroup-analysis>
- Unofficial AirPlay specification, service discovery — field definitions for `tsid`, `gpn`, `gid`,
  `igl`, `gcgl`, `pgid`, `pgcgl`, `pi`, `psi`:
  <https://openairplay.github.io/airplay-spec/service_discovery.html>
- shairport-sync AIRPLAY2.md and discussion#1461 — no stereo pair support, and Mike Brady on the
  timing accuracy it would demand:
  <https://github.com/mikebrady/shairport-sync/blob/master/AIRPLAY2.md>,
  <https://github.com/mikebrady/shairport-sync/discussions/1461>
- Music Assistant discussion#5253, "Apple HomePod stereo pairing" — MA sees pair members
  individually; sync-group-with-left/right is the standing workaround:
  <https://github.com/orgs/music-assistant/discussions/5253>
- pyatv protocol documentation — `gid`/`igl`/`gcgl` in service discovery, group streaming
  undocumented: <https://pyatv.dev/documentation/protocols/>
- Apple tvOS user guide, "Use HomePod or AirPlay-enabled speakers with Apple TV 4K" — any AirPlay
  speaker as default audio output, and the system-alerts/game-sounds carve-out:
  <https://support.apple.com/guide/tv/homepod-airplay-enabled-speakers-apple-tv-4k-atvb2490ae15/tvos>
- shairport-sync discussion#1588, "Advertise as HomePod" — the pre-tvOS 26 question and why it no
  longer applies: <https://github.com/mikebrady/shairport-sync/discussions/1588>
- Apple Support Communities 256218866, "AirPlay Cannot Play Audio on Multiple Speakers in tvOS 26" —
  the unconfirmed multi-destination regression report:
  <https://discussions.apple.com/thread/256218866>
