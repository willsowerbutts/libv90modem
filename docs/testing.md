# Testing

The signal path is fixed point, so the same tests print the same output on
every target; the tests check that, and they check behaviour against
simulated lines, against independent peers where there are any, and
against recordings of real calls.

## `make check`

The host suite:

- every block's own test (`test/test_*.c`, linked with the sources), each
  printing `name: N checks, 0 failures`; then `ALL TESTS PASS`;
- `test/api/test_api.c`, the public API against the static library as it
  is installed (only `v90m_*` visible), with a simulated caller from the
  development archive; `test/api/test_ec_fields.c`, the V.42 endpoint's
  field lists (`api/v90modem_fields.h`: range check, configuration in,
  defaults and status out) against the code they replaced, at and around
  every field's bounds;
- `examples/call_loop` (a caller and an answerer back to back through the
  public API);
- the real-modem fixtures through `v90m-replay` (below);
- `check-symbols`: the static library defines only `v90m_*` (its
  internals renamed `v90m__i_*`) and the compiler's helpers;
- `check-version`: the Makefile's `VERSION` is the header's
  `V90M_VERSION_*`, README's "This is X" and the `.so` names of
  [getting-started.md](getting-started.md) (`version: 0.6.0 everywhere`);
- `check-soname`: the shared library's SONAME is
  `libv90modem.so.MAJOR.MINOR` while MAJOR is 0 (`soname:
  libv90modem.so.0.6`);
- `check-headers`: every header under `api/`, `modem/`, `v34/`, `v90/`,
  `v92/`, `v32/`, `v42/` and `include/` compiles alone with only `-I. -Iinclude`
  (each includes what it uses, by a path relative to itself);
- the tests of the subdirectories `v34`, `v90`, `v92`, `v42` and `v32`.

It ends with `ALL HOST CHECKS PASS`. The simulated peers and channels are
in `sim/` (calling modems for V.8 / V.22bis, V.32bis, V.34, V.90 and V.92;
channels with loss, delay, echo, noise, clock and carrier offset; the PCM
downstream as an analogue modem's A/D has it) and `test/v90link.h` (a
whole line between the library's analogue and digital modems). They may
use knowledge a real modem does not have: they are a test reference, not
code to ship (`DESIGN.md`, 5).

## `make verify` and `make verify-phase`

Two commands prove that a change did what it says and nothing else.

**`make verify`** (`tools/verify.sh`) runs before every commit, some 6 to
7 minutes on 16 threads. Seven stages run at once, each in a build tree of
its own, each printing one line; then `VERIFY PASS (S s)`, or `VERIFY
FAILED` and the end of each failed stage's log (`build/verify/log/`):

| stage | what it proves |
|---|---|
| host | `make check`, then every program's output against its golden file (`tools/golden.sh check`) and the split programs' cases against the whole (`tools/split-check.sh`) |
| replay | the replay proof (below) |
| bench | the benches' transmit and received-bit hashes against `test/golden/bench.txt` (`tools/bench-hashes.sh`) |
| ubsan | `make check-ubsan`, the fixtures included |
| armv7hf | the short suite cross-built for armv7hf (musl) under qemu, less `tools/ports-slow.txt`, against the host's short set built with `-DTEST_FORCE_SHORT` (`tools/short-cmp.sh`) |
| bigend | the same short suite for aarch64be (big-endian, musl) against the same host set |
| fixed | `check-fixed` and `check-symbols` |

`A7` and `BE` name the armv7hf and aarch64be compilers (default: Bootlin's
under `../toolchains`), `RECS` the recordings.

**Golden files.** `test/golden/out/<dir>/<program>.txt` holds what each
test program printed (140 programs), filtered by `tools/out-filter.sh` (no
CPU or wall time, no structure sizes). `make golden` runs `make check` and
compares; `make golden-record` writes them. The rule: a golden file is
re-recorded only when a change explains it, and the commit lists each
file re-recorded and why. A change that should alter no output (a
refactor, a faster form of the same arithmetic) re-records nothing; a new
program's own golden file is not a re-record.

**The replay proof** (`tools/replay-proof.sh`, `make replay-proof`). The
106 recordings of real calls (private, never in the repository: `RECS`,
their names and hashes in `test/golden/recordings.sha256`) go through
`v90m-replay` in six flag sets (none, `-9`, `-x`, `-x -9`, `-k`, `-k -9`):
636 runs, whose trace, decided bits and exit code are hashed against
`test/golden/replay.sha256`. `check` compares, `record` writes the
manifest, `diff REF` replays with this tree and with the tool built from a
git REF and prints the start of each trace that differs: that is how a
release says which recorded calls it changes, and why
(`docs/history/audit-0.6.md`, "Replay proof against v0.5.0").
`V90M_NO_RECS=1` skips it (the CI has no recordings).

**Split programs.** On ARM the slowest programs (`SPLIT_TESTS` in the
Makefile: `test_p34`, `test_orig_v92`, `test_orig_seeds`,
`test_orig_bell`) run case by case in parallel (`prog --cases` says how
many, `prog N` runs one). `tools/split-check.sh` proves that the cases,
one after another, print what the whole program prints.

**`make verify-phase`** (`tools/verify-phase.sh`) is the release proof,
hours long: the armv5 (uClibc, the box's ABI) suite, the port matrix
(below; the whole suites on host gcc, armv5, s390x and riscv64, QUICK on
the rest, every target gating), the armv7hf short suite in full, the
benches' hashes on the host against qemu, the CRd scan of the recordings
(`make crd-scan`), and the `cost-*` counts into `build/costs.txt`; then
`VERIFY-PHASE PASS`. Its figures go into the release's history file
(`docs/history/audit-0.6.md` for 0.6.0).

## The CI

`.github/workflows/ci.yml`, on every push and pull request: host gcc and
clang (`make check`, the golden files, the bench hashes, an install and the
example built with pkg-config), `check-ubsan`, armv7hf under qemu against
the host's short set, s390x (big-endian) under qemu the same way, and a
win32 (mingw) build. Nightly, on tags and by hand: the ARMv5 suite under
qemu. The replay proof is skipped there (`V90M_NO_RECS=1`): the recordings
are private, so `make verify` on a machine that has them is the gate.

## `make check-arm`

The same tests cross-built for ARM (`ARM_CC`) and run under qemu-user
(`QEMU`, default `qemu-arm-static`), `PAR` (default 8) programs at a time
(`tools/par-run.sh`), then the API test and the subdirectories' tests.
It ends with `ALL ARM CHECKS PASS`.

`ARM_CC` must name a 32-bit ARM cross compiler that links static
programs. Its default is an `arm-linux-gcc` under `../toolchain/*/bin`
(next to the source tree), else `arm-linux-gcc` on the `PATH`; without
either, give it:

```
make check-arm ARM_CC=/path/to/arm-linux-gcc QEMU=qemu-arm-static PAR=8
```

Locally Bootlin's toolchains serve (for example
`~/toolchains/armv7-eabihf--musl--stable-2023.08-1/bin/arm-linux-gcc`, or
the armv5-eabi uClibc one for the box's ARMv5); the CI's ARMv5 job uses
Debian's `arm-linux-gnueabi-gcc` (armel: ARMv5T, soft float) with
`qemu-arm-static`.

On 32-bit ARM some long tests run a shorter version (`TEST_SHORT`, set in
`test/test.h` unless `TEST_FULL` is defined), so their counts differ from
the host's. K56flex's are among them: `test_answer_k56` (fewer rates,
pads and robbed-bit patterns, one retrain), `test_k56enc` (four robbed-bit
masks against the oracle) and the API test's K56flex calls (the
renegotiation to 10 and the one with rate 0; the single retrain).

## `make check-ubsan`

The host suite (and the subdirectories') built with clang
(`UBSAN_CC`) and `-fsanitize=undefined -fno-sanitize-recover=undefined`:
a signed overflow or a shift of a negative number is what lets the same C
compute differently on another target or compiler, and any of them fails
the run. It ends with `ALL UBSAN CHECKS PASS`.

## The port matrix: `tools/check-ports.sh`

```
tools/check-ports.sh [OUT [target ...]]
```

Builds the whole suite (what `make check-arm` runs: every test, the API
test, the subdirectories' tests) for each target, runs it there
(qemu-user, wine, or the host itself), and compares each test program's
output with the reference target's, host gcc, program by program.

Targets: host gcc (the reference) and host clang; x86_64, i686, armv5
(uClibc), armv7hf, aarch64, aarch64be, mips, mipsel, ppc, ppc64, riscv64
(musl); s390x (glibc); m68k (uClibc); win32 (mingw, under wine). Big- and
little-endian, 32- and 64-bit. The cross compilers are Bootlin's
"stable-2023.08-1" toolchains under `TOOLCHAINS` (default `../toolchains`);
a target whose compiler or runner is missing is reported and skipped. The
matrix builds every target with `-DTEST_FULL`, so the whole tests run
everywhere; 32-bit x86 (i686, win32) builds with `-msse2 -mfpmath=sse`,
m68k with `-fno-omit-frame-pointer` (below).

**"Identical output"** means each program's output byte for byte, but for
the lines that measure the host and not the modem: CPU and wall time, and
the lines that print the size of structures in memory (pointer width,
alignment). One allowance: a C library whose `printf` rounds up where the
correct rounding is down (uClibc-ng, found by `tools/printf-probe.c` on the
target itself) may print such a number one unit larger in its last digit;
those lines are counted and reported, and nothing else may differ
(`tools/ports-cmp.awk`).

Settings (environment):

| | |
|---|---|
| `OUT` | per target its build tree, its log (`OUT/<target>.log`) and each program's output; default `build/ports`; not under /tmp (`TMPDIR` is set to `OUT/tmp`) |
| `PAR=4` | targets built and run at once |
| `JOBS=2` | `make -j` for each target |
| `QUICK="t1 t2 ..."` | targets that skip the slow programs of `tools/ports-slow.txt` |
| `SKIP="prog ..."` | programs no target but the reference runs |
| `COMPARE_ONLY=1` | compare what an earlier run left in `OUT`, run nothing |
| `TOOLCHAINS` | where the cross compilers are |

**m68k.** m68k-uclibc is a target like the others: its programs must print
what the host prints. Until 0.6.0 twelve programs built on the V.34
receiver core (`modem/qamrx`) failed there, which was put down to
qemu-m68k. It was the compiler: GCC 12.3 for m68k, with the frame pointer
omitted (-O1 and up), reads an incoming argument 8 bytes off around a
64-bit division (`docs/history/PROGRESS.md`, "m68k"). The two places in
the tree are written so that it compiles them right, and the matrix
builds m68k with `-fno-omit-frame-pointer` in case new code meets it.
qemu-m68k's 64-bit multiply and divide agree with a reference written
from the 68040 manual on 300,000 random operand sets.

## Other checks

- `make test-interop SPANDSP=PREFIX`: V.8 and V.22bis, both roles, against
  spandsp (host only). spandsp is a test peer, never copied.
- `make bench-v90`: a simulated V.90 call replayed through the answering
  modem on the host and under qemu, by the internals and through the
  public API (static and shared library); the transmit hashes must be
  equal. `bench-v90c`, `bench-v92c` and `bench-bell` do the same for the
  calling side, `bench-v90a` for its downstream receiver. A change that
  should not alter what the modem sends shows the same hash
  (`CONTRIBUTING.md`).
- `cost-*` and `prof-v90c` / `prof-v92c`: ARM instruction counts per
  second of line and per tick, from qemu (slow). `bench-x2` and `cost-x2`
  do the same for an x2 call of the simulated client.
- `make test-x2-courier COURIER_EMU=DIR [PAD=0|3|6] [MODS=x2]
  [STRICT_RENEG=1]`: an emulated U.S. Robotics Courier (courier-emu, kept
  outside the repository) calls the answering modem (host only;
  `tools/x2-peer/README.md`). With x2 it passes when the Courier
  acknowledges our V.42 data at 32000; `STRICT_RENEG=1` also requires it
  after the Courier's rate change, which no server passes on the published
  emulator.

## K56flex's checks

K56flex has no specification to test against, so its tests check it from
several sides, each saying what it supports (the design,
`docs/superpowers/specs/2026-11-21-k56flex-answering-design.md`,
"Verification"):

- **In `make check`:** the V.8bis messages and signals
  (`test_v8bismsg`, `test_v8bis`), the tables (`test_k56tab`), the encoder
  (`test_k56enc`), the training transmitter (`test_k56tx`), and whole
  calls against a simulated client built from the facts (`sim/k56call`,
  `test_answer_k56`, the API test): every rate, pad and robbed-bit pattern
  tried, the client's renegotiations and retrains, the time limits, the
  fallbacks to V.34 and V.90.
- **The oracles:** D-Modem's encoder and training (built outside the
  repository, `docs/history/k56-facts.md`, "Oracle") were run once; what
  they sent is kept as data in `test/data/k56enc_oracle.h` (every rate x
  pad x robbed-bit mask: a hash of 600 frames, and three frames in full)
  and `test/data/k56tx_oracle.h` (the training of 13 cases, segment by
  segment). `test_k56enc` and `test_k56tx` compare ours with them. An echo
  chamber, not an independent truth: D-Modem's server is what SM56 was
  shown to take.
- **`make crd-scan RECS=<dir>`:** the CRd detector of our V.8bis over
  every `.raw` recording in a directory (the callers' audio). Over the 106
  recordings of non-K56flex callers it must find nothing (`0 CRd
  detections`): a false CRd would delay the answer tone further.
- **`make test-k56-sm56 DMODEM=<dir> [MODS=k56|none] [RBS=<hex>]
  [DOWN=<bps>] [STRICT_RENEG=1]`:** Motorola's SM56 soft modem, run by
  D-Modem's harness (kept outside the repository, built unmodified), dials
  the answering modem through a bridge (`tools/k56-peer/`); it passes when
  both ends are in data mode and 4 KiB went each way over V.42, byte for
  byte. `RBS` robs bits toward SM56, `DOWN` caps our downstream. Host
  only, 32-bit (`glibc-devel.i686`); without `DMODEM` or the 32-bit
  toolchain it says so and exits 0. `tools/k56-peer/README.md`.
- **SM56's decoder:** `make k56-dump` writes our octets and data bits at
  every rate x pad x robbed-bit mask (2496) into `build/k56dump`; a small
  32-bit program outside the repository runs SM56's own decoder on them
  (`docs/history/k56-facts.md`, "Oracle"). All 2496 decode to the data
  sent.
- **`make bench-k56`, `make cost-k56`:** a simulated K56flex call at 56000
  (`tools/rec_k56.c`) replayed on the host and under qemu (the transmit
  hashes must be equal), and the ARM instructions a second of its data
  mode against V.90's.
- **`v90m-replay -k`:** a recording replayed with K56flex offered. V.8bis
  moves our answer tone later than the recorded caller expected, so this
  shows only that every recording completes; the proof that nothing else
  moved is the replay without `-k`, byte-identical to 0.4.0's.

## Recorded calls and the replay tool

`v90m-replay` (`make replay`, installed by `make install`) plays a
recorded call into the answering modem and reports what it made of it,
phase by phase:

```
v90m-replay [-a] [-R] [-9] [-x] [-k] [-B] [-M] [-F] [-u BPS] [-d BPS] [-l DBM0 | -r RMS] [-e SAMPLES] [-P CODE] [-s SAMPLE] [-b FILE] RECORDING
```

| | |
|---|---|
| `-a` | A-law line (mu-law otherwise) |
| `-R` | `retrain_on_step` |
| `-9` | V.92 offered (PCM upstream up to 48000 bit/s unless `-u`) |
| `-x` | x2 offered (`V90M_MOD_X2`) |
| `-k` | K56flex offered (`V90M_MOD_K56`): V.8bis first; the recording is lined up with our answer tone after it |
| `-B` | Bell 103 and Bell 212A offered too (`V90M_MOD_BELL103`, `V90M_MOD_BELL212A`), as a host may offer them (nsdiald's sidecar: `bell103`, `bell212`); without them a caller without V.8 hears no 2225 Hz from us 4 s into the V.22 wait |
| `-M` | in data mode transmit mark only (binary 1) in place of what the V.42 endpoint sends: "differs in 0" then proves the recording modem sent nothing but mark |
| `-F` | print the caller's V.42 frames, found in the bits the V.42 endpoint receives: a line each with the time, length, address, control, type (`SABME`, `UA`, `I N(S) N(R)`, `RR`, `XID`, ...) and whether its FCS (16 or 32 bits) held; their count at the end |
| `-u`, `-d BPS` | upstream / downstream rate ceilings |
| `-l DBM0` | transmit level (default -12), to 0.01 dB |
| `-r RMS` | transmit level as the rms itself, 1..16384 (`v90m_cfg.tx_level`): the level a recording's sidecar gives (nsdiald's `tx_level_rms`), which no `-l` may reach exactly; of `-l` and `-r` the last one given holds |
| `-e SAMPLES` | the echo delay, as the recording modem was told (default: measured) |
| `-P CODE` | INFO0d maximum power code, 1..31 (default 23) |
| `-s SAMPLE` | start the modem at this sample of the recording (0-based), for a recording that begins at the answer tone; times stay the recording's |
| `-b FILE` | the bits the V.90 / V.34 receiver decided (default `bits.txt`) |

The modem must be set up as the recording modem was: what it hears holds
the echo of what that modem sent. The last line counts the samples where
its transmit differs from the recording's; 0 until data mode means it was
set up right. The recording format (companded, or linear receive with the
`V90DLIN1` header) is in `tools/README.md`.

**Fixtures.** `test/fixtures/` holds seven recordings of real calls to the
answering modem, each ending 10 to 20 ms after the modem connected (the
handshake, no user data). `test/fixtures/fixtures.txt` lists for each the
replay options it was recorded with, the expected connection, the modem
and the date; `test/fixtures/check.sh` replays them and `make check` runs
it:

| file | modem | connected (down / up) |
|---|---|---|
| `cxa.raw.xz` | Conexant CX93001 (USB), `AT+PIG=0` | V.92 50666 / 25333 |
| `v34a.raw.xz` | Conexant CX93001, V.34 forced | V.34 31200 / 28800 |
| `v21a.raw.xz` | Conexant CX93001, V.21 forced | V.21 300 / 300 |
| `usr7.raw.xz` | U.S. Robotics 5686, firmware 5.4.62 | V.92 54666 / 40000 |
| `usr6.raw.xz` | U.S. Robotics 5686, firmware 5.4.62 | V.92 54666 / 32000 |
| `x2r2.raw.xz` | U.S. Robotics 56K of 1998, flash 5.1.4 | V.90 54666 / 28800 |
| `x2b.raw.xz` | U.S. Robotics 56K of 1998, flash 5.1.4 | V.34 28800 / 28800 |

A fault found on a call with a real modem comes with its recording as a
fixture and a test that fails without the fix (`CONTRIBUTING.md`).

**The calling side and the recordings.** There is no real modem for the
calling side to call yet. `tools/v90m_fields.c` (`make build/v90m-fields`,
not installed) reads what the caller of a recorded call sent, field by
field, and `test/test_realcheck.c` compares the library's calling side
with what real callers sent in 106 recorded calls: the formats of the
sequences, not how a real answerer responds to them. `tools/rec_orig.c`
records a simulated call of the library's caller in the same format, so
the same tools read it.
