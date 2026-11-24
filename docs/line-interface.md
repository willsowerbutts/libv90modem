# The line interface

The modem sits at a G.711 codec and works on samples at 8000 a second. The
program hands it what the line delivered and sends what it gives back, one
call of `v90m_process_*` per block. Everything here is in
`include/v90modem.h`; where a page says more, it says where from.

## Three ways to pass samples

| call | receive | transmit | use it when |
|---|---|---|---|
| `v90m_process_g711` | G.711 codewords | G.711 codewords | the program has the codec's codewords both ways; the interface V.90 / V.92 answering needs |
| `v90m_process_lin_rx` | 16-bit linear | G.711 codewords | the codec (or its driver) gives received samples as linear values but takes codewords to send |
| `v90m_process_linear` | 16-bit linear | 16-bit linear | linear both ways; a PCM modulation then sends the linear value of its codewords |

Use one of the three for a call. The linear scale is that of V.90 Table 1,
16-bit: what `v90m_g711_decode` gives for a codeword. `v90m_g711_encode`
and `v90m_g711_decode` convert between the two (`alaw`: 0 mu-law, 1 A-law).
0 dBm0 is an RMS of 15770 on that scale (`DESIGN.md`, 4.3).

**The law.** `v90m_cfg.alaw` is the codec's law: 0 mu-law, 1 A-law. It
applies to the codeword calls, to what the PCM modulations send, and to
the G.711 conversion around the calling modem.

**Answering, V.90 and V.92.** The digital (server) side's downstream is the
codec's exact codewords: the far modem decodes them. So the modem sends
codewords, not linear samples, wherever a PCM modulation may be offered,
and they must reach the network unchanged. Anything between the modem and
the network that changes a codeword (a gain, a transcoding, any
processing of the signal) breaks that. With `v90m_process_linear` the
modem sends the linear value of each codeword, which only works if the
codec turns it back into the same codeword.

**Answering, the receive side.** `v90m_cfg.codec_linear_rx` says whether
the received samples went through a G.711 quantiser: 1 when the codec
receives in 16-bit linear with no G.711 quantiser on the receive path; 0
when they went through G.711, also when they are handed over as linear
values. V.92 uses it when it designs the upstream constellations: with 1
it leaves out the quantiser's share of the noise. (v90d's HT802 records
linear, per `docs/history/PROGRESS.md`, answer-fix.)

**Calling, V.90 and V.92.** The analogue (client) side needs the received
samples linear as the line delivers them (`v90m_process_linear` or
`v90m_process_lin_rx`): a G.711 quantiser on our side would bury the PCM
downstream in its noise. `codec_linear_rx` is unused when calling.

## Blocks

`n` received samples in, `n` samples out, any `n` (1 included). The result
does not depend on how the samples are cut into blocks (`test/api/test_api.c`
checks it). Ten milliseconds (80 samples) is what the examples and v90d
use. The calls return `V90M_OK`, or `V90M_EINVAL` for `m` NULL, or `rx` or
`tx` NULL with `n > 0`.

Bits are made and taken inside the call: `getbit` for every bit the modem
transmits, `putbit` for every bit it receives, at the moment the bit is
needed or decided, and `on_event` right after the sample at which a state
change happened. A V.42 endpoint on the pump sees the timing it would see
sample by sample, whatever the block length.

## Clocks and timing

- **One sample in, one out.** The modem has no clock of its own: it runs
  at the rate the program feeds it. Receive and transmit must be the same
  8000 samples a second; the calling V.90 / V.92 modem assumes that its A/D
  and D/A share one clock (`modem/v90a.h`).
- **Keep up with the line.** The program must call it fast enough to keep
  the codec fed. On v90d's ARM926 at 400 MHz a design step of V.92
  answering once fell behind the line by 0.3 s at one step per sample,
  hence the paced design (`modem/answer.c`, `DESIGN.md` 4.2a). The CPU
  figures are in the README ("Memory and CPU") and in
  [limits.md](limits.md).
- **Clock offset.** The answering (digital) side runs on the codec's clock,
  which is the network's; its receivers follow the caller's clock. The
  calling V.90 / V.92 side measures its clock against the network's in
  Phase 2 and Phase 3 and follows it; the simulated lines it was tested on
  run up to +-100 ppm (`docs/history/PROGRESS.md`, C5 and C6).
- **Time-outs** are counted in samples, so line time is what counts, not
  wall time.

## Echo delay

`v90m_cfg.echo_delay` is the number of samples from a sample leaving the
modem to its echo starting back: the driver's buffering, both ways. It is
`>= 0`, or -1 (the default) for "measure it in every start-up".

- Answering V.90 / V.92, x2, K56flex: the echo canceller spans 12 ms,
  placed where the echo is; with -1 the distance is found by correlation
  during training (`DESIGN.md`, 4.2, `modem/ecdly.h`).
  `v90m_stats.echo_delay` reports the value in use. The search covers
  1280 samples (160 ms). With a round trip (Phase 2's) under 1024 samples
  (128 ms) it covers 0 to 160 ms; with a longer one it covers the round
  trip less 128 ms to the round trip plus 32 ms, where the far hybrid's
  echo comes back when the caller is behind a packet network (an ATA's or a
  VoIP gateway's jitter buffers: 302 ms on a Cisco IOS FXS port). This
  round-trip-centred search is Will Sowerbutts's `ecdly_near` (PR #1,
  V.90), extended to V.92, x2 and K56flex in 0.6.0. Its reach ends at
  about 3510 samples (439 ms); an echo later than that is not cancelled.
  The trade-off: with a round trip over 128 ms an echo earlier than the
  round trip less 128 ms (a near hybrid's, from the line interface's own
  path) is outside the search, and there is one canceller, so it is not
  cancelled. Behind a packet link (nsdiald, RTP) the digital modem has no
  hybrid of its own and the far echo is the only one; a host whose line
  interface has a hybrid of its own as well, on a long round trip, should
  give `echo_delay` explicitly only if the near echo is the stronger.
- Answering V.34: the search finds our own path's delay (0 to 160 ms) and
  a second canceller is placed a round trip later for the far echo
  (`modem/v34dx.c`; with a round trip over 160 ms, where a second search
  near it finds that echo), so both are cancelled; its reach is about
  500 ms.
- Calling V.34: -1 has it found in training (`modem/orig.h`).
- Calling V.90 / V.92: in this version the value is used as a fixed delay
  of the canceller's reference (a delay line of 512 samples, so 0 to 511);
  -1 is taken as 0 (`modem/orig.c`, `modem/v90a.c`). Set it to the
  driver's delay when it is known. `v90m_stats.echo_delay` is always -1
  when calling.

## Levels and ceilings

- `tx_level`: transmit RMS outside the PCM modes, 16-bit units, `>= 0`;
  default 3000 (about -14.4 dBm0 on the scale above). `v90m-replay`'s
  default is -12 dBm0. In the calling V.92 modem the PCM upstream is sent
  at this level as well (`DESIGN.md`, 4.2d).
- `max_tx_code` (answering, V.90 / V.92): the most downstream power the
  caller may ask for, -0.5 dBm0 less 0.5 dB per step, 1..31; 0 = 23 (-12
  dBm0, the figure `modem/answer.c` gives for the public network rules).
  Unused when calling (0..31 accepted).
- `max_down_bps` and `max_up_bps`: rate ceilings in bit/s, 0 = none.
  `max_down_bps` caps what we send, `max_up_bps` what we receive (named
  from the answering, server side), in V.34, V.90 and V.92, either role.
  V.92's PCM upstream (up to 48000) is under `max_up_bps` answering and
  `max_down_bps` calling; 33600 holds it at 33333. V.32bis (either role)
  runs one rate both ways, under the lower of the two. Unused in V.21,
  V.22, V.22bis and Bell.

## What the program does itself

The library is the modem from the moment the line is connected. The
program does the rest:

- **Hook and ringing.** Answering: create or `v90m_reset` the modem when
  the line goes off-hook; it begins with 200 ms of silence and the answer
  tone (`DESIGN.md`, 4.1). Go on-hook when the call has ended.
- **Dialling and call progress.** Calling: dial, and watch dial tone,
  ringing and busy, yourself. The calling role starts when the far end has
  answered; it then waits for the answer tone (10 s, then
  `V90M_END_NO_ANSWER_TONE`). `send_ci` sends CI before the answer tone
  (V.8 7.1).
- **The end of a call.** Stop when `v90m_state.end` is not
  `V90M_END_NONE` (or on `V90M_EV_ENDED`). To hang up, `v90m_cleardown`
  where the modulation has a cleardown signal (V.90, V.92, V.34); when it
  returns `V90M_ESTATE` there is nothing to send: just stop calling
  `v90m_process_*`. v90d ends every orderly call with a cleardown after
  the V.42 release, so the far modem reports the end at once (`DESIGN.md`,
  4.2).
- **Data.** The bit pump carries bits only; framing, error control and
  compression are separate objects ([error-control.md](error-control.md)).
  During a retrain or rate renegotiation the data stop
  (`V90M_EV_DATA_STOPPED`) and the program holds its side: v90d's driver
  holds V.42 and PPP, timers included, until data mode is back
  (`DESIGN.md`, 4.2).
- **Threads.** One thread per modem object at a time; callbacks run on the
  thread that called `v90m_process_*`. There is no global state, so any
  number of modems may run in one process.
