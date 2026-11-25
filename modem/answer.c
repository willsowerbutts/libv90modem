#include <string.h>
#include "ans_scheme.h"
#include "clock.h"
#include "pcmenc.h"
#include "../v90/v90_pcm.h"

/* The answering modem: init, V.8, the dispatch to the scheme of the phase
 * (modem/ans_scheme.h), the end of a call and the backstop of the phases.
 * The schemes: modem/ans_legacy.c, ans_v90.c, ans_v34.c, ans_x2.c,
 * ans_k56.c. */

#define MS(ms) ((int32_t)(ms) * 8)
/* The backstop of the phases before data mode (spec D5), above the limits
 * of their engines: a phase that runs longer ends the call. Far above a
 * normal Phase 2 (3 to 5 s), and the inner limits fire first. */
#define P2_BACKSTOP_MS 30000 /* ANS_P2, ANS_RETRAIN, V.34's Phase 2 within ANS_V34 */
#define P3_BACKSTOP_MS 60000 /* ANS_P3, ANS_P3X */

/* INFO0d maximum digital modem transmit power: -0.5 dBm0 - 0.5 dB * code;
 * 23 (ANS_MAX_TX_CODE) is the -12 dBm0 of the public network rules. A run
 * time setting now (answer_cfg.max_tx_code); V90_MAX_TX_CODE remains as the
 * compile-time default of that setting. */
#ifndef V90_MAX_TX_CODE
#define V90_MAX_TX_CODE ANS_MAX_TX_CODE
#endif

static int16_t tick_ended(answer_t *a, int16_t rx) { (void)a; (void)rx; return 0; }
/* ANS_FAILED: between engines for the echo delay */
static const ans_scheme_t ans_ended = { tick_ended, NULL, NULL, NULL, NULL, NULL, ans_mod_echo_delay, 0 };

/* the phase table: the scheme of each phase */
static const ans_scheme_t *const phase_scheme[] = {
    [ANS_V8] = &ans_v8, [ANS_V8BIS] = &ans_v8bis,
    [ANS_V22] = &ans_legacy, [ANS_V21] = &ans_legacy, [ANS_V32_SIL] = &ans_legacy, [ANS_V32] = &ans_legacy,
    [ANS_P2] = &ans_p2, [ANS_RETRAIN] = &ans_p2,
    [ANS_P3] = &ans_v90, [ANS_V90] = &ans_v90,
    [ANS_P3X] = &ans_v92, [ANS_V92] = &ans_v92,
    [ANS_V34] = &ans_v34, [ANS_X2] = &ans_x2,
    [ANS_K56] = &ans_k56, [ANS_K56RT] = &ans_k56rt,
    [ANS_FAILED] = &ans_ended
};
/* one row per phase: a new last phase without its row fails to compile */
typedef char phase_scheme_rows[sizeof phase_scheme / sizeof phase_scheme[0] == ANS_NPHASES ? 1 : -1];

void ans_enter(answer_t *a, ans_phase p) { a->phase = p; a->s = phase_scheme[p]; }

void ans_fail(answer_t *a, int why) { a->end = why; ans_enter(a, ANS_FAILED); a->connected = 0; a->t = 0; }
int ans_lost_or(const answer_t *a, int why) { return a->connected ? ANS_END_LOST : why; }
int ans_retrain_or(const answer_t *a, int why) { return a->connected && a->retraining ? ANS_END_RETRAIN_FAILED : ans_lost_or(a, why); }

void ans_rt_begin(answer_t *a, const rt_policy_t *p)
{
    if (a->rt_recent >= p->limit) { ans_fail(a, ANS_END_RETRAIN_FAILED); return; }
    a->retrains++; a->rt_recent++; a->retraining = 1;
}

void ans_rt_data_tick(answer_t *a, const rt_policy_t *p)
{
    if (++a->rt_clock < MS(p->forget_ms)) return;
    a->rt_clock = 0;
    if (a->rt_recent > 0) a->rt_recent--;
}

/* the scheme whose figures (signal to noise, round trip, echo delay between
 * engines) are the modulation's */
static const ans_scheme_t *mod_scheme(int mod)
{
    switch (mod) {
    case ANS_MOD_V21: case ANS_MOD_V22: case ANS_MOD_V22BIS: case ANS_MOD_V32BIS:
    case ANS_MOD_BELL103: case ANS_MOD_BELL212A: return &ans_legacy;
    case ANS_MOD_V90: return &ans_v90;
    case ANS_MOD_V92: return &ans_v92;
    case ANS_MOD_V34: return &ans_v34;
    case ANS_MOD_X2:  return &ans_x2;
    case ANS_MOD_K56: return &ans_k56;
    default:          return NULL;
    }
}

int ans_mod_echo_delay(const answer_t *a)
{
    const ans_scheme_t *m = mod_scheme(a->modulation);
    return m && m->echo_delay ? m->echo_delay(a) : -1;
}

/* V.34's Phase 2 (its retrains' included) is a state of the V.34 engine
 * within ANS_V34: the backstop's clock starts where it begins */
static int v34_phase2(const answer_t *a) { return a->phase == ANS_V34 && a->v34.state == V34A_PHASE2; }

/* the backstop of the phase (spec D5), samples; 0 = none: data mode, and
 * the phases their engines alone bound (V.8, x2 and K56flex start-ups,
 * ANS_K56RT's K56_RT_MS) */
static int32_t phase_budget(const answer_t *a)
{
    switch (a->phase) {
    case ANS_P2: case ANS_RETRAIN: return MS(P2_BACKSTOP_MS);
    case ANS_P3: case ANS_P3X:     return MS(P3_BACKSTOP_MS);
    case ANS_V34:                  return v34_phase2(a) ? MS(P2_BACKSTOP_MS) : 0;
    default:                       return 0;
    }
}

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
        ans_enter(a, ANS_V8BIS);
    } else {
        ans_enter(a, ANS_V8);
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
    ans_enter(a, ANS_V8); a->t = 0; a->t_phase = 0; a->used_v8 = 0;
    a->connected = 0; a->modulation = ANS_MOD_NONE; a->bitrate = 0; a->rx_bitrate = 0; a->pcm = 0xFF; a->end = ANS_END_NONE; a->retrains = 0; a->retraining = 0; a->rt_recent = 0; a->rt_clock = 0;
    memset(&a->v34_stop, 0, sizeof a->v34_stop);
    a->fbit = 1; a->cd_run = 0; a->cd_thresh = 0;
    a->info0a.c1664 = 0; a->v34.from_v90 = 0; memset(&a->v34.w, 0, sizeof a->v34.w);
    /* V.92 9.1: Phase 1 is that of V.90, and V.8 cannot say V.92 (the NOTE
     * there; Table 5/V.8 has one bit for "V.90 or V.92 digital modem"), so
     * nothing above depends on ANS_MODE_V92. Phase 2 says it. */
    a->v92_both = 0; a->v92_data = 0;
    a->pcm_up = local.has_pcm && (cfg->modes & ANS_MODE_V92) != 0;
    x2_offer(a);
    a->k56_agreed = 0;
    k56_offer(a);
}

/* before the first sample: in ANS_V8BIS (only answer_init enters it) or in
 * ANS_V8 without K56flex on offer (V.8 after V.8bis has it on offer), with
 * the clock at 0 */
static int before_first_tick(const answer_t *a)
{
    return a->t == 0 && (a->phase == ANS_V8BIS || (a->phase == ANS_V8 && !a->k56_offered));
}

/* the offers depend on the law: settled before the first sample, and a
 * later call changes nothing */
void answer_v90_cfg(answer_t *a, const p2d_cfg *cfg)
{
    if (!before_first_tick(a)) return;
    a->p2cfg = *cfg; x2_offer(a); k56_offer(a);
}

static int16_t tick_v8(answer_t *a, int16_t rx)
{
    int16_t tx = 0;
    /* V.32bis 6.1: a caller without V.8 answers the answer tone with a
     * steady 1800 Hz (only listened for during the tone: our own JM has
     * 1850 Hz in it) */
    if ((a->cfg.modes & V8_MOD_V32) && a->v8.state == V8A_ANSAM && v32_aadet_tick(&a->aadet, rx)) {
        ans_enter(a, ANS_V32_SIL); a->t = 0;
        return 0;
    }
    if (v8ans_tick(&a->v8, rx, &tx) != V8A_DONE) return tx;
    if (a->v8.result == V8A_RES_NO_CM) {           /* pre-V.8 caller */
        if (a->cfg.modes & V8_MOD_V22) ans_start_v22(a);
        else if (a->cfg.modes & V8_MOD_V21) ans_start_v21(a);
        else ans_fail(a, ANS_END_FAILED);
        return tx;
    }
    a->used_v8 = 1;
    if (a->v8.selected == V8_SEL_V90) ans_start_p2(a);     /* V.90 9.2.1.1.1 */
    else if (a->v8.selected == 1 && (a->cfg.modes & V8_MOD_V34)) ans_start_v34(a);
    else if (a->v8.selected == 3 && (a->cfg.modes & V8_MOD_V32)) ans_start_v32(a);
    else if (a->v8.selected == 4) ans_start_v22(a);
    else if (a->v8.selected == 12) ans_start_v21(a);
    else ans_fail(a, ANS_END_NO_COMMON);               /* nothing in common */
    return tx;
}

const ans_scheme_t ans_v8 = { tick_v8, NULL, NULL, NULL, NULL, NULL, NULL, 0 };

int16_t answer_tick(answer_t *a, int16_t rx)
{
    int16_t tx;
    ans_phase was = a->phase;
    int was_p2 = v34_phase2(a);
    int32_t budget;
    a->t = clk_sat_inc(a->t);
    tx = a->s->tick(a, rx);

    /* the backstop: its clock starts again with every phase (and with V.34's
     * Phase 2), not with the engines' own states */
    if (a->phase != was || (v34_phase2(a) && !was_p2)) a->t_phase = 0;
    else if (a->t_phase < INT32_MAX) a->t_phase++;
    budget = phase_budget(a);
    if (budget && a->t_phase > budget) {
        /* in a connected call, a retrain (its Phase 2 or Phases 3 and 4) that failed */
        ans_fail(a, ans_retrain_or(a, ANS_END_FAILED)); tx = 0;
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
    if (a->s->pcm_out) return a->pcm;
    return pcm_encode(alaw, tx);
}

uint8_t answer_tick_pcm(answer_t *a, uint8_t rx)
{
    return answer_tick_lin(a, (int16_t)v90_octet_linear(a->p2cfg.alaw, rx));
}

int answer_snr_db10(const answer_t *a)
{
    const ans_scheme_t *m;
    int64_t num, den;
    int v;
    if (!a->connected) return -1;
    m = mod_scheme(a->modulation);
    if (!m || !m->snr_den || m->snr_den(a, &num, &den) != 0) return -1;
    v = probe_db10(num, den > 0 ? den : 1);
    return v < 0 ? 0 : v > 600 ? 600 : v;
}

int answer_rtd(const answer_t *a)
{
    const ans_scheme_t *m;
    if (!a->connected) return -1;
    m = mod_scheme(a->modulation);
    return m && m->rtd ? m->rtd(a) : -1;
}

int answer_v34_echo(const answer_t *a, v34dx_echo_t *e)
{
    if (a->phase != ANS_V34 || (a->v34.state != V34A_DATA && a->v34.state != V34A_RENEG)) return -1;
    v34dx_echo(&a->v34.dx, e);
    return 0;
}

int answer_cleardown(answer_t *a) { return a->s->cleardown ? a->s->cleardown(a) : -1; }

int answer_retrain(answer_t *a) { return a->s->retrain ? a->s->retrain(a) : -1; }

int answer_moving(const answer_t *a)
{
    if (!a->connected || a->retraining) return 0;
    return a->s->moving ? a->s->moving(a) : 1;
}

int answer_end(const answer_t *a) { return a->phase == ANS_FAILED ? a->end : ANS_END_NONE; }

/* the engine of the phase (V.90, V.92, x2, K56flex); between engines (a
 * retrain: its silence, its Phase 2; the end) the modulation's */
int answer_echo_delay(const answer_t *a) { return a->s->echo_delay ? a->s->echo_delay(a) : -1; }

int answer_odp_bypass(const answer_t *a)
{
    /* 9.3.1/V.92: V.92 at both ends (INFO0) and LAPM in the protocol category of CM and JM */
    return a->v92_both && a->v8.cm.has_prot && a->v8.cm.prot == V8_PROT_LAPM
           && a->v8.jm.has_prot && a->v8.jm.prot == V8_PROT_LAPM;
}
