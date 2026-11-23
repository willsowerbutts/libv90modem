/* Answering modem: V.8 start-up, then the selected modulation.
 *   V.8 (ANSam, CM/JM/CJ) -> V.92, V.90, V.34, V.22bis / V.22 or V.21 (V.34 when
 *   V8_MOD_V34 is in the modes and the caller has no PCM mode, or V.90 is not
 *   offered; also when a V.90 caller selects V.34 at the end of V.90
 *   Phase 2, V.90 9.2.1.1.8, if V8_MOD_V34 is in the modes), or, when the caller is a
 *   pre-V.8 modem (no CM during ANSam), the legacy V.22bis answer sequence
 *   with a later fall-back to V.21.
 * V.92 (ANS_MODE_V92 next to ANS_MODE_V90): Phase 1 is V.90's and V.8 has no
 * way to say V.92 (V.92 9.1 and its NOTE), so JM is the same. Phase 2 says
 * it: INFO0d bit 27 (Table 15/V.92). If the caller's INFO0a has its bit 26
 * (Table 16), INFO1d takes the form of Table 17 with bit 70 = PCM upstream
 * on offer, and the caller's INFO1a decides (9.3/V.92): PCM upstream (Table
 * 18: Phases 3 and 4 and data mode of V.92, ../v92/v92_p34d), V.34 upstream
 * (V.90 as before) or V.34. Full start-up only: no quick connect, no short
 * Phase 2, no modem on hold.
 * x2 (ANS_MODE_X2, with V8_MOD_V34, on a mu-law line): offered to a caller
 * whose CM offers V.34 and no PCM, in its JM (modem/v8ans) and its INFO0a
 * (modem/p2ans). A client that sends the x2 marker after our L1 makes the
 * call x2 (ANS_X2: modem/x2d, PCM downstream, V.34 upstream); one that does
 * not still gets V.34, but sees that JM (carrier inverted after each JM)
 * and INFO0a, not those it would get without x2.
 * After the marker there is no way back to V.34: an x2 start-up that does
 * not reach data mode, or a client's rate change that does not get back to
 * it, ends the call with x2_failed.
 * K56flex (ANS_MODE_K56, with V8_MOD_V34, on a mu-law line): V.8bis comes
 * first (ANS_V8BIS: modem/v8bis), then V.8 as above whatever came of it. A
 * client whose MS carries the K56flex block is agreed (k56_agreed): x2 is
 * then not offered, and V.34 is told (v34ans_cfg.k56). V.34's Phase 2 then
 * flags our INFO1a when the client's INFO1c takes K56flex up and the symbol
 * rate is 3200 (modem/p2ans): the point of no return, ANS_K56: modem/k56d
 * (PCM downstream, V.34 upstream) to data mode. A K56flex start-up that
 * does not reach data mode ends the call with k56_failed; in data mode the
 * upstream's loss ends it with k56_lost, our cleardown with cleared = 2.
 * The client's retrain (its tone B in data mode) is ANS_K56RT: Phase 2 of
 * a retrain with INFO1a flagged again, then ANS_K56 from the start
 * (retraining until data mode); anything but our flagged INFO1a, or a
 * third retrain in a row (none forgotten until data mode holds 2 s), ends
 * the call with k56_failed. We never retrain K56flex.
 * One sample in, one sample out. The data interface is a bit pump: getbit is
 * called for every transmitted line bit, putbit for every received one, once
 * the connection is up. Characters travel start-stop framed in that bit stream
 * (modem/v14.h); an error-control layer can take the same pump over. */
#ifndef V90D_MODEM_ANSWER_H
#define V90D_MODEM_ANSWER_H
#include "v8ans.h"
#include "v22bis.h"
#include "v21.h"
#include "tonedet.h"
#include "async.h"
#include "p2d.h"
#include "p34d.h"
#include "v34ans.h"
#include "x2d.h"
#include "k56d.h"
#include "v8bis.h"
#include "../v32/v32.h"
#include "../v92/v92_p34d.h"

typedef enum {
    ANS_V8, ANS_V22, ANS_V21, ANS_FAILED,
    ANS_P2,              /* V.90 Phase 2 (probing / ranging) */
    ANS_P3,              /* V.90 Phases 3 and 4: training */
    ANS_V90,             /* V.90 data mode */
    ANS_RETRAIN,         /* retrain after V.90 Phase 2: 70 ms of silence, then ANS_P2 again */
    ANS_V34,             /* V.34: Phase 2 to data mode, its retrains included (modem/v34ans);
                            or V.34 from Phase 3 on as the call modem, when a V.90 caller
                            selected it in INFO1a (v34.from_v90; its retrains: ANS_RETRAIN) */
    ANS_V32,             /* V.32bis / V.32 from AC on (v32/) */
    ANS_V32_SIL,         /* a caller without V.8 sent AA during our answer tone: 75 ms of silence, then ANS_V32 */
    ANS_P3X,             /* V.92 Phases 3 and 4 with PCM upstream: training (v92/v92_p34d) */
    ANS_V92,             /* V.92 data mode, its rate renegotiations included; retrains: ANS_RETRAIN */
    ANS_X2,              /* x2: the client sent the marker; its start-up and data mode (u.x2) */
    ANS_V8BIS,           /* K56flex offered: V.8bis before V.8 (u.v8b), then ANS_V8 */
    ANS_K56,             /* K56flex: from our flagged INFO1a on (u.k56), its data mode included */
    ANS_K56RT            /* K56flex: the client's retrain (its tone B in data mode): 70 ms of
                            silence, then V.34's Phase 2 of a retrain (v34.p2, INFO1a flagged
                            again), then ANS_K56 from the start */
} ans_phase;

/* in answer_cfg.modes, next to the V8_MOD_ bits (which fit 16 bits): offer
 * V.90 as the digital modem */
#define ANS_MODE_V90 0x10000u
/* with ANS_MODE_V90: indicate V.92 in Phase 2 and offer PCM upstream to a
 * caller that indicates it too (use answer_tick_pcm, as for V.90) */
#define ANS_MODE_V92 0x20000u
/* answer a caller without V.8 that sends the Bell 103 mark (1270 Hz) in
 * Bell 103: 300 bit/s FSK like V.21, on that system's tones */
#define ANS_MODE_BELL103 0x40000u
/* with V8_MOD_V22: when the V.22bis answer sequence has gone unanswered by
 * a caller without V.8, send the Bell answer tone (2225 Hz) for a while:
 * a Bell 212A caller replies with 1200 bit/s as a V.22 caller would */
#define ANS_MODE_BELL212A 0x80000u
/* with V8_MOD_V34: offer x2 (U.S. Robotics, 1997) as its server to a caller
 * whose CM offers V.34 and no PCM; only on a mu-law line */
#define ANS_MODE_X2 0x100000u
/* with V8_MOD_V34: offer K56flex as its server in V.8bis before V.8
 * (modem/v8bis); only on a mu-law line */
#define ANS_MODE_K56 0x200000u

#define ANS_MOD_NONE   0
#define ANS_MOD_V21    1
#define ANS_MOD_V22    2     /* 1200 bit/s */
#define ANS_MOD_V22BIS 3     /* 2400 bit/s */
#define ANS_MOD_V90    4     /* PCM downstream, V.34 upstream */
#define ANS_MOD_V34    5
#define ANS_MOD_V32BIS 6     /* V.32bis or V.32: 4800 to 14400 bit/s */
#define ANS_MOD_V92    7     /* PCM both ways: 24000 to 48000 bit/s upstream */
#define ANS_MOD_BELL103 8    /* 300 bit/s, in ANS_V21 */
#define ANS_MOD_BELL212A 9   /* 1200 bit/s, in ANS_V22: V.22 begun after our 2225 Hz */
#define ANS_MOD_X2     10    /* x2: PCM downstream, V.34 upstream */
#define ANS_MOD_K56    11    /* K56flex: PCM downstream, V.34 upstream */

typedef struct {
    int           level;     /* transmit RMS, 16-bit sample units */
    unsigned      modes;     /* V8_MOD_V22 | V8_MOD_V21 ... what we offer */
    v22_getbit_fn getbit;
    v22_putbit_fn putbit;
    void         *user;
    /* bench settings; 0 in both is how it always was */
    int           rx_linear; /* the receive samples are not companded (a codec run in 16-bit
                                linear, answer_tick_lin): V.92 then designs the upstream
                                constellation without the G.711 quantiser's share of the noise */
    int           max_tx_code; /* INFO0d bits 33:37, the most power the caller may ask our
                                downstream to have: -0.5 dBm0 less 0.5 dB per step, 1..31;
                                0 = the default, ANS_MAX_TX_CODE (-12 dBm0) */
} answer_cfg;

#define ANS_MAX_TX_CODE 23

typedef struct {
    answer_cfg cfg;
    ans_phase  phase;
    int32_t    t;            /* samples in this phase */
    int        used_v8;      /* the caller spoke V.8 */
    v8ans_t    v8;
    v22bis_t   v22;
    p2d_cfg    p2cfg;
    int        echo_delay, max_down, max_up;    /* answer_v90_line */
    p2d_t      p2;
    info0_t    info0a;       /* the caller's INFO0a from V.90 Phase 2 (a retrain's has none) */
    p34d_t     p34;
    v34ans_t   v34;
    /* V.32bis, V.92, x2 and K56flex never meet in one call (V.92 needs the
     * PCM mode of V.8, x2 a CM without it and V.34 selected, K56flex the
     * V.8bis agreement, which turns x2 off; V.32 is what V.8 selects without
     * V.34 or a caller without V.8 asks for with AA, and no phase leads back
     * to V.8): they share their storage. u.v92 holds a call's V.92 engine
     * from ANS_P3X on, u.x2 the x2 engine from ANS_X2 on; u.v8b K56flex's
     * V.8bis, during ANS_V8BIS only (before any of the others), u.k56 the
     * K56flex engine from ANS_K56 on. */
    union { v32_t v32; v92d_t v92; x2d_t x2; v8bis_t v8b; k56d_t k56; } u;
    v32_aadet_t aadet;       /* 1800 Hz from the caller while the answer tone runs */
    uint8_t    pcm;          /* the codeword of this sample while V.90 runs */
    v21mod_t   fmod; v21demod_t fdem; arx_t uart;
    int        fbit, fsk_mod, via_bell; int32_t cd_run; int64_t cd_thresh;
    int16_t    bpf[128]; int bpos;   /* V.21 / Bell 103 receive band-pass history */
    const int16_t *bpf_taps;         /* v21_bpf or bell103_bpf */
    tonedet_t  bell;                 /* the Bell 103 caller's mark while we wait in ANS_V22 */
    int        connected, modulation, bitrate;
    int        rx_bitrate;   /* receive direction, where it differs (V.90 upstream) */
    int        cleared;      /* the call ended by a V.90 cleardown: 1 the caller's, 2 ours */
    int        retrains;     /* V.90 retrains begun */
    int        retraining;   /* from there until data mode is back: connected stays 1, no data moves */
    int        rt_recent;    /* retrains lately: one is forgotten per minute of data mode */
    int32_t    rt_clock;
    /* V.92 */
    int        v92_both;     /* both INFO0 sequences indicated V.92 (9.3/V.92): the Phase 2 of
                                every retrain is V.92's, and V.42 skips ODP / ADP (9.3.1/V.92) */
    int        pcm_up;       /* PCM upstream is on offer: bit 70 of the next INFO1d. Cleared
                                for the rest of the call when a V.92 start-up failed or the
                                line did not carry the lowest PCM upstream rate */
    int        v92_data;     /* this V.92 engine has reached data mode */
    /* x2 */
    int        x2_offered;   /* ANS_MODE_X2 with V.34, on a mu-law line */
    int        x2_failed;    /* the client sent the x2 marker and the x2 start-up did not
                                reach data mode, or a rate change of its own did not get
                                back to it: there is no way back to V.34, the call ended;
                                2: in data mode the client asked for a retrain (its Tone
                                A), which x2 cannot serve */
    /* K56flex */
    int        k56_offered;  /* ANS_MODE_K56 with V.34, on a mu-law line: ANS_V8BIS first */
    int        k56_agreed;   /* the client's MS carried the K56flex block (we sent ACK(1));
                                x2 is then not offered in this call */
    int        k56_failed;   /* past the point of no return, the K56flex start-up or a rate
                                change did not complete: the call ended */
    int        k56_lost;     /* K56flex data mode ended by the upstream's loss */
} answer_t;

void    answer_init(answer_t *a, const answer_cfg *cfg);
/* The same modem at the G.711 interface: one received codeword in, one out
 * (law from the V.90 configuration, mu-law by default). This is the interface
 * to use with a codec: V.90 needs its exact codewords on the line. The linear
 * answer_tick() is equivalent for everything else and returns the linear
 * value of the codeword while V.90 runs. */
uint8_t answer_tick_pcm(answer_t *a, uint8_t rx);
/* The same for a codec that receives in 16-bit linear and still sends G.711
 * levels (the box's SLIC in linear mode, fw/drv/pump.h): rx is the received
 * sample on the 16-bit scale of V.90 Table 1 (what v90_octet_linear gives for
 * a codeword, so a codec value needs no scaling), the result is the codeword
 * to send. Everything the modem receives it receives as linear samples
 * anyway; this entry only leaves out the G.711 decoder, and with it the
 * quantiser. Set cfg.rx_linear with it. */
uint8_t answer_tick_lin(answer_t *a, int16_t rx);
/* optional, between answer_init and the first tick: Phase 2 parameters other
 * than the defaults (-12 dBm0, mu-law, no I/O delay, 3200 and 3000 symbols/s) */
void    answer_v90_cfg(answer_t *a, const p2d_cfg *cfg);
/* optional, likewise (V.92, V.90 and V.34; for V.34 max_down is the ceiling of
 * what we send, max_up of what we receive): echo_delay = samples from handing a codeword to the
 * line until its echo starts to come back (a driver's buffering; see
 * p34d.h; -1 = not known, V.90 and V.92 measure it in every start-up);
 * max_down / max_up = rate ceilings in bit/s, 0 = none. The PCM upstream of
 * V.92 goes up to 48000 bit/s: a max_up of 33600 holds it at 33333. */
void    answer_v90_line(answer_t *a, int echo_delay, int max_down, int max_up);
int16_t answer_tick(answer_t *a, int16_t rx);
/* Tell the calling modem that we are hanging up, where the modulation has a
 * way to say so (V.90 9.7, V.92 9.11). Returns 0 when that has begun: the phase goes to
 * ANS_FAILED by itself within a fraction of a second, with cleared = 2.
 * Returns -1 when there is nothing to send: just stop. */
int     answer_cleardown(answer_t *a);
/* What the receiver makes of the caller's signal in data mode: signal to
 * error ratio at the decision point in units of 0.1 dB, clipped to 0..600.
 * -1 where the modulation's receiver has no such figure (V.32bis, V.21) or
 * there is no carrier. V.92: the mean square of the constellation points we
 * dictated in CPd over the decoder's noise estimate (v92_dec_quality); -1
 * while a V.92 call retrains. */
int     answer_snr_db10(const answer_t *a);
/* Round-trip delay of the connection as the handshake measured it, in
 * samples; -1 where the modulation does not measure one. */
int     answer_rtd(const answer_t *a);
/* V.34 data mode or a rate renegotiation: the echo cancellers (v34dx_echo).
 * Returns 0, or -1 otherwise. */
int     answer_v34_echo(const answer_t *a, v34dx_echo_t *e);
/* V.92, V.90 or V.34 data mode: start a retrain. Returns 0, or -1. */
int     answer_retrain(answer_t *a);
/* 1 while the modem carries user data in both directions: connected, not
 * retraining, and (V.90, V.92, V.34) not inside a rate renegotiation. */
int     answer_moving(const answer_t *a);
/* 1 when both modems indicated V.92 in Phase 2 (V.92 9.3) and LAPM in V.8:
 * the caller then skips the ODP / ADP exchange of V.42 (9.3.1/V.92). */
int     answer_odp_bypass(const answer_t *a);

#endif
