/* Calling modem, from the moment the far end has answered (dialling, tones
 * and call progress are the owner's) to data mode:
 *
 *   V.8 (modem/v8orig: answer tone, CI, CM / JM / CJ) ->
 *     JM selects PCM (V.90)     -> the V.90 analogue modem (modem/v90a: Phase 2
 *                                  to data mode, cleardown; PCM downstream,
 *                                  V.34 upstream; with ORIG_MODE_V92 too, V.92
 *                                  with PCM upstream when the answerer offers
 *                                  it in Phase 2, V.90 otherwise)
 *     JM selects V.34           -> V.34 call modem (modem/v34orig: Phase 2,
 *                                  Phases 3 and 4, data, retrains, rate
 *                                  renegotiation, cleardown)
 *     JM selects V.32bis / V.32 -> V.32bis 6.1, calling modem (v32_init_call)
 *     JM selects V.22bis / V.22 -> V.22bis 6.3.1.1, calling modem (v22m)
 *     JM selects V.21           -> V.21 channel 1 out (980 / 1180 Hz),
 *                                  channel 2 in (1650 / 1850 Hz)
 *   the answer tone is ANS (a pre-V.8 answerer, V.25):
 *     V.32bis on offer: V.32bis Annex A, the calling automode modem (A.2.1):
 *       AA once ANS has been heard for 1 s, while it lasts and after it;
 *       AC with a phase reversal                  -> V.32bis 6.1
 *       the unscrambled binary 1 of V.22bis for 155 ms: AA stops; ANS
 *         had lasted more than 800 ms from the start of AA -> 456 ms of
 *         silence, then V.22bis 6.3.1.1 (A.2.1.3); otherwise its timer Tc
 *         of 3.1 s runs first, AC still welcome (A.2.1.2)
 *       neither, 1.5 s after the end of ANS, V.21 on offer: AA stops (its
 *         1800 Hz would mask channel 2), a steady channel 2 mark -> V.21;
 *         AC from then on is still answered with AA (A.2.1.1)
 *     otherwise:
 *       its unscrambled binary 1 at 2400 Hz -> V.22bis 6.3.1.1
 *       a steady mark on V.21 channel 2     -> V.21
 *     Bell on offer: the Bell answer tone (below) as well
 *
 * Bell 103 and Bell 212A (ORIG_MODE_BELL103 / ORIG_MODE_BELL212A, off unless
 * asked for): V.8 has no way to offer them, so they are what a caller does
 * when no V.8 comes about. The Bell answer tone is 2225 Hz within some 10 Hz
 * (modem/belltone); the order:
 *   1. ANSam, and a V.8 mode offered          -> V.8, as above (Bell is not
 *                                                 in CM; the answerer chooses)
 *   2. 2225 Hz before any 2100 Hz tone was told (a pre-V.8 Bell answerer),
 *      or after ANS (V.25) while waiting for a legacy answerer:
 *        212A offered                         -> Bell 212A
 *        otherwise                            -> Bell 103
 *   3. only Bell offered: ANSam or ANS is no tone to answer with CM (nor is CI
 *      sent before it); silent (ORIG_BELL) after it, listening:
 *        2225 Hz, 212A offered                -> Bell 212A
 *        2225 Hz, only 103                    -> Bell 103
 *        only 103: a carrier in the Bell 103 answer channel (2025..2250 Hz,
 *          not 2100 Hz) for 150 ms            -> Bell 103 (as the plain
 *          modems of the time: a V.22 answerer's unscrambled ones, 2250 Hz,
 *          are such a carrier)
 *        212A and 103: that carrier for 6 s without 2225 Hz -> Bell 103 (an
 *          answerer that offers 212A plays 2225 Hz after 4 s of it; the
 *          library's does)
 *   4. Bell 212A started and no 1200 bit/s answer within 3 s, 103 offered
 *                                             -> Bell 103
 * Bell 212A: V.22 at 1200 bit/s as the calling modem (the low channel, 1200 Hz
 * carrier, the V.22 scrambler), its scrambled ones from the moment the tone is
 * told (no unscrambled ones are heard, so V.22's 456 ms wait has nothing to
 * time from), data 765 ms after the answerer's scrambled ones (6.3.1.2.1 c);
 * no 2400 bit/s, no retrain. Bell 103: 300 bit/s FSK, mark 1270 / space 1070
 * Hz out, 2225 / 2025 Hz in, start-stop as V.21.
 *
 * Data: a bit pump, as in modem/answer.h. V.21 carries start-stop characters
 * (handed on as start, 8 data, stop bits); V.22bis, V.32bis and V.34 a
 * synchronous stream. One sample in, one sample out, fixed point, no allocation. */
#ifndef V90D_MODEM_ORIG_H
#define V90D_MODEM_ORIG_H
#include "v8orig.h"
#include "v22bis.h"
#include "v21.h"
#include "async.h"
#include "v34orig.h"
#include "v90a.h"
#include "belltone.h"
#include "tonedet.h"
#include "../v32/v32.h"

typedef enum {
    ORIG_V8,          /* V.8: waiting for the answer tone, CM / JM */
    ORIG_LEGACY,      /* ANS heard: waiting for V.32bis, V.22bis or V.21 from the answerer */
    ORIG_V22,         /* V.22bis / V.22, start-up and data */
    ORIG_V21,         /* V.21 */
    ORIG_FAILED,
    ORIG_V32,         /* V.32bis / V.32, start-up and data */
    ORIG_V34,         /* V.34, Phase 2 to data mode, its retrains included */
    ORIG_V90,         /* V.90 analogue modem, Phase 2 to data mode */
    ORIG_BELL         /* only Bell offered, ANSam or ANS heard: silent, waiting for a Bell answerer */
} orig_phase;

/* the modulation of a connection: the numbers of ANS_MOD_* (modem/answer.h) */
#define ORIG_MOD_NONE   0
#define ORIG_MOD_V21    1
#define ORIG_MOD_V22    2
#define ORIG_MOD_V22BIS 3
#define ORIG_MOD_V34    5
#define ORIG_MOD_V32BIS 6
#define ORIG_MOD_V90    4     /* PCM downstream (received), V.34 upstream (sent) */
#define ORIG_MOD_V92    7     /* PCM both ways (V.92 with PCM upstream) */
#define ORIG_MOD_BELL103  8   /* 300 bit/s FSK on Bell 103's tones, in ORIG_V21 */
#define ORIG_MOD_BELL212A 9   /* 1200 bit/s, in ORIG_V22: V.22 begun on the Bell answer tone */

/* orig_cfg.modes: V.90 as the analogue modem, next to the V8_MOD_* bits
 * (the number of ANS_MODE_V90) */
#define ORIG_MODE_V90 0x10000u
/* ... and V.92 next to it (PCM upstream when the answerer offers it; the
 * number of ANS_MODE_V92) */
#define ORIG_MODE_V92 0x20000u
/* ... and the Bell modes, which V.8 cannot offer (the numbers of
 * ANS_MODE_BELL103 and ANS_MODE_BELL212A): see the header */
#define ORIG_MODE_BELL103  0x40000u
#define ORIG_MODE_BELL212A 0x80000u

/* why a call ended (orig_t.end) */
#define ORIG_END_NONE       0
#define ORIG_END_NO_TONE    1   /* no answer tone */
#define ORIG_END_NO_JM      2   /* ANSam, but no JM came back to CM */
#define ORIG_END_NO_COMMON  3   /* JM: no modulation in common (or one not built yet); V.32bis: no rate */
#define ORIG_END_NO_CARRIER 4   /* the modulation's start-up failed */
#define ORIG_END_LOST       5   /* the carrier went in data mode */
#define ORIG_END_CLEARDOWN  6   /* the answerer called for a cleardown (V.32bis R3 or E; V.34 11.7) */
#define ORIG_END_HANGUP     7   /* we cleared down (orig_cleardown, V.34 11.7) */
#define ORIG_END_RETRAIN    8   /* V.90 / V.92: a retrain failed, or one more was due past the
                                   limit (V90A_MAX_RETRAINS; the receiver's then cleared down);
                                   V.34, once connected: too many retrains in a row, or a
                                   retrain that failed (its Phase 2, or past V34O_RT_P2_MS
                                   in it; its Phases 3 and 4, the carrier lost in them
                                   included) */

typedef struct {
    int           level;     /* transmit RMS, 16-bit sample units */
    unsigned      modes;     /* V8_MOD_V22 | V8_MOD_V21 ... what we offer */
    int           send_ci;   /* CI before the answer tone (V.8 7.1) */
    int           max_bps;   /* V.32bis rate ceiling, bit/s; 0 = none */
    int           max_tx_bps, max_rx_bps;   /* V.34 ceilings, what we send / receive, bit/s; 0 = none */
    int           echo_delay;   /* V.34: samples from a sample handed out until its echo starts
                                   back (a driver's buffering); -1 = find it in training */
    int           law;       /* V.34 and V.90's upstream: 1 mu-law, 2 A-law when what we send
                                passes a G.711 codec (only codec values are sent), 0 linear */
    int           v90_table1; /* tests only: V.90 ignores what its DIL measured (Table 1 levels) */
    int           retrain_on_step; /* V.34: v34dx_cfg.retrain_on_step */
    v22_getbit_fn getbit;
    v22_putbit_fn putbit;
    void         *user;
} orig_cfg;

typedef struct {
    orig_cfg   cfg;
    orig_phase phase;
    int32_t    t;            /* samples in this phase (saturates) */
    int        used_v8;      /* the answerer spoke V.8 */
    v8orig_t   v8;
    /* the engine after V.8, by phase: one at a time, so they share memory
     * (each start_* initialises its own). ORIG_LEGACY (Annex A) runs the
     * V.22bis and V.32bis callers together, and Bell 212A is V.22bis's */
    union {
        struct { v22bis_t v22; v32_t v32; } leg;   /* ORIG_LEGACY, ORIG_V22, ORIG_V32 */
        v34orig_t v34;                             /* ORIG_V34 */
        v90a_t    v90;                             /* ORIG_V90 */
    } e;
    v21mod_t   fmod; v21demod_t fdem; arx_t uart;
    int        fbit; int32_t cd_run; int64_t cd_thresh;
    int16_t    bpf[128]; int bpos;  /* V.21 channel 2 receive band-pass history */
    int32_t    mark_run;     /* ORIG_LEGACY: how long a V.21 channel 2 mark has been heard */
    /* ORIG_LEGACY with V.32bis on offer (Annex A): the answer tone, and AA */
    uint32_t   at_ph;        /* 2100 Hz detector */
    int32_t    at_i1, at_q1, at_i2, at_q2;
    int32_t    at_on, at_off;      /* samples the tone has been there / away */
    int        aa;           /* AA goes out */
    int32_t    t_aa;         /* samples since AA began (-1: not yet) */
    int32_t    ans_rest;     /* ANS heard from the start of AA to its end (-1: not ended) */
    int32_t    t_quiet;      /* samples since ANS ended (-1: not ended) */
    int32_t    tc;           /* A.2.1.2: samples since the unscrambled binary 1 was first heard (-1: not) */
    /* Bell */
    belltone_t btone;        /* the Bell answer tone, 2225 Hz */
    tonedet_t  t2100;        /* ORIG_BELL: 2100 Hz (ANSam, ANS), which is no Bell 103 carrier */
    int32_t    bell_run;     /* ORIG_BELL: samples of a carrier in the Bell 103 answer channel that is not 2100 Hz */
    int        fsk_mod;      /* ORIG_V21: ORIG_MOD_V21 or ORIG_MOD_BELL103 */
    int        via_bell;     /* ORIG_V22: begun on the Bell answer tone (Bell 212A) */
    int        b212_failed;  /* a Bell 212A start got no answer: Bell 103 from now on */
    const int16_t *bpf_taps; /* the FSK receive band-pass: v21h_bpf or bell103h_bpf */
    int        connected, modulation, bitrate;
    v34dx_stop_t v34_stop;   /* V.34: what last stopped the data, or began its training again */
    int        rx_bitrate;   /* what we receive, where it may differ (V.34); 0 = as bitrate */
    int        end;          /* ORIG_END_* once phase is ORIG_FAILED */
} orig_t;

void    orig_init(orig_t *o, const orig_cfg *cfg);
int16_t orig_tick(orig_t *o, int16_t rx);
/* 1 while user data moves both ways: connected and not retraining */
int     orig_moving(const orig_t *o);
/* the receiver's signal to error ratio in data mode, 0.1 dB (0..600), as
 * answer_snr_db10; -1 where there is none (V.21) */
int     orig_snr_db10(const orig_t *o);
/* V.22bis at 2400 bit/s (6.4), V.32bis (7.1), V.34 (11.5.1.1), V.90 (9.5.2.1)
 * or V.92 (9.7.2.1) in data mode: start a retrain. 0, or -1 (V.90 / V.92:
 * also when V90A_MAX_RETRAINS have run without a minute of data between) */
int     orig_retrain(orig_t *o);
/* 1 while a retrain runs (connected stays 1, no data moves); a V.34 rate
 * renegotiation is not one */
int     orig_retraining(const orig_t *o);
/* retrains begun this call, by either end */
int     orig_retrains(const orig_t *o);
/* V.34, V.90, V.92 data mode: rate renegotiation (V.34 11.6, V.90 9.6.2.1,
 * V.92 9.8.2.1), max_rx_bps > 0 a new ceiling for what we receive (V.90 /
 * V.92: never above the configured one; 0 keeps the ceiling and designs the
 * downstream anew on the line as it is); cleardown (11.7, 9.7, 9.11), the
 * phase goes to ORIG_FAILED by itself with ORIG_END_HANGUP. 0, or -1 (other
 * modulations have neither) */
int     orig_renegotiate(orig_t *o, int max_rx_bps);
int     orig_cleardown(orig_t *o);
/* V.32bis: the round trip measured in the start-up (NT less the answerer's
 * 64 T); V.34: Phase 2's RTDEc. Samples; -1 where there is none */
int     orig_rtd(const orig_t *o);

#endif
