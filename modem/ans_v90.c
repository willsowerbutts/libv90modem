/* The answering modem's V.90 and V.92 schemes (modem/ans_scheme.h): Phase 2
 * and the retrain's silence before it (ans_p2: ANS_P2, ANS_RETRAIN), V.90
 * Phases 3 and 4 and data mode (ans_v90: ANS_P3, ANS_V90), V.92 with PCM
 * upstream (ans_v92: ANS_P3X, ANS_V92), and the hand-off to V.34 when the
 * caller's INFO1a selects it. */
#include <string.h>
#include "ans_scheme.h"
#include "../v90/v90_pcm.h"

#define MS(ms) ((int32_t)(ms) * 8)
/* The CPd design of a V.92 start-up takes one step every so many samples:
 * at one per sample the unit's ARM926 fell behind the line by 0.3 s in the
 * first calls with a real V.92 modem (v92/README.md, reading 56). At four
 * the design took 4.5 to 7.5 s, and a U.S. Robotics 5686 waits 3.5 s for
 * CPd after its SUVu; at two it is done in 2 to 3 s and the line driver
 * counted no late block in a call (v90d's PROGRESS.md, 2026-11-16). */
#ifndef V92_DESIGN_PACE
#define V92_DESIGN_PACE 2
#endif

/* V.90 retrains in a row: the fourth due ends the call (checked before it
 * is counted); one is forgotten per minute of data mode. V.92's and V.34's
 * after a V.90 Phase 2 are these. */
const rt_policy_t ans_rt_v90 = { 3, 60000 };

/* Jd (V.90 Table 13, V.92 Table 21): the downstream rates the ceiling allows; 0 = all */
static uint32_t jd_rates(const answer_t *a)
{
    uint32_t m = 0;
    if (a->max_down > 0) {              /* Jd: bit r allows (r + 21) * 8000 / 6 bit/s */
        int r;
        for (r = 0; r < V90_NUM_RATES; r++)
            if ((long)(r + 21) * 8000 / 6 <= a->max_down) m |= 1u << r;
        if (!m) m = 1;                  /* the lowest rate at least */
    }
    return m;
}

/* V.8 selected V.90 (9.2.1.1.1): INFO0d after the 75 ms */
void ans_start_p2(answer_t *a)
{
    p2d_init(&a->p2, &a->p2cfg);
    /* 9.3/V.92: bit 27 of INFO0d; the rest follows from INFO0a */
    if (a->cfg.modes & ANS_MODE_V92) p2d_offer_v92(&a->p2, a->pcm_up);
    ans_enter(a, ANS_P2); a->t = 0;
}

/* Phase 2 is done: INFO1a says how the caller will send */
static void start_p34(answer_t *a)
{
    p34d_cfg c;
    int sr = a->p2.info1a.up_sr, hi = (a->p2cfg.up_hi_carrier >> sr) & 1;
    c.alaw = a->p2cfg.alaw;
    c.uinfo = a->p2.info1a.uinfo; c.md_len = a->p2.info1a.md_len; c.rtd = a->p2.rtd;
    c.echo_delay = a->echo_delay;
    c.max_up_rate = a->max_up;
    c.down_rates = jd_rates(a);
    c.getbit = a->cfg.getbit; c.putbit = a->cfg.putbit; c.user = a->cfg.user;
    /* V.34 Table 2: carrier as a ratio of the symbol rate */
    if (sr == 3) { c.sym_rate = 3000; c.car_num = hi ? 2 : 3; c.car_den = hi ? 3 : 5; }
    else { c.sym_rate = 3200; c.car_num = hi ? 3 : 4; c.car_den = hi ? 5 : 7; }
    /* a symbol rate we did not offer cannot be served */
    if ((sr != 3 && sr != 4) || p34d_init(&a->p34, &c) != 0) { ans_fail(a, ans_retrain_or(a, ANS_END_FAILED)); return; }
    ans_enter(a, ANS_P3); a->t = 0;
}

/* Phase 2 is done and INFO1a is the one of Table 18/V.92: the caller will
 * send PCM (9.3/V.92). Phases 3 and 4 of V.92 9.5.1 and 9.6.1 with what
 * INFO1a says of the caller's precoder and prefilter. */
static void start_v92(answer_t *a)
{
    v92d_cfg c;
    const info1a_t *i = &a->p2.info1a;
    memset(&c, 0, sizeof c);
    c.alaw = a->p2cfg.alaw;
    c.uinfo = i->uinfo; c.md_len = i->md_len; c.rtd = a->p2.rtd;
    c.sections = i->sections;
    c.ltot = 192 + 64 * i->ltot_code; c.lmax = 128 + 64 * i->lmax_code;     /* Table 18 bits 14:17 */
    /* the same figure p34d gets; like it, v92d measures when it is not known */
    c.echo_delay = a->echo_delay > V92D_MAX_ECHO_DELAY ? V92D_MAX_ECHO_DELAY : a->echo_delay;
    c.max_up_rate = a->max_up;
    c.down_rates = jd_rates(a);
    /* INFO0d bits 33:37 bound the power of the caller's choice of downstream
     * constellation, and with it our echo in data mode: -0.5 dBm0 less half
     * a dB per step; 0 dBm0 is 15770 RMS on the 16-bit linear scale, and a
     * step of 0.5 dB a factor of 967 / 1024 */
    {
        int k; uint32_t r = 15770;
        for (k = 0; k <= a->p2cfg.max_tx_code; k++) r = r * 967 / 1024;
        c.down_rms = (int)r;
    }
    c.dsn_pace = V92_DESIGN_PACE;
    c.rx_linear = a->cfg.rx_linear;
    c.getbit = a->cfg.getbit; c.putbit = a->cfg.putbit; c.user = a->cfg.user;
    a->v92_data = 0;
    if (v92d_init(&a->u.v92, &c) != 0) { ans_fail(a, ans_retrain_or(a, ANS_END_FAILED)); return; }
    ans_enter(a, ANS_P3X); a->t = 0;
}

/* A retrain is due: the 70 ms of silence, then Phase 2 again (V.90 9.5.1,
 * V.92 9.7.1). A line that needs too many in a row is no line (ans_rt_v90). */
void ans_v90_retrain(answer_t *a)
{
    ans_rt_begin(a, &ans_rt_v90);
    if (a->phase == ANS_FAILED) return;
    ans_enter(a, ANS_RETRAIN); a->t = 0;
}

/* V.90 Phase 2 is done and INFO1a selects V.34 (bits 37:39 in 0..5; V.90
 * 9.2.1.1.8): we go on as the call modem of 11.3.1.1/V.34, silent until
 * the caller's S, which comes 70 ms after its INFO1a. Our carrier, filter
 * and power reduction are INFO1a's; the caller's carrier is the one our
 * INFO1d named for the symbol rate. Not served when V.34 is not offered as
 * a data mode here, or when INFO1a asks for what the V.34 engine does not do
 * (see v34_call_cfg_from_v90). */
static void start_v34_call(answer_t *a)
{
    v34ans_cfg c;
    v34ans_watch_t w = a->v34.w;        /* in a retrain, the call's best SNR stands */
    int sr = a->p2.info1a.up_sr;
    /* a retrain's Phase 2 has no INFO0: what the caller's INFO0a said holds */
    if (a->p2.n_info0d > 0) a->info0a = a->p2.info0a;
    ans_v34_cfg(a, &c);
    if (!(a->cfg.modes & V8_MOD_V34) || sr > 5 || !((a->p2cfg.up_sr_mask >> sr) & 1)
        || v34ans_init_call(&a->v34, &c, &a->p2.info1a, (a->p2cfg.up_hi_carrier >> sr) & 1,
                            a->info0a.c1664, a->p2.rtd) != 0) { ans_fail(a, ans_retrain_or(a, ANS_END_FAILED)); return; }
    a->v34.w = w;
    ans_enter(a, ANS_V34); a->t = 0;
}

/* ---- ans_p2: ANS_P2, ANS_RETRAIN ---- */

static int16_t tick_p2(answer_t *a, int16_t rx)
{
    int16_t tx;
    if (a->phase == ANS_RETRAIN) {
        /* 9.5.1: silence for 70 +- 5 ms, then Tone B and Phase 2 from 9.2.1.1.3 */
        if (a->t >= MS(70)) {
            p2d_init_retrain(&a->p2, &a->p2cfg);
            /* "any subsequent retrains shall use Phase 2 of ITU-T V.92" (9.3/V.92):
             * INFO1d of Table 17, its bit 70 saying whether PCM upstream is still on */
            if (a->v92_both) p2d_retrain_v92(&a->p2, a->pcm_up);
            a->phase = ANS_P2; a->t = 0;
        }
        return 0;
    }
    tx = p2d_tick(&a->p2, rx);
    if (a->p2.state == P2D_DONE) {
        /* a retrain's Phase 2 has no INFO0: what the first one found holds */
        if (a->p2.n_info0d > 0) a->v92_both = a->p2.v92 && a->p2.info0a.v92;
        if (a->p2.mode == P2D_MODE_V92) start_v92(a);
        else if (a->p2.info1a.v90) start_p34(a);
        else start_v34_call(a);
    }
    else if (a->p2.state == P2D_FAILED) ans_fail(a, ans_retrain_or(a, ANS_END_FAILED));
    return tx;
}

/* between engines: the echo delay is the modulation's */
const ans_scheme_t ans_p2 = { tick_p2, NULL, NULL, NULL, NULL, NULL, ans_mod_echo_delay, 0 };

/* ---- ans_v90: ANS_P3, ANS_V90 ---- */

static int16_t tick_v90(answer_t *a, int16_t rx)
{
    int16_t tx;
    a->pcm = p34d_tick(&a->p34, rx);
    tx = (int16_t)v90_octet_linear(a->p2cfg.alaw, a->pcm);
    if (a->p34.state == P34_RETRAIN) {
        ans_v90_retrain(a); tx = 0;
    } else if (a->p34.state == P34_FAILED) {
        ans_fail(a, a->p34.error == P34_ERR_CLEARDOWN ? ANS_END_CLEARDOWN_FAR
                    : a->p34.error == P34_ERR_HANGUP ? ANS_END_CLEARDOWN_OURS : ans_retrain_or(a, ANS_END_FAILED));
        tx = 0;
    } else if (a->p34.state == P34_DATA) {
        /* also after a rate renegotiation */
        a->connected = 1; a->modulation = ANS_MOD_V90;
        a->bitrate = a->p34.down_rate; a->rx_bitrate = a->p34.up_rate;
        a->phase = ANS_V90; a->retraining = 0;
        ans_rt_data_tick(a, &ans_rt_v90);
    }
    return tx;
}

static int v90_cleardown(answer_t *a) { return a->phase == ANS_V90 ? p34d_cleardown(&a->p34) : -1; }
static int v90_retrain(answer_t *a) { return a->phase == ANS_V90 ? p34d_retrain(&a->p34) : -1; }
static int v90_moving(const answer_t *a) { return a->phase == ANS_V90 ? a->p34.tx_ready && a->p34.up.rx_ready : 1; }
static int v90_snr_den(const answer_t *a, int64_t *num, int64_t *den)
{
    *num = 65536; *den = a->p34.up.rx.mse;          /* |e|^2 / |d|^2 in Q16 */
    return 0;
}
static int p2_rtd(const answer_t *a) { return a->p2.rtd; }
static int v90_echo_delay(const answer_t *a) { return a->p34.ecd.delay; }

const ans_scheme_t ans_v90 = { tick_v90, v90_cleardown, v90_retrain, v90_moving, v90_snr_den, p2_rtd, v90_echo_delay, 1 };

/* ---- ans_v92: ANS_P3X, ANS_V92 ---- */

static int16_t tick_v92(answer_t *a, int16_t rx)
{
    v92d_t *m = &a->u.v92;
    int16_t tx;
    a->pcm = v92d_tick(m, rx);
    tx = (int16_t)v90_octet_linear(a->p2cfg.alaw, a->pcm);
    if (m->state == V92D_FAILED && (m->error == V92D_ERR_CLEARDOWN || m->error == V92D_ERR_HANGUP
                                    || m->error == V92D_ERR_CARRIER)) {
        ans_fail(a, m->error == V92D_ERR_CLEARDOWN ? ANS_END_CLEARDOWN_FAR      /* 9.11 */
                    : m->error == V92D_ERR_HANGUP ? ANS_END_CLEARDOWN_OURS
                    : ans_retrain_or(a, ANS_END_FAILED));           /* or the caller is gone */
        tx = 0;
    } else if (m->state == V92D_FAILED || m->state == V92D_RETRAIN) {
        /* 9.5.1.2 and 9.6.1.2: what recovers a start-up that went wrong
         * is a retrain; so does the caller's Tone A (9.7.1.2) and our
         * own wish, asked for or v92d's (a rate renegotiation that
         * stalled, 9.8). Phase 2 of that retrain is V.92's (9.3), and
         * it measures the line again: PCM upstream is offered in it
         * again, except when
         *   - this start-up never reached data mode, whoever gave up, or
         *   - v92d failed, or retrains because the upstream did not hold
         *     even the lowest PCM rate in data mode (the line does not
         *     carry PCM upstream, whatever a new design says):
         * then bit 70 of INFO1d says "no PCM upstream" for the rest of
         * the call and the caller is left with V.34 upstream (V.90). */
        if (m->state == V92D_FAILED || !a->v92_data || m->up_floor) a->pcm_up = 0;
        ans_v90_retrain(a); tx = 0;
    } else if (m->state == V92D_DATA) {
        /* also after a rate renegotiation */
        a->connected = 1; a->modulation = ANS_MOD_V92;
        a->bitrate = m->down_rate; a->rx_bitrate = m->up_rate;
        a->phase = ANS_V92; a->retraining = 0; a->v92_data = 1;
        ans_rt_data_tick(a, &ans_rt_v90);
    }
    return tx;
}

static int v92_cleardown(answer_t *a) { return a->phase == ANS_V92 ? v92d_cleardown(&a->u.v92) : -1; }
static int v92_retrain(answer_t *a) { return a->phase == ANS_V92 ? v92d_retrain(&a->u.v92) : -1; }
static int v92_moving(const answer_t *a) { return a->phase == ANS_V92 ? a->u.v92.tx_ready && a->u.v92.rx_ready : 1; }
static int v92_snr_den(const answer_t *a, int64_t *num, int64_t *den)
{
    /* the decoder's noise estimate against the mean square of the points
     * of CPd, over the six constellation frame intervals; only while that
     * decoder runs */
    const v92_map_t *mp = &a->u.v92.cpd.map;
    int j, k, n = 0;
    if (a->phase != ANS_V92 || !a->u.v92.rx_ready) return -1;
    *num = 0;
    for (j = 0; j < V92_CFRAME; j++) {
        int lc = v92_lc(mp, j);
        for (k = 0; k < lc; k++) { int64_t v = v92_point(mp, j, k); *num += v * v; n++; }
    }
    if (!n) return -1;
    *num /= n; *den = v92_dec_quality(&a->u.v92.dec);
    return 0;
}
static int v92_echo_delay(const answer_t *a) { return a->u.v92.ecd.delay; }

const ans_scheme_t ans_v92 = { tick_v92, v92_cleardown, v92_retrain, v92_moving, v92_snr_den, p2_rtd, v92_echo_delay, 1 };
