/* V.34 Phases 3 and 4 and data mode, either role. See v34dx.h. */
#include <string.h>
#ifdef P34D_TRACE
#include <stdint.h>
#include <stdio.h>
int16_t v34dx_trace_tx(int16_t y);
#endif
#include "v34dx.h"
#include "clock.h"
#include "probe.h"
#include "pcmenc.h"
#include "../v90/v90_pcm.h"

#define MS(ms) ((int32_t)(ms) * 8)

/* our choices for the lengths the Recommendation leaves open */
#define TRN_KNOWN      512      /* symbols of the far TRN used as a known reference */
#define EC_SETTLE      80       /* samples of TRN before the canceller looks at it */
#define EC_FAST        8800     /* samples at full NLMS step */
#define EC_SLOW        800      /* ... and at a small one */
#define TRN4_SYMS      1280     /* TRN of Phase 4: >= 512 T, <= 2000 ms (533 ms at 2400) */
#define RENEG_TRN_MS   100      /* TRN of a rate renegotiation: optional, <= 2000 ms */
#define FAR_MIN_RTD    56       /* a far echo later than this gets its own canceller */
#define FAR_LEAD       16
/* time limits */
#define T_FIRST_S_MS   6000     /* call: INFO1a to the first S, 70 ms nominal */
#define T_S_AFTER_J    600      /* 11.3.2.2.1 / 11.4.2.1.1, plus a round trip */
#define T_FAR_J_MS     2800     /* 11.3.2.1.1 / 11.3.2.2.2, plus two round trips */
#define T_PHASE4_MS    2500     /* 11.4.2.1.2 / 11.4.2.2.2, plus three round trips */
#define T_RENEG_S_MS   300      /* S heard but no S-bar: answer with S all the same */
#define T_RENEG_MS     6000
#define CLEAR_MPS      6        /* MP sequences of a cleardown before we go silent regardless */
/* is the line still good enough for the rate? (as p34d; v34up.h) */
#define Q_MARGIN_DB10  30
#define Q_STEP_DB10    60       /* rate_down, retrain_on_step: fallen this far, retrain */

enum { RX_OFF, RX_WAIT_S, RX_KNOWN, RX_SEQ, RX_HOLD, RX_SRES, RX_DATA };

static void tx_go(v34dx_t *p, int sig)
{
    p->txsig = sig; p->txn = 0;
    p->tx_first[sig] = p->now;
}

static void enter(v34dx_t *p, v34dx_state s) { p->state = s; p->t = 0; }
static int32_t far_from(const v34dx_t *p);

static void stop(v34dx_t *p)
{
    p->tx_ready = p->up.rx_ready = 0;
    p->txsig = DXS_SIL; p->txn = 0;
}

static void fail(v34dx_t *p, int err)
{
#ifdef P34D_TRACE
    printf("          V.34 engine fails: error %d in state %d; B1 %d errors in %d bits\n", err, (int)p->state, (int)p->up.vrx.b1_errors, (int)p->up.vrx.b1_bits);
#endif
    stop(p); p->error = err; enter(p, DX_FAILED);
}

static void stopped(v34dx_t *p, int by, int why) { p->stop.by = (uint8_t)by; p->stop.why = (uint8_t)why; }

/* why: ours, for what; DX_WHY_ASKED with ours 0, the far end's tone */
static void retrain(v34dx_t *p, int ours, int why)
{
    stopped(p, ours ? DX_BY_NEAR : DX_BY_FAR, why);
    stop(p); p->retrain_ours = ours; enter(p, DX_RETRAIN);
}

static int far_poly(const v34dx_t *p) { return p->cfg.answer ? V34_GPC : V34_GPA; }
static int own_poly(const v34dx_t *p) { return p->cfg.answer ? V34_GPA : V34_GPC; }

static void neg_reset(v34dx_t *p)
{
    p->got_far_mp = p->got_far_mpa = p->got_e = p->far_clear = 0;
    p->cand_ok = 0; p->rates_done = 0;
    p->mp.ack = 0; p->mp_sent = p->mpa_sent = 0;
}

int v34dx_init(v34dx_t *p, const v34dx_cfg *cfg)
{
    int k, i, samples;
    memset(p, 0, sizeof *p);
    p->cfg = *cfg;
    if (v34mod_init(&p->mod, cfg->sym_rate, cfg->tx_car_num, cfg->tx_car_den, cfg->tx_preemph,
                    cfg->level, cfg->tx_pwr_red) != 0 || cfg->rx_car_den <= 0) {
        p->state = DX_FAILED; p->error = DX_ERR_CFG;
        return -1;
    }
    ec_init(&p->ec); ec_init(&p->fec); p->ec_mu = -1;
    if (cfg->echo_delay > DX_MAX_ECHO_DELAY) p->cfg.echo_delay = DX_MAX_ECHO_DELAY;
    ecdly_init(&p->ecd, p->cfg.echo_delay);
    sdet_init(&p->sdet, cfg->sym_rate, cfg->rx_car_num, cfg->rx_car_den);
    p->sdet.div = 16;               /* Phase 3: nothing but S can be there */
    qamrx_init(&p->up.rx, cfg->sym_rate, cfg->rx_car_num, cfg->rx_car_den);
    p->tx_pts = 4; p->tx_g = 16384;
    samples = EC_SETTLE + EC_FAST + EC_SLOW;
    if (cfg->echo_delay < 0) samples += ECDLY_SAMPLES;
    p->far_search = cfg->rtd > ECDLY_MAXLAG;
    if (p->far_search) samples = far_from(p) + ECDLY_SAMPLES + EC_FAST + EC_SLOW;
    p->trn_syms = (int32_t)(((int64_t)samples * cfg->sym_rate) / 8000) + 1;
    if (p->trn_syms < 512) p->trn_syms = 512;
    p->trn4_syms = TRN4_SYMS;
    tonedet_init(&p->tone, cfg->answer ? TONEDET_1200 : TONEDET_2400);  /* the far modem's retrain tone: Tone B / Tone A */
    /* MP: what we ask and what we can do */
    p->mp.type = 0;
    for (k = 0; k < 3; k++) for (i = 0; i < 2; i++) {
        p->mp.h[k][i] = cfg->ask_h[k][i];
        if (cfg->ask_h[k][i]) p->mp.type = 1;
    }
    p->mp.trellis = (uint8_t)cfg->ask_trellis; p->mp.nonlinear = (uint8_t)(cfg->ask_nonlinear != 0);
    p->mp.shaping = (uint8_t)(cfg->ask_shaping != 0); p->mp.asym = (uint8_t)!cfg->symmetric;
    for (k = 0; k < 14; k++) {
        v34_map_t m;
        unsigned rate = 2400u * (unsigned)(k + 1);
        if (rate > 28800 && !cfg->peer_1664) break;
        if (v34_map_params((unsigned)cfg->sym_rate, rate, 0, &m) == 0) p->mp.rates |= (uint16_t)(1u << k);
    }
    if (cfg->answer) {
        p->rxmode = RX_OFF;
        p->state = DX_P3_SIL;
    } else {
        p->rxmode = RX_WAIT_S;
        p->state = DX_P3_WAIT_S;
    }
    return 0;
}

/* ---- the data signalling rates (11.4.1.1.3, 11.4.1.2.3) ---- */

static int decide_rates(v34dx_t *p)
{
    int ac, ca;
    if (p->rates_done) return 0;
    if (p->cfg.answer) { v34_mp_rates(&p->mp, &p->far_mp, &ac, &ca); p->tx_rate = ac; p->rx_rate = ca; }
    else               { v34_mp_rates(&p->far_mp, &p->mp, &ac, &ca); p->tx_rate = ca; p->rx_rate = ac; }
    if (!p->tx_rate || !p->rx_rate) return -1;
    p->rates_done = 1;
    return 0;
}

/* the ceilings of our MP: receive from what the line gave the 4-point
 * signal (each 3 dB above 9 dB is one bit per symbol), transmit what we can */
static void set_mp_limits(v34dx_t *p, int measure)
{
    int nrx, ntx = p->cfg.max_tx_rate ? p->cfg.max_tx_rate / 2400 : 14;
    int top = p->cfg.peer_1664 ? 14 : 12, low = p->cfg.sym_rate == 2400 ? 1 : 2;
    if (measure) {
        int32_t b10;
        p->snr_db10 = probe_db10(65536, p->up.rx.mse > 0 ? p->up.rx.mse : 1);
        b10 = (p->snr_db10 - V34UP_SNR0_DB10) * 10 / 30;
        p->line_n = (int)(((int64_t)p->cfg.sym_rate * b10) / 10) / 2400;
    }
    nrx = p->line_n;
    if (p->cfg.max_rx_rate && nrx > p->cfg.max_rx_rate / 2400) nrx = p->cfg.max_rx_rate / 2400;
    if (nrx > top) nrx = top;
    if (nrx < low) nrx = low;
    if (ntx > top) ntx = top;
    if (ntx < low) ntx = low;
    if (p->cfg.answer) { p->mp.max_ca = (uint8_t)nrx; p->mp.max_ac = (uint8_t)ntx; }
    else               { p->mp.max_ac = (uint8_t)nrx; p->mp.max_ca = (uint8_t)ntx; }
}

static void load_mp(v34dx_t *p)
{
    v34_mp_t m = p->mp;
    if (p->clearing || p->far_clear) m.max_ca = m.max_ac = 0;      /* 11.7 */
    if (p->in_reneg && p->cfg.reneg_type0) m.type = 0;
    p->mplen = v34_mp_build(&m, p->mpbits);
}

/* ---- transmitter: one symbol ---- */

static int tx_getbit(void *user)
{
    v34dx_t *p = (v34dx_t *)user;
    return p->cfg.getbit ? p->cfg.getbit(p->cfg.user) : 1;
}

static int tx_begin_data(v34dx_t *p)
{
    v34_cfg_t c;
    int k;
    memset(&c, 0, sizeof c);
    if (decide_rates(p) != 0) return -1;
    c.symbol_rate = (uint16_t)p->cfg.sym_rate; c.data_rate = (uint16_t)p->tx_rate;
    c.trellis = p->far_mp.trellis; c.expanded = p->far_mp.shaping; c.nonlinear = p->far_mp.nonlinear;
    c.scrambler = (uint8_t)own_poly(p);
    for (k = 0; k < 3; k++) { c.h[k][0] = p->far_h[k][0]; c.h[k][1] = p->far_h[k][1]; }
    if (v34_params_init(&p->tpar, &c) != 0) return -1;
    p->tx_dg = (int32_t)v34_gain_q14(p->tpar.energy_tx_q8);
    return 0;
}

static void tx_symbol(v34dx_t *p, int32_t *re, int32_t *im)
{
    v34_pt_t pt;
    int32_t g = 16384;
    int k, nb;
    unsigned b;

    *re = *im = 0;
    if (p->txsig == DXS_SIL) return;
    /* a renegotiation's or cleardown's S, once the mapping frame is out */
    if (p->txsig == DXS_DATA && p->tx_next && p->vtx.pos >= 8) { p->tx_next = 0; tx_go(p, DXS_S); }
    p->tx_count[p->txsig]++;
    switch (p->txsig) {
    case DXS_S:
        v34_s(p->txn, V34_S_LEN, re, im);
        if (++p->txn == V34_S_LEN) tx_go(p, DXS_SBAR);
        return;
    case DXS_SBAR:
        v34_sbar(p->txn, re, im);
        if (++p->txn == V34_SBAR_LEN) {
            if (p->phase4) { v34_tmod_init(&p->tmod, own_poly(p), p->tx_pts); tx_go(p, DXS_TRN4); }
            else tx_go(p, DXS_PP);
        }
        return;
    case DXS_PP:
        v34_pp(p->txn, re, im);
        if (++p->txn == V34_PP_LEN) { v34_tmod_init(&p->tmod, own_poly(p), 4); p->trn_t = 0; tx_go(p, DXS_TRN); }
        return;
    case DXS_TRN:
        pt = v34_tmod_trn(&p->tmod);
        if (++p->txn >= p->trn_syms) tx_go(p, DXS_J);
        break;
    case DXS_J: {
        const uint8_t *j = p->cfg.ask_16pt ? v34_j16 : v34_j4;
        k = (int)(p->txn & 7) * 2;
        pt = v34_tmod_put(&p->tmod, (unsigned)(j[k] | (j[k + 1] << 1)));
        p->txn++;
        if ((p->txn & 7) == 0 && p->tx_next) {
            p->tx_next = 0;
            tx_go(p, p->cfg.answer ? DXS_SIL : DXS_JP);
        }
        break;
    }
    case DXS_JP:
        k = (int)p->txn * 2;
        pt = v34_tmod_put(&p->tmod, (unsigned)(v34_jprime[k] | (v34_jprime[k + 1] << 1)));
        if (++p->txn == 8) { v34_tmod_init(&p->tmod, own_poly(p), p->tx_pts); tx_go(p, DXS_TRN4); }
        break;
    case DXS_TRN4:
        pt = v34_tmod_trn(&p->tmod); g = p->tx_g;
        p->txn++;
        if (p->tx_next) { p->tx_next = 0; load_mp(p); tx_go(p, DXS_MP); }
        break;
    case DXS_MP:
        nb = p->tx_pts == 16 ? 4 : 2;
        for (b = 0, k = 0; k < nb; k++) b |= (unsigned)p->mpbits[p->txn * nb + k] << k;
        pt = v34_tmod_put(&p->tmod, b); g = p->tx_g;
        if (++p->txn * nb >= p->mplen) {
            p->mp_sent++;
            if (p->mp.ack) p->mpa_sent++;
            p->txn = 0;
            if (p->clearing) {
                if (p->mp_sent >= CLEAR_MPS || (p->mpa_sent >= 1 && p->got_far_mpa)) { fail(p, DX_ERR_HANGUP); return; }
                if (p->got_far_mp) p->mp.ack = 1;
                load_mp(p);
            } else if (p->far_clear) {
                if (p->mpa_sent >= 1) { fail(p, DX_ERR_CLEARDOWN); return; }
                p->mp.ack = 1;
                load_mp(p);
            } else if (p->mpa_sent >= 1 && (p->got_far_mpa || p->got_e)) {
                if (tx_begin_data(p) != 0) { fail(p, DX_ERR_RATE); return; }
                tx_go(p, DXS_E);
            } else {
                if (p->got_far_mp) p->mp.ack = 1;
                load_mp(p);
            }
        }
        break;
    case DXS_E:
        nb = p->tx_pts == 16 ? 4 : 2;
        pt = v34_tmod_put(&p->tmod, (1u << nb) - 1); g = p->tx_g;
        if (++p->txn * nb >= 20) { v34_tx_init(&p->vtx, &p->tpar, tx_getbit, p); p->tx_starts++; tx_go(p, DXS_DATA); }
        break;
    default:    /* DXS_DATA */
        if (!p->tx_next && !v34_tx_in_b1(&p->vtx)) p->tx_ready = 1;
        v34_tx_symbol(&p->vtx, &pt, NULL); g = p->tx_dg;
        break;
    }
    *re = (pt.re * g) >> 9; *im = (pt.im * g) >> 9;
}

/* ---- sequences: J, J', MP, E ---- */

static void rx_begin_data(v34dx_t *p);

static void on_mp(v34dx_t *p, const v34_mp_t *m)
{
    /* two in a row that agree (MP and MP' differ in the acknowledge bit
     * only): what TRN looks like through the differential decoder passes
     * the CRC once in 65536 tries */
    v34_mp_t a = *m, b = p->cand;
    a.ack = b.ack = 0;
    if (!p->cand_ok || memcmp(&a, &b, sizeof a) != 0) {
        memset(&p->cand, 0, sizeof p->cand);
        p->cand = *m; p->cand_ok = 1;
        if (!p->got_far_mp) return;
        /* a different MP after an accepted one: wait for its repetition */
        return;
    }
    p->cand = *m;
    p->far_mp = *m;
    if (m->type) memcpy(p->far_h, m->h, sizeof p->far_h);
    p->got_far_mp = 1;
    if (m->ack) p->got_far_mpa = 1;
    if (v34_mp_is_cleardown(m)) p->far_clear = 1;
}

static void seq_try(v34dx_t *p)
{
    v34_mp_t m;
    memset(&m, 0, sizeof m);
    if (p->fn < 85) return;
    if (v34_mp_parse(p->fbuf, p->fn, &m) > 0) { p->fn = 0; on_mp(p, &m); }
}

static void seq_bit(v34dx_t *p, int bit)
{
    p->jsr = (uint16_t)((p->jsr >> 1) | (bit << 15)); p->jbits++;
    if (p->seq_j) {
        if (!p->got_j) {
            int ty = p->jsr == word16(v34_j4) ? 1 : p->jsr == word16(v34_j16) ? 2 : 0;
            if (ty) {
                if (ty == p->j_type && p->jbits - p->j_at == 16) { p->got_j = 1; p->far_16pt = ty == 2; }
                p->j_type = ty; p->j_at = p->jbits;
            }
        } else if (!p->got_jp && ((p->jbits - p->j_at) & 15) == 0 && p->jsr == word16(v34_jprime)) {
            /* TRN follows, with the points our J asked for */
            p->got_jp = 1; p->t_jp = p->now;
            p->seq_j = 0;
            if (p->cfg.ask_16pt) { v34_tdem_set_points(&p->up.tdem, 16); p->rx16 = 1; }
        }
    }
    if (!p->seq_mp) return;
    if (p->fn > 0 && p->fn < DX_FBUF) p->fbuf[p->fn++] = (uint8_t)bit;
    int sync = seqhunt_bit(&p->sh, bit);
    if (bit) {
        /* E: 20 ones; inside MP a start bit breaks every run at 17 */
        if (p->got_far_mp && !p->far_clear && p->sh.ones == 20 && !p->got_e) {
            p->got_e = 1;
            rx_begin_data(p);
        }
        return;
    }
    if (sync) {
        if (p->fn > 18) { p->fn -= 18; seq_try(p); if (p->state == DX_FAILED) return; }
        memset(p->fbuf, 1, 17); p->fbuf[17] = 0; p->fn = 18;
    } else if (p->fn > 0 && (p->fn % 17) == 0) {
        seq_try(p);
    }
    if (p->fn >= DX_FBUF) p->fn = 0;
}

/* ---- data mode, receive ---- */

static void put_data(void *user, int bit)
{
    v34dx_t *p = (v34dx_t *)user;
    if (p->up.rx_ready && p->cfg.putbit) p->cfg.putbit(p->cfg.user, bit);
}

/* E has just ended: B1 starts with the next symbol */
static void rx_begin_data(v34dx_t *p)
{
    v34_cfg_t c;
    int k;
    memset(&c, 0, sizeof c);
    if (decide_rates(p) != 0) { fail(p, DX_ERR_RATE); return; }
    c.symbol_rate = (uint16_t)p->cfg.sym_rate; c.data_rate = (uint16_t)p->rx_rate;
    c.trellis = (uint8_t)p->cfg.ask_trellis; c.expanded = (uint8_t)(p->cfg.ask_shaping != 0);
    c.nonlinear = (uint8_t)(p->cfg.ask_nonlinear != 0);
    c.scrambler = (uint8_t)far_poly(p);
    for (k = 0; k < 3; k++) { c.h[k][0] = p->cfg.ask_h[k][0]; c.h[k][1] = p->cfg.ask_h[k][1]; }
    if (v34up_data_begin(&p->up, &c, put_data, p) != 0) { fail(p, DX_ERR_RATE); return; }
    p->mse_ref = (p->up.rx.mse > 0 ? p->up.rx.mse : 1) << 12;
    p->rxmode = RX_DATA;
    p->rx_starts++;
    p->seq_mp = 0;
    sdet_init(&p->sdet, p->cfg.sym_rate, p->cfg.rx_car_num, p->cfg.rx_car_den);    /* watches for S from here on */
}

/* ---- rate renegotiation ---- */

static void reneg_begin(v34dx_t *p)
{
    if (!p->in_reneg) { p->in_reneg = 1; neg_reset(p); }
}

/* S for 128 T, S-bar for 16 T, a short TRN, MP: all with four points (11.6),
 * from the end of the mapping frame being sent */
static void start_reneg_tx(v34dx_t *p)
{
    reneg_begin(p);
    p->tx_ready = 0;
    p->phase4 = 1; p->tx_pts = 4; p->tx_g = 16384;
    p->trn4_syms = (int32_t)RENEG_TRN_MS * p->cfg.sym_rate / 1000;
    /* in data mode S follows the mapping frame under way (tx_next), so that
     * every bit taken from getbit is sent whole: up to 7 symbols later */
    p->tx_next = p->txsig == DXS_DATA;
    if (!p->tx_next) tx_go(p, DXS_S);
    p->t_neg = p->now; enter(p, DX_P4_TRN);
}

/* the receiver's error has been above what the rate needs for a second */
static void rate_down(v34dx_t *p)
{
    int32_t now = probe_db10(65536, p->up.rx.mse > 0 ? p->up.rx.mse : 1);
    int32_t snr = now - Q_MARGIN_DB10;
    int32_t b10 = (snr - V34UP_SNR0_DB10) * 10 / 30;
    int n = (int)(((int64_t)p->cfg.sym_rate * b10) / 10) / 2400, cur = p->rx_rate / 2400;
    int32_t was = probe_db10((int64_t)65536 << 12, p->mse_ref > 0 ? p->mse_ref : 1);
#ifdef P34D_TRACE
    if (p->cfg.retrain_on_step)
        printf("          V.34 rate_down: SNR now %d, lately %d, at training %d, rate's limit %d (0.1 dB)\n",
               (int)now, (int)was, (int)p->snr_db10, probe_db10(65536, p->up.mse_limit));
#endif
    /* Fallen in a step, not over seconds: the path changed, as when a
     * jitter buffer takes one more packet and our echo comes a packet
     * later. Whether it fell fast tells this apart, not how far: where the
     * SNR lands after our echo moves depends on the echo, not on the rate. */
    if (p->cfg.retrain_on_step && was - now >= Q_STEP_DB10) {
        retrain(p, 1, DX_WHY_SNR_STEP);
        return;
    }
    if (n >= cur) n = cur - 1;
    if (n < (p->cfg.sym_rate == 2400 ? 1 : 2)) { retrain(p, 1, DX_WHY_SNR_DECLINED); return; }
    p->line_n = n;
    set_mp_limits(p, 0);
    p->rate_drops++;
    stopped(p, DX_BY_NEAR, DX_WHY_SNR_DECLINED);
    start_reneg_tx(p);
}

/* S in data mode (11.6): what the far end sent before it is data, every
 * whole mapping frame of it. The decoder's decision delay still holds some
 * (S among the newest inputs): they go on now, but for a frame of margin
 * (v34_rx_flush_before). Called on each sample from S heard on, until S is
 * followed (an S too new in the decoder is waited for, SDET_RUN_WAIT) */
static void s_heard(v34dx_t *p)
{
    if (!v34up_s_flush(&p->up, &p->sdet)) return;
#ifdef P34D_TRACE
    if (p->cfg.retrain_on_step && p->state == DX_DATA)
        printf("          V.34 far end renegotiates: SNR now %d, at training %d, rate's limit %d (0.1 dB)\n",
               probe_db10(65536, p->up.rx.mse > 0 ? p->up.rx.mse : 1), (int)p->snr_db10, probe_db10(65536, p->up.mse_limit));
#endif
    reneg_begin(p);
    sres_resume(&p->sres);
    p->rxmode = RX_SRES;
    /* (else its answer to our own S) */
    if (p->state == DX_DATA) { stopped(p, DX_BY_FAR, DX_WHY_ASKED); enter(p, DX_RN_S); }
}

/* ---- echo: delay search and training during the TRN of Phase 3 ---- */

/* the TRN sample the far search starts on: after the near search, and late
 * enough that every lag it tries reaches back into TRN, not S or PP (whose
 * period would give peaks at many lags) */
static int32_t far_from(const v34dx_t *p)
{
    int32_t from = EC_SETTLE + (p->cfg.echo_delay < 0 ? ECDLY_SAMPLES : 0);
    int32_t trn = (int32_t)ecdly_near_base(p->cfg.rtd) + 8;
    return from > trn ? from : trn;
}

static void ec_schedule(v34dx_t *p, int16_t rxs, int16_t x)
{
    int32_t n = ++p->trn_t, from = EC_SETTLE;
    int measure = p->cfg.echo_delay < 0;
    /* how late is our echo (ecdly.h), when not known? On the samples as they
     * are. Two cancellers on one error share the step: each at full step
     * would be one filter at twice the step NLMS can take */
    if (!p->far_search) {
        p->ec_mu = v34up_ec_schedule(&p->ec, &p->ecd, measure, 0, rxs, n, &from, EC_FAST, p->far_on ? 1 : 0);
    } else {
        /* then the far end's echo, where it is now: a packet network's jitter
         * buffers can have moved it since Phase 2 measured the round trip.
         * The same measurement around that round trip (ecdly_near), on what
         * the near canceller, learning alone, leaves: a near echo many times
         * the far one would otherwise outweigh it in a short window */
        int32_t at = far_from(p);
        if (n < at) {
            p->ec_mu = v34up_ec_schedule(&p->ec, &p->ecd, measure, 0, rxs, n, &from, at, 0);
        } else {
            if (n == at) {
                p->far_back = p->ecd.delay;
                ecdly_restart(&p->ecd);
                ecdly_near(&p->ecd, p->cfg.rtd);
            } else if (n <= at + ECDLY_SAMPLES && ecdly_measure(&p->ecd, n - at, x, 0)) {
                int d = p->ecd.delay + 8 - FAR_LEAD;    /* ecdly leaves delay 8 before the peak */
                p->ecd.delay = p->far_back;
                p->far_back = d - p->ecd.delay;
            }
            p->ec_mu = n < at + ECDLY_SAMPLES ? 0
                     : n < at + ECDLY_SAMPLES + EC_FAST ? (p->far_on ? 1 : 0) : 3;
        }
        from = at + ECDLY_SAMPLES;
    }
    if (n == from) {
        /* the far end's echo of our signal, a round trip later */
        int lag;
        if (p->cfg.rtd_has_io) {
            p->cfg.rtd_has_io = 0;
            p->cfg.rtd = p->cfg.rtd > p->ecd.delay + 8 ? p->cfg.rtd - p->ecd.delay - 8 : 0;
        }
        if (!p->far_search) p->far_back = p->cfg.rtd - FAR_LEAD;
        lag = p->ecd.delay + p->far_back;
        p->far_on = p->cfg.rtd > FAR_MIN_RTD && p->far_back > 0 && lag + EC_NTAP < ECDLY_HIST - 8;
        /* (its first sample of the fast step: the mu above came from far_on
         * as it was before this sample) */
        p->ec_mu = p->far_on ? 1 : 0;
    }
}

/* ---- receiver ---- */

/* silent or sending J: 1 when the S-to-S-bar transition before PP has come */
static int wait_s(v34dx_t *p, int16_t x)
{
    (void)qamrx_tick(&p->up.rx, x);
    if (p->md_wait > 0) {
        if (--p->md_wait == 0) { sdet_init(&p->sdet, p->cfg.sym_rate, p->cfg.rx_car_num, p->cfg.rx_car_den); p->sdet.div = 16; }
        return 0;
    }
    if (!sdet_tick(&p->sdet, x)) return 0;
    if (p->cfg.md_len > 0 && !p->md_done) { p->md_done = 1; p->md_wait = MS(p->cfg.md_len * 35); return 0; }
    v34up_sync_pp(&p->up, &p->sdet, p->cfg.sym_rate);
    v34trn_init(&p->up.ref, p->cfg.answer ? V34T_GPC : V34T_GPA);
    p->rxmode = RX_KNOWN;
    return 1;
}

static void rx_symbol(v34dx_t *p)
{
    qamrx_t *r = &p->up.rx;

    switch (p->rxmode) {
    case RX_KNOWN:
        if (v34up_known(&p->up, TRN_KNOWN)) {
            v34_tdem_init(&p->up.tdem, far_poly(p), 4);
            p->rx16 = 0; p->seq_j = 1; p->seq_mp = 0; p->got_j = p->got_jp = 0; p->j_type = 0;
            p->fn = p->sh.ones = 0;
            p->rxmode = RX_SEQ;
        }
        break;
    case RX_SEQ: {
        /* decisions on 4 or 16 points; TRN, J, J', MP and E all look like this */
        int k, nb = p->rx16 ? 4 : 2;
        unsigned bits = p->rx16 ? v34up_dec16(&p->up, 0) : v34up_dec4(&p->up, QRX_EQ | QRX_CARRIER | QRX_TIMING);
        for (k = 0; k < nb && p->rxmode == RX_SEQ; k++) seq_bit(p, (int)((bits >> k) & 1));
        break;
    }
    case RX_SRES:
        if (sres_symbol(&p->sres, r)) {
            /* TRN, then MP: scrambler and differential encoder of the far
             * transmitter start from zero at TRN; the descrambler finds its
             * way within 23 bits whatever it sees first */
            int pts = (!p->in_reneg && p->cfg.ask_16pt) ? 16 : 4;
            v34_tdem_init(&p->up.tdem, far_poly(p), pts);
            p->rx16 = pts == 16;
            p->seq_j = 0; p->seq_mp = 1; p->fn = p->sh.ones = 0;
            p->rxmode = RX_SEQ;
        }
        break;
    case RX_DATA: {
#ifdef P34D_TRACE
        int was_ready = p->up.rx_ready;
#endif
        /* the watches are off during a renegotiation */
        int ev = v34up_data(&p->up, !p->in_reneg, !p->in_reneg);
#ifdef P34D_TRACE
        if (!was_ready && p->up.rx_ready)
            printf("          V.34 B1 good: %d errors in %d bits, mse %d\n", (int)p->up.vrx.b1_errors, (int)p->up.vrx.b1_bits, (int)r->mse);
#endif
        if (p->up.rx_ready && !p->in_reneg && r->mse <= p->up.mse_limit)
            p->mse_ref += ((r->mse << 12) - p->mse_ref) >> 12;
        switch (ev) {
        case V34UP_B1_BAD:
            /* in a renegotiation (the far end's can come before our
             * rate_down sees a step): what failed is our training, not the line */
            if (p->in_reneg && p->cfg.retrain_on_step) retrain(p, 1, DX_WHY_B1);
            else fail(p, DX_ERR_B1);
            break;
        case V34UP_DEAD:   retrain(p, 1, DX_WHY_NO_SIGNAL); break;
        case V34UP_SLOW:   rate_down(p); break;
        case V34UP_LOST:   fail(p, DX_ERR_CARRIER); break;
        default: break;
        }
        break;
    }
    default:    /* RX_HOLD: keep the grid running, change nothing */
        qamrx_update(r, r->zr, r->zi, 0, 12);
        break;
    }
}

/* ---- the procedure ---- */

int16_t v34dx_tick(v34dx_t *p, int16_t rxs)
{
    int16_t y, x;
    int32_t lim;

    p->now++; p->t++;
    if (p->state == DX_FAILED || p->state == DX_RETRAIN) return 0;

    if (v34mod_due(&p->mod)) {
        int32_t re, im;
        tx_symbol(p, &re, &im);
        v34mod_put(&p->mod, re, im);
        if (p->state == DX_FAILED) return 0;
    }
    y = v34mod_tick(&p->mod);
    if (p->cfg.law) y = (int16_t)v90_octet_linear(p->cfg.law == 2, pcm_encode(p->cfg.law == 2, y));

#ifdef P34D_TRACE
    ecdly_put(&p->ecd, v34dx_trace_tx(y));  /* live/replay.c: what the box really sent */
#else
    ecdly_put(&p->ecd, y);
#endif
    if (p->far_on) {
        /* both learn from what is left after both */
        x = ec_tick(&p->ec, ecdly_ref(&p->ecd, 0), rxs, -1);
        x = ec_tick(&p->fec, ecdly_ref(&p->ecd, p->far_back), x, p->ec_mu);
        ec_adapt(&p->ec, x, p->ec_mu);
    } else {
        x = ec_tick(&p->ec, ecdly_ref(&p->ecd, 0), rxs, p->ec_mu);
    }
    ecdly_next(&p->ecd);
    p->pw_in += ((int32_t)rxs * rxs - p->pw_in) >> 8;
    p->pw_res += ((int32_t)x * x - p->pw_res) >> 8;

    /* receiver */
    if (p->rxmode == RX_WAIT_S) {
        if (wait_s(p, x)) enter(p, DX_P3_RX);
    } else if (p->rxmode != RX_OFF) {
        if (v34up_tick(&p->up, x, p->rxmode == RX_DATA)) rx_symbol(p);
        if (p->state == DX_FAILED || p->state == DX_RETRAIN) return y;
        if (p->rxmode == RX_DATA) {
            (void)sdet_tick(&p->sdet, x);
            if (p->sdet.present >= SDET_PRESENT) s_heard(p);
        }
    }
    /* the far modem's retrain tone; not during our cleardown, which goes on
     * to its own end (DX_ERR_HANGUP) */
    if (p->state >= DX_P4_TRN && !p->clearing && tonedet_tick(&p->tone, x)) { retrain(p, 0, DX_WHY_ASKED); return y; }

    /* transmitter sequencing and time limits */
    switch (p->state) {
    case DX_P3_SIL:
        if ((int64_t)p->t >= p->cfg.lead) { tx_go(p, DXS_S); enter(p, DX_P3_TX); }
        break;
    case DX_P3_WAIT_S:
        if ((int64_t)p->t > MS(T_FIRST_S_MS) + MS(p->cfg.md_len * 35)) fail(p, DX_ERR_NO_S);
        break;
    case DX_P3_TX:
        if (p->txsig == DXS_TRN) ec_schedule(p, rxs, x);
        if (p->txsig == DXS_J) {
            p->ec_mu = -1;
            if (p->cfg.answer) {
                sdet_init(&p->sdet, p->cfg.sym_rate, p->cfg.rx_car_num, p->cfg.rx_car_den);
                p->sdet.div = 16;
                p->rxmode = RX_WAIT_S;
            } else {
                sres_init(&p->sres);
                p->rxmode = RX_SRES;
            }
            enter(p, DX_P3_J);
        }
        break;
    case DX_P3_J:
        /* (the answer modem leaves this state through wait_s) */
        if (!p->cfg.answer && p->rxmode == RX_SEQ) {
            /* 11.4.1.1.1: finish this J, one J', TRN with the points the far J asked */
            p->phase4 = 1;
            p->tx_pts = p->far_16pt ? 16 : 4;
            p->tx_g = p->far_16pt ? (int32_t)v34_gain_q14(V34_ENERGY_16PT_Q8) : 16384;
            p->tx_next = 1;
            p->t_neg = p->now; enter(p, DX_P4_TRN);
        } else if ((int64_t)p->t > MS(T_S_AFTER_J) + p->cfg.rtd + MS(p->cfg.md_len * 35)) {
            retrain(p, 1, DX_WHY_TIMEOUT);
        }
        break;
    case DX_P3_RX:
        if (p->cfg.answer && p->txsig == DXS_J) p->tx_next = 1;     /* 11.3.1.2.4: silence */
        if (p->got_j) {
            if (p->cfg.answer) {
                /* 11.3.1.2.6, 11.4.1.2.1: S, S-bar, TRN while J, J', TRN, MP come in */
                p->phase4 = 1;
                p->tx_pts = p->far_16pt ? 16 : 4;
                p->tx_g = p->far_16pt ? (int32_t)v34_gain_q14(V34_ENERGY_16PT_Q8) : 16384;
                p->seq_mp = 1; p->fn = p->sh.ones = 0;
                tx_go(p, DXS_S);
                p->t_neg = p->now; enter(p, DX_P4_TRN);
            } else {
                /* 11.3.1.1.3: our S, S-bar, PP, TRN, J; nothing to receive meanwhile */
                p->rxmode = RX_HOLD;
                tx_go(p, DXS_S);
                enter(p, DX_P3_TX);
            }
        } else if ((int64_t)p->t > MS(T_FAR_J_MS) + 2 * p->cfg.rtd) {
            retrain(p, 1, DX_WHY_TIMEOUT);
        }
        break;
    case DX_P4_TRN:
    case DX_P4_MP:
    case DX_P4_E:
        /* 11.4.2.x.2: E (and here B1 too) within 2500 ms and three round trips */
        lim = p->in_reneg ? MS(T_RENEG_MS) : MS(T_PHASE4_MS) + 3 * p->cfg.rtd;
        if (clk_diff(p->now, p->t_neg) > lim) {
            /* our cleardown's time-out ends it as itself, not as a retrain */
            if (p->clearing) fail(p, DX_ERR_HANGUP); else retrain(p, 1, DX_WHY_TIMEOUT);
            break;
        }
        if (p->state == DX_P4_TRN) {
            if (p->txsig == DXS_TRN4 && p->txn >= p->trn4_syms && !p->tx_next) {
                set_mp_limits(p, !p->in_reneg);
                p->tx_next = 1;
                enter(p, DX_P4_MP);
            }
        } else if (p->state == DX_P4_MP) {
            if (p->txsig == DXS_E || p->txsig == DXS_DATA) enter(p, DX_P4_E);
        } else if (p->tx_ready && p->up.rx_ready) {
            if (p->in_reneg) p->n_reneg++;
            p->in_reneg = 0;
            enter(p, DX_DATA);
        }
        break;
    case DX_RN_S:
        /* 11.6.1.2: after the S-to-S-bar transition, our S. If the S-bar was
         * missed the far modem is waiting all the same. */
        if (p->rxmode == RX_SEQ || p->t > MS(T_RENEG_S_MS)) start_reneg_tx(p);
        break;
    default:
        break;
    }
    return y;
}

int v34dx_renegotiate(v34dx_t *p, int max_rx_rate)
{
    if (p->state != DX_DATA) return -1;
    if (max_rx_rate > 0) {
        p->cfg.max_rx_rate = max_rx_rate;
        set_mp_limits(p, 0);
    }
    stopped(p, DX_BY_NEAR, DX_WHY_ASKED);
    start_reneg_tx(p);
    return 0;
}

int v34dx_cleardown(v34dx_t *p)
{
    if (p->state != DX_DATA) return -1;
    p->clearing = 1;
    stopped(p, DX_BY_NEAR, DX_WHY_CLEARDOWN);
    start_reneg_tx(p);
    return 0;
}

int v34dx_retrain(v34dx_t *p)
{
    if (p->state < DX_P4_TRN || p->state >= DX_RETRAIN) return -1;
    retrain(p, 1, DX_WHY_ASKED);
    return 0;
}

void v34dx_stop_take(v34dx_t *p, v34dx_stop_t *to)
{
    if (p->stop.why == DX_WHY_NONE) return;
    *to = p->stop;
    p->stop.why = DX_WHY_NONE;
}

static int loss_db10(const ec_t *c)
{
    int64_t n = ec_norm(c);
    return n > 0 ? probe_db10((int64_t)1 << 42, n) : -1;
}

void v34dx_echo(const v34dx_t *p, v34dx_echo_t *e)
{
    const int64_t fs = (int64_t)32767 * 32767;
    e->in_db10 = probe_db10(fs, p->pw_in > 0 ? p->pw_in : 1);
    e->res_db10 = probe_db10(fs, p->pw_res > 0 ? p->pw_res : 1);
    e->near_delay = p->ecd.delay;
    e->near_peak = ec_peak(&p->ec);
    e->near_loss_db10 = loss_db10(&p->ec);
    if (p->far_on) {
        e->far_delay = p->ecd.delay + p->far_back;
        e->far_peak = ec_peak(&p->fec);
        e->far_loss_db10 = loss_db10(&p->fec);
    } else {
        e->far_delay = e->far_peak = e->far_loss_db10 = -1;
    }
}
