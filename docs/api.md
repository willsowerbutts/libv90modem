# API reference

The authority is [`include/v90modem.h`](../include/v90modem.h): every
field and function is commented there with the roles it applies to and
its error returns. This page groups the same content by topic and adds
nothing the header does not say, except where it names another source.
The API is not stable before 1.0; any 0.x version may change it
(`CHANGELOG.md`).

Rules the library keeps: C99, fixed-point arithmetic in the signal path,
no memory allocation after `v90m_create()` / `v90m_ec_create()`, no global
state (any number of modems in one process, one thread per object at a
time). Callbacks (`getbit`, `putbit`, `on_event`) are only ever made from
inside `v90m_process_*()` on the calling thread.

What a callback may do to the modem that calls it: `v90m_retrain`,
`v90m_cleardown`, `v90m_get_state`, `v90m_get_stats` (and anything on
another object: every `v90m_ec_*` and `v90m_async_*` function).
`v90m_reset` returns `V90M_ESTATE` and changes nothing. `v90m_destroy`
takes effect at the end of the sample under way: no event after it, the
rest of the block is silence (linear 0, mu-law 0xFF, A-law 0xD5), the
modem is freed (when it came from `v90m_create`) and `v90m_process_*`
returns `V90M_OK`. After a `v90m_destroy` from `getbit` or `putbit` the
rest of that sample still calls `getbit` and `putbit` with the host's
`user` pointer: free the user object (a `v90m_ec_t`, a `v90m_async_t`)
only after `v90m_process_*` returns, and never from a callback of a modem
that goes on. `v90m_init` on the modem's own memory is not allowed.

NULL: every function checks the object it is given (and the struct that
`v90m_get_state`, `v90m_get_stats` and `v90m_ec_get_status` fill): with
NULL a function returning `int` returns `V90M_EINVAL`, one returning
`size_t` or `unsigned long` 0, a `void` one does nothing. Except the four
called for every bit, `v90m_ec_tx_bit`, `v90m_ec_rx_bit`,
`v90m_async_getbit` and `v90m_async_putbit`: their object must not be
NULL. `V90M_EINVAL` is -1, which is true in C: the functions read as yes
or no, `v90m_ec_tx_idle`, `v90m_ec_tx_busy` and `v90m_ec_get_break`, say
yes for NULL, and a loop `while (v90m_ec_get_break(e, &o, &l))` with a
NULL `e` never ends and leaves `o` and `l` unset. Compare with 1, or check
the object first.

## Version

| | |
|---|---|
| `V90M_VERSION_MAJOR`, `V90M_VERSION_MINOR`, `V90M_VERSION_PATCH` | the header's version: 0, 6, 0 |
| `const char *v90m_version(void)` | "0.6.0": the version of the library linked in (may differ from the header) |

## Error codes

Negative returns.

| | value | meaning |
|---|---|---|
| `V90M_OK` | 0 | |
| `V90M_EINVAL` | -1 | a NULL or misaligned argument, or a configuration value out of range |
| `V90M_ENOTSUP` | -2 | a combination not implemented in this version: V.92 without V.90; x2 or K56flex without V.34, or calling; answering Bell 212A without V.22; calling, no modulation the calling role has |
| `V90M_ENOMEM` | -3 | create could not allocate, or init was given too little memory |
| `V90M_ESTATE` | -4 | not possible in the present state of the call |

## Lifecycle

| | |
|---|---|
| `void v90m_cfg_default(v90m_cfg *cfg)` | answering, the default modulations, `tx_level` 3000, `echo_delay` -1, the rest 0 or NULL |
| `v90m_t *v90m_create(const v90m_cfg *cfg, int *err)` | one modem; the only allocation the library makes for it. NULL on failure with `*err` (`err` may be NULL): `V90M_EINVAL` (cfg NULL or a value out of range), `V90M_ENOTSUP`, `V90M_ENOMEM`; `V90M_OK` otherwise |
| `size_t v90m_mem_size(void)` | octets `v90m_init` needs |
| `v90m_t *v90m_init(void *mem, size_t len, const v90m_cfg *cfg, int *err)` | the modem in memory of the caller's, at least `v90m_mem_size()` octets, aligned for any type; nothing is allocated, and `v90m_destroy()` then does nothing. Errors as `v90m_create`, and `V90M_EINVAL` for mem NULL or misaligned, `V90M_ENOMEM` for len too small |
| `int v90m_reset(v90m_t *m, const v90m_cfg *cfg)` | back to the start of a new call (the line went off-hook), same memory, the role and everything else from cfg. `V90M_OK`; or `V90M_EINVAL` (m or cfg NULL, a value out of range), `V90M_ENOTSUP`, or `V90M_ESTATE` (from a callback of m's), the modem left as it was |
| `void v90m_destroy(v90m_t *m)` | NULL is fine; from a callback it takes effect at the end of the sample under way (above) |

Memory bounds for this version, on every platform (they change with the
version; the init functions check the length given):

| | |
|---|---|
| `V90M_MEM_SIZE` | 108 KiB, for `v90m_init` |
| `V90M_EC_MEM_SIZE` | 50 KiB, for `v90m_ec_init` |
| `V90M_ASYNC_MEM_SIZE` | 2048, for `v90m_async_init` |
| `v90m_align` | a union for the alignment: `union { v90m_align a; uint8_t b[V90M_MEM_SIZE]; } mem;` |

## Configuration: `v90m_cfg`

| field | |
|---|---|
| `role` | `V90M_ROLE_ANSWER` (0, the called modem) or `V90M_ROLE_ORIGINATE` (1, the calling modem) |
| `modulations` | `V90M_MOD_*`; default all but V.92, Bell, x2 and K56flex. Either role: at least one (0 gives `V90M_EINVAL`), and V.92 only with V.90 (else `V90M_ENOTSUP`); x2 and K56flex answering only and with V.34 (else `V90M_ENOTSUP`) |
| `alaw` | the codec's law: 0 mu-law, 1 A-law |
| `codec_linear_rx` | answering: 1 = the codec receives in 16-bit linear, with no G.711 quantiser on the receive path (V.92 then designs the upstream constellation without its share of the noise); 0 = the received samples went through G.711, also when handed over as linear values. Calling: unused |
| `tx_level` | transmit RMS outside PCM modes, 16-bit units, 0 .. `V90M_TX_LEVEL_MAX` (16384; else `V90M_EINVAL`); default 3000. Near the top the peaks of V.34-family signals clip: a calling V.90 modem's V.34 upstream at 15360 or 16384 goes over full scale some tens of samples a call, and whether its B1 then gets through depends on the upstream rate the answerer picks. Back to back at 16384 (mu-law) the answerer picked 26400 and B1 failed up to 0.5.0; since 0.6.0's start-up it picks 24000 and the call connects. That is luck of the measurement, not headroom: 15360 is the level tested to connect |
| `max_tx_code` | answering, V.90 / V.92: the most downstream power the caller may ask for, -0.5 dBm0 less 0.5 dB per step, 1..31; 0 = 23 (-12 dBm0). Calling: unused (0..31 accepted) |
| `max_down_bps`, `max_up_bps` | rate ceilings, bit/s, >= 0; 0 = none. `max_down` caps what we send, `max_up` what we receive (named from the answering, server side), in V.34, V.90 and V.92, either role: V.92's PCM upstream (up to 48000) under `max_up` answering, `max_down` calling; 33600 holds it at 33333. V.32bis (either role): one rate both ways, under the lower of the two. V.21, V.22, V.22bis, Bell: unused. With x2 or K56flex, `max_up` 1..2399 is `V90M_EINVAL` (their V.34 upstream starts at 2400) |
| `echo_delay` | samples from a sample leaving to its echo starting back (the driver's buffering), >= 0; -1 = measure it in every start-up. Calling V.90 / V.92: never measured, used as given, 0 .. 511 (-1 taken as 0, more as 511); see [line-interface.md](line-interface.md) |
| `retrain_on_step` | V.34, either role: 1 = when what we receive falls in a step (6 dB below what it was a second or so before), retrain rather than renegotiate down, and retrain when a rate renegotiation's training fails rather than end the call. For a path whose echo can move during a call (a VoIP jitter buffer): the echo cancellers do not adapt in data mode, and only a retrain trains them again. Answering, also retrain when the SNR has stayed 6 dB or more below the call's best for 30 s. Default 0 |
| `getbit` | `int (*)(void *user)`: the next bit to send, 0 or 1 |
| `putbit` | `void (*)(void *user, int bit)`: a bit received |
| `on_event` | `void (*)(void *user, v90m_event ev)`; may be NULL: poll `v90m_get_state()` instead |
| `send_ci` | calling: send CI before the answer tone (V.8 7.1) |
| `user` | passed to `getbit`, `putbit` and `on_event` |

### Modulations: `V90M_MOD_*`

| | value | |
|---|---|---|
| `V90M_MOD_V21` | 0x0001 | |
| `V90M_MOD_V22` | 0x0002 | V.22 and V.22bis |
| `V90M_MOD_V32BIS` | 0x0004 | V.32bis and V.32 (both roles) |
| `V90M_MOD_V34` | 0x0008 | both roles |
| `V90M_MOD_V90` | 0x0010 | answering: the digital (server) side; calling: the analogue (client) side |
| `V90M_MOD_V92` | 0x0020 | with `V90M_MOD_V90` (alone: `V90M_ENOTSUP`): PCM upstream on offer (answering), or asked for (calling); not in the defaults |
| `V90M_MOD_BELL103` | 0x0040 | not in the defaults, either role |
| `V90M_MOD_BELL212A` | 0x0080 | not in the defaults, either role; answering only with `V90M_MOD_V22` (without: `V90M_ENOTSUP`) |
| `V90M_MOD_X2` | 0x0100 | answering only (calling: `V90M_ENOTSUP`), with `V90M_MOD_V34` (without: `V90M_ENOTSUP`), not in the defaults: x2 as its server, offered on a mu-law line to a caller whose CM offers V.34 and no PCM; see [answering.md](answering.md), "x2" |
| `V90M_MOD_K56` | 0x0200 | answering only (calling: `V90M_ENOTSUP`), with `V90M_MOD_V34` (without: `V90M_ENOTSUP`), not in the defaults; experimental: K56flex as its server, offered on a mu-law line in V.8bis before V.8 (every caller then hears our answer tone later: 1.790 s without V.8bis, 3.807 s for a V.8bis caller without K56flex, 3.413 s for a K56flex client); see [answering.md](answering.md), "K56flex" |

What each does in each role: [answering.md](answering.md),
[calling.md](calling.md), and the header's comments above `v90m_role` and
`V90M_MOD_BELL103`.

## Sample I/O

```c
int v90m_process_g711(v90m_t *m, const uint8_t *rx, uint8_t *tx, size_t n);
int v90m_process_lin_rx(v90m_t *m, const int16_t *rx, uint8_t *tx, size_t n);
int v90m_process_linear(v90m_t *m, const int16_t *rx, int16_t *tx, size_t n);
```

`n` received samples in, `n` samples to send out. Any `n`, 1 included: the
result does not depend on how the samples are cut into blocks. Use one of
the three for a call: `_g711` codewords both ways (the interface for a
codec, and the one V.90 / V.92 need); `_lin_rx` a receive side in 16-bit
linear (Table 1 scale: what `v90m_g711_decode` gives), codewords out;
`_linear` linear both ways (a PCM modulation then sends the linear value of
its codewords). Return `V90M_OK`, or `V90M_EINVAL` (m NULL, or rx or tx
NULL with n > 0). A `v90m_destroy` from a callback ends the block early
(above). See [line-interface.md](line-interface.md).

### G.711

| | |
|---|---|
| `uint8_t v90m_g711_encode(int alaw, int16_t linear)` | the nearest codeword |
| `int16_t v90m_g711_decode(int alaw, uint8_t codeword)` | its linear value |

On the scale of V.90 Table 1 (16-bit, the scale the modem works in);
`alaw` 0 = mu-law, 1 = A-law.

## Events and state

`v90m_event`, reported to `on_event` from inside `v90m_process_*()` right
after the sample at which the change happened, before the next one is
processed:

| | |
|---|---|
| `V90M_EV_CONNECTED` (1) | data mode reached: `v90m_state` has the modulation and rates |
| `V90M_EV_RATES` | the rates or the modulation changed (renegotiation, retrain) |
| `V90M_EV_DATA_STOPPED` | data stopped moving (retrain, rate renegotiation) |
| `V90M_EV_DATA_MOVING` | data moves again both ways (error control: say so to V.42) |
| `V90M_EV_ENDED` | the modem stopped: see `v90m_state.end` |

`void v90m_get_state(const v90m_t *m, v90m_state *st)`: cheap, fine to read
after every sample, and after the end (`end` says why).

| `v90m_state` | |
|---|---|
| `connected` | data mode reached, and kept through retrains |
| `moving` | user data moves both ways now |
| `retraining` | a retrain is under way (connected, no data moves); 0 in a rate renegotiation |
| `modulation` | `v90m_conn` in force (during a retrain: the one before) |
| `tx_bps`, `rx_bps` | our transmit / receive rate (rx = tx where symmetric) |
| `used_v8` | the far modem spoke V.8 |
| `odp_bypass` | answering, V.92 9.3.1: the caller skips V.42's ODP / ADP (start the V.42 endpoint without detection); calling: 0 |
| `end` | `v90m_end` |

### `v90m_conn` (the modulation of a connection)

`V90M_CONN_NONE` 0, `V90M_CONN_V21` 1, `V90M_CONN_V22` 2,
`V90M_CONN_V22BIS` 3, `V90M_CONN_V90` 4, `V90M_CONN_V34` 5,
`V90M_CONN_V32BIS` 6, `V90M_CONN_V92` 7, `V90M_CONN_BELL103` 8,
`V90M_CONN_BELL212A` 9 (V.22 at 1200 bit/s begun on the 2225 Hz Bell answer
tone), `V90M_CONN_X2` 10 (x2: PCM downstream, `tx_bps` 32000 to 56000;
V.34 upstream), `V90M_CONN_K56FLEX` 11 (K56flex: PCM downstream, `tx_bps`
32000 to 56000 in steps of 2000, the figure a Rockwell modem prints; V.34
upstream in `rx_bps`). The last four were appended; the values above them
stay as they were.

### `v90m_end`

| | |
|---|---|
| `V90M_END_NONE` (0) | |
| `V90M_END_FAILED` | no connection was made (start-up failed) |
| `V90M_END_LOST` | the connection was lost (any loss once connected but those below) |
| `V90M_END_CLEARDOWN_FAR` | the far modem cleared down |
| `V90M_END_CLEARDOWN_OURS` | after `v90m_cleardown()` |
| `V90M_END_NO_ANSWER_TONE` | calling only: no answer tone came |
| `V90M_END_NO_JM` | calling only: ANSam came, but no JM in answer to CM (V.8) |
| `V90M_END_NO_COMMON` | V.8 found no modulation in common with the far modem |
| `V90M_END_RETRAIN_FAILED` | V.90 / V.92, V.34 (either role) (appended): a retrain failed (in its Phase 2, 3 or 4), or one more was due past the limit (see `v90m_retrain`); answering x2: the client asked for a retrain in data mode or its rate change (its Tone A), which x2 cannot serve |
| `V90M_END_X2_FAILED` | answering (appended): the client took up x2 (its marker came) and the x2 start-up, or a rate change of the client's, did not complete |
| `V90M_END_K56_FAILED` | answering (appended): K56flex was agreed and went past the point of no return (our INFO1a flagged), and the start-up, a rate renegotiation or a retrain of the client's did not complete, or the client retrained a third time in a row |

## Stats

`void v90m_get_stats(const v90m_t *m, v90m_stats *st)`:

| `v90m_stats` | |
|---|---|
| `modulation`, `tx_bps`, `rx_bps` | as in the state |
| `snr_db10` | receiver signal to error, 0.1 dB, 0..600; -1 = none. While connected, the engine's figure, in a retrain too; where it has none, the last measured while data moved. After the end of the call, that last value (-1 only if none was measured this call), until `v90m_reset` |
| `rtd` | round-trip delay measured in the handshake, samples; -1 = none (answering: V.34, V.90, V.92, x2, K56flex; calling: V.32bis, V.34, V.90, V.92); live or kept as `snr_db10` is |
| `echo_delay` | answering V.90 / V.92 / x2 / K56flex: in use (configured or measured) by the engine running now, during a retrain or after the end the call's; samples; -1 = none, and always -1 calling |
| `retrains` | begun, this call |
| `renegotiations` | rate renegotiations that stopped the data, this call: counted when data moves again, when it becomes a retrain, or when the call ends in it; a cleardown's stop, ours or the far end's, is not one (answering: V.34, V.90, V.92, x2, K56flex; calling: V.32bis, V.34, V.90, V.92) |
| `ec_in_db10`, `ec_res_db10` | (appended, as the next two rows) answering V.34, in data mode or a rate renegotiation: received power into the echo cancellers and left after them, 0.1 dB below full scale; -1 otherwise |
| `ec_near_delay`, `ec_near_peak`, `ec_near_loss_db10` | likewise: where the near canceller's 12 ms window starts and the strongest tap in it, samples; the echo path's loss as modelled, 0.1 dB (-1 while empty) |
| `ec_far_delay`, `ec_far_peak`, `ec_far_loss_db10` | likewise, the canceller of the echo a round trip later; -1 when not in use. An echo that moves out of its window is no longer cancelled |

## Control calls

| | |
|---|---|
| `int v90m_retrain(v90m_t *m)` | ask for a retrain. Answering: in data mode of V.92, V.90, V.34, V.32bis (x2, K56flex: `V90M_ESTATE`; a K56flex client's own retrain is followed, see [answering.md](answering.md)). Calling: once connected in V.22bis at 2400 bit/s, V.32bis, V.34, V.90, V.92. `V90M_OK`, or `V90M_ESTATE` (none of those, or one under way). Calling V.90 / V.92: 70 ms of silence and Tone A, Phase 2 to data mode again (some 7 to 11 s); also during a rate renegotiation; `V90M_ESTATE` when one is under way or three have run without a minute of data mode between them. A retrain that fails, or one more due past that limit (the far end's, or one the receiver needs), ends the call with `V90M_END_RETRAIN_FAILED` (the receiver's after a cleardown, V.90 9.7 / V.92 9.11) |
| `int v90m_cleardown(v90m_t *m)` | hang up, telling the far modem where the modulation has a way to (V.90 9.7, V.92 9.11, V.34 11.7, either role): `V90M_OK` when that has begun (`V90M_EV_ENDED` follows within a fraction of a second; V.92 answering: the cleardown is a rate sequence (CPd) with data rate 0, drn = 0, and a caller that does not acknowledge it is given up to 3.5 s); `V90M_ESTATE` when there is nothing to send (V.21, V.22, V.22bis, V.32bis, Bell, or not in data mode): just stop calling `v90m_process_*`. x2 and K56flex (answering, in data mode or a rate change): `V90M_OK`, and the call ends at once with nothing sent |

## Start-stop characters (V.14 basic)

Characters (1 start, 8 data LSB first, 1 stop, mark idle) on the bit pump
of a connected modem, without error control: pass `v90m_async_getbit` and
`v90m_async_putbit` as the modem's `getbit` / `putbit` with the object as
`user`. Fixed 512-octet queues each way; a character received with the
receive queue full is dropped and not counted.

| | |
|---|---|
| `v90m_async_t *v90m_async_create(int *err)` | NULL on failure, `*err` as for `v90m_create` |
| `size_t v90m_async_mem_size(void)` | |
| `v90m_async_t *v90m_async_init(void *mem, size_t len, int *err)` | `*err` as for `v90m_init` |
| `void v90m_async_reset(v90m_async_t *a)` | |
| `void v90m_async_destroy(v90m_async_t *a)` | NULL is fine |
| `int v90m_async_getbit(void *a)`, `void v90m_async_putbit(void *a, int bit)` | the bit pump |
| `size_t v90m_async_write(v90m_async_t *a, const uint8_t *buf, size_t n)` | octets accepted |
| `size_t v90m_async_read(v90m_async_t *a, uint8_t *buf, size_t max)` | octets delivered |
| `size_t v90m_async_tx_pending(const v90m_async_t *a)` | queued or being sent |
| `unsigned long v90m_async_frame_errors(const v90m_async_t *a)` | |

## V.42 error control (LAPM)

One endpoint, on the bit pump of a connected modem: feed it the modem's
bits (`v90m_ec_tx_bit` from getbit, `v90m_ec_rx_bit` from putbit), tick it
every millisecond, and move user octets with write / read. With V.42bis or
V.44 compression when both ends agree; falls back to start-stop (no error
control) when the far end has no V.42 and fallback is on. How to use it:
[error-control.md](error-control.md).

### `v90m_ec_cfg`

| field | |
|---|---|
| `originator` | 1 = the calling end, 0 = the answering end |
| `detect` | run the detection phase (V.42 7.2.1), 0..1 |
| `fallback` | when detection fails: 1 = start-stop, 0 = disconnect |
| `t400_ms` | detection phase timer, >= 0; 0 = the default, 750 |
| `establish_ms` | answerer: give up when no SABME comes this long after detection, >= 0; 0 = never |
| `comp_policy` | `V90M_COMP_AUTO` (0) / `V90M_COMP_ALWAYS` (1) / `V90M_COMP_NEVER` (2) for our encoder |
| `lapm.use_xid` | originator: negotiate with XID before SABME, 0..1 |
| `lapm.k_tx`, `lapm.k_rx` | window, 1..32 (`k_tx`), 1..127 (`k_rx`) |
| `lapm.n401_tx`, `lapm.n401_rx` | information field, octets, 1..256 |
| `lapm.fcs32` | 0..1 |
| `lapm.v42bis_tx`, `lapm.v42bis_rx` | V.42bis wanted, our send / receive direction, 0..1 |
| `lapm.v42bis_p1`, `lapm.v42bis_p2` | codewords 512..2048, string length 6..250 |
| `lapm.v44_tx`, `lapm.v44_rx` | V.44 wanted (preferred over V.42bis), 0..1 |
| `lapm.v44_p1_tx`, `lapm.v44_p1_rx` | V.44 codewords, 256..2048 |
| `lapm.v44_p2_tx`, `lapm.v44_p2_rx` | V.44 string length, 32..255 |
| `lapm.v44_p3_tx`, `lapm.v44_p3_rx` | V.44 history, octets, 512..4096 |
| `lapm.t401_ms`, `lapm.t403_ms`, `lapm.n400` | `t401_ms` >= 1; `t403_ms` >= 0, 0 = off; `n400` >= 1 |

A value outside these ranges is `V90M_EINVAL` from create, init and reset.

### Functions

| | |
|---|---|
| `void v90m_ec_cfg_default(v90m_ec_cfg *cfg, int originator)` | |
| `v90m_ec_t *v90m_ec_create(const v90m_ec_cfg *cfg, int *err)` | NULL on failure, `*err` as for `v90m_create` (`V90M_EINVAL` for a value outside the ranges above) |
| `size_t v90m_ec_mem_size(void)` | |
| `v90m_ec_t *v90m_ec_init(void *mem, size_t len, const v90m_ec_cfg *cfg, int *err)` | `*err` as for `v90m_init` |
| `int v90m_ec_reset(v90m_ec_t *e, const v90m_ec_cfg *cfg)` | `V90M_OK`, or `V90M_EINVAL` for e or cfg NULL or a value outside the ranges, the endpoint left as it was |
| `void v90m_ec_destroy(v90m_ec_t *e)` | NULL is fine |
| `int v90m_ec_tx_bit(v90m_ec_t *e)` | from the modem's getbit |
| `void v90m_ec_rx_bit(v90m_ec_t *e, int bit)` | from the modem's putbit |
| `void v90m_ec_tick(v90m_ec_t *e, unsigned ms)` | timers |
| `void v90m_ec_line_stopped(v90m_ec_t *e)` | on `V90M_EV_DATA_STOPPED`: the falling edge of `v90m_state.moving` (1 to 0), within the sample it happens (from the event callback, or a `v90m_get_state` after each sample). Notes the frame in flight and begins no new frame until the line is back. Optional |
| `void v90m_ec_line_restored(v90m_ec_t *e)` | on `V90M_EV_DATA_MOVING`: the rising edge of `moving` (0 to 1), within the sample it happens. After `v90m_ec_line_stopped`: aborts the frame the stop cut if it is still going, and opens the next frame with two whole flags; in any case LAPM polls when frames are unacknowledged |
| `size_t v90m_ec_write(v90m_ec_t *e, const uint8_t *data, size_t len)` | |
| `size_t v90m_ec_write_space(const v90m_ec_t *e)` | |
| `size_t v90m_ec_read(v90m_ec_t *e, uint8_t *data, size_t max)` | |
| `size_t v90m_ec_read_avail(const v90m_ec_t *e)` | |
| `int v90m_ec_tx_idle(const v90m_ec_t *e)` | everything written was acknowledged |
| `int v90m_ec_tx_busy(const v90m_ec_t *e)` | a frame is still leaving the transmitter |
| `void v90m_ec_release(v90m_ec_t *e)` | orderly: DISC, wait for UA |
| `void v90m_ec_abort(v90m_ec_t *e)` | the line is gone: stop without a word, in every state; `v90m_ec_state` is `V90M_EC_DISCONNECTED` (reason `V90M_EC_R_LOCAL`) on return |
| `int v90m_ec_send_break(v90m_ec_t *e, int opt, int len10ms)` | V.42 12.3.1; `V90M_OK` when the break is on its way; `V90M_EINVAL` for an `opt` other than the three `V90M_BRK_*`; `V90M_ESTATE` when error control is not up (`V90M_EC_CONNECTED`) or a break of ours is still pending |
| `int v90m_ec_get_break(v90m_ec_t *e, int *opt, int *len10ms)` | 1 with `*opt` and `*len10ms` when a break came (in its place among the octets read), else 0 |
| `int v90m_ec_state(const v90m_ec_t *e)` | `V90M_EC_DETECT` ... |
| `void v90m_ec_get_status(const v90m_ec_t *e, v90m_ec_status *st)` | |

Break options: `V90M_BRK_NONDESTRUCTIVE_INSEQ` (0x00),
`V90M_BRK_NONDESTRUCTIVE_EXPEDITED` (0x40), `V90M_BRK_DESTRUCTIVE_EXPEDITED`
(0xC0); `len10ms` is the length in 10 ms units.

### Enums

- States: `V90M_EC_DETECT` (0), `V90M_EC_ESTABLISH`, `V90M_EC_CONNECTED`,
  `V90M_EC_ASYNC`, `V90M_EC_RELEASING`, `V90M_EC_DISCONNECTED`.
- Why an endpoint is `V90M_EC_DISCONNECTED` (`v90m_ec_status.reason`):
  `V90M_EC_R_NONE` (0), `V90M_EC_R_LOCAL`, `V90M_EC_R_PEER_DISC`,
  `V90M_EC_R_PEER_DM`, `V90M_EC_R_N400`, `V90M_EC_R_PROTOCOL`,
  `V90M_EC_R_NEGOTIATION`, `V90M_EC_R_NO_EC` (16), `V90M_EC_R_COMPRESSION`,
  `V90M_EC_R_NO_SABME`.
- Compression: `V90M_EC_COMP_NONE` (0), `V90M_EC_COMP_V42BIS`,
  `V90M_EC_COMP_V44`.

### `v90m_ec_status`

| field | |
|---|---|
| `state`, `reason` | as above |
| `k_tx`, `k_rx`, `n401_tx`, `n401_rx` | |
| `comp_alg` | `V90M_EC_COMP_*`, agreed |
| `comp_tx`, `comp_rx` | agreed, per direction |
| `enc_on`, `dec_on` | our encoder / decoder in use now |
| `alg_on` | `V90M_EC_COMP_*` of the encoder / decoder in use |
| `comp_n2`, `comp_n7`, `v44_n2_tx` ... `v44_n8_rx` | compression parameters |
| `fcs32` | |
| `tx_queued`, `rx_avail`, `unacked` | |
| `rx_frames`, `rx_frames_bad`, `tx_i`, `tx_i_retx`, `rx_i`, `rx_i_dropped`, `tx_rej`, `rx_rej`, `t401_expiries`, `violations` | counters |
| `comp_in`, `comp_out`, `decomp_in`, `decomp_out` | compression counters |
| `async_overruns`, `async_framing` | start-stop fallback counters |
