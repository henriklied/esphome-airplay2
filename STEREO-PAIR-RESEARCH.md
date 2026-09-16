# Stereo pairs, groups, and a wireless 2.1 — research notes

Desk research 2026-09-12, **revised 2026-09-16 after a measurement pass against the house network**.

The 2026-09-12 version was desk research only, built on secondhand avahi dumps quoted in GitHub
issues. Measuring the real devices contradicted four of its claims. Sections 1 and 2 below have
been rewritten; the old text is not preserved, because leaving a superseded reading next to the
measured one is how a future pass gets misled twice.

Written to answer: what would it take for our board to be treated as part of a speaker group?

## 0. What the house actually advertises (measured 2026-09-16)

Captured with a stdlib mDNS query run **on devbox** (192.168.0.94). mDNS does not cross Tailscale,
so this cannot be done from a laptop off-site.

| Device | Model | Addr | `tsid` | `gid` | `gpn` | `igl` | `fex` | `flags` |
|---|---|---|---|---|---|---|---|---|
| Projektor (Apple TV) | AppleTV11,1 | .0.175 | — | `CAD95DFE…FE21` | Projektor | **1** | `1d9/St5/Fzw4oQ4E` | `0x18644` |
| Stue oppe | AudioAccessory1,1 | .0.210 | `A0518D59…71A1` | `CAD95DFE…FE21` | Projektor | 0 | `AMp/StBrNTwQoS54` | `0xb8c04` |
| Stue oppe (2) | AudioAccessory1,1 | .0.201 | `A0518D59…71A1` | `CAD95DFE…FE21` | Projektor | 0 | `AMp/StBrNTwQoS54` | `0xbac04` |
| Sverres rom | AudioAccessory5,1 | .0.37 | — | `2461D383…+091A34E5…` | — | 1 | `AMp/StBLNTwQoS54` | `0x98404` |
| Johns Rom | AudioAccessory5,1 | .0.189 | — | `74830FBF…+FF716721…` | — | 1 | `AMp/StBrNTwQoS54` | `0x98404` |
| Geneva Model M (ours) | AudioAccessory5,1 | 10.0.40.146 | — | — | — | — | — | `0x4` |

Reproduce with `tools/mdns_watch.py`, which must run on a host in the speakers' broadcast domain:

```bash
scp tools/mdns_watch.py henriklied@192.168.0.94:/tmp/
ssh henriklied@192.168.0.94 "python3 /tmp/mdns_watch.py --filter 'Stue oppe' --filter Projektor"
```

It holds a passive listener on 224.0.0.251:5353 alongside an active poller and logs every TXT field
transition with a millisecond timestamp, decoding `flags` and `features` bits as it goes. Running it
across a full ungroup/regroup/swap cycle is in `captures/pair-lifecycle-2026-09-16.log`, analysed
in 1.1 and 1.2.

The group leader carries the fields that settle the structure:

```
Projektor      (AppleTV11,1,      .0.175)  igl=1  pgmid=0  psgsz=3  psgtp=1  pgm=0
Stue oppe (2)  (AudioAccessory1,1 .0.201)  igl=0  pgmid=1  tsm=0  pgcgl=1  flags bit 13 set
Stue oppe      (AudioAccessory1,1 .0.210)  igl=0  pgmid=2  tsm=0  pgcgl=1

all three:  gid = psgid = pgid = CAD95DFE-3423-5CFF-BEDE-D5283E06FE21   gpn=Projektor   gcgl=1
both pods:  tsid = A0518D59-B671-5753-9472-7CF7284571A1
```

**`pgmid` is the member index.** 0 on the Apple TV, 1 on `Stue oppe (2)`, 2 on `Stue oppe` — a
contiguous ordinal over the three members, leader first, with the tight-sync leader at 1. This is
the per-member index the 2026-09-12 text went looking for in a `gid` `+N+` suffix; it exists, but it
lives in its own key. Undocumented, like the rest of the `psg*`/`pgm*` family.

**`psgsz=3`.** The group is three members — the Apple TV plus both HomePods — and the Apple TV
leads it (`igl=1`; both HomePods `igl=0`). It exists because the pair is that Apple TV's default
audio output, which is section 5 of this document already live in the house.

So the real structure is two nested levels, not one:

- **Group** — `gid` / `gpn` / `psgid` / `psgsz`, three members, led by the Apple TV. This is what
  the picker collapses into one entry named by `gpn`.
- **Tight-sync set** — `tsid`, the two HomePods only. This is the sample-locked stereo pair.

`psgsz`, `psgtp`, `pgm`, `pgmid`, `tsm` and `fex` are documented nowhere public — neither openairplay's
service discovery page nor the airplay2-rs TXT table (which is otherwise the most complete found,
and adds `hkid` homeKitHomeUUID) lists any of the five — so the readings above are ours. Caveat: they come from an Apple TV
on tvOS 18.6 while the HomePods are on 26.6, so they may be an older-tvOS spelling.

## 1. Corrections to the 2026-09-12 desk research

**`fex` `StBL`/`StBr` is not a left/right channel marker.** Both members of the real pair advertise
`StBr`. Two *standalone* HomePod minis in different rooms carry `StBL` (Sverres rom) and `StBr`
(Johns Rom). The old text called the L/R meaning "confirmed by listening test", quoting the 2026
capture study; the house refutes it. Whatever `fex` encodes, it is not pair channel assignment.

**The per-member role marker is in `flags`, not `fex`, and it has a name.** The two pair members
differ in exactly one bit across their whole TXT: `0xb8c04` vs `0xbac04`, i.e. bit 13. The
airplay2-rs status-flags table names bit 13 **`TightSyncIsGroupLeader`**, so `Stue oppe (2)` leads
the tight-sync set and `Stue oppe` follows. Bit 14 is `TightSyncBuddyNotReachable` and is clear on
both, i.e. they can see each other.

That gives the pair its *own* leader, distinct from the group leader. Three roles are in play at
once: the Apple TV leads the group (`igl=1`), `Stue oppe (2)` leads the tight-sync set
(`flags` bit 13), and `Stue oppe` follows both.

> **Self-correction, twice.** A first reading claimed bits 11 and 17 were "where pair state lives";
> a second retracted both. The lifecycle capture (1.1) settles it: **bit 11 `DeviceSupportsRelay`
> does track group membership** — it clears the instant ungrouping starts and is re-added when the
> follower rejoins. Bit 17 `ReceiverSessionIsActive` really is just session state. So the original
> claim was half right and the retraction over-corrected.

**`gid` `+<index>+` is the *transitional* form, and the index is not a channel.** A settled
three-member group shows one plain shared UUID; a settled standalone HomePod shows the two-part
`UUID+UUID`. The three-part `UUID+<n>+UUID` that owntone#1413 could not account for appears only
while membership is changing (1.1), and `<n>` is the member ordinal, not a channel marker. Both
readings in the older text — "it might be a channel index" and later "no index exists" — are wrong;
it exists, it is an ordinal, and it is transient.

**`igl=0` on both pair members**, not `1`. Leadership sits with the Apple TV. The old claim that
"`igl`/`gcgl` are both 1 on a standing pair" is wrong; `gcgl=1` holds, `igl=1` does not.

**A three-member group is a shape Apple supports.** The old text worried that "a three-member pair
is not a shape Apple's UI has" and predicted at best a folded group entry. `psgsz=3` is running in
the house today. That concern is retired.

What survives unchanged: `tsid` is present only on the pair and identical across it; `gpn` is the
name the picker shows; both members stay individually visible as ordinary `_airplay._tcp` records.

### 1.1 The pair lifecycle, measured

Full trace in `captures/pair-lifecycle-2026-09-16.log`: the operator pressed "Ungroup accessories"
in the Home app, re-created the pair, then pressed swap-left-right twice, with
`tools/mdns_watch.py` running throughout.

**Teardown** (11:56:51.4 -> 11:56:57.1, 5.7 s). Both HomePods mutate first and together: `flags`
bit 11 clears, `gid` gains its ordinal suffix (`...+1` on the device whose `pgmid` was 1, `...+2` on
`pgmid` 2), `pgcgl`/`pgid` drop. 0.2 s later each appends a fresh per-member UUID, giving the
three-part form. At +1.6 s the tight-sync leader's `gid` prefix becomes **`A0518D59...`, which was
the `tsid`** — the pair momentarily becomes its own group named by its tight-sync UUID — and its
`igl` flips 0->1. The follower drops `tsid`/`tsm` first. Only at +3.7 s does the **Apple TV** drop
`gpn`, `psgid`, `psgsz`, `psgtp`, `pgm`, `pgmid`.

**Rebuild** (11:57:51.7 -> 11:57:59.7, 8 s). The device that becomes tight-sync leader goes first
and alone, minting `tsid=64C27BB9...`, setting `gpn`, asserting `flags` bit 13 and adopting the new
`tsid` as its `gid` prefix — all in a single announcement. The follower copies `tsid`/`gpn`/`tsm`
0.65 s later, and only at +2.4 s settles into its role: `igl` 1->0, `flags` bits 11 and 17 added,
`pgcgl`/`pgid` added.

Three consequences:

- **The leader is a lagging reporter, not the authority.** The Apple TV publishes `psgsz` 3.7 s
  *after* the members have already left. This materially weakens the blocker in 3.6.
- **`tsid` is asserted, not negotiated.** One device mints it and the other copies it. No handshake
  is visible from outside, consistent with the real exchange happening over HAP.
- **`igl=0` on both is not a pair signature.** It means the pair sits inside a larger group. A bare
  pair has its tight-sync leader at `igl=1`.

Two undocumented `flags` bits turned up. Bit 21 is set on the tight-sync leader of a *bare* pair
(`0x29a404`) and was absent when that same device sat inside the Apple TV's group (`0xbac04`); it
persists rather than being transient. Bit 19 is set on every Apple device seen, in every state.

### 1.2 Channel assignment is not in mDNS

The operator pressed the Home app's swap-left-and-right control **twice**, 53 s apart. The watcher
recorded **no transition of any kind**, and a fresh query afterwards returned TXT records
byte-identical to the pre-swap state.

So no mDNS field carries physical channel. In the settled bare pair both members share
`fex=AMp/StBrNTwQoS54` *and* an identical `gid`; the only fields distinguishing them are `flags`
bits 13/21 versus 11/17, which 1.1 shows are leader/follower role, not channel.

**This closes the question rather than leaving it open, and in our favour.** Channel assignment is
local device state configured over HomeKit and stored on the speaker — which is exactly what
`audio_channel_mode: left | right | mono` already is. It also fits the capture study's finding that
the sender transmits byte-identical full-stereo RTP to both members. There is nothing here to
reverse-engineer and nothing to advertise.

## 2. What is still unknown

**How a pair is created.** Unchanged. The Home app flow that establishes `tsid` is documented
nowhere, and every third-party project digests mDNS rather than speaking it.

**Whether a non-Apple receiver can claim membership.** Still no published attempt.

**What the Home app actually says to the HomePods.** Not capturable. HAP sessions are
ChaCha20-Poly1305 encrypted after a pair-verify whose long-term Ed25519 keys live in the iOS
keychain and the HomePod's secure storage, and pair-verify is mutually authenticated, so there is no
MITM position either. A passive capture yields endpoints, timing and packet sizes only. Note also
that HomeKit state syncs through a home hub (the Projektor Apple TV is one) and iCloud, so the
exchange may not be local at all.

The tractable substitute is `tools/mdns_watch.py` across a pair/unpair: it shows which device
mutates first, in what order the fields appear, and whether unpairing leaves residue. That is the
mechanism as far as it is observable, and it yields the end state to forge — which is what we
actually need if senders resolve membership from mDNS.

**Whether implementing HomeKit would help.** No, for stereo pairing. A Denon Home 250 is fully
MFi-licensed and present in the Home app, and gets no "use as stereo pair" button; neither do Sonos
or B&O. Across eight years of licensed third-party AirPlay 2 speakers there is not one with Home app
stereo pairing, so this is gated on being an Apple speaker rather than on MFi, HAP, or any field we
could implement. Building `_hap._tcp` would buy Home *membership*, which is a different question —
and the open one in section 6, since the Apple TV's Audio Output menu may filter by it. We are
closer than it looks: `crypto_module` already has SRP-6a 3072, Ed25519, X25519, ChaCha20-Poly1305
and TLV8 for AirPlay's transient variant. What is missing is the persistent half (an LTPK store for
paired controllers; only our own identity persists today, `crypto/crypto_module.cpp:512`), the
accessory attribute database, and event notifications.

**Where the sender gets its member list.** The load-bearing question. Either it resolves the group
from mDNS at play time, or it reads members from the HomeKit database and uses mDNS only to resolve
addresses. One structural argument for mDNS: `gpn` is the group *public* name, and a guest iPhone
that is not in the Home has no HomeKit record for the pair yet still has to render it as one
speaker and stream to both. **Confirmed on 2026-09-16 by the operator: a device outside the Home
does see the pair as a single icon.** So the collapse is derivable from mDNS alone for that class
of sender. Whether the owner's own phone takes the same path or prefers HomeKit is untested, and it
is the phone that matters.

## 3. What we would need to build

Measured gap between our board and a real group member, in dependency order.

### 3.1 Identity fields (small, and now load-bearing)

`transport/mdns_airplay.cpp:65` hardcodes `pi=00000000-0000-0000-0000-000000000000`. Every real
device carries a distinct UUID, and `psi` (derived from the deviceid) alongside it; our board
advertises no `psi` at all.

This is not cosmetic. The SETPEERSX body keys its per-member clock-port map by GUID (see 3.3), so a
nil identifier is the natural thing for a sender to reject or collide.

### 3.2 Grouping TXT fields

`gid`, `gpn`, `tsid`, `igl=0`, `gcgl=1` added to the static TXT array at
`transport/mdns_airplay.cpp:59`. Roughly twenty lines. The values to mirror are known — section 0
lists them.

Our board already sets `model=AudioAccessory5,1` (`mdns_airplay.cpp:31`), so the "iOS may gate pair
display on `model=AudioAccessory*`" failure mode predicted in the old section 3 is already
pre-satisfied.

### 3.3 Feature bit 52, and actually parsing SETPEERS

The sharpest finding of the measurement pass. Decoding the features words against the published
bitfield table:

```
ours     features=0x405C4A00,0x1C340     hi bits: 38 40 41 46 47 48
HomePod  features=0x4A7FCA00,0x3C354BD0  hi bits: 36 38 39 40 41 43 46 48 50 52 53 58 59 60 61
```

We claim 40 (`SupportsBufferedAudio`) and 41 (`SupportsPTP`), the two bits a device needs to show as
multi-room capable. We do **not** claim **bit 52, `SupportsSetPeersExtendedMessage`** — the bit that
says this receiver can be handed a group peer set. We also lack 43 (`SupportsSystemPairing`). Bit 52
is named identically in two independent bit tables (SteeBono's wiki and airplay2-rs), so the reading
is not resting on one source.

Worth knowing what our profile currently *is*: our upper word `0x1C340` is byte-identical to the
AirPort Express 2 signature published in airplay2-rs 2.2.3 (`0x445D0A00,0x1C340`), and the Sonos
Symfonisk shares it too. Across the full 64 bits we differ from an AirPort Express only in bits 14,
16 and 26. **We are advertising an AirPort-Express-class third-party speaker, not a HomePod-class
one** — and no AirPort Express is a group member either.

And the behaviour matches the missing bit. `transport/transport_module.cpp:1358` registers
`SETPEERS` and `SETPEERSX` (dispatch table line 1429-1430) but **discards the body** — `raw` and
`raw_len` are voided on the first two lines. The handler uses the message purely as a trigger to
call `ptp_clock_init()`.

The body is what group membership is actually made of. SETPEERS is a plist array of member IP
addresses; SETPEERSX is the structured form:

```
{'Addresses': ['fe80::...', '...'],
 'ClockID': <uint64>,
 'ClockPorts': {GUID1: port, GUID2: port, GUIDN: port},
 'DeviceType': 0,
 'ID': GUID,
 'SupportsClockPortMatchingOverride': bool}
```

`ClockID` names the PTP grandmaster the group is locked to. We already have the machinery to use it
— `ptp_clock_set_master_clock_id()` (`timing/ptp_clock.h:115`) — but today it is only ever called
from the anchor path (`audio/audio_receiver.cpp:553`), i.e. we learn the master from
SETRATEANCHORTIME rather than from the group. That is adequate for a lone receiver and is the wrong
source for a member of a group, which is told its master earlier and explicitly.

So 3.3 is: set bit 52 in `AIRPLAY_FEATURES_HI` (`mdns_airplay.cpp:29`), parse the plist in
`handle_setpeers`, drive `ptp_clock_set_master_clock_id()` from `ClockID`, and key our entry in
`ClockPorts` off the real `pi` from 3.1.

### 3.4 Status flags

Our `flags=0x4` sets exactly one bit, 2 (`AudioCableAttached`). A pair member carries `0xb8c04`:

```
 2 AudioCableAttached                10 DeviceWasSetupForHKAccessControl
11 DeviceSupportsRelay               13 TightSyncIsGroupLeader   (leader only)
15 IsAppleMusicSubscriber            16 CloudLibraryIsOn
17 ReceiverSessionIsActive           19 (undocumented, set on every Apple device seen)
```

Bit 10 `DeviceWasSetupForHKAccessControl` is the one most likely to be gated on, since it asserts
the device participates in Home access control at all. Bits 13 and 14 are the tight-sync role and
reachability bits we would have to drive ourselves, and 17 should track whether a session is live.
Bits 15 and 16 are account state we cannot honestly claim.

### 3.5 The subnet constraint

Our boards are on `10.0.40.0/24`; the group and its senders are on `192.168.0.0/24`.

AirPlay 2 receivers check that the timing peer is in the same IP subnet and drop the peer list when
it is not. shairport-sync#1361 documents both the IPv4 and IPv6 checks and a reporter who got
cross-subnet AirPlay 2 working only by commenting both out; the maintainer never ruled on whether
that is safe.

Two consequences. First, if we implement 3.3 we should not import that check. Second, and larger:
PTP is a different multicast group (`224.0.1.129`) from mDNS. devbox on `192.168.0.94` can see the
board's mDNS records, so mDNS is being reflected across the VLANs — but that says nothing about
whether PTP event traffic reaches `10.0.40.0/24`. **Tight sync with the HomePods is impossible
unless the board hears the same grandmaster they do.** Unmeasured; this is the tcpdump to run.

### 3.6 The hard blocker: `psgsz` belongs to the leader

Group size is advertised by the leader. The Apple TV publishes `psgsz=3`. We cannot edit the Apple
TV's advertisement, so a board that self-declares into `gid=CAD95DFE…` contradicts the leader's own
count of its members.

Whether a sender reconciles the two or simply trusts the leader is unknown, and it is still the
first thing worth testing.

**Downgraded 2026-09-16.** The lifecycle capture (1.1) shows the Apple TV publishing `psgsz` 3.7 s
*after* the members had already changed state. A leader that learns last is reporting group
membership, not constituting it, which makes a stale or contradicted count much less likely to be
load-bearing than it looked.

### 3.7 Out of reach

Genuine Home membership needs `_hap._tcp` and accessory status. A generic HAP accessory is
achievable (HomeSpan exists). The **AirPlay-2-speaker HAP profile** that Sonos and B&O ship requires
MFi licensing and in practice a hardware authentication coprocessor. Past all that, the Home app
offers stereo pairing only between two HomePods of the same model. Treat this tier as closed.

## 4. Consequence for a 2.1 here

Unchanged by the measurement pass, and still the cheapest real outcome.

The wire behaviour of a genuine pair **is already what this component does**: every receiver gets
the full stereo stream, splits locally, all locked to the sender's PTP clock. The 2026 capture study
found the sender transmits byte-identical RTP to both members (1756/1756 packet pairs, max send skew
~247 us), so channel handling is entirely local — which is exactly what `audio_channel_mode: left |
right | mono` does. No audio-path machinery is missing.

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

### The one real code gap for 2.1: per-board output delay

PTP aligns the boards to the sample, but a sub three metres from the mains, plus crossover group
delay, needs millisecond trim and there is no knob. Playout time comes from
`audio_output_get_next_playout_time_ns()` (`audio/audio_output.cpp:664`) and
`timing/audio_timing.cpp:82`, both adding a fixed `OUTPUT_PIPELINE_LATENCY_US` (5000). A signed
config value into both shifts playout.

Implement as **delay-only**: a board cannot play earlier than its pipeline permits, so advancing one
board means delaying the other two.

### Sync quality, if we sit beside a real pair

Our PTP client is a passive multicast listener that never sends DELAY_REQ (`timing/ptp_clock.h:10`),
so every measured offset carries a one-way network-delay bias that the asymmetric filter chases
toward the minimum but never removes (`ptp_clock.cpp:292`). FIELD-NOTES #3 records both boards
locked to the same Mac grandmaster on independent sockets and independent IGMP joins, which is good
evidence the model works for N receivers — and incidentally explains the capture study's
"unexplained" observation that PTP ran with only one pair member: Sync and Follow_Up are multicast,
every member listens, and only one need exchange unicast Delay_Req.

But playing *alongside* a pair in one room is a harder target than multiroom in separate rooms.
Residual bias of the order of the pair's own ~247 us skew shows as image shift and comb filtering,
not as a late speaker. Two-way PTP would likely be needed to sit inside a tight-sync set.

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

## 5. The other architecture (the Sonos model)

Sonos stereo pairs, per owntone#1413, *remove* both speakers from AirPlay entirely and expose **one**
AirPlay endpoint; the pair's own software distributes and synchronises internally. AirPlay never
learns there are two speakers.

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

**Order of attack is unchanged: run the TXT experiment in section 3 first.** If iOS refuses, the
fallback is not this relay — it is living with separate icons selected together in Control Center,
which already plays correctly and in sync. The relay is only worth building if that becomes
genuinely intolerable.

## 6. Apple TV as the source, and default audio output

Goal: the boards selected automatically when the Apple TV wakes, the way a HomePod pair is.

**This is no longer HomePod-only.** Until tvOS 26 only a HomePod or HomePod mini could be a
permanent audio output. tvOS 26 opened Settings > Video and Audio > Audio Output to any
AirPlay-enabled speaker, and the choice survives a power cycle, with one carve-out: system alerts
and game sounds keep playing through the TV speakers. The shairport-sync "Advertise as HomePod"
thread (2022-2025) was built on the older premise and is moot.

**Partly answered by the measurement pass.** The "Projektor" Apple TV has the HomePod pair as its
audio output and expresses that as a three-member group it leads (section 0). So the mechanism is
visible on the wire and we know exactly which fields it uses.

**And the two layers are independent.** Ungrouping the pair in the Home app also destroyed the
audio-output group, and tvOS warned the operator it would have to be re-added manually. The Home
app owns the pair (`tsid`, `flags` bit 13); tvOS Audio Output owns the group around it
(`gid`/`gpn`/`psgid`/`psgsz`/`pgmid`, Apple TV at `igl=1`). **A permanent output group therefore
needs only the group layer. None of the stereo-pair machinery is on the path.**

**Projektor is the wrong Apple TV to test against.** It reports `osvers=18.6`, `srcvers=870.14.1`.
The "any AirPlay speaker as permanent audio output" capability arrived in tvOS 26; on tvOS 18 the
menu is HomePod-only, which is precisely why that Apple TV has a HomePod pair and could never have
taken our board. Two others in the house are on tvOS 26.6:

| Apple TV | Model | Addr | osvers | srcvers |
|---|---|---|---|---|
| hjemmekinoen | AppleTV6,2 | 10.0.40.97 | 26.6 | 960.13.1 |
| Barnas | AppleTV5,3 | 10.0.40.242 | 26.6 | 960.13.1 |
| Projektor | AppleTV11,1 | 192.168.0.175 | **18.6** | 870.14.1 |

Both tvOS 26 boxes sit on `10.0.40.0/24` — **the same subnet as the boards**. That dissolves the
same-subnet timing-peer constraint in 3.5 as well as the version gate, so a custom permanent output
group should be attempted against `hjemmekinoen` or `Barnas`, never against `Projektor`.

Still untested: whether tvOS 26 lists *our* component in that menu at all. The boards advertise `_airplay._tcp` and `_raop._tcp`
only, with no `_hap._tcp` and no HomeKit accessory. If tvOS filters the Audio Output list by Home
membership rather than plain AirPlay discovery, no TXT field fixes that.

**The multi-destination question.** There are user reports that tvOS 26 regressed selecting several
AirPlay speakers at once — that where tvOS 18 would play to multiple destinations, only one outputs
afterwards (discussions.apple.com thread 256218866; unconfirmed). If an Apple TV routes to a single
destination only, separate icons cannot carry TV audio at all, and collapsing them stops being
cosmetic. Note the group we measured is led by an Apple TV still on tvOS 18.6.

**Lip sync.** The component honours the sender's `latencyMin` — 250 ms default on realtime, 0 on
buffered (`transport/transport_module.cpp:1026`) — so the Apple TV has what it needs to delay video
to match. Separately, tvOS 26 drew widespread lip-sync complaints on AirPlay audio output in
general, so budget for that being an Apple-side variable rather than a fault here.

## 7. Order of attack

0. **Look for the board in Audio Output on `hjemmekinoen` or `Barnas`** (both tvOS 26.6, both on
   the boards' own subnet — see section 6). Costs one menu visit and decides whether a custom
   permanent output group is reachable at all. If the board is listed, a *single*-board permanent
   output may need no code at all, and only a two-board group needs 3.2 and 3.3.
1. **Does PTP cross the VLAN?** tcpdump for `224.0.1.129` on `10.0.40.0/24` while the pair plays.
   Only matters for a group spanning subnets; moot if the group is built on a `10.0.40.x` Apple TV.
2. **Owner vs guest.** The guest device collapses the pair (confirmed). Test whether the owner's
   phone does too, or whether it is reading HomeKit. Decides whether mDNS forgery can ever work on
   the phone that matters.
3. **Identity and fields** — 3.1 plus 3.2, then look at the picker.
4. **Bit 52 and the SETPEERS parser** — 3.3, only once 1-3 say the group path is open.

### Risk note

Unlike the all-our-own-boards version of this experiment, forging `tsid`/`gid` against records
HomeKit treats as authoritative may confuse the Home app's view of the real pair, and if a sender
does accept the board into the group, it is unknown whether a board that then fails pair-verify
degrades the group or takes it down. Recovery is unpairing and re-pairing the HomePods. Do not run
this the evening the house wants music.

## Sources

- Measured 2026-09-16 on devbox (192.168.0.94) via a stdlib mDNS query. Primary data for section 0;
  supersedes the secondhand dumps below wherever they disagree.
- owntone#1413, "Handle Airplay stereo group as one speaker instead of two individual speakers" —
  the paired/unpaired avahi dumps, the `gid` `+`-suffix discussion, the "we do not know the protocol"
  quote, and the note on Sonos pairs:
  <https://github.com/owntone/owntone-server/issues/1413>
- owntone#704, "Airplay 2 - paired receivers support - stereo" — first `tsid`/`gpn` observation:
  <https://github.com/owntone/owntone-server/issues/704>
- owntone#1291, "Stereo HomePods show as two devices and switch playback between eachother":
  <https://github.com/owntone/owntone-server/issues/1291>
- enxi06/airplay2-stereogroup-analysis (2026-06) — macOS to HomePod mini pair captures: identical
  dual streams, PTP to one member. **Its `fex` `StBL`/`StBr` left/right conclusion is contradicted
  by section 0**; the dual-stream and skew findings still stand:
  <https://github.com/enxi06/airplay2-stereogroup-analysis>
- Unofficial AirPlay specification, service discovery — field definitions for `tsid`, `gpn`, `gid`,
  `igl`, `gcgl`, `pgid`, `pgcgl`, `pi`, `psi`, plus `hgid` (home group UUID) and `hmid` (household
  ID), neither of which our HomePods advertise. Does **not** document `fex`, `psgid`, `psgsz`,
  `psgtp`, `pgm` or `pgmid`:
  <https://openairplay.github.io/airplay-spec/service_discovery.html>
- SteeBono/airplayreceiver wiki, AirPlay2 Protocol — features bitfield table used for the decode in
  3.3: <https://github.com/SteeBono/airplayreceiver/wiki/AirPlay2-Protocol>
- lmcgartland/airplay2-rs, `AIRPLAY_2_SPEC.md` — an unofficial implementer RFC for the *sender*
  side, and the best single reference found. Source for the complete `flags` table that names bit 13
  `TightSyncIsGroupLeader` and bit 14 `TightSyncBuddyNotReachable` (section 2.5), a second
  independent features table confirming bit 52 (2.2), published per-device feature signatures
  including the AirPort Express 2 value that matches ours (2.2.3), and the HomePod access-control
  notes (section 9). It confirms our transient pair-setup path exactly: `X-Apple-HKP: 4`, SRP M1-M4,
  PIN `3939`, skip pair-verify — which is what `crypto/crypto_module.cpp:923` already does. It does
  **not** cover receiver-side group membership: `tsid` appears only as a TXT key, section 8
  ("Multi-room Peer Coordination") is three sentences of SHOULDs, and `psgid`/`psgsz`/`psgtp`/`fex`
  are absent. The core unknown in section 2 is untouched by it:
  <https://raw.githubusercontent.com/lmcgartland/airplay2-rs/refs/heads/master/AIRPLAY_2_SPEC.md>
- openairplay/airplay2-receiver — the SETPEERS and SETPEERSX body shapes quoted in 3.3; note that
  this implementation logs the plist and does nothing with it:
  <https://github.com/openairplay/airplay2-receiver>
- shairport-sync#1361, "AirPlay 2 over different IP Subnets (or VLAN/VPN)" — the same-subnet timing
  peer checks and the comment-them-out workaround:
  <https://github.com/mikebrady/shairport-sync/issues/1361>
- shairport-sync AIRPLAY2.md and discussion#1461 — no stereo pair support, and Mike Brady on the
  timing accuracy it would demand:
  <https://github.com/mikebrady/shairport-sync/blob/master/AIRPLAY2.md>,
  <https://github.com/mikebrady/shairport-sync/discussions/1461>
- Music Assistant discussion#5253, "Apple HomePod stereo pairing":
  <https://github.com/orgs/music-assistant/discussions/5253>
- Apple tvOS user guide, "Use HomePod or AirPlay-enabled speakers with Apple TV 4K":
  <https://support.apple.com/guide/tv/homepod-airplay-enabled-speakers-apple-tv-4k-atvb2490ae15/tvos>
- Apple Support Communities 256218866, "AirPlay Cannot Play Audio on Multiple Speakers in tvOS 26":
  <https://discussions.apple.com/thread/256218866>
