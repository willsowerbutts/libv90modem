#include <string.h>
#include "answer.h"
#include "pcmenc.h"
#include "v21_bpf.h"
#include "bell103_bpf.h"
#include "../v90/v90_pcm.h"

#define MS(ms) ((int32_t)(ms) * 8)
#define MAX_RETRAINS 3       /* V.90 retrains in a row (one is forgotten per minute of data mode);
                                K56flex's: the third in a row ends the call (all forgotten
                                once data mode holds K56_RT_DATA_MS) */
#define K56_RT_DATA_MS  2000 /* K56flex data mode this long: its retrains are not "in a row" */
#define K56_RT_MS       9000 /* K56flex: the client's retrain to our Phase 2's end */

#define V22_TOTAL_MS     30000   /* give up on V.22bis start-up */
#define V22_LEGACY_MS     8000   /* pre-V.8 caller silent this long: try V.21 */
#ifndef V22_BELL_MS
#define V22_BELL_MS       4000   /* silent this long: the Bell answer tone from then on */
#endif
#define V21_WAIT_MS      20000
#define V21_CD_ON_MS       100
#define CD_LOSS_MS         400
/* INFO0d maximum digital modem transmit power: -0.5 dBm0 - 0.5 dB * code;
 * 23 (ANS_MAX_TX_CODE) is the -12 dBm0 of the public network rules. A run
 * time setting now (answer_cfg.max_tx_code); V90_MAX_TX_CODE remains as the
 * compile-time default of that setting. */
/* The CPd design of a V.92 start-up takes one step every so many samples:
 * at one per sample the unit's ARM926 fell behind the line by 0.3 s in the
 * first calls with a real V.92 modem (v92/README.md, reading 56). At four
 * the design took 4.5 to 7.5 s, and a U.S. Robotics 5686 waits 3.5 s for
 * CPd after its SUVu; at two it is done in 2 to 3 s and the line driver
 * counted no late block in a call (v90d's PROGRESS.md, 2026-11-16). */
#ifndef V92_DESIGN_PACE
#define V92_DESIGN_PACE 2
#endif
#ifndef V90_MAX_TX_CODE
#define V90_MAX_TX_CODE ANS_MAX_TX_CODE
#endif
#define V21_CD_AMP         110   /* about -46 dBm0, as in v8ans */
#define BELL103_MARK_BLOCKS 20   /* 200 ms of the caller's 1270 Hz and little else */
#define BELL103_1270 ((uint32_t)(((uint64_t)BELL103_O_MARK << 32) / 8000))
#if V21_BPF_N != BELL103_BPF_N
#error "the two receive band-passes share one loop"
#endif

static void bpf_reset(answer_t *a, const int16_t *taps)
{
    for (a->bpos = 0; a->bpos < 128; a->bpos++) a->bpf[a->bpos] = 0;
    a->bpos = 0; a->bpf_taps = taps;
}

static void start_v22(answer_t *a)
{
    v22m_init(&a->v22, V22_ANSWERER, 1, a->cfg.level, a->cfg.getbit, a->cfg.putbit, a->cfg.user);
    bpf_reset(a, bell103_bpf);
    tonedet_init(&a->bell, BELL103_1270);
    v21mod_init(&a->fmod, BELL103_A_MARK, BELL103_A_SPACE, 8000, 300, (int16_t)(a->cfg.level * 181 / 128));
    a->via_bell = 0;
    a->phase = ANS_V22; a->t = 0;
}

/* 300 bit/s FSK: V.21 (we send the high channel) or, bell = 1, Bell 103
 * (we send 2225 / 2025 Hz and listen on 1270 / 1070 Hz) */
static void start_fsk(answer_t *a, int bell)
{
    /* peak amplitude of a sine with the configured RMS */
    v21mod_init(&a->fmod, bell ? BELL103_A_MARK : V21H_MARK, bell ? BELL103_A_SPACE : V21H_SPACE, 8000, 300, (int16_t)(a->cfg.level * 181 / 128));
    v21demod_init(&a->fdem, bell ? BELL103_O_MARK : V21L_MARK, bell ? BELL103_O_SPACE : V21L_SPACE, 8000, 300);
    arx_init(&a->uart, 8000, 300);
    a->cd_thresh = v21demod_energy_of(&a->fdem, V21_CD_AMP);
    a->fbit = 1; a->cd_run = 0; a->fsk_mod = bell ? ANS_MOD_BELL103 : ANS_MOD_V21;
    bpf_reset(a, bell ? bell103_bpf : v21_bpf);
    tonedet_init(&a->bell, BELL103_1270);
    a->phase = ANS_V21; a->t = 0;
}

static void start_v21(answer_t *a) { start_fsk(a, 0); }

/* Keep the echo of our own channel (1650 / 1850 Hz, or 2025 / 2225 Hz in
 * Bell 103) out of the receiver: its correlators alone reject it by less
 * than 20 dB, which an ordinary hybrid (and nobody calling) turned into a
 * carrier indication. */
static int16_t v21_bandpass(answer_t *a, int16_t x)
{
    int32_t acc = 0;
    int k;
    a->bpf[a->bpos] = x;
    for (k = 0; k < V21_BPF_N; k++) acc += (int32_t)a->bpf[(a->bpos - k) & 127] * a->bpf_taps[k] >> 4;
    a->bpos = (a->bpos + 1) & 127;
    acc >>= 11;
    return (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
}

static void fail(answer_t *a) { a->phase = ANS_FAILED; a->connected = 0; a->t = 0; }

/* x2 is offered with V.34 on a mu-law line only (design "Calls made in the
 * design"; an A-law line gets V.34) */
static void x2_offer(answer_t *a)
{
    a->x2_offered = (a->cfg.modes & ANS_MODE_X2) && (a->cfg.modes & V8_MOD_V34) && !a->p2cfg.alaw;
    if (a->x2_offered) v8ans_offer_x2(&a->v8, a->p2cfg.alaw);
    else a->v8.x2_offer = 0;
}

/* K56flex is offered with V.34 on a mu-law line only (k56 design, decision
 * 17): then V.8bis comes first. Settled before the first sample. */
static void k56_offer(answer_t *a)
{
    a->k56_offered = (a->cfg.modes & ANS_MODE_K56) && (a->cfg.modes & V8_MOD_V34) && !a->p2cfg.alaw;
    if (a->t != 0 || (a->phase != ANS_V8 && a->phase != ANS_V8BIS)) return;
    if (a->k56_offered) {
        v8bis_init(&a->u.v8b, (int16_t)(a->cfg.level * 181 / 128), V8B_NS_DEFAULT);
        a->phase = ANS_V8BIS;
    } else {
        a->phase = ANS_V8;
    }
}

void answer_init(answer_t *a, const answer_cfg *cfg)
{
    v8_menu_t local;
    a->cfg = *cfg;
    local.call_function = V8_CALLF_DATA;
    local.modes = (uint16_t)cfg->modes;
    local.n_mod_octets = 3;
    local.has_pcm = (cfg->modes & ANS_MODE_V90) != 0;
    /* V.8 6.3: PCM rides on V.34, so the V.34 bit goes out with it; the
     * flag says whether V.34 itself may be selected as the data mode */
    local.pcm = !local.has_pcm ? 0 : (cfg->modes & V8_MOD_V34) ? V8_PCM_DIGITAL : (V8_PCM_DIGITAL | V8_PCM_NO_V34_DATA);
    if (local.has_pcm) local.modes |= V8_MOD_V34;
    a->echo_delay = 0; a->max_down = a->max_up = 0;
    v32_aadet_init(&a->aadet);
    a->p2cfg.tx_dbm0 = -12; a->p2cfg.max_tx_code = cfg->max_tx_code >= 1 && cfg->max_tx_code <= 31 ? cfg->max_tx_code : V90_MAX_TX_CODE; a->p2cfg.alaw = 0; a->p2cfg.io_delay = 0;
    a->p2cfg.up_sr_mask = (1 << 4) | (1 << 3);
    a->p2cfg.up_hi_carrier = (1 << 4) | (1 << 3);
    /* LAPM is echoed when the caller asks for it (V.8 6.4). Offering V.90 as
     * the digital modem only means something to the caller if we also say
     * that we sit on a digital connection (6.5, bit b7): a Conexant client
     * that got JM with "digital modem" but "analogue access" started V.34
     * (recorded on the box, 2026-11-15). */
    local.has_prot = 1; local.prot = V8_PROT_LAPM;
    local.has_access = local.has_pcm; local.access = local.has_pcm ? V8_ACC_DIGITAL : 0;
    v8ans_init(&a->v8, 8000, (int16_t)(cfg->level * 181 / 128), &local);
    a->phase = ANS_V8; a->t = 0; a->used_v8 = 0;
    a->connected = 0; a->modulation = ANS_MOD_NONE; a->bitrate = 0; a->rx_bitrate = 0; a->pcm = 0xFF; a->cleared = 0; a->retrains = 0; a->retraining = 0; a->rt_recent = 0; a->rt_clock = 0;
    a->fbit = 1; a->cd_run = 0; a->cd_thresh = 0;
    a->info0a.c1664 = 0; a->v34.from_v90 = 0;
    /* V.92 9.1: Phase 1 is that of V.90, and V.8 cannot say V.92 (the NOTE
     * there; Table 5/V.8 has one bit for "V.90 or V.92 digital modem"), so
     * nothing above depends on ANS_MODE_V92. Phase 2 says it. */
    a->v92_both = 0; a->v92_data = 0;
    a->pcm_up = local.has_pcm && (cfg->modes & ANS_MODE_V92) != 0;
    a->x2_failed = 0;
    x2_offer(a);
    a->k56_agreed = a->k56_failed = a->k56_lost = 0;
    k56_offer(a);
}

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
    if ((sr != 3 && sr != 4) || p34d_init(&a->p34, &c) != 0) { fail(a); return; }
    a->phase = ANS_P3; a->t = 0;
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
    if (v92d_init(&a->u.v92, &c) != 0) { fail(a); return; }
    a->phase = ANS_P3X; a->t = 0;
}

/* A retrain is due: the 70 ms of silence, then Phase 2 again (V.90 9.5.1,
 * V.92 9.7.1). A line that needs MAX_RETRAINS in a row is no line. */
static int begin_retrain(answer_t *a)
{
    if (a->rt_recent >= MAX_RETRAINS) { fail(a); return -1; }
    a->retrains++; a->rt_recent++; a->retraining = 1;
    a->phase = ANS_RETRAIN; a->t = 0;
    return 0;
}

static void v34_cfg(const answer_t *a, v34ans_cfg *c)
{
    c->level = a->cfg.level;
    c->law = a->p2cfg.alaw ? 2 : 1;
    c->io_delay = a->echo_delay;
    c->sr_mask = 0;
    c->max_tx_rate = a->max_down; c->max_rx_rate = a->max_up;
    c->getbit = a->cfg.getbit; c->putbit = a->cfg.putbit; c->user = a->cfg.user;
    c->x2 = v8ans_x2(&a->v8);
    c->k56 = a->k56_agreed;
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
    int sr = a->p2.info1a.up_sr;
    /* a retrain's Phase 2 has no INFO0: what the caller's INFO0a said holds */
    if (a->p2.n_info0d > 0) a->info0a = a->p2.info0a;
    v34_cfg(a, &c);
    if (!(a->cfg.modes & V8_MOD_V34) || sr > 5 || !((a->p2cfg.up_sr_mask >> sr) & 1)
        || v34ans_init_call(&a->v34, &c, &a->p2.info1a, (a->p2cfg.up_hi_carrier >> sr) & 1,
                            a->info0a.c1664, a->p2.rtd) != 0) { fail(a); return; }
    a->phase = ANS_V34; a->t = 0;
}

/* The x2 client's marker ended Phase 2 (p2ans P2N_X2): its symbol rate and
 * carrier, and the round trip, from there; silent until its J. A marker x2d
 * cannot serve ends the call, said as x2's. */
static void start_x2(answer_t *a)
{
    x2d_cfg c;
    memset(&c, 0, sizeof c);
    c.marker = a->v34.p2.x2_marker;
    c.rtd = a->v34.p2.rtd;
    c.echo_delay = a->echo_delay;
    c.max_up_rate = a->max_up;
    c.getbit = a->cfg.getbit; c.putbit = a->cfg.putbit; c.user = a->cfg.user;
    a->phase = ANS_X2; a->t = 0; a->pcm = 0x7f;
    if (x2d_init(&a->u.x2, &c) != 0) { a->x2_failed = 1; fail(a); }
}

/* Our flagged INFO1a ended Phase 2 (p2ans P2N_K56): no way back to V.34.
 * The upstream's carrier (our choice in INFO1a) and the round trip from
 * there; a configuration k56d cannot serve ends the call, said as
 * K56flex's. */
static void start_k56(answer_t *a)
{
    k56d_cfg c;
    memset(&c, 0, sizeof c);
    c.rtd = a->v34.p2.rtd;
    c.echo_delay = a->echo_delay;
    c.hi_carrier = a->v34.p2.info1a.hi_carrier;
    c.max_up_rate = a->max_up; c.max_down_rate = a->max_down;
    c.getbit = a->cfg.getbit; c.putbit = a->cfg.putbit; c.user = a->cfg.user;
    a->phase = ANS_K56; a->t = 0; a->pcm = 0xff;
    if (a->v34.p2.sym_rate != 3200 || k56d_init(&a->u.k56, &c) != 0) { a->k56_failed = 1; fail(a); }
}

/* V.8 selected V.34 as the data mode: INFO0a starts right after the 75 ms
 * of silence that v8ans has just finished (V.34 11.2.1.2.1) */
static void start_v34(answer_t *a)
{
    v34ans_cfg c;
    v34_cfg(a, &c);
    v34ans_init(&a->v34, &c);
    a->phase = ANS_V34; a->t = 0;
}

/* V.8 selected V.32 (item 3 covers V.32bis), or a caller without V.8 sent
 * AA: AC goes out now; the 75 ms of silence are behind us either way.
 * V.32bis has one rate for both directions: the lower ceiling counts. */
static void start_v32(answer_t *a)
{
    static const int rate[5] = { 4800, 7200, 9600, 12000, 14400 };
    v32_cfg c;
    int cap = a->max_down, i;
    if (a->max_up > 0 && (cap <= 0 || a->max_up < cap)) cap = a->max_up;
    c.level = a->cfg.level;
    c.rates = V32_R4800;
    for (i = 1; i < 5; i++) if (cap <= 0 || rate[i] <= cap) c.rates |= 1u << i;
    c.getbit = a->cfg.getbit; c.putbit = a->cfg.putbit; c.user = a->cfg.user;
    c.echo_delay = a->echo_delay < 0 ? 0 : a->echo_delay;
    c.no_auto_retrain = 0;
    v32_init(&a->u.v32, &c);
    a->phase = ANS_V32; a->t = 0;
}

void answer_v90_cfg(answer_t *a, const p2d_cfg *cfg) { a->p2cfg = *cfg; x2_offer(a); k56_offer(a); }

static int16_t tick_v21(answer_t *a, int16_t rx)
{
    int level, c, done, i;
    int16_t out;

    int16_t x = v21_bandpass(a, rx);

    /* waiting for a caller without V.8 in V.21: one that sends 1270 Hz is a
     * Bell 103 modem (a V.21 caller's mark is 980 Hz). Decided in 60 ms,
     * before the V.21 receiver's 100 ms take the tone for its carrier. */
    if (!a->connected && !a->used_v8 && a->fsk_mod == ANS_MOD_V21 && (a->cfg.modes & ANS_MODE_BELL103)
        && tonedet_tick(&a->bell, x)) { start_fsk(a, 1); return 0; }
    level = v21demod_tick(&a->fdem, x);
    if (a->fdem.energy < a->cd_thresh) {
        level = 1;
        if (a->cd_run > 0) a->cd_run = 0;
        if (a->connected && --a->cd_run < -MS(CD_LOSS_MS)) { fail(a); return 0; }
    } else {
        if (a->cd_run < 0) a->cd_run = 0;
        if (a->cd_run < MS(V21_CD_ON_MS)) a->cd_run++;
        else if (!a->connected) { a->connected = 1; a->modulation = a->fsk_mod; a->bitrate = 300; }
    }
    c = arx_tick(&a->uart, level);
    if (c >= 0 && a->connected) {            /* hand the character on as start-stop bits */
        a->cfg.putbit(a->cfg.user, 0);
        for (i = 0; i < 8; i++) a->cfg.putbit(a->cfg.user, (c >> i) & 1);
        a->cfg.putbit(a->cfg.user, 1);
    }
    out = v21mod_tick(&a->fmod, a->fbit, &done);
    if (done) a->fbit = a->connected ? (a->cfg.getbit(a->cfg.user) & 1) : 1;
    if (!a->connected && a->t > MS(V21_WAIT_MS)) fail(a);
    return out;
}

int16_t answer_tick(answer_t *a, int16_t rx)
{
    int16_t tx = 0;
    a->t++;
    switch (a->phase) {
    case ANS_V8BIS:
        /* K56flex's V.8bis; V.8 follows from its own silence, whatever came
         * of it. An agreement turns x2 off for the call (k56 design,
         * decision 5): no x2 JM, no x2 INFO0a. */
        if (v8bis_tick(&a->u.v8b, rx, &tx) != V8B_DONE) break;
        a->k56_agreed = v8bis_result(&a->u.v8b) == V8B_RES_AGREED;
        if (a->k56_agreed) a->v8.x2_offer = 0;
        a->phase = ANS_V8; a->t = 0;
        break;

    case ANS_V8:
        /* V.32bis 6.1: a caller without V.8 answers the answer tone with a
         * steady 1800 Hz (only listened for during the tone: our own JM has
         * 1850 Hz in it) */
        if ((a->cfg.modes & V8_MOD_V32) && a->v8.state == V8A_ANSAM && v32_aadet_tick(&a->aadet, rx)) {
            a->phase = ANS_V32_SIL; a->t = 0;
            break;
        }
        if (v8ans_tick(&a->v8, rx, &tx) != V8A_DONE) break;
        if (a->v8.result == V8A_RES_NO_CM) {           /* pre-V.8 caller */
            if (a->cfg.modes & V8_MOD_V22) start_v22(a);
            else if (a->cfg.modes & V8_MOD_V21) start_v21(a);
            else fail(a);
            break;
        }
        a->used_v8 = 1;
        if (a->v8.selected == V8_SEL_V90) {            /* V.90 9.2.1.1.1: INFO0d after the 75 ms */
            p2d_init(&a->p2, &a->p2cfg);
            /* 9.3/V.92: bit 27 of INFO0d; the rest follows from INFO0a */
            if (a->cfg.modes & ANS_MODE_V92) p2d_offer_v92(&a->p2, a->pcm_up);
            a->phase = ANS_P2; a->t = 0;
        }
        else if (a->v8.selected == 1 && (a->cfg.modes & V8_MOD_V34)) start_v34(a);
        else if (a->v8.selected == 3 && (a->cfg.modes & V8_MOD_V32)) start_v32(a);
        else if (a->v8.selected == 4) start_v22(a);
        else if (a->v8.selected == 12) start_v21(a);
        else fail(a);                                  /* nothing in common */
        break;

    case ANS_V22:
        /* A Bell 103 caller takes our unscrambled ones (2250 Hz is in them)
         * or the answer tone before them for its carrier and replies with a
         * steady 1270 Hz. Listened for behind the band-pass that keeps our
         * own signal out: its echo may be the louder of the two. A V.22
         * caller's scrambled signal is spread over the band and a V.22bis
         * caller's S1 is 900 and 1500 Hz: neither is one tone at 1270 Hz. */
        if (!a->connected && !a->used_v8 && (a->cfg.modes & ANS_MODE_BELL103) && a->v22.state == V22S_WAIT) {
            (void)tonedet_tick(&a->bell, v21_bandpass(a, rx));
            if (a->bell.blocks >= BELL103_MARK_BLOCKS) { start_fsk(a, 1); break; }
        }
        {
            /* Nobody has answered our unscrambled ones: a Bell 212A (or a
             * Bell 103) caller may be waiting for 2225 Hz and nothing else.
             * It gets that in place of the ones while the V.22bis modem
             * goes on listening; what a 212A caller then sends is what a
             * V.22 caller sends, and the start-up continues from there. */
            int tone = !a->connected && !a->used_v8 && (a->cfg.modes & ANS_MODE_BELL212A)
                       && a->v22.state == V22S_WAIT && a->t > MS(V22_BELL_MS);
            tx = v22m_tick(&a->v22, rx);
            if (tone) {
                int done;
                if (a->v22.state == V22S_WAIT) tx = v21mod_tick(&a->fmod, 1, &done);
                else a->via_bell = 1;
            }
        }
        if (!a->connected) {
            if (a->v22.tx_ready && a->v22.rx_ready) {
                a->connected = 1; a->bitrate = a->v22.bitrate;
                a->modulation = a->bitrate == 2400 ? ANS_MOD_V22BIS : a->via_bell ? ANS_MOD_BELL212A : ANS_MOD_V22;
                a->cd_run = 0;
            } else if (!a->used_v8 && (a->cfg.modes & V8_MOD_V21) && a->v22.state == V22S_WAIT
                       && !a->v22.rx.carrier && a->t > MS(V22_LEGACY_MS)) {
                start_v21(a);
            } else if (a->t > MS(V22_TOTAL_MS) || a->v22.state == V22S_FAILED) {
                fail(a);
            }
        } else {
            if (a->v22.state == V22S_FAILED) fail(a);      /* a retrain (V.22bis 6.4) that never completed */
            else if (a->v22.rx.carrier) a->cd_run = 0;
            else if (++a->cd_run > MS(CD_LOSS_MS)) fail(a);
        }
        break;

    case ANS_V21:
        tx = tick_v21(a, rx);
        break;

    case ANS_P2:
        tx = p2d_tick(&a->p2, rx);
        if (a->p2.state == P2D_DONE) {
            /* a retrain's Phase 2 has no INFO0: what the first one found holds */
            if (a->p2.n_info0d > 0) a->v92_both = a->p2.v92 && a->p2.info0a.v92;
            if (a->p2.mode == P2D_MODE_V92) start_v92(a);
            else if (a->p2.info1a.v90) start_p34(a);
            else start_v34_call(a);
        }
        else if (a->p2.state == P2D_FAILED) fail(a);
        break;

    case ANS_P3X:
    case ANS_V92: {
        v92d_t *m = &a->u.v92;
        a->pcm = v92d_tick(m, rx);
        tx = (int16_t)v90_octet_linear(a->p2cfg.alaw, a->pcm);
        if (m->state == V92D_FAILED && (m->error == V92D_ERR_CLEARDOWN || m->error == V92D_ERR_HANGUP
                                        || m->error == V92D_ERR_CARRIER)) {
            if (m->error == V92D_ERR_CLEARDOWN) a->cleared = 1;     /* 9.11 */
            if (m->error == V92D_ERR_HANGUP) a->cleared = 2;
            fail(a); tx = 0;                                        /* or the caller is gone */
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
            (void)begin_retrain(a); tx = 0;
        } else if (m->state == V92D_DATA) {
            /* also after a rate renegotiation */
            a->connected = 1; a->modulation = ANS_MOD_V92;
            a->bitrate = m->down_rate; a->rx_bitrate = m->up_rate;
            a->phase = ANS_V92; a->retraining = 0; a->v92_data = 1;
            if (++a->rt_clock >= MS(60000)) { a->rt_clock = 0; if (a->rt_recent > 0) a->rt_recent--; }
        }
        break;
    }

    case ANS_P3:
    case ANS_V90:
        a->pcm = p34d_tick(&a->p34, rx);
        tx = (int16_t)v90_octet_linear(a->p2cfg.alaw, a->pcm);
        if (a->p34.state == P34_RETRAIN && a->rt_recent >= MAX_RETRAINS) {
            fail(a); tx = 0;                /* a line that needs this many is no line */
        } else if (a->p34.state == P34_RETRAIN) {
            a->retrains++; a->rt_recent++; a->retraining = 1;
            a->phase = ANS_RETRAIN; a->t = 0; tx = 0;
        } else if (a->p34.state == P34_FAILED) {
            if (a->p34.error == P34_ERR_CLEARDOWN) a->cleared = 1;
            if (a->p34.error == P34_ERR_HANGUP) a->cleared = 2;
            fail(a); tx = 0;
        } else if (a->p34.state == P34_DATA) {
            /* also after a rate renegotiation */
            a->connected = 1; a->modulation = ANS_MOD_V90;
            a->bitrate = a->p34.down_rate; a->rx_bitrate = a->p34.up_rate;
            a->phase = ANS_V90; a->retraining = 0;
            if (++a->rt_clock >= MS(60000)) { a->rt_clock = 0; if (a->rt_recent > 0) a->rt_recent--; }
        }
        break;

    case ANS_RETRAIN:
        /* 9.5.1: silence for 70 +- 5 ms, then Tone B and Phase 2 from 9.2.1.1.3 */
        if (a->t >= MS(70)) {
            p2d_init_retrain(&a->p2, &a->p2cfg);
            /* "any subsequent retrains shall use Phase 2 of ITU-T V.92" (9.3/V.92):
             * INFO1d of Table 17, its bit 70 saying whether PCM upstream is still on */
            if (a->v92_both) p2d_retrain_v92(&a->p2, a->pcm_up);
            a->phase = ANS_P2; a->t = 0;
        }
        break;

    case ANS_V34:
        tx = v34ans_tick(&a->v34, rx);
        if (a->v34.from_v90) {
            /* the caller chose V.34 in INFO1a: a retrain is Phase 2 of V.90
             * again, whoever began it (V.90 9.2.1.1.8), with the silence and
             * the limit of the V.90 retrain */
            if (a->v34.state == V34A_RETRAIN) {
                if (a->rt_recent >= MAX_RETRAINS) { fail(a); tx = 0; break; }
                a->retrains++; a->rt_recent++; a->retraining = 1;
                a->phase = ANS_RETRAIN; a->t = 0; tx = 0;
                break;
            }
            if (a->v34.state == V34A_DATA && ++a->rt_clock >= MS(60000)) { a->rt_clock = 0; if (a->rt_recent > 0) a->rt_recent--; }
        }
        if (a->v34.state == V34A_X2) {           /* the client's x2 marker */
            start_x2(a); tx = 0;
            break;
        }
        if (a->v34.state == V34A_K56) {          /* our flagged INFO1a: no way back to V.34 */
            start_k56(a); tx = 0;
            break;
        }
        if (a->v34.state == V34A_ENDED) {
            if (a->v34.error == V34A_ERR_CLEARDOWN) a->cleared = 1;
            if (a->v34.error == V34A_ERR_HANGUP) a->cleared = 2;
            fail(a); tx = 0;
        } else if (a->v34.state == V34A_DATA) {
            /* also after a rate renegotiation or a retrain */
            a->connected = 1; a->modulation = ANS_MOD_V34;
            a->bitrate = v34ans_tx_rate(&a->v34); a->rx_bitrate = v34ans_rx_rate(&a->v34);
            a->retraining = 0;
        } else if (a->connected) {
            /* no data moves in either, but only a retrain is a retrain */
            a->retraining = a->v34.state != V34A_RENEG;
        }
        if (!a->v34.from_v90) a->retrains = a->v34.retrains;
        break;

    case ANS_V32_SIL:
        if (a->t >= MS(75)) start_v32(a);
        break;

    case ANS_V32:
        tx = v32_tick(&a->u.v32, rx);
        if (a->u.v32.state == V32S_FAILED) { fail(a); tx = 0; break; }   /* carrier loss included */
        if (a->u.v32.connected) {
            a->connected = 1; a->modulation = ANS_MOD_V32BIS;
            a->bitrate = a->rx_bitrate = a->u.v32.bitrate;    /* may change at a retrain */
        }
        a->retraining = a->u.v32.retraining; a->retrains = a->u.v32.retrains;
        break;

    case ANS_X2: {
        x2d_t *d = &a->u.x2;
        a->pcm = x2d_tick(d, rx);
        tx = (int16_t)v90_octet_linear(0, a->pcm);
        if (d->state == X2D_ENDED) {
            /* there is no way back to V.34 once the marker came (design
             * "Calls made in the design"): a start-up that did not reach
             * data mode, or a rate change of the client's that did not
             * complete, is said as x2's; in data mode the line's loss,
             * our cleardown and the client's retrain (its Tone A, said as
             * a retrain that failed) end the call */
            if (d->error == X2D_ERR_RETRAIN) a->x2_failed = 2;     /* the client's retrain: said as a retrain that failed */
            else if (!a->connected || (d->error != X2D_ERR_CARRIER && d->error != X2D_ERR_HANGUP)) a->x2_failed = 1;
            if (d->error == X2D_ERR_HANGUP) a->cleared = 2;
            fail(a); tx = 0;
        } else if (d->state == X2D_DATA) {
            a->connected = 1; a->modulation = ANS_MOD_X2;
            a->bitrate = x2d_down_rate(d); a->rx_bitrate = x2d_up_rate(d);
            a->retraining = 0;
        } else if (a->connected) {
            a->retraining = 1;              /* the client's rate change: no data moves up */
        }
        break;
    }

    case ANS_K56: {
        k56d_t *d = &a->u.k56;
        a->pcm = k56d_tick(d, rx);
        tx = (int16_t)v90_octet_linear(0, a->pcm);
        if (d->state == K56D_ENDED) {
            /* past the point of no return (k56 design, decision 7): a
             * start-up or a renegotiation that did not reach data mode is
             * said as K56flex's; in data mode the upstream's loss, our
             * cleardown and the client's (an MP with rate 0) end it */
            if (d->error == K56D_ERR_HANGUP) a->cleared = 2;
            else if (d->error == K56D_ERR_CLEARDOWN) a->cleared = 1;     /* its renegotiation MP, rate 0 */
            else if (a->connected && d->error == K56D_ERR_CARRIER) a->k56_lost = 1;
            else a->k56_failed = 1;
            fail(a); tx = 0;
        } else if (d->state == K56D_RETRAIN) {
            /* its tone B in data mode (decision 14): retrains in a row, the
             * third ends the call as K56flex's */
            a->retrains++; a->retraining = 1; a->rt_clock = 0;
            if (++a->rt_recent >= MAX_RETRAINS) { a->k56_failed = 1; fail(a); tx = 0; break; }
            a->phase = ANS_K56RT; a->t = 0; tx = 0;
        } else if (d->state == K56D_DATA) {
            a->connected = 1; a->modulation = ANS_MOD_K56;
            a->bitrate = k56d_down_rate(d); a->rx_bitrate = k56d_up_rate(d);
            a->retraining = 0;
            /* data again: once it holds, the retrains before it are not in a row */
            if (a->rt_recent && ++a->rt_clock >= MS(K56_RT_DATA_MS)) { a->rt_clock = 0; a->rt_recent = 0; }
        } else if (a->connected) {
            a->retraining = 1;              /* the client's renegotiation or retrain */
        }
        break;
    }

    case ANS_K56RT:
        /* the client's retrain (k56 design, decision 14; V.34 11.5): 70 ms
         * of silence, then Phase 2 of a retrain with INFO1a flagged again
         * (no INFO0: its INFO0c of the first holds); our flagged INFO1a
         * starts K56flex afresh. Anything else (INFO1c 0x200, Phase 2
         * failed, too long) ends the call: no way back to V.34. */
        if (a->t <= MS(70)) {
            if (a->t == MS(70)) {
                p2ans_cfg c;
                memset(&c, 0, sizeof c);
                c.level = a->v34.cfg.level;
                c.io_delay = a->v34.cfg.io_delay > 0 ? a->v34.cfg.io_delay : 0;
                c.sr_mask = a->v34.cfg.sr_mask;
                c.max_rate = a->v34.cfg.max_rx_rate;
                c.k56 = 1;
                p2ans_init_retrain(&a->v34.p2, &c, &a->v34.info0c);
            }
            break;
        }
        tx = p2ans_tick(&a->v34.p2, rx);
        if (a->v34.p2.state == P2N_K56) {
            start_k56(a); tx = 0;
        } else if (a->v34.p2.state == P2N_DONE || a->v34.p2.state == P2N_FAILED || a->v34.p2.state == P2N_X2
                   || a->t > MS(K56_RT_MS)) {
            a->k56_failed = 1; fail(a); tx = 0;
        }
        break;

    case ANS_FAILED:
        break;
    }
    return tx;
}

void answer_v90_line(answer_t *a, int echo_delay, int max_down, int max_up)
{
    a->echo_delay = echo_delay; a->max_down = max_down; a->max_up = max_up;
}

uint8_t answer_tick_lin(answer_t *a, int16_t rx)
{
    int alaw = a->p2cfg.alaw;
    int16_t tx = answer_tick(a, rx);
    if (a->phase == ANS_P3 || a->phase == ANS_V90 || a->phase == ANS_P3X || a->phase == ANS_V92 || a->phase == ANS_X2
        || a->phase == ANS_K56) return a->pcm;
    return pcm_encode(alaw, tx);
}

uint8_t answer_tick_pcm(answer_t *a, uint8_t rx)
{
    return answer_tick_lin(a, (int16_t)v90_octet_linear(a->p2cfg.alaw, rx));
}

int answer_snr_db10(const answer_t *a)
{
    int64_t num, den;
    int v;
    if (!a->connected) return -1;
    switch (a->modulation) {
    case ANS_MOD_V90: num = 65536; den = a->p34.rx.mse; break;          /* |e|^2 / |d|^2 in Q16 */
    case ANS_MOD_V92: {
        /* the decoder's noise estimate against the mean square of the points
         * of CPd, over the six constellation frame intervals; only while that
         * decoder runs */
        const v92_map_t *mp = &a->u.v92.cpd.map;
        int j, k, n = 0;
        if (a->phase != ANS_V92 || !a->u.v92.rx_ready) return -1;
        num = 0;
        for (j = 0; j < V92_CFRAME; j++) {
            int lc = v92_lc(mp, j);
            for (k = 0; k < lc; k++) { int64_t v = v92_point(mp, j, k); num += v * v; n++; }
        }
        if (!n) return -1;
        num /= n; den = v92_dec_quality(&a->u.v92.dec);
        break;
    }
    case ANS_MOD_V34: num = 65536; den = a->v34.dx.rx.mse; break;
    case ANS_MOD_X2: num = 65536; den = a->u.x2.rx.mse; break;           /* the V.34 upstream */
    case ANS_MOD_K56: num = 65536; den = a->u.k56.rx.mse; break;         /* the V.34 upstream */
    case ANS_MOD_V22: case ANS_MOD_V22BIS:
        /* 4096 = one unit squared; the points (1 and 3 units on each axis)
         * have a mean power of 10, and the four of 1200 bit/s have just that */
        num = 10 * 4096; den = a->v22.rx.mse; break;
    default: return -1;
    }
    v = probe_db10(num, den > 0 ? den : 1);
    return v < 0 ? 0 : v > 600 ? 600 : v;
}

int answer_rtd(const answer_t *a)
{
    if (!a->connected) return -1;
    switch (a->modulation) {
    case ANS_MOD_V90: case ANS_MOD_V92: return a->p2.rtd;
    case ANS_MOD_X2: case ANS_MOD_K56: return a->v34.p2.rtd;
    /* V.34 after V.90 Phase 2 has that Phase 2's figure */
    case ANS_MOD_V34: return a->v8.selected == V8_SEL_V90 ? a->p2.rtd : a->v34.p2.rtd;
    default: return -1;
    }
}

int answer_v34_echo(const answer_t *a, v34dx_echo_t *e)
{
    if (a->phase != ANS_V34 || (a->v34.state != V34A_DATA && a->v34.state != V34A_RENEG)) return -1;
    v34dx_echo(&a->v34.dx, e);
    return 0;
}

int answer_cleardown(answer_t *a)
{
    if (a->phase == ANS_V34) return v34ans_cleardown(&a->v34);
    if (a->phase == ANS_V92) return v92d_cleardown(&a->u.v92);
    if (a->phase == ANS_X2) return x2d_cleardown(&a->u.x2);
    if (a->phase == ANS_K56) return k56d_cleardown(&a->u.k56);
    if (a->phase != ANS_V90) return -1;
    return p34d_cleardown(&a->p34);
}

int answer_retrain(answer_t *a)
{
    if (a->phase == ANS_V34) return v34ans_retrain(&a->v34);
    if (a->phase == ANS_V32) return v32_retrain(&a->u.v32);
    if (a->phase == ANS_V92) return v92d_retrain(&a->u.v92);
    if (a->phase == ANS_K56 || a->phase == ANS_K56RT) return -1;   /* we never retrain K56flex (k56 design, decision 14) */
    if (a->phase != ANS_V90) return -1;
    return p34d_retrain(&a->p34);
}

int answer_moving(const answer_t *a)
{
    if (!a->connected || a->retraining) return 0;
    if (a->phase == ANS_V90) return a->p34.tx_ready && a->p34.rx_ready;
    if (a->phase == ANS_V92) return a->u.v92.tx_ready && a->u.v92.rx_ready;
    if (a->phase == ANS_X2) return a->u.x2.state == X2D_DATA;
    if (a->phase == ANS_K56) return a->u.k56.state == K56D_DATA;
    if (a->phase == ANS_V34) return a->v34.state == V34A_DATA;
    return 1;
}

int answer_odp_bypass(const answer_t *a)
{
    /* 9.3.1/V.92: V.92 at both ends (INFO0) and LAPM in the protocol category of CM and JM */
    return a->v92_both && a->v8.cm.has_prot && a->v8.cm.prot == V8_PROT_LAPM
           && a->v8.jm.has_prot && a->v8.jm.prot == V8_PROT_LAPM;
}
