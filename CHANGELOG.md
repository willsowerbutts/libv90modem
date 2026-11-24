# Changelog

What each version of libv90modem changes for a program that uses it. The
API is not stable before 1.0: a 0.x version may change it, and every
change is listed here. Rebuild against the new header with every version;
`V90M_MEM_SIZE` and the configuration struct have changed in most.

"Simulation and own answerer only" means: tried against simulated lines and
this library's own answering modem, never against a real modem or server.

## Unreleased

Added (both roles):
- `v90m_cfg.retrain_on_step`, default 0: V.34 retrains, rather than
  renegotiates down, when what it receives falls in a step, as when a VoIP
  jitter buffer moves our echo out of the cancellers' reach, and when a
  rate renegotiation's training fails. `v90m-replay -R` sets it. With it
  off, nothing changes.
- Answering, `retrain_on_step` also retrains when the SNR has stayed 6 dB
  or more below the call's best for 30 s, as after a retrain that trained
  while the path was still moving; less and less often while that does not
  help.

Added (answering role):
- `v90m_stats.ec_*` (appended): V.34's echo cancellers, in data mode.

Changed (V.34, both roles):
- When the round trip is past 160 ms (a packet network), the far echo is
  searched for in Phase 3, the start-up's and every retrain's, rather than
  taken to be at the round trip Phase 2 measured: a jitter buffer may have
  moved it since. TRN is longer for the search, by 0.2 to 0.5 s. A calling
  modem without a far echo no longer loses downstream there (0.6.0's
  docs/limits.md: 12000 bit/s at 400 ms).

## 0.6.0 (2026-11-24)

Security (V.92, calling role; no 0.5.x patch: upgrade to 0.6.0):
- The V.92 analogue modem's upstream encoder could be made to divide by
  zero or read out of bounds by the digital modem it calls.
  - Affected: 0.3.0, 0.4.0 and 0.5.0, since the V.92 analogue modem came
    in (eff5db5).
  - Exposed: only programs that enable V.92 when calling
    (`V90M_MOD_V92` with `V90M_ROLE_ORIGINATE`). V.92 is not in the
    default modulations, and the answering side is not affected.
- How: the upstream starts B1u and data as soon as a CPd acknowledges
  ours, while the downstream goes on decoding CPds until Ed. Every later
  CPd with a valid CRC replaced the stored one, whose map and filter the
  running encoder read at every symbol. With one such CPd, a broken or
  hostile server could:
  - set a modulus Mi of 0: a division by zero, and the host process dies
    (SIGFPE);
  - set a constellation index past the six sets: the encoder reads up to
    about 2.3 KB past the map, and transmits what it reads.
- Changed:
  - The encoder takes its own copies of the map and the filter, checked
    (`v92_map_check`, `v92_filt_check`), when it starts. A later CPd
    reaches neither.
  - A later CPd that names an upstream rate and fails those checks is
    refused, as a CPd whose CRC fails is.
- Behaviour change for such a CPd (no valid CPd is affected):
  - Before, the call ended with `V90M_END_FAILED` (in a retrain
    `V90M_END_RETRAIN_FAILED`; inside the calling modem `V90A_ERR_CPD`)
    when the upstream started on it. Now the CPd is ignored, and the call
    ends with the same public end code (inside, `V90A_ERR_PHASE4`) once
    Phase 4 has not reached B1d 20 s after INFO1a.
  - The caller goes on acknowledging after a refused CPd, so the upstream
    that CPd would have set up never starts: the call ends at that 20 s
    Phase 4 limit.
- During the 0.6.0 work, a trim of zero filter taps briefly cached
  lengths in the encoder that could read out of bounds if the filter
  changed under it. That was never released; the trim now works on the
  encoder's own copy.

An audit of the whole library, its fixes, and the same arithmetic in
fewer instructions. Rebuild against the new header and link again: the
SONAME is `libv90modem.so.0.6`, and `V90M_MEM_SIZE` is smaller.

For a program, in short (each in detail below):
- End codes: answering, a connection lost in data mode ends with
  `V90M_END_LOST` (it gave `V90M_END_FAILED`, which now means only that no
  connection was made); a retrain past the limit or one that fails with
  `V90M_END_RETRAIN_FAILED`, and V.8 with nothing in common with
  `V90M_END_NO_COMMON`, as on the calling side. The calling side's V.34
  gives `V90M_END_RETRAIN_FAILED` where it gave `V90M_END_LOST`.
  docs/calling.md now says which end code a far-end hang-up gives.
- Callbacks: `v90m_reset` from a callback of the same modem is
  `V90M_ESTATE`; `v90m_destroy` from one takes effect at the end of the
  sample.
- NULL: every function checks its object; only the four per-bit
  functions do not.
- Ranges: `tx_level` above `V90M_TX_LEVEL_MAX` (16384), x2 / K56flex
  `max_up_bps` 1..2399 and every `v90m_ec_cfg` field outside its range are
  `V90M_EINVAL`; answering Bell 212A without V.22 is `V90M_ENOTSUP`.
- Memory: `V90M_MEM_SIZE` 108 KiB (120), `v90m_mem_size()` 106320 on
  x86-64 (118824), 106016 on ARMv5 (118512).
- Bounds: every wait of a call is bounded (Phase 2 and its retrains,
  V.22bis's S1, V.92's cleardown, LAPM's recoveries with `n400`), and
  long calls no longer overflow a clock (docs/limits.md, "Bounds and
  clocks").
- K56flex answering decodes a client that ignores our MP and sends x2's
  upstream (it cost the call before), at about x2's CPU.
- Start-stop (`v90m_async_*`): the queues hold all 512 octets (511
  before).

What has been shown:
- The replay proof: the 106 recorded real calls replayed in six
  configurations (636 runs) against 0.5.0's tool. 102 runs differ, and
  every difference is one a change below explains (the end codes, the
  upstream frames held at S delivered, the click blanking in the
  recordings' last 72 ms, SUVd between repeated CPds after our transmit
  left the recording, the end-of-call summary's renegotiations and the
  SNR and round trip kept after the call); the other 534 are identical,
  data included
  (`docs/history/audit-0.6.md`, "Replay proof against v0.5.0").
- Every refactor and every CPU change below was proven byte-identical:
  every test program's output (golden files), the 636 replays and the
  benches' hashes unchanged, under UBSan, and on armv7hf and big-endian
  aarch64 against the host (`make verify`, at every commit).
- The port matrix passed at the release proof: 15 targets, each printing
  what host gcc prints, program by program (229 programs; the whole
  suites on host gcc, ARMv5 uClibc, s390x and riscv64, 219 on the rest):
  x86-64 and i686, aarch64 and big-endian aarch64, ARMv7 hard-float,
  ARMv5, riscv64, s390x, ppc, ppc64, mips, mipsel and m68k (big-endian
  ones among them), Windows (mingw, under wine) and clang. The ARMv5 and
  m68k runs differ only where their C library's printf rounds up.
- Not shown: the calling side against a real modem or server (still
  simulation and our own answerer only); K56flex against a real modem.

Added (tools):
- `v90m-replay -s SAMPLE` starts the modem at that sample of the recording
  (0-based) with no search for the answer tone and no silence assumed before
  it, for recordings that begin at the answer tone; the times in the report
  stay the recording's. With `-k`, `-s` skips the alignment after V.8bis.
  `-l` takes a decimal level such as `-14.4` (to 0.01 dB; whole numbers give
  the level they always did). A bad `-l` or `-s` value prints the usage and
  exits 2: values the old `-l` read through `atoi` as something (`abc` as 0,
  `1e3` as 1) are now rejected. Without `-s` the run is unchanged.
  `test/fixtures/replay-opts.sh` tests both (part of `make test-fixtures`).
- `v90m-replay -r RMS` sets the transmit level as the rms itself (1..16384),
  the `tx_level_rms` a recording's sidecar gives: no `-l` reaches 3000
  exactly, and a replay at a level a step away differs from the recording
  within seconds. Of `-l` and `-r` the last one given holds. Tested in
  `test/fixtures/replay-opts.sh`.
- `v90m-replay -B` offers Bell 103 and Bell 212A as well, as a host may
  (nsdiald's sidecar lists them): without it a recording of a caller
  without V.8 differs from 4 s into the V.22 wait, where the recording
  modem sent the Bell 212A answer tone. `-M` transmits mark only in data
  mode, so that "differs in 0" proves the recording modem sent nothing
  else. `-F` prints the caller's V.42 frames (time, length, address,
  control, type, FCS-16 or FCS-32 good or bad) from the bits the V.42
  endpoint receives. Without them the output is unchanged. Tested in
  `test/fixtures/replay-opts.sh` on a simulated V.34 call with V.42 on
  both sides (`rec_orig v34 OUT ec`, new).

Fixed (API, either role):
- A cleardown, ours (`v90m_cleardown`) or the far end's, no longer counts as
  a rate renegotiation: a call that ended by cleardown ended with
  `v90m_stats.renegotiations` 1 (data stopped while still connected). A
  data stop that is not a retrain counts as a renegotiation when data
  moves again, when it becomes a retrain, or when the call ends in it,
  whatever the end except a cleardown (ours or the far end's); in any
  engine. Real renegotiations (V.34, V.90, V.92, x2, K56flex) are counted
  as before, but a live reader sees the count at one of those moments, not
  at the stop.
- `v90m_stats.snr_db10` and `rtd` keep the last value measured while data
  moved (a retrain's figures are not kept), and give it after the end of
  the call, a cleardown's included, until `v90m_reset`; they read -1 only
  when none was measured in the call (they read -1 as soon as the call was
  over, so an end-of-call log carried none). While connected the engine's
  own figure is returned, in a retrain too; the kept value only where the
  engine has none (-1).

Changed (answering role; a host that matched on `V90M_END_FAILED` for a
dropped call should look at `V90M_END_LOST`):
- A connection lost in data mode (V.21, Bell 103, V.22, V.22bis, Bell 212A,
  V.32bis, V.34, V.90, V.92, x2) or in a rate renegotiation now ends with
  `V90M_END_LOST`; it gave `V90M_END_FAILED`, which now means only that no
  connection was made.
- V.90, V.92 and V.34: a retrain past the limit in a row (the fourth), or
  one that does not get back to data mode, ends with
  `V90M_END_RETRAIN_FAILED`, as on the calling side (it gave
  `V90M_END_FAILED`).
- V.8 with no modulation in common with the caller ends with
  `V90M_END_NO_COMMON`, as on the calling side (it gave `V90M_END_FAILED`).
- `v90m_state.retraining` is 0 during a rate renegotiation of V.34, x2 and
  K56flex, as it was for V.90 and V.92 (it was 1); `v90m_stats.renegotiations`
  now counts those renegotiations too. `retraining` is still 1 during a
  retrain, K56flex's included.
- `v90m_stats.echo_delay` is that of the engine running now: after a V.92
  retrain that withdrew PCM upstream, V.90's (it gave V.92's figure until
  V.90 data mode); during a retrain and after the end, the call's.
  Answering, after a V.90 retrain that settled on V.34, it is -1: V.34
  as the call's modem has no echo delay figure.
- Phase 2 is bounded whatever the caller does: one that repeats its INFO0
  (V.90, V.92, V.34, x2, K56flex) or, in V.90 and V.92, reverses Tone A
  once but never twice kept the answering modem in Phase 2 for as long as
  it went on. Now the exchange ends after 20 s (from its start, or from a
  restart V.34 counts), and V.90's recovery (9.2.1.2.3) is tried at most
  4 times. Above those, a backstop ends the call when V.90's or V.92's
  Phase 2 (a retrain's included) or V.34's lasts 30 s, or V.90's or
  V.92's Phases 3 and 4 last 60 s: `V90M_END_FAILED`, or
  `V90M_END_RETRAIN_FAILED` in a retrain. A normal Phase 2 takes 3 to 5 s;
  no recorded call comes near the limits.
- `v90m-replay` traces: the end code and the "retraining" lines change
  accordingly (renegotiations of V.34, x2 and K56flex no longer print
  retraining 1).

Changed (answering role, K56flex):
- A client that ignores our MP and sends x2's upstream (64 states,
  non-linear, expanded shaping) is decoded: its B1 fails under what our MP
  asked for (16 states, linear, minimum shaping), and the points kept
  since its E are decoded again under x2's configuration (3 every 2
  symbol periods, the live ones joining them; caught up after about
  110 ms); if B1 passes there, the data goes on under it (it ended with
  `V90M_END_K56_FAILED`). Its data mode costs about what x2's does on the
  CPU. A wrong B1 under both still ends the call, about 40 ms later than
  before. Calls whose B1 passes are unchanged.
  Simulation only.
- A client that gives up its renegotiation and retrains (its tone B during
  the renegotiation) is followed as a retrain from data mode is; the call
  ended at the renegotiation's time limit (`V90M_END_K56_FAILED`, 6 s).
  Simulation only.

Changed (calling role):
- V.90 and V.92: Sd-bar is waited for 2000 ms from the start of Ja, not
  1500 (a stated deviation from V.90 9.3.2.4 and V.92 9.5.2.2.1, which add
  no round trip: a digital modem may wait 500 ms after a whole Ja, so on a
  round trip of about 0.9 s or more one that follows the text cannot meet
  1500 ms; a live server answered about 2 s after Ja on a VoIP path). A
  call with no Sd now begins its retrain (or ends: `V90M_END_FAILED`, or
  `V90M_END_RETRAIN_FAILED` in a retrain; `V90A_ERR_SD` inside the
  calling modem) 500 ms later; nothing changes when Sd comes.
- V.90 and V.92: the DIL asks for Ucodes 0 to 100 only, the loudest at or
  under a third of full scale in either law (V.90 8.4.1 leaves the DIL's
  contents to the analogue modem); it asked for 0 to 119. A softphone's
  gain control was seen to hold louder codewords down and read what
  followed low for a third of a second (reported by an independent
  softmodem's live calls). The constellations never use an unmeasured
  level, so CP stays under a third of full scale too: most simulated lines
  keep their rate, some move a step either way. The DIL is 171 ms
  shorter (4 passes of 1818 symbols, not 2160) and Ja's descriptor 154
  bits shorter; start-up reaches data mode about 220 ms earlier. The
  robbed-bit verdict is taken at a quarter of the Ucodes measured from 64
  up (it was a fixed 16 of 64).
- V.34, V.90 and V.92: the far end's L2 is read for 280 ms, not 460 (V.34
  11.2.1.1.5, V.90 9.2.2.1.8: at most 500). Our tone now begins 290 ms
  after the far L2 began, which leaves the far end 300 ms or more before
  its own wait ends (600 ms plus a round trip, V.34 11.2.2.2.3; V.90's
  digital modem 650 ms, 9.2.1.2.5); at 460 ms it had about 100 ms. A real
  answering modem that used nearly all of those 100 ms ignored the
  reversal that followed (live calls reported by an independent
  softmodem). Phase 2 is 180 ms shorter; the probe analysis on 14 blocks of
  20 ms instead of 23 moves some INFO1c / INFO1a choices and so the rates
  of some simulated lines by a step either way. The answering side still
  reads 460 ms (docs/limits.md).
- A retrain's Phase 2 is bounded whatever the answerer does. An answerer
  that kept repeating its INFO0 (V.90 and V.92: INFO0d; V.34: INFO0a) sent
  the calling modem back to the start of the exchange each time, for as
  long as it went on, with `retraining` 1 and no `V90M_EV_ENDED`. Each
  repeat now counts as one of Phase 2's 4 restarts; past them Phase 2
  fails. Above that, a backstop ends the call when a retrain's Phase 2
  lasts 30 s: `V90M_END_RETRAIN_FAILED`. Only a call that has reached data
  mode is affected (before it, the 60 s start-up limit applies as before).
- V.34: too many retrains in a row once connected, a retrain's Phase 2
  that fails (the restarts above, or any of its own limits), the 30 s
  limit above, and a retrain that fails in Phases 3 and 4 (its training,
  or the carrier lost in it) end with `V90M_END_RETRAIN_FAILED`, as in
  V.90 and V.92 and on the answering side (they gave `V90M_END_LOST`).
- V.90 and V.92: a Tone B (the server's retrain) during a cleardown of
  ours no longer turns it into a retrain. After `v90m_cleardown` the
  cleardown goes on and ends with `V90M_END_CLEARDOWN_OURS`. The same holds
  for the cleardowns the modem makes by itself: when no downstream rate
  fits (a server retrain no longer rescues it), and when one retrain more
  is due past the limit (`V90M_END_RETRAIN_FAILED`, as before).
- V.90 and V.92: PCM is chosen only when the answerer's JM names a digital
  modem (V.8's PCM category, "V.90/V.92 digital"). A JM naming only "PCM
  analogue" (an analogue V.90/V.92 modem answering, which cannot carry a
  PCM downstream) selected V.90 and the call failed in Phase 2
  (`V90M_END_FAILED`); it now gives V.34 (or the lowest mode in
  common). Against this library's answerer, whose JM names the digital
  modem, nothing changes.
- V.92: a CPd that carries only some of its three parts (moduli, filters,
  constellations; Table 30's bits 19 to 21), as a digital modem may send
  in a rate renegotiation when only part of it changed, is merged with the
  CPd before it: the parts it leaves out are kept. It was refused, and the
  renegotiation became a retrain (8 to 11 s each; past the limit,
  `V90M_END_RETRAIN_FAILED`). The first CPd after Phase 3 (start-up or
  retrain) must still bring all three parts, else that start-up or
  retrain fails as before. This library's answerer always sends all
  three; no real server's partial CPd has been seen.

Changed (V.34, both roles):
- A cleardown of ours (`v90m_cleardown`) is no longer turned into a
  retrain: the far modem's retrain tone (Tone A to the caller, Tone B to
  the answerer) heard during it is ignored, and the renegotiation's
  time-out in it ends it as the cleardown it is. Either way the call ends
  with `V90M_END_CLEARDOWN_OURS` (it began a retrain, which ran on to
  whatever end the retrain found).

Changed (the upstream QAM receiver in data mode: V.34 both roles, V.90
answering, x2 and K56flex):
- A click on the line in data mode no longer leaves the upstream deaf for
  the rest of the call. A sample more than 18 dB above the input RMS
  measured at S (the AGC gain is set then and stays fixed in data mode) now
  enters the equaliser as silence; a full-scale click is only caught when
  the line was below about -12 dBm0 at S (see docs/limits.md). A full-scale
  click of 0.75 ms used to move every equaliser tap as it passed, and at
  the band edges the tracking never took that back: x2 decoded garbage for
  the rest of the call with no end code; V.90 and V.34 went into a rate
  renegotiation or a retrain. Now the bits around the click are lost and
  the upstream is clean again at once. Training is not affected (some of
  its peaks are that loud). In the recorded calls, the output changes only
  where the line's hang-up click arrives in data mode, in the last 72 ms of
  the recording.

Changed (V.22bis, both roles):
- The wait for the end of a far-end S1 is bounded at 1 s (S1 is 100 ms): a
  steady tone at the receive carrier (1200 or 1205 Hz for the answerer,
  2400 for the caller) looks like S1 for as long as it lasts and kept a
  connected call in that wait with no end code for as long as it went on
  (600 s in the test). In a retrain the wait now ends with our own retrain
  signal, so the retrain limit applies and the call ends `V90M_END_LOST`
  (about 11 s); in start-up it goes back to waiting for the far end.
- The V.22bis receiver forms its carrier-loop and equaliser products in 64
  bits (they overflowed a signed 32-bit value on a full-scale click into a
  high gain early in the data, an undefined behaviour); output is
  unchanged where they did not overflow.

Changed (V.92, both roles):
- The detector of Ru, Su and Sd (the answering V.92 modem's Phase 3, data
  mode and rate renegotiation; the calling side's wait for Sd) forms its
  correlations in 64 bits: with samples past about 23170 they overflowed a
  signed 32-bit value, an undefined behaviour. Output is unchanged where
  they did not overflow; a full-scale Ru (to 32767 and -32768) is detected.
- The CPd design (answering V.92) refuses, as an unusable channel, a
  channel estimate whose arithmetic leaves 32 bits; the call ends as for
  any failed design (`V92D_ERR_DESIGN`). On a long low-pass tail, which the
  caller's own training decides, its vectors overflowed a signed 32-bit
  value (and the residual ISI a 64-bit one), an undefined behaviour after
  which it ran on with wrapped values. A residual ISI 16 times the
  cursor's power or more now saturates its estimate instead of wrapping.
  Designs that stayed in range are unchanged.
- The CPd design's trials no longer read past the end of their channel
  buffer when the cursor sits so late (a channel peak in the last taps of
  the estimate) that the feedback taps after it reach the end; the read
  was an undefined behaviour. Other designs are unchanged.
- Answering V.92, data mode and rate renegotiation: a line that goes dead
  (the caller hangs up and the network sends the idle code) ends the call
  as a carrier loss (`V90M_END_LOST`) after the usual half second. The
  echo canceller then gives out its own estimate of our echo, as loud as
  the echo was: the carrier watch took that for the caller's signal and
  data was delivered from it, and when what we sent was periodic (our Rd)
  the Ru detector took it for the caller's Ru and started a rate
  renegotiation (or, in ours, went on to Phase 4) instead. Now the carrier
  watch wants the caller's power in the line as well as after the
  canceller, and no Ru is taken while the line has brought nothing but the
  idle code (within +-8) for 24 samples. The caller's Ru is detected as
  before, down to the same level.

Fixed:
- Answering role, V.90 and V.92: a caller that picks a quiet TRN1d (a
  small UINFO in INFO1a) and sends loud audio while it should be silent
  drove the echo canceller's taps past their 32-bit range (signed
  overflow), and a loud transmit through such taps then overflowed the
  adaptation step. The taps now stop at their largest value, and the
  step is held. A robustness fix
  against hostile audio, with no memory-safety impact: the hostile
  caller's own call is the one that suffers. Every other call is
  unchanged.
- Answering role, x2, K56flex, V.34, V.90 and V.92: the receiver of the
  V.34 upstream overflowed (signed overflow) on loud or abrupt audio after
  S, such as full-scale noise or runs of -32768, and its mse could go
  negative. Its arithmetic is now defined throughout and gives the same
  results as before, the taps are held within range, and the mse stays
  between 0 and its clamp (6 dB). A robustness fix against hostile audio,
  with no memory-safety impact; every call computes as it did, the
  recorded calls' hang-up clicks included.
- Answering role, V.92: an echo estimate past about 3.1 M (canceller
  taps at their limit, as the hostile caller above can make them) wrapped
  the design's noise term, so that the line read as quiet and the design
  picked too dense a constellation for the caller's upstream. The term now
  saturates. Only the hostile caller's own call was affected.
- Answering role, a V.21 caller without V.8 (three ISP calls,
  2026-11-23): it answers ANSam with its mark, a steady 980 Hz, sends no
  CM and waits for our 1650 Hz. We went on to V.22 and fell back to V.21
  only after 8 s of silence, which its mark never gave (V.22's carrier
  detect took it), and sent V.22 and the 2225 Hz Bell tone until the
  caller hung up. Now, with V.21 offered, 200 ms of 980 Hz in the V.22
  wait (heard as Bell 103's 1270 Hz is, behind the same band-pass) starts
  V.21 answering at once: connected about 0.35 s after the mark is heard.
  V.22, V.22bis, Bell 212A and Bell 103 callers, V.8 callers and a silent
  or noisy line are answered as before; no recorded call changes.
- Answering role, a caller behind a packet network (an ATA's or a VoIP
  gateway's jitter buffers): the echo of the far hybrid comes back about a
  round trip late, 300 ms on a Cisco IOS FXS port, past the 160 ms the echo
  delay search covered; the canceller sat on nothing, the upstream error
  stayed near -16 dB and the caller retrained. The search now ends a little
  past Phase 2's round trip, as V.90 does since Will Sowerbutts's
  `ecdly_near` (PR #1), in V.92 (PCM upstream), x2 and K56flex too
  (K56flex also starts it that much later, when the echo of TRN1A is back).
  A round trip under 128 ms changes nothing. V.34 needed nothing: its far
  canceller is placed on the round trip already. Tested with a far echo
  20 dB down at 300 ms and 400 ms, and at 100 ms, for V.90, V.92, x2,
  K56flex and V.34: the delay within 12 samples of the echo, the upstream
  error below -30 dB (V.92: its decoder's SNR above 30 dB), the data clean.
- V.92, answering role: our cleardown (`v90m_cleardown`) ends with
  `V90M_END_CLEARDOWN_OURS` within 3.5 s whatever the caller does. It
  waited for the caller's Ru-bar and a new CPd design: a caller that no
  longer heard us (or a design refused) turned the hang-up into a retrain
  8 s later and the call ended some 18 s after the request. Now no design
  is made (the CPd in use, with drn = 0) and nothing in the cleardown
  retrains; any end of it is ours.
- V.92, answering role: our cleardown sends what V.92 9.11 asks, a rate
  renegotiation (9.8.1.1) that carries drn = 0: Rd, Rd-bar, TRN2d, then
  SUVd until the caller's SUVu, then a single CPd with drn = 0 followed by
  SUVd, with the acknowledge bit once the caller's CPu is in (9.8.1.1.2,
  9.6.1.1.2). It is over once the caller has acknowledged the CPd and has
  our SUVd' (where 9.6.1.1.4 would send Ed): some 0.3 s with the simulated
  caller. Before, four CPds went out back to back right after TRN2d, with
  no SUVd and no wait for the caller's SUVu; only this library's own
  caller and the simulator took that. A caller that never acknowledges
  is given 3.5 s.
- V.92, calling role: the digital modem's cleardown (a CPd with drn = 0)
  is acknowledged before we go silent, as V.92 9.6.2.1.2 asks: what we
  send after it (SUVu, or CPu if it was not yet out) carries the
  acknowledge bit, and the call ends with `V90M_END_CLEARDOWN_FAR` at
  9.6.2.1.4's point (our acknowledging sequence out, the digital modem's
  SUVd' / CPd' or Ed in), one sequence later, or 100 ms + a round trip
  after the CPd when no acknowledge comes (9.6.2.1.3's wait). No E2u: it
  leads to B1u and data, which drn = 0 rules out. We hung up on the CPd at
  once, so this library's answerer, which waits for the acknowledge,
  ended only at its 3.5 s limit; now about 0.4 s after its request.
- V.92, answering role: a CPd the caller does not answer (9.6.1.1.3) is
  repeated one copy at a time, with SUVd between the copies: 9.6.1.1.2's
  single CPd followed by SUVd, once per copy (V.92's Figures 12 to 14 draw
  no repeat). The repeats went out back to back until the answer came;
  a real Agere engine is reported to clear down on that. A caller that
  answers the first CPd (every real call recorded) sees no change.
- V.92, answering role: the rate drop the answering modem starts itself
  when the upstream decoder reports bad frames asks for one step (4000
  bit/s) below the rate in use. Every earlier drop was counted again: in a
  call that began at 40000 the second drop asked for 28000 instead of
  32000, and from 48000 the call lost PCM upstream at the fourth drop
  rather than (at the latest) the seventh. The drops also held every later
  renegotiation down, the caller's included (one whose ceiling fell below
  24000 bit/s ended in a retrain without PCM upstream); they no longer do.
- V.92, answering role: a caller whose data mode arrives louder or weaker
  than the CPd design asked for (implementations differ on the power
  reference of V.92 8.8.3) is decoded. B1u failed (`V92D_ERR_B1`, then a
  retrain with PCM upstream off) when the caller was off by 10 % or more
  either way. The answering modem now measures the caller's gain on the
  first 144 symbols of B1u (their power against the design's trial of the
  encoder, then the scale at which the symbols sit on the points) and, if
  it is off, decodes B1u again from its start with the samples scaled.
  Simulated, 0.4 to 3.0 of the design are measured within 1 %; data flows
  without error from 0.4 to 1.5 on every line tried, and above that on a
  short line (docs/limits.md for the limits). A scale is taken only within
  3/4 to 4/3 of what the power says, and only where at least 3/4 of the
  symbols sit on the points and clearly more than at the designed level;
  where none is taken (every recorded call) the call is decoded exactly as
  before. The measurement costs one tick of some 175 k ARM instructions in
  any call whose B1u does not sit well on the points at the designed level
  (21 of the 26 recorded calls that reach B1u), 14 k in the others.
- V.90 and V.92, calling role: the last data frame before every Rd of
  the digital modem's (its rate renegotiation or cleardown, or its answer
  to a renegotiation or cleardown of ours) is delivered. It was dropped,
  21 to 42 bits at each such Rd: V.42 sent it again, without error control
  it was lost.
- V.90, answering role: the upstream data the caller sent before its S (a
  rate renegotiation or cleardown, the caller's, or its answer to ours) is
  delivered but for one mapping frame. The decoder's decision delay still
  held some 13 ms of it (24 4D symbols, up to 5 mapping frames) when S was
  heard, and it was dropped; V.42 sent it again, without error control it
  was lost. Now the whole mapping frames that end at least one frame before
  where S was found to begin go on (S's start is found from its two points
  among the last symbols; a channel turns the first few, so the start can
  be found late, and the frame of margin keeps a frame that S cut, or that
  holds S, from being delivered). Calls without S in data mode are
  unchanged.
- V.92, answering role: the same for the caller's Ru. The data frames the
  decoder held that end at least one data frame before where Ru was found
  to begin (Ru repeats every 6 samples) are delivered; they were dropped
  (some 13 ms). What the caller's prefilter held when Ru began (about
  `dly` / 12 data frames, 10 at 32000 bit/s) never reaches the line and is
  still lost without error control (V.92 begins Ru on a data frame
  boundary): docs/limits.md.
- V.90, calling role: S (a renegotiation or cleardown) begins once the
  mapping frame under way is sent, up to 7 symbols later. It began at
  once, and the rest of the frame, its bits already taken from the host,
  was not sent.
- V.34, both roles; x2 and K56flex, answering (their V.34 upstream): the
  data the far end sent before its S in data mode (a rate renegotiation or
  cleardown, either end's, or x2's rate change) is delivered but for one
  mapping frame, as V.90's above. The decoder's decision delay held up to
  5 mapping frames when S was heard, and they were dropped. An S heard
  before the decoder holds four symbols of it (its carrier line can build
  up early; the newest two decoder inputs already look like S's points) is
  waited for, up to 32 samples, so that its start can be found; this
  applies to V.90 answering too. One that is not found still drops them,
  as before, and what is heard as S on noise is taken at once, as before.
- V.34, both roles: our S (a rate renegotiation or cleardown, ours or our
  answer to the far end's) begins once the mapping frame under way is
  sent, up to 7 symbols later. It began at once, and the rest of the
  frame, its bits already taken from the host, was not sent. Together:
  exactly one mapping frame is lost each way across a renegotiation
  between two of ours, where 5 or 6 were (390 to 468 bits at 31200 bit/s).
  Calls without S in data mode are unchanged.
- x2 and K56flex, answering: the client's S is watched for from its B1's
  first symbol on. It was watched for only once data moved both ways, so
  an S that came while its B1 was being decided or our own data had not
  begun (K56flex: up to some 67 ms after its E; x2: until our start
  segment was out, some 510 ms) was decoded as data until it was heard:
  up to 5 mapping frames of bits that were not data (390 at 31200 bit/s)
  were delivered, or (x2) the rate change was never seen and the rest of
  the call was garbage. Now such an S is followed as any renegotiation:
  the frames before it go on but for the margin frame once B1 has passed,
  nothing before it is data if B1 has not been decided. x2: data moves
  again only once our answer's start segment is out (`x2tx` no longer
  reports data next at the end of a start segment with the client's rate
  change pending), and none of the host's bits go down before it. K56flex: our R2
  then waits for our first data frame (with our MP, MP' or E still going
  out it began at once, which the client does not look for). An S within
  about 40 ms (60 ms for a client that ignores our MP) of its data ended
  the call (`V90M_END_K56_FAILED`); it is now followed too.
- K56flex, answering, a client that ignores our MP: its S while the stored
  points are being caught up with (about 110 ms after its B1 failed under
  what we asked) lost 6 to 18 mapping frames (the decoder was behind the
  line: S was not found in it, and the points still stored were dropped).
  Now the stored points go on into the decoder at the same pace during its
  S and S-bar, up to where S begins: the margin frame is lost, as
  otherwise. Calls without S near the start of data are unchanged.
- V.92, calling role: the clock offset measured on the digital modem's L2
  can no longer overflow (undefined behaviour in C) when the line turns
  its tones far beyond 1000 ppm per block; it gives the +-1000 ppm limit,
  as it did whenever the computation did not overflow. Every other result
  is unchanged.
- m68k: built with GCC 12.3 for m68k (and maybe other versions) at -O1 and
  up, the V.34 receiver's carrier (`qamrx_init`) and the answering V.90
  receiver's clock (`v90arx_clock`) were computed from the wrong stack
  slot (a compiler wrong-code bug), so V.34 did not train and the V.90
  clock was garbage (a value that depends on the binary's link layout).
  Both are now written in a form that compiler gets right; the result on
  every other target is the same as before. The port matrix builds m68k
  with `-fno-omit-frame-pointer`, with which the suite passes under GCC 12.3.
- Long calls, both roles, every modulation: the modems' sample clocks and
  counters no longer overflow (undefined behaviour in C) after 2^31 samples
  or symbols. The V.34 receiver (the upstream of V.34, V.90 answering, x2
  and K56flex) stopped after 58 to 62 hours of data mode, the V.90 calling
  receiver and the V.92 answerer's upstream decoder after 74.6 hours, each
  with the call left in data mode and no data moving. The clocks now wrap
  or stop at their limit as defined C, and the receivers' sample and
  symbol numbers are brought back every 2^30 in data mode; the V.34
  receiver's symbol count goes on from 0 past 2^31 - 1, so that its
  timing nudge every 1024 symbols keeps running (held at the limit it
  would have stopped after 174 hours at 3429 baud). A host that
  keeps calling `v90m_process_*` after `V90M_EV_ENDED` no longer overflows
  them either. Shorter calls are unchanged.

Changed (API: callbacks, NULL, configuration checks):
- From a callback (`getbit`, `putbit`, `on_event`), `v90m_reset` on the
  modem that calls it returns `V90M_ESTATE` and changes nothing (it reset
  the modem under the block being processed: undefined behaviour), and
  `v90m_destroy` takes effect at the end of the sample under way: no event
  after it, the rest of the block is silence (linear 0, mu-law 0xFF, A-law
  0xD5), the modem is freed when owned, and `v90m_process_*` returns
  `V90M_OK` (it freed the modem under the block: a use after free).
  `v90m_init` on the modem's own memory from a callback is documented as
  not allowed.
- NULL: every function checks its object (and the struct that
  `v90m_get_state`, `v90m_get_stats`, `v90m_ec_get_status` fill): an `int`
  function returns `V90M_EINVAL` (`v90m_ec_state`, `v90m_ec_tx_idle`,
  `v90m_ec_tx_busy`, `v90m_ec_get_break` included), a `size_t` or
  `unsigned long` one 0, a `void` one does nothing; `v90m_retrain`,
  `v90m_cleardown`, `v90m_ec_reset` and most `v90m_ec_*` and
  `v90m_async_*` functions crashed. The four per-bit functions
  (`v90m_ec_tx_bit`, `v90m_ec_rx_bit`, `v90m_async_getbit`,
  `v90m_async_putbit`) are not checked: their object must not be NULL.
- `tx_level` above `V90M_TX_LEVEL_MAX` (16384, new) is `V90M_EINVAL`: a
  larger one overflowed (`INT_MAX`: undefined behaviour) or wrapped the
  16-bit tone amplitude (from 23171 up: a sign-flipped, clipped answer
  tone).
- Answering with x2 or K56flex, `max_up_bps` 1..2399 is `V90M_EINVAL`: it
  left the client no upstream rate after the point of no return, and the
  call ended `V90M_END_X2_FAILED` / `V90M_END_K56_FAILED`.
- Answering, `V90M_MOD_BELL212A` without `V90M_MOD_V22` is `V90M_ENOTSUP`
  (it was accepted and Bell 212A never offered).
- `v90m_ec_create`, `v90m_ec_init`, `v90m_ec_reset` check the
  configuration: a value outside the ranges now written next to each field
  of `v90m_ec_cfg` is `V90M_EINVAL` (it was clamped or taken as something
  else: `comp_policy` 3 as AUTO, `establish_ms` < 0 as never, `t403_ms` < 0
  as off, `v42bis_p1` above 2048 agreed but not run, and so on).
- `v90m_ec_send_break` with an `opt` other than the three `V90M_BRK_*` is
  `V90M_EINVAL` (0x80 went out as a combination V.42 does not define).
- `v90m_ec_tick` with `ms` above `INT_MAX` expires the timers (it extended
  them).
- Memory: `V90M_MEM_SIZE` is 108 KiB (it was 120 KiB), and
  `v90m_mem_size()` 106320 octets on x86-64 (118824), 106016 on ARMv5
  (118512): the calling side's modulations after V.8 share memory, which
  leaves the answering side's state the larger. Rebuild against the new
  header.

Fixed (error control):
- XID values longer than they need to be: a value padded with leading
  zero octets past 4 (V.44's past 3) was read from its leading octets
  (N401 sent as `00 00 00 08 00` read as 1 bit, V.44 P1T 2048 in 4
  octets as 8, and the call released for a negotiation failure). The
  padding is now read through; a longer value that is not zero there is
  out of range, as one too large is. A 4-octet k or V.42 bis value of
  2^31 or more was taken as absent (the default); it is now out of range
  too: for k and P1 our own value is used, and a V.42 bis P2 that large
  is refused as a negotiation failure, as any P2 above 250 already is.
  Calls whose XID values fit are
  unchanged.
- `v90m_ec_abort` stops the endpoint in every state, and
  `v90m_ec_state` says `V90M_EC_DISCONNECTED` (reason `V90M_EC_R_LOCAL`)
  on return. In the detection phase it did nothing (the endpoint went on
  to start-stop mode at T400 and delivered what the line carried); in
  start-stop mode it did nothing (data kept moving, writes were taken);
  in error-corrected mode the state showed it only after the next call.
- An originator with the detection phase takes the answerer's HDLC flags
  as the sign of an error-correcting answerer, as the answerer takes the
  originator's: against an answerer without the detection phase (V.42
  7.2.1) it went to start-stop mode at T400 and read the flags as
  characters, while the answerer waited for a SABME.
- Documented: after a `v90m_destroy` from `getbit` or `putbit`, the rest of
  that sample still calls `getbit` and `putbit` with the host's `user`
  pointer; free the user object only after `v90m_process_*` returns.

Changed (error control, V.42 / LAPM):
- Our XID command gives the HDLC optional functions in three octets (bit 16
  set; it gave four): the form real modems send, and some (an Agere
  SV92PP) drop everything after a mask of another length, our V.42bis and
  V.44 offer with it. Answers still have the form of the command.
- A responder that answers a smaller `k` or `N401` than we proposed for our
  sending direction is taken at its word (it was raised to our value or
  the default, more frames than it said it could take).
- Bounded waits (a ruling that departs from V.42, which starts the count
  again at each recovery): after `n400` recoveries (timer recoveries, and
  go-backs on a REJ that acknowledges nothing) with I frames
  unacknowledged and none acknowledged in between, the next one releases
  the link with `V90M_EC_R_N400`. A peer answering every poll, or every I
  frame with such a REJ, but never acknowledging kept the link up for
  ever. A busy peer (RNR) is not counted; a destructive break does not
  start the count again.
- V.42bis P1 is proposed at most at `V42COMP_MAX_CODEWORDS` (2048), and a
  compression direction agreed but not runnable releases the link with
  `V90M_EC_R_COMPRESSION`. (Through the API, `v42bis_p1` above 2048 is
  already `V90M_EINVAL`; the endpoint underneath agreed 4096 and then ran
  uncompressed while the far end compressed.)
- The HDLC receiver's count of consecutive flags saturates (at 65535): on
  an idle link with T403 off it overflowed after about 85 hours (signed
  overflow).
- `v90m_ec_mem_size()` unchanged (49792 on x86-64).

Changed (build and install):
- The shared library's SONAME is `libv90modem.so.MAJOR.MINOR` while MAJOR
  is 0: `libv90modem.so.0.6` for this release (it was `.so.0` for every
  0.x, so a program built against 0.4 loaded 0.5 and passed it structs of
  the old layout), `.so.MAJOR` from 1.0. The links installed are
  `libv90modem.so` and `libv90modem.so.0.6`. Programs linked against the
  shared library must be linked again.
- The static library no longer defines `v90m__answer`, `v90m__orig` and
  `v90m__ec` (internal accessors that looked like API): renamed
  `v90m__i_*` like every other internal. `make check-symbols` rejects
  them; `make check-soname` (part of `make check`) checks the SONAME.
- Internal, not API: a host whose tests reach into the library's
  internals must change them. In the static library `v90m__answer()` is
  now `v90m__i_answer()` (the sources, `api/v90modem_priv.h`, keep the
  short name), and what it points at changed: `answer_t`
  (`modem/answer.h`) is reorganised (its schemes moved to
  `modem/ans_*.c`; `answer_tick` and the phase names stay, the fields
  behind them may not), and the calling side's
  `orig_t` keeps its engines in one union (`o->e.v90`, `o->e.v34`,
  `o->e.leg`). Nothing in `include/v90modem.h` is affected; internals may
  change again in any version.
- `make uninstall` also removes a `libv90modem.so.0` link left by 0.5.0 or
  earlier once it points at nothing. Packagers: the `.so.0` link is no
  longer installed.
- `make check-version` also checks README's "This is X" and the `.so`
  names in docs/getting-started.md; `make check-headers` covers `api/`.
- `make verify` (per commit: the golden outputs of every test program,
  the replay proof of the recorded calls, the bench hashes, UBSan, the
  armv7hf and big-endian short suites, the fixed-point check) and `make
  verify-phase` (the release proof: armv5, the port matrix, the benches
  under qemu, the costs); `make cost-v42`, `make cost-x2-v42`.
  docs/testing.md.

Fixed (start-stop characters, V.14):
- The queues of `v90m_async_*` hold 512 octets each way, as documented;
  they held 511 (Will Sowerbutts). A character received with the receive
  queue full is still dropped without a count (documented).

Faster (output identical, bit for bit; ARMv5 instructions counted under
qemu, M a second of line in data mode, 0.5.0 -> 0.6.0):
- x2 answering: 82.57 -> 56.58 (32000); with our V.42 endpoint on its
  data (`make cost-x2-v42`, new), 32000 / 56000: no compression 58.89 /
  61.50, V.42bis 69.24 / 77.19, V.44 89.25 / 108.78. Still one x2 call
  per box: two with V.42bis would need 2 x 77.19 = 154.37 M of about
  122 M.
- V.90 answering 39.20 -> 31.19; V.34 answering 40.53 -> 33.42; K56flex
  answering 39.27 -> 32.12, a client that ignores our MP 84.43 -> 58.60.
- V.90 calling 55.11 / 59.31 -> 46.97 / 50.95 (0 / 100 ppm); V.92
  calling 69.95 / 74.32 -> 62.12 / 66.52. Bell 103 / 212A 9.88 / 4.66
  (9.86 / 4.43: 212A's rise is the 64-bit products of the V.22bis fix).
- V.42, per endpoint at 56 kbit/s both ways (before -> after the V.42
  work): no compression 7.96 -> 5.21, V.42bis 11.81 -> 9.08, V.44 12.19
  -> 9.48.
- How: the V.34 non-linear decoder without most 64-bit divisions, the
  Viterbi add-compare-select and the subset slicer in one pass, divisions
  by powers of two as shifts, the interpolators in straight runs, each LMS
  step forming the next dot product, V.42's per-bit path inline with the
  FCS by table, and smaller exact forms (`docs/history/audit-0.6.md`,
  Tasks 30 to 36). Each was checked against the code it replaced, by
  exhaustive or wide random tests where the arithmetic changed, and by the
  unchanged outputs above.
- A compiler builtin is used only behind a C99 fallback with the same
  result (`modem/compiler.h`); `-DV90M_NO_BUILTINS` takes the fallbacks.

Refactored (no change in behaviour): the answering modem is one file per
scheme behind a phase table (`modem/ans_*.c`); the V.34 upstream receiver
is one (`modem/v34up`) for V.90, V.34, x2 and K56flex; the V.90 / V.92
analogue modem is split by job (`modem/v90a_*.c`); V.42's configuration
and status lists are generated from one table
(`api/v90modem_fields.h`); V.42bis and V.44 behind one interface.
DESIGN.md, section 2.

Documentation: docs/api.md, the header and docs/answering.md agree on
`rtd` and `echo_delay` (x2 and K56flex included; calling, `echo_delay`
-1 is taken as 0 and more than 511 as 511); docs/calling.md: run V.42 when
calling (our CM always names LAPM), and the end codes of a far-end
hang-up; docs/answering.md: a caller that never sends CJ is served after
5 s of JM; DESIGN.md: the implementation-defined behaviours assumed
(arithmetic right shift of a negative value, wrapping conversion to a
signed type); docs/limits.md: "Bounds and clocks", the 0.6.0 costs.

## 0.5.0 (2026-11-21)

K56flex, Rockwell's and Lucent's 56k scheme of 1997, on the answering
side, as its server: experimental, and off unless asked for. With it off,
the answering modem's octets are 0.4.0's on all 106 recorded calls (the
replay byte-identical with V.92 and x2 each on and off), and 0.4.0's tests
pass. The x2 and V.42 fixes below change behaviour where they apply: an
x2 client's Tone A now ends the call; V.42's receiver takes a frame whose
opening flag was lost after a run of ones, on every call with error
control; V.42's transmitted bits change only for a program that calls the
new `v90m_ec_line_stopped`.

What has been shown: K56flex (answering, experimental, off by default)
connects to Motorola's SM56 soft modem run under emulation, V.42 data both
ways at 32000 to 52000 bit/s down and 31200 up, with no pad and with
robbed bits; its decoder takes our data at every rate, pad and robbed-bit
pattern; not yet tried on a Rockwell or Lucent modem.

Added (answering role):
- K56flex (`V90M_MOD_K56`, with `V90M_MOD_V34`, not in the defaults),
  offered on a mu-law line in V.8bis before V.8: at least 400 ms of
  silence after answering, then CRe; a caller that answers with CRd gets
  our CL with the K56flex block (the CL a real K56flex server sent in a
  recorded call), and one whose MS carries the block gets ACK(1): K56flex
  is agreed, and x2 (if offered too) is not offered in that call. V.8
  follows in every case and decides as without K56flex: a caller whose CM
  offers PCM while V.90 is offered gets V.90.
- An agreed client that asks for K56flex in its INFO1c, with the upstream
  at 3200 symbols a second, gets INFO1a flagged for K56flex (the point of
  no return), then K56flex's training and data: PCM downstream at the
  rate the client asks for, 32000 to 56000 bit/s in steps of 2000 (capped
  by `max_down_bps`), behind no pad, 3 dB or 6 dB, with robbed bits as the
  client reports them; V.34 upstream as our MP asks it (16 states, linear,
  minimum shaping), up to 33600 (capped by `max_up_bps`). Any other agreed
  call is V.34, as without K56flex.
- The client's rate renegotiations and retrains are followed; we start
  neither.
- The ring lists and constellations of every rate and pad are included
  data (`modem/k56tab.c`, about 1.9 KB, generated by
  `tools/gen_k56_tables.py` from the values D-Modem published from
  Motorola's SM56 driver; no rule we tried reproduces them).
  `docs/limits.md`, "Included tables (K56flex)".
- Tools: `v90m-replay -k` (K56flex offered), `make crd-scan RECS=<dir>`
  (the CRd detector over recordings), `make test-k56-sm56 DMODEM=<dir>`
  (Motorola's SM56 under D-Modem's harness, kept outside the repository,
  as the caller), `make bench-k56`, `make cost-k56`, `make k56-dump`.

API changes:
- `V90M_MOD_K56` (0x0200): answering only (calling: `V90M_ENOTSUP`), and
  only with `V90M_MOD_V34` (without: `V90M_ENOTSUP`). Not offered with
  `alaw`.
- `V90M_CONN_K56FLEX` (appended to `v90m_conn`): `tx_bps` is the
  downstream payload rate, 32000 to 56000 (a Rockwell modem prints the
  same figure), `rx_bps` the client's V.34 upstream.
- `V90M_END_K56_FAILED` (appended to `v90m_end`): the client took up
  K56flex (our INFO1a flagged) and its start-up, a rate renegotiation or
  a retrain of the client's did not complete, or it retrained a third
  time in a row. There is no way back to V.34 after our flagged INFO1a.
- In K56flex `v90m_retrain` gives `V90M_ESTATE`; `v90m_cleardown` ends the
  call at once, with nothing sent (`V90M_END_CLEARDOWN_OURS`). The client's
  renegotiation gives `V90M_EV_DATA_STOPPED`, `V90M_EV_RATES`,
  `V90M_EV_DATA_MOVING`; one asking for rate 0 ends the call as
  `V90M_END_CLEARDOWN_FAR`. The client's retrain gives
  `V90M_EV_DATA_STOPPED` with `retraining` set, then `V90M_EV_DATA_MOVING`;
  `v90m_stats.retrains` counts it. Only its tone B in data mode is
  followed: a client that retrains again during a training ends the call
  at that training's time limit (`V90M_END_K56_FAILED`).
- With `V90M_MOD_K56` set every caller hears our answer tone later than
  without it: 1.790 s for a caller without V.8bis, 3.807 s for a V.8bis
  caller without K56flex, 3.413 s for a K56flex client (measured in the
  simulator).
- `V90M_MEM_SIZE` and `v90m_mem_size()` are unchanged (118824 on x86-64,
  118512 on 32-bit ARM).

Added (error control):
- `v90m_ec_line_stopped`, for `V90M_EV_DATA_STOPPED` (the falling edge of
  `v90m_state.moving`), called within the sample it happens, as
  `v90m_ec_line_restored` now is on the rising edge. After it,
  `v90m_ec_line_restored` aborts the frame the stop cut if it is still
  going and opens the next frame with two whole flags; without it,
  `v90m_ec_line_restored` only polls, as before. Optional.
  `v90m_ec_mem_size()` is 32 octets more (49792 on x86-64);
  `V90M_EC_MEM_SIZE` is unchanged.

Fixed (found on a live x2 call: a U.S. Robotics 56K FAX EXT, flash 5.1.4,
x2 at 56000 / 28800 with PPP, 24 of 255 blocks resent; confirmed on a
second live call with the fixes: 3 min 50 s, two rate changes up, 4 of
293 resent, link NAKs 12 -> 2, pings 90 of 90, worst 590 ms against
2.9 s):
- V.42 receiver: a frame whose opening flag was lost after a run of ones
  (seven or more) is taken when its FCS holds. A U.S. Robotics x2 client
  resumes that way after its rate change; the frame was dropped, and five
  frames were resent each time after the client's poll, some 2.6 s later.
- x2: the client's Tone A in data mode or its rate change (a retrain it
  asks for, which x2 cannot serve) ends the call with
  `V90M_END_RETRAIN_FAILED`, within some 70 ms in the replay of the first
  call, instead of the watchdog's `V90M_END_LOST` many seconds later. The
  confirming call had no retrain, so this is tested by replay only.
- `v90m-replay` decodes x2 data mode both ways, its echo canceller fed the
  recorded transmit (built with the trace hooks only; the library is
  unchanged by it), and prints each change of the modem's `connected`,
  `moving` and `retraining`.

Limits (`docs/limits.md`, "K56flex (answering)"): offered only behind
`V90M_MOD_K56`; no A-law K56flex; we never renegotiate or retrain
ourselves, so a degrading upstream ends the call at the watchdog
(`V90M_END_LOST`); the tables of Lucent clients are assumed to be the
same and are unverified. K56flex data mode costs about what V.90's does
(39.0 M ARM instructions a second at 56000 against V.90 answering's 38.9 M,
`make cost-k56`): it asks the client for V.90's upstream options.

## 0.4.0 (2026-11-20)

x2, U.S. Robotics' 56k scheme of 1997, on the answering side, as its
server. Off unless asked for. With it off, the answering role is as in
0.3.0; the calling role's V.8 receiver now also accepts a JM whose carrier
is inverted after each repeat (see "Changed (calling role)").

What has been shown: at 32000 bit/s (rate index 1, the start-up rate) x2 is
decoded by the emulated U.S. Robotics Courier, V.42 data acknowledged.
Above 32000 it is octet-identical to the U.S. Robotics I-modem and D-Modem's
server; the published courier-emu decodes no server above index 1; not yet
confirmed on a real modem. A real call from a U.S. Robotics modem decides
the rates above 32000.

Added (answering role):
- x2 (`V90M_MOD_X2`, with `V90M_MOD_V34`, not in the defaults): offered to
  a caller whose V.8 CM offers V.34 and no PCM, on a mu-law line. A U.S.
  Robotics x2 client that takes it up (its marker after our L1) gets x2:
  PCM downstream at the rate it asks for, 32000 to 56000 bit/s (all 15
  rate indices), behind no pad, 3 dB or 6 dB, and its V.34 upstream (up to
  28800). The client's rate changes in data mode are followed. Any other
  caller with x2 offered still gets V.34, but sees an x2 server's JM (the
  carrier inverted after each JM) and INFO0a (max_sr_diff 4, cme 1), as from
  a U.S. Robotics server. Tried in simulation and on recordings; not yet on
  a live call from a non-x2 V.34 modem. A caller whose V.8 receiver cannot
  take the inverted JM would fail V.8 (this library's 0.3.0 calling side
  did).
- The level banks of every rate and pad are included data
  (`modem/x2bank.c`, about 4.5 KB, generated by `tools/gen_x2_banks.py`
  from the values D-Modem published from the Quad x2 firmware; no rule we
  tried reproduces them). `docs/limits.md`, "Included tables (x2)".

API changes:
- `V90M_MOD_X2` (0x0100): answering only (calling: `V90M_ENOTSUP`), and
  only with `V90M_MOD_V34` (without: `V90M_ENOTSUP`).
- `V90M_CONN_X2` (appended to `v90m_conn`): `tx_bps` is the downstream
  payload rate, `rx_bps` the client's V.34 upstream.
- `V90M_END_X2_FAILED` (appended to `v90m_end`): the client took up x2 and
  its start-up, or one of its rate changes, did not complete. There is no
  way back to V.34 after the client's marker.
- `v90m_retrain` gives `V90M_ESTATE` in x2; `v90m_cleardown` ends an x2
  call at once.
- `V90M_MEM_SIZE` is unchanged (120 KiB); `v90m_mem_size()` is 8 octets
  more (118824 on x86-64, 118512 on ARMv5).

Limits (`docs/limits.md`): no A-law x2 (not offered with `alaw`), robbed-bit
lines not handled, no x2 calling side; the free sign of data frames differs
from the I-modem's (believed not checked by the client, unproven above
index 1); behind 3 dB the octets `dc` / `ed` are sent for the levels `e2` /
`f1`, as the Quad and the I-modem send them. x2 data mode costs about twice
V.90's CPU (82 to 84 M ARM instructions a second against 38.9 M): one x2
call per ARM926 box. K56flex is still not supported.

Changed (calling role):
- The V.8 receiver no longer reads the 180-degree phase flip between an
  x2 server's JMs as a character (an FF octet is taken as mark; no V.8
  octet is FF), which lost every JM after the first. Covered by the
  existing calling tests; not tried against an x2-offering answerer.

## 0.3.0 (2026-11-19)

The calling side gains V.92 and Bell, retrains and rate renegotiation in
V.90 / V.92, and fixes from comparing it with real callers' recordings.
Still simulation and own answerer only, all of it.

Added (calling role):
- V.92 as the analogue modem (`V90M_MOD_V92` with `V90M_MOD_V90`): PCM
  upstream when the answerer offers it in Phase 2, V.90 otherwise; rate
  renegotiation begun by the answerer; cleardown either way.
- Bell 103 and Bell 212A (`V90M_MOD_BELL103`, `V90M_MOD_BELL212A`, not in
  the defaults): on the 2225 Hz Bell answer tone, or on a carrier in the
  Bell answer channel when only Bell is offered; Bell 212A falls back to
  Bell 103 when no 1200 bit/s answer comes. The 212A handshake follows
  V.22; it was not checked against a Bell 212A specification. V.8 never
  falls back to Bell.
- V.90 and V.92: retrains begun by either end (`v90m_retrain`, or the
  answerer's Tone B) and rate renegotiation begun by either end; the
  modem begins one of its own when its receiver's error moves far from
  what the rate was chosen for, and a retrain when the line's level moved
  or the receiver is lost. Three retrains without a minute of data mode
  between them end the call.

API changes:
- `V90M_ROLE_ORIGINATE` accepts `V90M_MOD_V92` (only with
  `V90M_MOD_V90`: `V90M_ENOTSUP` without it), `V90M_MOD_BELL103` and
  `V90M_MOD_BELL212A`; nothing is refused with `V90M_ENOTSUP` any more but
  V.92 without V.90.
- `V90M_END_RETRAIN_FAILED` (appended to `v90m_end`): a calling V.90 /
  V.92 retrain failed, or one more was due past the limit.
- `v90m_retrain`: in the calling role also V.90 and V.92.
- `v90m_reset(m, NULL)` returns `V90M_EINVAL` (it crashed before).
- **Changed:** `v90m_ec_send_break` returns `V90M_OK` or `V90M_ESTATE`
  (it returned 0 or -1, and -1 is `V90M_EINVAL`). A caller testing for
  0 is unaffected; one testing for -1 must test for `V90M_ESTATE`.
- **Changed:** the answering role now refuses what the calling role
  refused: no modulations at all (`V90M_EINVAL`), and `V90M_MOD_V92`
  without `V90M_MOD_V90` (`V90M_ENOTSUP`). Before, it accepted both.
- `V90M_MEM_SIZE` 112 KiB -> 120 KiB (the calling side's receiver).
- The static library's internal symbols are renamed `v90m__i_*` instead
  of made local (some targets' relocations need them global); a program
  must not define names that begin `v90m__i_`. Still only the `v90m_*`
  functions are meant for use.
- The header's comments now say, per field and function, which role it
  applies to and every error return.

Fixed (calling role):
- The CM now carries the PSTN access category whenever it carries the PCM
  one, as every real V.90 / V.92 caller's CM does (V.8 6.3).
- The first JM after ANSam was lost (its receiver read part of the answer
  tone as a character); it is read now (V.8 8.1.2).
- V.90: data mode collapsed in some calls (the DIL's order, the band edge's
  image in the equaliser design, a stale channel estimate after a hold);
  the DIL's levels were snapped with the wrong gain behind a pad; the V.92
  timing loop moved during the DIL.
- V.32bis: S was taken on the end of the answerer's AC, and our TRN was
  too short for the answerer to choose the rate by.
- The calling side's heavy computations (the equaliser design, the DIL
  analysis, the CPt / CP designs) are spread over many samples: the
  heaviest single sample on ARMv5 went from 9.3 M to under 100 k
  instructions, V.92 data mode from 114 M to 74 M a second.

Fixed (answering role):
- V.92: bursts of upstream errors at 34666 to 37333 bit/s with a G.711
  codec on the receive path (the receiver's baseline follower).
- V.92: PCM upstream was dropped for the rest of the call after a retrain
  the answering modem began by itself.
- Phase 2's start and the end of probing cost half what they did on ARM
  (the same output).

Build and tests:
- `examples/call_loop.c`: a calling modem and an answering modem back to
  back through the public API; built by `make`, run by `make check`.
- `make check-ubsan`; `make check-arm` runs its programs in parallel;
  `tools/check-ports.sh`, the port matrix (16 targets, gcc and clang,
  little- and big-endian, 32- and 64-bit, win32): every target but m68k
  (an emulator fault) prints what the host prints.
- `CHANGELOG.md`.

## 0.2.0 (2026-11-17)

The calling role, from the moment the far end has answered (dialling and
call progress stay the program's). Simulation and own answerer only, all
of it. Bell on the answering side.

Added (calling role):
- V.8: CI (optional), CM / JM / CJ; an answer tone detector that tells
  ANSam from ANS.
- V.21, V.22 and V.22bis, after V.8 or after a pre-V.8 answer tone; V.22bis
  retrain.
- V.32bis and V.32: start-up (also Annex A after a pre-V.8 answer tone),
  retrain, the rate renegotiation the answerer asks for.
- V.34: Phase 2 with our own INFO1c decision, retrain, rate renegotiation,
  cleardown.
- V.90 as the analogue modem: the PCM downstream receiver, the DIL
  measured (pads, robbed bits, the law), CP designed, data both ways; the
  answerer's rate renegotiation followed; cleardown either way. No retrain
  yet.

Added (answering role):
- Bell 103 (`V90M_MOD_BELL103`, not in the defaults): a caller without
  V.8 that sends 1270 Hz gets 300 bit/s FSK. Tried with a real modem.
- Bell 212A (`V90M_MOD_BELL212A` with `V90M_MOD_V22`, not in the
  defaults): 2225 Hz after 4 s of an unanswered V.22bis answer sequence; a
  caller that then starts as V.22 does is a Bell 212A call at 1200 bit/s.
  Tried only with a modem that would also have taken V.22.

API changes:
- `V90M_ROLE_ORIGINATE` works (it gave `V90M_ENOTSUP` in 0.1.0) for V.21,
  V.22, V.22bis, V.32bis, V.34 and V.90; V.92 and Bell there still gave
  `V90M_ENOTSUP`.
- `v90m_cfg.send_ci` (calling: send CI before the answer tone), placed
  before `user`: the struct's layout changed.
- `V90M_END_NO_ANSWER_TONE`, `V90M_END_NO_JM`, `V90M_END_NO_COMMON`
  (appended, calling only).
- `V90M_MOD_BELL103` (0x0040), `V90M_MOD_BELL212A` (0x0080);
  `V90M_CONN_BELL103` = 8 and `V90M_CONN_BELL212A` = 9 (appended).
- The rate ceilings mean what we send (`max_down_bps`) and what we receive
  (`max_up_bps`) in either role; V.32bis takes the lower of the two both
  ways.
- `v90m_retrain` and `v90m_cleardown` in the calling role (V.22bis at 2400
  bit/s, V.32bis, V.34; cleardown in V.34 and V.90).
- `V90M_MEM_SIZE` 100 KiB -> 112 KiB.

Changed (answering role):
- The echo delay measurement and the retrain tone watch are one shared
  piece each (from v90d 0.4.1); the echo delay is looked for over a longer
  history.

## 0.1.0 (2026-11-16)

The answering modem of v90d as a library of its own, with a public API.

- Answering: V.8; V.92 and V.90 as the digital (server) side; V.34;
  V.32bis and V.32; V.22bis and V.22; V.21; callers without V.8 (the
  V.22bis answer sequence, V.32bis AA, V.21 after 8 s). Retrains in V.90,
  V.92, V.34 and V.32bis; rate renegotiation; cleardown in V.90, V.92 and
  V.34. Tried against real
  modems (a Conexant CX93001, a U.S. Robotics 5686 and a U.S. Robotics
  56K of 1998).
- V.14 start-stop framing and V.42 error control (LAPM, detection phase,
  start-stop fallback, breaks) with V.42bis and V.44 compression, as
  objects of their own on the modem's bit pump.
- API: create, or init in the caller's memory (`V90M_MEM_SIZE`, 100 KiB),
  reset, destroy; a configuration struct (role, modulations, law, linear
  receive, transmit level, power and rate ceilings, echo delay); block
  calls for G.711 codewords, a linear receive side, or linear both ways,
  any block length; events and a state poll; stats. The calling role gave
  `V90M_ENOTSUP`.
- Static and shared library, pkg-config file, `make install`; the
  `v90m-replay` tool that plays a recorded call into the modem; seven
  recordings of real calls as test fixtures.
