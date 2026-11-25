# libv90modem

A voice-band data modem in software: V.92, V.90, V.34, V.32bis, V.22bis,
V.22, V.21, Bell 212A and Bell 103, started by V.8, and U.S. Robotics' x2
and (experimental) K56flex on the answering side, with V.42 error control
(LAPM) and V.42bis / V.44 compression. It answers (for V.90 and V.92 as the
digital, server side) and calls (for V.90 and V.92 as the analogue, client
side). Portable C99, fixed-point in the signal path, no allocation after
start, no global state. It works at a G.711 codec, 8000 samples per second:
give it the samples it hears, it gives back the samples to send.

It was written for v90d, a dial-up server that runs in a Grandstream HT802
analogue telephone adapter, from the ITU-T recommendations. The answering
side has answered real modems there (see "Tried on real modems"); the
calling side has not yet called one.

**The API is not stable before version 1.0.** This is 0.6.0; what changed
from one version to the next is in `CHANGELOG.md`.

## How to use

```
make                    # build/libv90modem.a, .so, libv90modem.pc, the examples
make check              # the test suite on the host
make install PREFIX=/usr/local
cc -std=c99 prog.c $(pkg-config --cflags --libs libv90modem)
```

A complete program: an answering modem with V.92 on offer, G.711
codewords in on stdin and out on stdout, 10 ms a block; it polls the
state for the connection, rate changes and the end. Replace the `fread` /
`fwrite` with your codec.

```c
#include <stdio.h>
#include <v90modem.h>

static int  get_bit(void *user) { (void)user; return 1; }      /* the next bit to send */
static void put_bit(void *user, int bit) { (void)user; (void)bit; }   /* a bit received */

int main(void)
{
    v90m_cfg cfg;
    v90m_state st, was = { 0 };
    uint8_t rx[80], tx[80];             /* 10 ms of G.711 codewords each way */
    int err;
    v90m_t *m;

    v90m_cfg_default(&cfg);             /* answering; V.90, V.34, V.32bis, V.22bis, V.22, V.21 */
    cfg.role = V90M_ROLE_ANSWER;        /* or V90M_ROLE_ORIGINATE, see below */
    cfg.modulations |= V90M_MOD_V92;
    cfg.getbit = get_bit;
    cfg.putbit = put_bit;
    if (!(m = v90m_create(&cfg, &err))) { fprintf(stderr, "error %d\n", err); return 1; }

    while (fread(rx, 1, sizeof rx, stdin) == sizeof rx) {       /* what the line brought */
        v90m_process_g711(m, rx, tx, sizeof rx);
        fwrite(tx, 1, sizeof tx, stdout);                       /* what to send */
        fflush(stdout);
        v90m_get_state(m, &st);
        if (st.connected && !was.connected)
            fprintf(stderr, "CONNECT %d/%d, modulation %d\n", st.tx_bps, st.rx_bps, (int)st.modulation);
        else if (st.connected && (st.tx_bps != was.tx_bps || st.rx_bps != was.rx_bps))
            fprintf(stderr, "rates now %d/%d\n", st.tx_bps, st.rx_bps);
        if (st.end != V90M_END_NONE) { fprintf(stderr, "ended: %d\n", (int)st.end); break; }
        was = st;
    }
    v90m_destroy(m);
    return 0;
}
```

The data moves as bits: `getbit` and `putbit` are called from inside
`v90m_process_*` for every bit sent and received once connected (here
ones go out and what arrives is dropped).

- **Answering or calling.** `V90M_ROLE_ANSWER` is the called modem, for
  V.90 and V.92 the digital (server) side; it has answered real modems.
  `V90M_ROLE_ORIGINATE` calls once the far end has answered (dialling and
  call progress are the program's), for V.90 and V.92 as the analogue
  (client) side, and needs its received samples linear there
  (`v90m_process_linear` or `v90m_process_lin_rx`). The calling side has
  been tried in simulation and against this library's own answerer only,
  never against a real modem or server.
- **x2 and K56flex** (answering only) are off by default. To offer them,
  set `cfg.modulations |= V90M_MOD_X2` or `cfg.modulations |=
  V90M_MOD_K56` (each needs `V90M_MOD_V34`, which the default has).
  K56flex is experimental: tried against an emulated client only, and with
  it on every caller hears the answer tone up to 3.8 s later
  ([docs/answering.md](docs/answering.md), "K56flex").
- **Error control** is a separate object on the bit pump: a V.42 endpoint
  (`v90m_ec_*`: LAPM with V.42bis or V.44, start-stop fallback) fed from
  `getbit` / `putbit` and ticked every millisecond, or a start-stop framer
  (`v90m_async_*`) for a modem without error control.

The rest, with a complete answering and calling program each:
[docs/getting-started.md](docs/getting-started.md); every call and field:
[docs/api.md](docs/api.md).

## Documentation

[`docs/`](docs/README.md) has the user documentation: getting started, the
line interface, the answering and calling roles, error control, the API
reference, testing, and the known limits. `DESIGN.md` is how the modem is
built and why; `docs/history/` keeps the development logs.

## Standards

| | answering | calling | notes |
|---|---|---|---|
| V.8 | ANSam, CM / JM / CJ | CI (optional), CM / JM / CJ | without V.8: answered with the V.22bis answer sequence, V.32bis AA, V.21 after 8 s; calling after a pre-V.8 answer tone in V.22bis, V.21 or V.32bis (Annex A) |
| V.21 | yes | yes | 300 bit/s |
| V.22, V.22bis | yes | yes | 1200 / 2400 bit/s, retrain; guard tone answering |
| V.32, V.32bis | yes | yes | 4800 to 14400 bit/s, retrain, rate renegotiation |
| V.34 | yes | yes | Phase 2 to data mode, retrains, rate renegotiation, cleardown |
| V.90 | digital (server) side | analogue (client) side | PCM downstream, V.34 upstream. Both: retrains and rate renegotiation begun by either end, cleardown. Calling: the DIL measured (pads, robbed bits, the law); its receiver asks for a renegotiation or a retrain when the line changes |
| V.92 | digital side | analogue side | PCM upstream (24000 to 48000 bit/s) after V.90's start-up, retrains and rate renegotiation as V.90; falls back to V.90 when the other end does not take PCM upstream |
| Bell 103 | `V90M_MOD_BELL103`, not in the defaults | the same | 300 bit/s FSK; answering a caller without V.8 that sends 1270 Hz; calling on the 2225 Hz Bell answer tone or a carrier in the Bell answer channel |
| x2 | `V90M_MOD_X2` with V.34, not in the defaults: the server side | no | U.S. Robotics' 56k of 1997: PCM downstream 32000 to 56000 bit/s, V.34 upstream; the client's rate changes followed; mu-law only; no way back to V.34 once the client takes it up (below, and [docs/answering.md](docs/answering.md)) |
| K56flex | `V90M_MOD_K56` with V.34, not in the defaults: the server side; experimental | no | Rockwell's and Lucent's 56k of 1997, agreed in V.8bis before V.8: PCM downstream 32000 to 56000 bit/s behind no pad, 3 dB or 6 dB, with robbed bits; V.34 upstream; the client's renegotiations and retrains followed; mu-law only; no way back to V.34 once the client takes it up ([docs/answering.md](docs/answering.md)) |
| Bell 212A | `V90M_MOD_BELL212A` with V.22, not in the defaults | the same | 1200 bit/s; answering with 2225 Hz after 4 s of an unanswered V.22bis answer sequence; calling on hearing 2225 Hz (Bell 103 when no 1200 bit/s answer comes) |
| V.14 | yes | yes | start-stop characters on the synchronous bit pump |
| V.42 | yes | yes | LAPM, detection phase, start-stop fallback, breaks |
| V.42bis, V.44 | yes | yes | V.44 preferred when both ends have it |

### What has been tried where

- **Answering, against real modems:** V.92, V.90, V.34, V.32bis, V.22bis,
  V.22, V.21 and Bell 103 on the HT802 (table below). Bell 212A answering
  only on a test build with the 2225 Hz tone from the start, by a modem
  that would also have taken V.22; no modem that calls as 212A and nothing
  else has been on the line.
- **x2 (answering), one real modem:** the owner's U.S. Robotics 56K FAX EXT
  (flash 5.1.4) called the box (v90d with this library, 2026-11-21): x2 at
  56000 / 28800 with PPP, held 3 min 50 s until we hung up, two rate changes
  up, no retrain, the modem's SNR 59.7 dB; its counters 293 blocks sent, 4
  resent, 2 link NAKs, block error rate 0; pings across PPP 90 of 90
  (average 148 ms, worst 590 ms). Not shown on a live call: a client's
  retrain (it still ends the call, now as `V90M_END_RETRAIN_FAILED`; tested
  by replaying an earlier recording only), a pad or robbed bits, any other
  x2 modem. Before that call: at 32000 bit/s decoded by the emulated U.S.
  Robotics Courier, and above it octet-identical to the U.S. Robotics
  I-modem and D-Modem's server ([docs/limits.md](docs/limits.md)).
- **K56flex (answering), against Motorola's SM56 under emulation, not yet
  a real modem:** K56flex (answering, experimental, off by default)
  connects to Motorola's SM56 soft modem run under emulation, V.42 data
  both ways at 32000 to 52000 bit/s down and 31200 up, with no pad and
  with robbed bits; its decoder takes our data at every rate, pad and
  robbed-bit pattern; not yet tried on a Rockwell or Lucent modem. Its
  octets also equal D-Modem's server's at every rate, pad and robbed-bit
  pattern, and its training's segment by segment
  ([docs/limits.md](docs/limits.md)).
- **Calling, in simulation only:** every modulation of the calling side has
  run against simulated lines and this library's own answerer, never
  against a real modem or a real server. What it sends has been compared,
  field by field, with what real callers sent in 106 recorded calls
  (`test/test_realcheck.c`); that checks the sequences' formats, not how a
  real answerer responds to them.

### Not implemented, or not checked

- V.92 quick connect, short Phase 2 and modem-on-hold, either role.
- The Bell 212A calling handshake follows V.22 and what this library's
  answerer accepts: it has not been checked against a Bell 212A
  specification document. The 3 s and 6 s fallbacks to Bell 103 are this
  library's choices.
- V.8 never falls back to Bell: a caller that has heard ANSam with a V.8
  modulation on offer goes V.8, and a V.8 exchange with nothing in common
  ends the call (`V90M_END_NO_COMMON`).
- Calling against real modems and real servers: untested (above).
- A rate renegotiation of the calling side's own is not in the public API
  (the analogue V.90 / V.92 modem does one by itself when its receiver
  needs it).
- x2: answering only, off unless `V90M_MOD_X2` is set; no A-law, no
  robbed-bit lines, no calling side. K56flex: answering only, experimental,
  off unless `V90M_MOD_K56` is set; no A-law, no calling side, and we never
  renegotiate or retrain ourselves ([docs/limits.md](docs/limits.md)).
- Fax (V.17, V.29, V.27ter), V.23, half-duplex V.34.

## Building

```
make                    # build/: libv90modem.a, libv90modem.so, libv90modem.pc, the examples
make check              # the simulator suite on the host, the API test, examples/call_loop,
                        # the real-modem fixtures, check-symbols
make check-arm          # the same tests cross-built (ARM_CC) and run under qemu-arm (QEMU),
                        # PAR (8) programs at a time
make check-ubsan        # the host suite under clang -fsanitize=undefined
make install PREFIX=/usr/local      # DESTDIR works; make uninstall takes it out
make clean
```

`make install` puts `v90modem.h`, the static and shared libraries, the
`.pc` file and the `v90m-replay` tool under `$(DESTDIR)$(PREFIX)` (`LIBDIR`,
`INCDIR`, `BINDIR`, `PCDIR` override the parts). A program builds with
`pkg-config --cflags --libs libv90modem` (or `--static` for the static
library, which also needs `-lm`). The static library defines the `v90m_*`
functions and nothing else that a program could clash with: its internals
are renamed `v90m__i_*` (`make check-symbols`).

**Cross builds.** Set `CC` (and `BUILD`, to keep the objects apart); `ar`,
`ld`, `objcopy` and `nm` are taken from the same prefix:

```
make CC=arm-linux-gcc BUILD=build/arm
make check-arm ARM_CC=/path/to/arm-linux-gcc QEMU=qemu-arm-static
```

**Port matrix.** `tools/check-ports.sh [OUT [target ...]]` builds the whole
suite for each target, runs it there (qemu-user, wine or the host) and
compares every program's output with host gcc's, line for line: the signal
path is fixed point, so every target must print the same. Targets: host
gcc and clang; x86_64, i686, armv5 (uClibc), armv7hf, aarch64, aarch64be,
mips, mipsel, ppc, ppc64, riscv64 (musl); s390x (glibc); m68k (uClibc);
win32 (mingw, under wine). The cross compilers are Bootlin's
"stable-2023.08-1" toolchains under `TOOLCHAINS` (default
`../toolchains`); a target without its compiler or runner is skipped and
said so. `OUT` must not be under /tmp. `QUICK="target ..."` skips the slow
programs of `tools/ports-slow.txt` there, `SKIP="prog ..."` skips programs
on every target but the reference. Last full run
(`docs/history/PROGRESS.md`, ports-2): every target but m68k identical to
host gcc. m68k's failures were not qemu's: GCC 12.3 for m68k miscompiles
two 64-bit divisions in the library (`docs/history/PROGRESS.md`, "m68k").
They are now written so that it compiles them right, the matrix builds m68k
with `-fno-omit-frame-pointer` as well, and m68k gates like every other
target. On m68k with a gcc of your own, build with `-fno-omit-frame-pointer`.

`make test-interop SPANDSP=PREFIX` runs V.8 and V.22bis (both roles)
against spandsp (host only). `make bench-v90` replays a simulated V.90 call
through the answering modem on the host and under qemu, by the internals
and by the public API (static and shared library): the transmit hashes must
be equal. `bench-v90c`, `bench-v92c` and `bench-bell` do the same for the
calling side.

## Memory and CPU

One modem, either role, takes `v90m_mem_size()` octets, at most
`V90M_MEM_SIZE` (108 KiB) on every platform: 106392 on x86-64, 106096 on
ARMv5. The answering side's state sets it; the calling side's, whose
modulations after V.8 share memory, is some 52 KiB, so making the calling
side smaller no longer lowers `v90m_mem_size()`. A V.42 endpoint takes at
most `V90M_EC_MEM_SIZE` (50 KiB; 49792 octets on x86-64), a start-stop
framer `V90M_ASYNC_MEM_SIZE` (2 KiB; 1064 octets). All three can live in
memory of the caller's (`v90m_init`, `v90m_ec_init`, `v90m_async_init`);
nothing is allocated after that.

CPU, as ARM instructions counted under qemu (ARMv5, gcc -O2; cycles on a
real ARM926 have not been measured): the box v90d runs on, an ARM926EJ-S
at 400 MHz, runs the answering modem's V.90 data mode of 0.5.0 at 32 % of
its CPU, which makes it about 122 M counted instructions a second. 0.6.0
does the same arithmetic in fewer instructions, with output identical bit
for bit (`docs/history/audit-0.6.md`):

| | M instructions per second of line, data mode (0.5.0 in brackets) | heaviest single tick, at 0.5.0 |
|---|---|---|
| answering, V.90 | 31.19 (39.20) | 87 k |
| answering, V.34 | 33.42 (40.53) | not measured |
| answering, x2 (32000) | 56.58 (82.57) | not measured |
| answering, x2 with V.42 (our endpoint counted), 32000 / 56000: no compression; V.42bis; V.44 | 58.89 / 61.50; 69.24 / 77.19; 89.25 / 108.78 (text that compresses far beyond real traffic: a ceiling) | not measured |
| answering, K56flex (56000) | 32.12 (39.27); a client that ignores our MP 58.60 (84.43) | not measured |
| calling, V.90 (0 / 100 ppm clock offset) | 46.97 / 50.95 (55.11 / 59.31) | 98 k (start of Phase 2) |
| calling, V.92 (0 / 100 ppm) | 62.12 / 66.52 (69.95 / 74.32) | 98 k |
| calling, Bell 103 / Bell 212A | 9.88 / 4.66 (9.86 / 4.43) | 2.3 k / 2.8 k |
| V.42 endpoint, 56 kbit/s both ways: none / V.42bis / V.44 | 5.21 / 9.08 / 9.48 (before the V.42 work: 7.96 / 11.81 / 12.19) | |

The heaviest ticks were measured at 0.5.0 and not again; the changes of
0.6.0 only made ticks cheaper.

x2 costs about 1.8 times V.90 (`make cost-x2`, which measures V.90
answering the same way): the V.34 upstream the client dictates (64-state
trellis, non-linear encoding, expanded shaping). One x2 call fits the box, with V.42bis too (77.19 M at
56000); two do not (2 x 77.19 = 154.37 M of about 122 M; even without
compression 2 x 61.50 = 123.0 M). K56flex
costs about what V.90 does (`make cost-k56`), because it asks the client
in its MP for the upstream V.90 has (16 states, linear, minimum shaping);
a client that ignores that and sends x2's upstream costs about what x2
does.

The calling side's retrains and rate renegotiations stay under 98 k a tick
(a 10 ms block of the box is about 1.22 M). The measurements and how they
were made are in `docs/history/PROGRESS.md` ("cpu") and, for 0.6.0,
`docs/history/audit-0.6.md`.

## Examples

Two examples, built by `make`, use the public API only.
`examples/answer_file.c` answers a recorded call and prints the connect
line:

```
$ build/answer_file -9 -u 33600 cxa.raw
cxa.raw: 26.4 s, 16-bit linear receive; answer tone at 6.692 s
CONNECT 50666/25333 V.92 at 24.805 s; SNR 60.0 dB, round trip 263 samples
```

`examples/call_loop.c` calls this library's own answerer over a 4-wire
line in memory (`make check` runs it), sends a line of text each way and
clears down; `-m` picks what the caller offers:

```
$ build/call_loop
libv90modem 0.6.0
caller   CONNECT 40000/50666 V.92
answerer CONNECT 50666/40000 V.92
caller   got "hello from the answerer"
answerer got "hello from the caller"
caller   SNR 60.0 dB, round trip 2 samples
answerer ended: far end cleared down
caller   ended: we cleared down
line time 12.51 s
```

## Tried on real modems

Calls made to the library inside v90d on an HT802, with the modems below
calling; the figures are downstream / upstream bit/s.

| modem | firmware | date | what connected |
|---|---|---|---|
| Conexant CX93001 (USB, V.92) | | 2026-11-15 | every modulation; V.90 49333 to 53333 / 28800; V.92 50666 / 25333 (`AT+PIG=0`); V.34 31200 / 28800 |
| U.S. Robotics 5686 (56K FAX EXT, V.92) | 5.4.62 | 2026-11-16, 17 | V.90 54666 / 26400; V.92 54666 / 40000; V.22 1200; V.21 and Bell 103 300; Bell 212A 1200 (test build, above) |
| U.S. Robotics 56K FAX EXT of 1998 (x2, V.90 upgrade) | flash 5.1.4 | 2026-11-16 | V.90 54666 to 56000 / 28800; V.34 28800 when its V.90 start-up failed |
| the same | flash 5.1.4 | 2026-11-21 | x2 56000 / 28800 with PPP, 3 min 50 s, two rate changes up, 4 of 293 blocks resent (0.5.0's V.42 and x2 fixes) |

Not tried: any other modem, and the calling side against any real modem or
server. Seven of these calls are kept as fixtures (`test/fixtures/`), which
`make check` replays.

## Reporting a bug

Most faults show only with one brand of modem. Please send a recording of
the call: what the modem heard and what it sent, at the codec, in the
format of `tools/README.md` (v90d writes it with `-w FILE`). Then

```
v90m-replay [options as the modem was set up] RECORDING
```

plays it into the answering modem and prints what it made of the call,
phase by phase. Say which modem called (make, model, firmware: `ATI3`,
`ATI6`, `ATI7` on most), how the library was configured, and what happened.
A fix comes with that recording as a test fixture (`CONTRIBUTING.md`).

## Licence

GPL-2.0-only, see `LICENSE`.
