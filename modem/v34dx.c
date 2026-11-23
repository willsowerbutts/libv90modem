/* V.34 Phases 3 and 4 and data mode, either role. See v34dx.h. */
#include <string.h>
#ifdef P34D_TRACE
#include <stdint.h>
#include <stdio.h>
int16_t v34dx_trace_tx(int16_t y);
#endif
#include "v34dx.h"
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
#define CD_LOSS_SYMS   1600
/* is the line still good enough for the rate? (as p34d) */
#define Q_SNR0_DB10    90
#define Q_SLACK_DB10   45
#define Q_MARGIN_DB10  30
#define Q_BAD_SYMS     3200
#define Q_DEAD_MSE     (65536 / 8)

enum { RX_OFF, RX_WAIT_S, RX_KNOWN, RX_SEQ, RX_HOLD, RX_SRES, RX_DATA };

static uint16_t word16(const uint8_t *b)
{
    uint16_t w = 0; int i;
    for (i = 0; i < 16; i++) w |= (uint16_t)(b[i] << i);
    return w;
}

static void tx_go(v34dx_t *p, int sig)
{
    p->txsig = sig; p->txn = 0;
    p->tx_first[sig] = p->now;
}

static void enter(v34dx_t *p, v34dx_state s) { p->state = s; p->t = 0; }

static void stop(v34dx_t *p)
{
    p->tx_ready = p->rx_ready = 0;
    p->txsig = DXS_SIL; p->txn = 0;
}

static void fail(v34dx_t *p, int err)
{
#ifdef P34D_TRACE
    printf("          V.34 engine fails: error %d in state %d; B1 %d errors in %d bits\n", err, (int)p->state, (int)p->vrx.b1_errors, (int)p->vrx.b1_bits);
#endif
    stop(p); p->error = err; enter(p, DX_FAILED);
}

static void retrain(v34dx_t *p, int ours) { stop(p); p->retrain_ours = ours; enter(p, DX_RETRAIN); }

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
    qamrx_init(&p->rx, cfg->sym_rate, cfg->rx_car_num, cfg->rx_car_den);
    p->tx_pts = 4; p->tx_g = 16384;
    samples = EC_SETTLE + EC_FAST + EC_SLOW;
    if (cfg->echo_delay < 0) samples += ECDLY_SAMPLES;
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
        p->snr_db10 = probe_db10(65536, p->rx.mse > 0 ? p->rx.mse : 1);
        b10 = (p->snr_db10 - Q_SNR0_DB10) * 10 / 30;
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
        if (!v34_tx_in_b1(&p->vtx)) p->tx_ready = 1;
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
            if (p->cfg.ask_16pt) { v34_tdem_set_points(&p->tdem, 16); p->rx16 = 1; }
        }
    }
    if (!p->seq_mp) return;
    if (p->fn > 0 && p->fn < DX_FBUF) p->fbuf[p->fn++] = (uint8_t)bit;
    if (bit) {
        p->ones++;
        /* E: 20 ones; inside MP a start bit breaks every run at 17 */
        if (p->got_far_mp && !p->far_clear && p->ones == 20 && !p->got_e) {
            p->got_e = 1;
            rx_begin_data(p);
        }
        return;
    }
    if (p->ones >= 17) {
        if (p->fn > 18) { p->fn -= 18; seq_try(p); if (p->state == DX_FAILED) return; }
        memset(p->fbuf, 1, 17); p->fbuf[17] = 0; p->fn = 18;
    } else if (p->fn > 0 && (p->fn % 17) == 0) {
        seq_try(p);
    }
    p->ones = 0;
    if (p->fn >= DX_FBUF) p->fn = 0;
}

/* ---- data mode, receive ---- */

static void put_data(void *user, int bit)
{
    v34dx_t *p = (v34dx_t *)user;
    if (p->rx_ready && p->cfg.putbit) p->cfg.putbit(p->cfg.user, bit);
}

/* E has just ended: B1 starts with the next symbol */
static void rx_begin_data(v34dx_t *p)
{
    v34_cfg_t c;
    int k;
    int32_t d;
    memset(&c, 0, sizeof c);
    if (decide_rates(p) != 0) { fail(p, DX_ERR_RATE); return; }
    c.symbol_rate = (uint16_t)p->cfg.sym_rate; c.data_rate = (uint16_t)p->rx_rate;
    c.trellis = (uint8_t)p->cfg.ask_trellis; c.expanded = (uint8_t)(p->cfg.ask_shaping != 0);
    c.nonlinear = (uint8_t)(p->cfg.ask_nonlinear != 0);
    c.scrambler = (uint8_t)far_poly(p);
    for (k = 0; k < 3; k++) { c.h[k][0] = p->cfg.ask_h[k][0]; c.h[k][1] = p->cfg.ask_h[k][1]; }
    if (v34_params_init(&p->rpar, &c) != 0) { fail(p, DX_ERR_RATE); return; }
    v34_rx_init(&p->vrx, &p->rpar, put_data, p);
    p->rx_dg = (int32_t)v34_gain_q14(p->rpar.energy_tx_q8);
    d = Q_SNR0_DB10 + 30 * p->rx_rate / p->cfg.sym_rate - Q_SLACK_DB10;
    p->mse_limit = 65536 >> (d / 30);
    p->mse_limit -= p->mse_limit * (d % 30) / 60;
    p->q_bad = p->q_dead = 0;
    p->rxmode = RX_DATA;
    p->rx_starts++;
    p->seq_mp = 0;
    p->quiet = 0;
    sdet_init(&p->sdet, p->cfg.sym_rate, p->cfg.rx_car_num, p->cfg.rx_car_den);    /* watches for S from here on */
}

/* ---- rate renegotiation ---- */

static void reneg_begin(v34dx_t *p)
{
    if (!p->in_reneg) { p->in_reneg = 1; neg_reset(p); }
}

/* S for 128 T, S-bar for 16 T, a short TRN, MP: all with four points (11.6) */
static void start_reneg_tx(v34dx_t *p)
{
    reneg_begin(p);
    p->tx_ready = 0;
    p->phase4 = 1; p->tx_pts = 4; p->tx_g = 16384;
    p->trn4_syms = (int32_t)RENEG_TRN_MS * p->cfg.sym_rate / 1000;
    p->tx_next = 0;
    tx_go(p, DXS_S);
    p->t_neg = p->now; enter(p, DX_P4_TRN);
}

/* the receiver's error has been above what the rate needs for a second */
static void rate_down(v34dx_t *p)
{
    int32_t snr = probe_db10(65536, p->rx.mse > 0 ? p->rx.mse : 1) - Q_MARGIN_DB10;
    int32_t b10 = (snr - Q_SNR0_DB10) * 10 / 30;
    int n = (int)(((int64_t)p->cfg.sym_rate * b10) / 10) / 2400, cur = p->rx_rate / 2400;
    if (n >= cur) n = cur - 1;
    if (n < (p->cfg.sym_rate == 2400 ? 1 : 2)) { retrain(p, 1); return; }
    p->line_n = n;
    set_mp_limits(p, 0);
    p->rate_drops++;
    start_reneg_tx(p);
}

static void s_heard(v34dx_t *p)
{
    p->rx_ready = 0;
    reneg_begin(p);
    sres_resume(&p->sres);
    p->rxmode = RX_SRES;
    if (p->state == DX_DATA) enter(p, DX_RN_S);
}

/* ---- echo: delay search and training during the TRN of Phase 3 ---- */

static void ec_schedule(v34dx_t *p, int16_t rxs)
{
    int32_t n = ++p->trn_t, from = EC_SETTLE;
    if (p->cfg.echo_delay < 0) {
        /* how late is our echo (ecdly.h)? On the samples as they are */
        if (ecdly_measure(&p->ecd, n - EC_SETTLE, rxs, 0)) ec_init(&p->ec);
        from += ECDLY_SAMPLES;
    }
    if (n == from) {
        /* the far end's echo of our signal, a round trip later */
        int lag;
        if (p->cfg.rtd_has_io) {
            p->cfg.rtd_has_io = 0;
            p->cfg.rtd = p->cfg.rtd > p->ecd.delay + 8 ? p->cfg.rtd - p->ecd.delay - 8 : 0;
        }
        lag = p->ecd.delay + p->cfg.rtd - FAR_LEAD;
        p->far_on = p->cfg.rtd > FAR_MIN_RTD && lag + EC_NTAP < ECDLY_HIST - 8;
    }
    /* two cancellers on one error share the step: each at full step would
     * be one filter at twice the step NLMS can take */
    if (n < from) p->ec_mu = -1;
    else if (n < from + EC_FAST) p->ec_mu = p->far_on ? 1 : 0;
    else p->ec_mu = 3;
}

/* ---- receiver ---- */

/* silent or sending J: 1 when the S-to-S-bar transition before PP has come */
static int wait_s(v34dx_t *p, int16_t x)
{
    int32_t d;
    (void)qamrx_tick(&p->rx, x);
    if (p->md_wait > 0) {
        if (--p->md_wait == 0) { sdet_init(&p->sdet, p->cfg.sym_rate, p->cfg.rx_car_num, p->cfg.rx_car_den); p->sdet.div = 16; }
        return 0;
    }
    if (!sdet_tick(&p->sdet, x)) return 0;
    if (p->cfg.md_len > 0 && !p->md_done) { p->md_done = 1; p->md_wait = MS(p->cfg.md_len * 35); return 0; }
    /* PP(0) is centred 16.5 symbols after the S / S-bar boundary, and the
     * event came SDET_DELAY samples after it */
    d = (int32_t)(((int64_t)132000 * 256) / p->cfg.sym_rate) - (SDET_DELAY(&p->sdet) + 1) * 256;
    qamrx_sync(&p->rx, d);
    v34trn_init(&p->ref, p->cfg.answer ? V34T_GPC : V34T_GPA);
    p->rxmode = RX_KNOWN;
    return 1;
}

static int16_t clip16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

static void rx_symbol(v34dx_t *p)
{
    qamrx_t *r = &p->rx;
    int32_t dr, di;
    int n = r->nsym - 1;

    p->zpw += (((int64_t)r->zr * r->zr + (int64_t)r->zi * r->zi) - p->zpw) / 16;
    switch (p->rxmode) {
    case RX_KNOWN:
        if (n < V34_PP_LEN) v34_pp(n, &dr, &di); else v34trn_next(&p->ref, &dr, &di);
        qamrx_update(r, dr, di, QRX_EQ | QRX_CARRIER | (n > 300 ? QRX_TIMING : 0), n < 600 ? 10 : 11);
        if (n >= V34_PP_LEN + TRN_KNOWN - 1) {
            v34_tdem_init(&p->tdem, far_poly(p), 4);
            p->rx16 = 0; p->seq_j = 1; p->seq_mp = 0; p->got_j = p->got_jp = 0; p->j_type = 0;
            p->fn = p->ones = 0;
            p->rxmode = RX_SEQ;
        }
        break;
    case RX_SEQ: {
        /* decisions on 4 or 16 points; TRN, J, J', MP and E all look like this */
        v34_pt_t pt; unsigned bits; int k, nb = p->rx16 ? 4 : 2;
        if (p->rx16) {
            int32_t lev = (int32_t)(((int64_t)QRX_REF * v34_gain_q14(V34_ENERGY_16PT_Q8)) >> 14);
            int qr = r->zr >= 2 * lev ? 3 : r->zr >= 0 ? 1 : r->zr >= -2 * lev ? -1 : -3;
            int qi = r->zi >= 2 * lev ? 3 : r->zi >= 0 ? 1 : r->zi >= -2 * lev ? -1 : -3;
            dr = qr * lev; di = qi * lev;
            pt.re = (int16_t)(qr * V34_ONE); pt.im = (int16_t)(qi * V34_ONE);
            qamrx_update(r, dr, di, QRX_EQ | QRX_CARRIER | QRX_TIMING | QRX_DENSE, 12);
        } else {
            dr = r->zr < 0 ? -QRX_REF : QRX_REF; di = r->zi < 0 ? -QRX_REF : QRX_REF;
            pt.re = (int16_t)(dr < 0 ? -V34_ONE : V34_ONE); pt.im = (int16_t)(di < 0 ? -V34_ONE : V34_ONE);
            qamrx_update(r, dr, di, QRX_EQ | QRX_CARRIER | QRX_TIMING, 12);
        }
        bits = v34_tdem_put(&p->tdem, pt);
        for (k = 0; k < nb && p->rxmode == RX_SEQ; k++) seq_bit(p, (int)((bits >> k) & 1));
        break;
    }
    case RX_SRES:
        if (sres_symbol(&p->sres, r)) {
            /* TRN, then MP: scrambler and differential encoder of the far
             * transmitter start from zero at TRN; the descrambler finds its
             * way within 23 bits whatever it sees first */
            int pts = (!p->in_reneg && p->cfg.ask_16pt) ? 16 : 4;
            v34_tdem_init(&p->tdem, far_poly(p), pts);
            p->rx16 = pts == 16;
            p->seq_j = 0; p->seq_mp = 1; p->fn = p->ones = 0;
            p->rxmode = RX_SEQ;
        }
        break;
    case RX_DATA: {
        v34_pt_t in, tent, sent;
        int64_t g = p->rx_dg;
        in.re = clip16((int32_t)(((int64_t)r->zr * 512) / g)); in.im = clip16((int32_t)(((int64_t)r->zi * 512) / g));
        in = v34_nl_decode(&p->rpar, in);
        v34_rx_put(&p->vrx, in, &tent);
        sent = v34_nl_encode(&p->rpar, tent);
        dr = (int32_t)(((int64_t)sent.re * g) / 512); di = (int32_t)(((int64_t)sent.im * g) / 512);
        qamrx_update(r, dr, di, QRX_EQ | QRX_CARRIER | QRX_TIMING | QRX_DENSE, 13);
        if (!p->rx_ready && p->vrx.b1_bits >= p->rpar.map.N) {
            if (p->vrx.b1_errors * 8 > p->vrx.b1_bits) { fail(p, DX_ERR_B1); return; }
#ifdef P34D_TRACE
            printf("          V.34 B1 good: %d errors in %d bits, mse %d\n", (int)p->vrx.b1_errors, (int)p->vrx.b1_bits, (int)r->mse);
#endif
            p->rx_ready = 1;
        }
        if (p->rx_ready && !p->in_reneg) {
            if (r->mse > Q_DEAD_MSE) { if (++p->q_dead > Q_BAD_SYMS) { retrain(p, 1); return; } }
            else p->q_dead = 0;
            if (r->mse > p->mse_limit) { if (++p->q_bad > Q_BAD_SYMS) { p->q_bad = 0; rate_down(p); return; } }
            else p->q_bad = 0;
        }
        if (p->zpw < (int64_t)QRX_REF * QRX_REF / 8) {
            if (++p->quiet > CD_LOSS_SYMS) fail(p, DX_ERR_CARRIER);
        } else {
            p->quiet = 0;
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
        x = ec_tick(&p->fec, ecdly_ref(&p->ecd, p->cfg.rtd - FAR_LEAD), x, p->ec_mu);
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
        if (qamrx_tick(&p->rx, x)) rx_symbol(p);
        if (p->state == DX_FAILED || p->state == DX_RETRAIN) return y;
        if (p->rxmode == RX_DATA) {
            (void)sdet_tick(&p->sdet, x);
            if (p->sdet.present >= SDET_PRESENT) s_heard(p);
        }
    }
    if (p->state >= DX_P4_TRN && tonedet_tick(&p->tone, x)) { retrain(p, 0); return y; }

    /* transmitter sequencing and time limits */
    switch (p->state) {
    case DX_P3_SIL:
        if (p->t >= p->cfg.lead) { tx_go(p, DXS_S); enter(p, DX_P3_TX); }
        break;
    case DX_P3_WAIT_S:
        if (p->t > MS(T_FIRST_S_MS) + MS(p->cfg.md_len * 35)) fail(p, DX_ERR_NO_S);
        break;
    case DX_P3_TX:
        if (p->txsig == DXS_TRN) ec_schedule(p, rxs);
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
        } else if (p->t > MS(T_S_AFTER_J) + p->cfg.rtd + MS(p->cfg.md_len * 35)) {
            retrain(p, 1);
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
                p->seq_mp = 1; p->fn = p->ones = 0;
                tx_go(p, DXS_S);
                p->t_neg = p->now; enter(p, DX_P4_TRN);
            } else {
                /* 11.3.1.1.3: our S, S-bar, PP, TRN, J; nothing to receive meanwhile */
                p->rxmode = RX_HOLD;
                tx_go(p, DXS_S);
                enter(p, DX_P3_TX);
            }
        } else if (p->t > MS(T_FAR_J_MS) + 2 * p->cfg.rtd) {
            retrain(p, 1);
        }
        break;
    case DX_P4_TRN:
    case DX_P4_MP:
    case DX_P4_E:
        /* 11.4.2.x.2: E (and here B1 too) within 2500 ms and three round trips */
        lim = p->in_reneg ? MS(T_RENEG_MS) : MS(T_PHASE4_MS) + 3 * p->cfg.rtd;
        if (p->now - p->t_neg > lim) { retrain(p, 1); break; }
        if (p->state == DX_P4_TRN) {
            if (p->txsig == DXS_TRN4 && p->txn >= p->trn4_syms && !p->tx_next) {
                set_mp_limits(p, !p->in_reneg);
                p->tx_next = 1;
                enter(p, DX_P4_MP);
            }
        } else if (p->state == DX_P4_MP) {
            if (p->txsig == DXS_E || p->txsig == DXS_DATA) enter(p, DX_P4_E);
        } else if (p->tx_ready && p->rx_ready) {
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
    start_reneg_tx(p);
    return 0;
}

int v34dx_cleardown(v34dx_t *p)
{
    if (p->state != DX_DATA) return -1;
    p->clearing = 1;
    start_reneg_tx(p);
    return 0;
}

int v34dx_retrain(v34dx_t *p)
{
    if (p->state < DX_P4_TRN || p->state >= DX_RETRAIN) return -1;
    retrain(p, 1);
    return 0;
}

static int loss_db10(const ec_t *c)
{
    int64_t n = ec_norm(c);
    return n > 0 ? probe_db10((int64_t)1 << 48, n) : -1;
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
        e->far_delay = p->ecd.delay + p->cfg.rtd - FAR_LEAD;
        e->far_peak = ec_peak(&p->fec);
        e->far_loss_db10 = loss_db10(&p->fec);
    } else {
        e->far_delay = e->far_peak = e->far_loss_db10 = -1;
    }
}
