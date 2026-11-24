# Answering

`V90M_ROLE_ANSWER`, the default role: the called modem, and for V.90 and
V.92 the digital (server) side. This is the side that has answered real
modems: a Conexant CX93001, a U.S. Robotics 5686 and a U.S. Robotics 56K of
1998, in calls to the library inside v90d on a Grandstream HT802 (the
table in the top-level README, "Tried on real modems"). Seven of those
calls are kept as test fixtures.

How it is built is in `DESIGN.md`, sections 4.1, 4.2 and 4.2a.

## What it offers

`v90m_cfg.modulations`; the default (`v90m_cfg_default`) is everything but
V.92 and Bell:

| flag | what | default |
|---|---|---|
| `V90M_MOD_V90` | V.90, the digital side: PCM downstream, V.34 upstream | on |
| `V90M_MOD_V92` | V.92 PCM upstream on offer; only with `V90M_MOD_V90` (alone: `V90M_ENOTSUP`) | off |
| `V90M_MOD_V34` | V.34 | on |
| `V90M_MOD_V32BIS` | V.32bis and V.32 | on |
| `V90M_MOD_V22` | V.22bis and V.22 | on |
| `V90M_MOD_V21` | V.21 | on |
| `V90M_MOD_BELL103` | Bell 103, for a caller without V.8 | off |
| `V90M_MOD_BELL212A` | Bell 212A, with `V90M_MOD_V22`, for a caller without V.8 | off |
| `V90M_MOD_X2` | x2, U.S. Robotics' 56k of 1997, as its server; only with `V90M_MOD_V34` (without: `V90M_ENOTSUP`) | off |
| `V90M_MOD_K56` | K56flex, Rockwell's and Lucent's 56k of 1997, as its server, experimental; only with `V90M_MOD_V34` (without: `V90M_ENOTSUP`) | off |

At least one is required (0 gives `V90M_EINVAL`).

## The start of a call

200 ms of silence, then ANSam with phase reversals and V.8: the caller's
CM, our JM, its CJ. (With `V90M_MOD_K56` on a mu-law line, V.8bis comes
first: below, "K56flex".) The menu decides:

- **V.90**, when the caller offers PCM (an analogue V.90 modem): Phase 2,
  then Phases 3 and 4, then data. Downstream PCM at 8000 symbols a second;
  upstream V.34 at 3000 or 3200 symbols a second.
- **V.92**, with `V90M_MOD_V92` on: the same Phase 2 says so, and a caller
  that is V.92 too and selects PCM upstream gets V.92 Phases 3 and 4 and
  data, PCM upstream at 24000 to 48000 bit/s. Every other caller goes the
  V.90 or V.34 way. Two V.92 modems that both named LAPM in V.8 skip V.42's
  detection phase (V.92 9.3.1): `v90m_state.odp_bypass` is 1, and the V.42
  endpoint should then start without detection
  ([error-control.md](error-control.md)).
- **V.34**, **V.32bis**, **V.22bis / V.22** or **V.21**, as V.8 selects.
- **No V.8 at all**: the V.22bis answer sequence after ANSam, V.32bis AA,
  V.21 after 8 s.
- **Bell 103** (`V90M_MOD_BELL103`): a caller without V.8 that sends 1270 Hz
  (the Bell 103 mark) gets 300 bit/s FSK on 2225 / 2025 Hz, start-stop and
  no error control, as V.21.
- **Bell 212A** (`V90M_MOD_BELL212A` with `V90M_MOD_V22`): when the V.22bis
  answer sequence has gone unanswered for 4 s by a caller without V.8, the
  2225 Hz Bell answer tone replaces the unscrambled ones for 4 s; a caller
  that then starts as V.22 does is a Bell 212A call at 1200 bit/s.
- **x2** (`V90M_MOD_X2` with `V90M_MOD_V34`, mu-law only): when the
  caller's CM offers V.34 and no PCM, our JM and INFO0a are an x2
  server's. A U.S. Robotics x2 client then sends its marker after our L1
  and the call goes x2 (below); any other caller still gets V.34, but
  sees an x2 server's JM (carrier inverted after each JM) and INFO0a, not
  the ones it would get without x2. Not yet tried on a live non-x2 V.34
  call.
- **K56flex** (`V90M_MOD_K56` with `V90M_MOD_V34`, mu-law only): a caller
  that agreed K56flex in V.8bis, gets V.34 selected in V.8 and asks for
  K56flex in its INFO1c goes K56flex (below); every other caller goes as
  without K56flex, only later.

`v90m_state.used_v8` says whether the caller spoke V.8.

## What the program sees

- `V90M_EV_CONNECTED`: data mode reached; `v90m_state` has the modulation
  (`V90M_CONN_*`) and `tx_bps` / `rx_bps` (our transmit / receive rate;
  for V.90 and V.92 that is downstream / upstream).
- `V90M_EV_DATA_STOPPED` and `V90M_EV_DATA_MOVING` around every retrain and
  rate renegotiation; `v90m_state.retraining` during a retrain (not
  during a rate renegotiation, in any modulation).
- `V90M_EV_RATES` when the rates or the modulation changed.
- `V90M_EV_ENDED` once, with `v90m_state.end`: `V90M_END_FAILED` (no
  connection was made), `V90M_END_LOST` (the connection was lost: any
  loss once connected, a renegotiation included),
  `V90M_END_CLEARDOWN_FAR`, `V90M_END_CLEARDOWN_OURS`,
  `V90M_END_NO_COMMON` (V.8 found no modulation in common with the
  caller), `V90M_END_RETRAIN_FAILED` (V.90, V.92, V.34: a retrain failed,
  or one more was due past the limit, below; x2: the client's retrain,
  below),
  `V90M_END_X2_FAILED` (x2, below), `V90M_END_K56_FAILED` (K56flex,
  below). The code is the same as the calling role's for the same event.
  `V90M_END_NO_ANSWER_TONE` and `V90M_END_NO_JM` are the calling role's
  only. A caller whose V.8 sends CM but never CJ after our JM is served
  anyway: JM stops after 5 s (V.8 8.2.3 leaves its length open) and the
  modulation JM selected starts, as when CJ comes; a missed CJ does not
  lose the call.

`v90m_get_stats` adds the receiver's signal to error (`snr_db10`, 0.1 dB),
the round-trip delay measured in the handshake (`rtd`, samples: V.34, V.90,
V.92, x2, K56flex; both the engine's figure while connected, and after the
end the last measured while data moved), the echo delay in use (V.90, V.92, x2, K56flex: that of the engine
running now; during a retrain or after the end, that of the modulation of
the call), and the counts of retrains and of rate renegotiations that
stopped the data (V.34, V.90, V.92, x2, K56flex), each counted when data
moves again, when it becomes a retrain, or when the call ends in it; the
stop of a cleardown, ours or the far end's, does not count. In V.34 it
also gives the echo cancellers (`ec_*`): where they look, the loss of the
echo they found, and the power before and after them.

## x2

x2 has no published specification; it is built from the facts in
`docs/history/x2-facts.md`. It is offered only with `V90M_MOD_X2` and
`V90M_MOD_V34`, answering, to a caller whose CM offers V.34 and no PCM, on
a mu-law line: never with `alaw` set. Robbed-bit lines are not handled.

- **Start-up.** After the client's marker: its training, our sync frames
  and records, then data at the rate the client asks for, at first
  32000 bit/s (rate index 1). `V90M_EV_CONNECTED` with `V90M_CONN_X2`;
  `tx_bps` is the downstream payload (32000 to 56000), `rx_bps` the
  client's V.34 upstream (up to 28800). U.S. Robotics prints a rate one
  step of 1333 above the payload (33333 for 32000); the emulated
  Courier's CONNECT figure did not follow our rate at all (48000 or
  50666; `docs/history/x2-facts.md`, "Harness").
- **Rate changes.** The client asks for a new rate in data mode (a
  Courier about 12 s in, and again later); we answer at the rate and for
  the pad (none, 3 dB, 6 dB) its request names: `V90M_EV_DATA_STOPPED`,
  then `V90M_EV_RATES` and `V90M_EV_DATA_MOVING`, about 0.75 s later;
  `retraining` stays 0 and `v90m_stats.renegotiations` counts it. Its S
  is watched for from our start segment on (its B1, its E a record before
  it): one before our start segment is out is followed the same way, data
  moving only once our answer's start segment is out, none of the host's
  bits going down before (data had not moved: no `V90M_EV_DATA_STOPPED`,
  and it is not counted).
- **No way back to V.34.** Once the marker has come, a start-up or rate
  change that does not complete, or a request we cannot serve, ends the
  call with `V90M_END_X2_FAILED`; the client's Tone A (a retrain it asks
  for) ends it with `V90M_END_RETRAIN_FAILED`. `v90m_retrain` gives
  `V90M_ESTATE`; `v90m_cleardown` ends the call at once (`V90M_END_CLEARDOWN_OURS`).
- **What has been shown.** The owner's U.S. Robotics 56K FAX EXT (flash
  5.1.4) called the box (v90d with this library, 2026-11-21): x2 at 56000 /
  28800 with PPP, held 3 min 50 s until we hung up, two rate changes up, no
  retrain, the modem's SNR 59.7 dB; its counters 293 blocks sent, 4 resent,
  2 link NAKs, block error rate 0; pings across PPP 90 of 90 (average 148
  ms, worst 590 ms). Not shown on a live call: a client's retrain (it ends
  the call), pads, robbed bits, another x2 modem ([limits.md](limits.md)).
- **CPU.** x2 data mode costs about 1.8 times V.90's (56.58 M ARM
  instructions a second against 31.19 M; the upstream the client
  dictates): one x2 call per ARM926 box, with V.42bis too; two do not fit
  ([limits.md](limits.md)).

## K56flex

K56flex has no published specification; it is built from the facts in
`docs/history/k56-facts.md`, and it is experimental. K56flex (answering,
experimental, off by default) connects to Motorola's SM56 soft modem run
under emulation, V.42 data both ways at 32000 to 52000 bit/s down and
31200 up, with no pad and with robbed bits; its decoder takes our data at
every rate, pad and robbed-bit pattern; not yet tried on a Rockwell or
Lucent modem. It is offered only with `V90M_MOD_K56` and `V90M_MOD_V34`,
answering, on a mu-law line: never with `alaw` set.

- **V.8bis, before V.8.** After answering, at least 400 ms of silence, then
  CRe (1375 + 2002 Hz, then 400 Hz). A caller that answers with CRd within
  1 s gets our CL, whose NS field carries the K56flex block; a caller
  whose MS carries the block gets ACK(1), and K56flex is agreed (x2, if
  offered too, is then not offered in that call). V.8 follows in every
  case, from its 200 ms of silence, and decides as without K56flex: a
  caller whose CM offers PCM while V.90 is offered gets V.90, and one that
  gets V.34 goes on to Phase 2.
- **Every caller's answer tone comes later.** ANSam comes 1.790 s later
  than without K56flex for a caller without V.8bis, 3.807 s later for a
  V.8bis caller without K56flex (CRd, no MS), and 3.413 s later for a
  K56flex client (measured in the simulator). A caller that gives up in
  that time is not known; the live calls before v90d offers K56flex check
  it.
- **The point of no return.** In the V.34 Phase 2 of an agreed call, a
  client whose INFO1c asks for K56flex (frequency offset `0x001`), with
  3200 symbols a second chosen for the upstream, gets our INFO1a flagged
  for K56flex. After that there is no way back to V.34: the K56flex
  training follows, then data. Any other agreed call is V.34.
- **Start-up.** Our training down, the client's on the V.34 upstream with
  its J, which reports the pad (none, 3 dB, 6 dB) and the robbed-bit
  symbols; its MP names the rate it wants, ours answers with the rate:
  r = the lowest of the client's, `max_down_bps`'s and 13, 30000 + 2000 r
  bit/s. `V90M_EV_CONNECTED` with `V90M_CONN_K56FLEX`, about 8.3 s after
  our flagged INFO1a in the simulator; `tx_bps` is the downstream payload
  (32000 to 56000, as a Rockwell modem prints it), `rx_bps` the client's
  V.34 upstream (the highest rate its MP allows, capped by `max_up_bps`).
- **A client that ignores our MP.** Our MP asks the client's transmitter
  for 16 states, linear, minimum shaping, and nothing it sends says what
  it will use. A client that sends x2's upstream regardless (64 states,
  non-linear, expanded shaping) fails B1 under what we asked; its points
  since its E are kept (192 symbols), decoded again under x2's
  configuration, 3 every 2 symbol periods until the receiver has caught up
  (about 110 ms; B1 is decided about 36 ms after the retry), the
  equaliser back as it was at its E, and if B1 passes there the call goes
  on under it (a renegotiation starts from the configuration that passed,
  and tries the other if its B1 fails). Otherwise, as before,
  `V90M_END_K56_FAILED`. Its S while catching up: the stored points go on
  into the decoder at the same pace up to where S begins, and the data
  before S goes on but for one mapping frame, as without the catch-up.
  Only the simulated client has done it.
- **Renegotiation.** The client's (its S in data mode) is followed: our
  data stops at a frame's end, our short training, its MP, our MP, then
  data at the new rate: `V90M_EV_DATA_STOPPED`, `V90M_EV_RATES`,
  `V90M_EV_DATA_MOVING` (`retraining` 0; `v90m_stats.renegotiations`
  counts it). An MP with rate 0 ends the call as
  `V90M_END_CLEARDOWN_FAR`. We never start one. Its S is watched for from
  its B1 on: one before our data has begun is followed the same way, our
  R2 after our first data frame (data had not moved: no
  `V90M_EV_DATA_STOPPED`, and it is not counted). Not while its E has come
  before our MP is out (an early E: our MP, MP' and E still to go); an S
  there is decoded as data until our MP has begun.
- **Retrain.** The client's (its tone B in data mode) is followed: V.34's
  retrain Phase 2 with INFO1a flagged again, then the whole training:
  `V90M_EV_DATA_STOPPED` with `retraining` set, then
  `V90M_EV_DATA_MOVING` at the rates it agrees (some 10.5 s later in the
  simulator), and `v90m_stats.retrains` counts it. A tone B in data mode
  or in the client's renegotiation (one it gives up) is a retrain; a
  client that gives up during a training (the first, or a retrain's) and
  retrains again is not followed, and the call ends at that training's
  time limit (`V90M_END_K56_FAILED`). A retrain from its renegotiation
  is counted twice, in `renegotiations` (its S stopped the data) and in
  `retrains`. The third retrain in a row from data mode ends the call
  too; retrains are forgotten after 2 s of data mode. We never start
  one: `v90m_retrain` gives `V90M_ESTATE`.
- **Cleardown and failures.** `v90m_cleardown` ends the call at once
  (`V90M_END_CLEARDOWN_OURS`), with nothing sent: no K56flex cleardown is
  known. A start-up, renegotiation or retrain that does not complete in
  its time ends the call with `V90M_END_K56_FAILED`; an upstream lost in
  data mode, with `V90M_END_LOST`.
- **CPU.** K56flex data mode costs about the same as V.90's (it asks the
  client for V.90's upstream options); with a client that ignores our MP,
  about x2's (58.60 M ARM instructions a second against 32.12 M; 84 M and
  39 M before 0.6.0's CPU work), and, as measured before that work, up
  to 128 M a second
  for the 110 ms of catching up after its B1, about the box's whole CPU:
  v90d's answers come at most 11.7 ms late, inside its
  20 ms of prefill ([limits.md](limits.md), `docs/history/audit-0.6.md`).

## Retrains, rate renegotiation, cleardown

- **Retrains** in V.92, V.90, V.34 and V.32bis, begun by either end.
  `v90m_retrain` asks for one in data mode of those (`V90M_ESTATE`
  otherwise, or while one is under way). In V.90 the far end's Tone A is
  listened for through Phase 4 and data mode; our own retrain is 70 ms of
  silence and Phase 2 again from Tone B. A call is let go at the fourth
  retrain in a row (`DESIGN.md`, 4.2), V.90, V.92 and V.34 with
  `V90M_END_RETRAIN_FAILED`, as on the calling side; so does a retrain of
  theirs that does not get back to data mode.
- **Rate renegotiation** in V.32bis, V.34, V.90 and V.92 (the standards
  table of the top-level README); `v90m_stats.renegotiations` counts those
  of V.34, V.90 and V.92 (V.32bis's are not seen apart from its retrains),
  and those of x2 and K56flex. In V.90 and V.92 the caller's (its S) is followed;
  the upstream rate is ours to set, and when the receiver's error stays
  above what the rate needs for a second the modem renegotiates with a
  lower ceiling; when nothing lower is left, or nothing is received, it
  retrains.
- **V.92 after a retrain.** A retrain runs V.92's Phase 2 again and the
  caller selects again: PCM upstream stays on offer after any retrain of a
  call that reached V.92 data mode, whoever began it. It is withdrawn (the
  call goes on as V.90) when the V.92 start-up never reached data mode,
  when the V.92 engine failed, or when it retrains because the upstream did
  not hold even the lowest PCM rate.
- **Cleardown** in V.90 (9.7), V.92 (9.11) and V.34 (11.7), either way:
  `v90m_cleardown` returns `V90M_OK` when it has begun, and
  `V90M_EV_ENDED` follows within a fraction of a second. In V.92 the
  cleardown is a rate sequence (CPd) with data rate 0 (drn = 0); a caller
  that does not acknowledge it is given up to 3.5 s.
  `V90M_ESTATE` means there is nothing to send (V.21, V.22, V.22bis,
  V.32bis, Bell, or not in data mode): just stop calling
  `v90m_process_*`.
- **Carrier.** V.92 has no carrier detector of its own in data mode; a
  carrier watch ends the call when the caller is gone.

## Configuration that matters here

- `codec_linear_rx` and `max_tx_code` are answering only
  ([line-interface.md](line-interface.md)).
- `max_up_bps` caps what we receive: the V.34 upstream, or V.92's PCM
  upstream (up to 48000; 33600 holds it at 33333). `max_down_bps` caps the
  PCM downstream.
- `echo_delay` -1 measures the driver's delay in every start-up.

## Limits

- No V.92 quick connect, short Phase 2 or modem-on-hold.
- x2 and K56flex: above, and their limits in [limits.md](limits.md) (x2:
  A-law, robbed bits, no way back to V.34, the free sign; K56flex: A-law,
  no renegotiation or retrain of ours, the tables of Lucent clients, the
  later answer tone).
- No fax (V.17, V.29, V.27ter), no V.23, no half-duplex V.34.
- Bell 212A answering was tried only on a test build with the 2225 Hz tone
  from the start, by a modem that would also have taken V.22.
- The V.92 upstream design's noise model is about 2 dB short at the top
  upstream rates, on the simulator's lines ([limits.md](limits.md)).
- Only the three modems above have called it; the U.S. Robotics of 1998
  also in x2 (one call, above); none in K56flex.
