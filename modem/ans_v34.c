/* The answering modem's V.34 scheme (modem/ans_scheme.h): ANS_V34, from
 * V.8 (Phase 2 to data mode and its retrains, modem/v34ans) or after a V.90
 * Phase 2 whose INFO1a selected V.34 (v34.from_v90: its retrains are
 * V.90's, ANS_RETRAIN). V.34's Phase 2 ends in the x2 and K56flex
 * start-ups too. */
#include "ans_scheme.h"

void ans_v34_cfg(const answer_t *a, v34ans_cfg *c)
{
    c->level = a->cfg.level;
    c->law = a->p2cfg.alaw ? 2 : 1;
    c->io_delay = a->echo_delay;
    c->sr_mask = 0;
    c->max_tx_rate = a->max_down; c->max_rx_rate = a->max_up;
    c->getbit = a->cfg.getbit; c->putbit = a->cfg.putbit; c->user = a->cfg.user;
    c->x2 = v8ans_x2(&a->v8);
    c->k56 = a->k56_agreed;
    c->retrain_on_step = a->cfg.retrain_on_step;
}

/* V.8 selected V.34 as the data mode: INFO0a starts right after the 75 ms
 * of silence that v8ans has just finished (V.34 11.2.1.2.1) */
void ans_start_v34(answer_t *a)
{
    v34ans_cfg c;
    ans_v34_cfg(a, &c);
    v34ans_init(&a->v34, &c);
    ans_enter(a, ANS_V34); a->t = 0;
}

static int16_t tick(answer_t *a, int16_t rx)
{
    int16_t tx = v34ans_tick(&a->v34, rx);
    if (a->connected) v34dx_stop_take(&a->v34.dx, &a->v34_stop);
    if (a->v34.from_v90) {
        /* the caller chose V.34 in INFO1a: a retrain is Phase 2 of V.90
         * again, whoever began it (V.90 9.2.1.1.8), with the silence and
         * the limit of the V.90 retrain */
        if (a->v34.state == V34A_RETRAIN) { ans_v90_retrain(a); return 0; }
        if (a->v34.state == V34A_DATA) ans_rt_data_tick(a, &ans_rt_v90);
    }
    if (a->v34.state == V34A_X2) {           /* the client's x2 marker */
        ans_start_x2(a);
        return 0;
    }
    if (a->v34.state == V34A_K56) {          /* our flagged INFO1a: no way back to V.34 */
        ans_start_k56(a);
        return 0;
    }
    if (a->v34.state == V34A_ENDED) {
        ans_fail(a, a->v34.error == V34A_ERR_CLEARDOWN ? ANS_END_CLEARDOWN_FAR
                    : a->v34.error == V34A_ERR_HANGUP ? ANS_END_CLEARDOWN_OURS
                    : a->v34.error == V34A_ERR_RETRAINS ? ANS_END_RETRAIN_FAILED : ans_retrain_or(a, ANS_END_FAILED));
        tx = 0;
    } else if (a->v34.state == V34A_DATA) {
        /* also after a rate renegotiation or a retrain */
        a->connected = 1; a->modulation = ANS_MOD_V34;
        a->bitrate = v34ans_tx_rate(&a->v34); a->rx_bitrate = v34ans_rx_rate(&a->v34);
        a->retraining = 0;
    } else if (a->connected) {
        /* no data moves: a retrain, or a rate renegotiation (not a retrain) */
        a->retraining = a->v34.state != V34A_RENEG;
    }
    if (!a->v34.from_v90) a->retrains = a->v34.retrains;
    return tx;
}

static int cleardown(answer_t *a) { return v34ans_cleardown(&a->v34); }
static int retrain(answer_t *a) { return v34ans_retrain(&a->v34); }
static int moving(const answer_t *a) { return a->v34.state == V34A_DATA; }
static int snr_den(const answer_t *a, int64_t *num, int64_t *den)
{
    *num = 65536; *den = a->v34.dx.up.rx.mse;
    return 0;
}
/* V.34 after V.90 Phase 2 has that Phase 2's figure */
static int rtd(const answer_t *a) { return a->v8.selected == V8_SEL_V90 ? a->p2.rtd : a->v34.p2.rtd; }

const ans_scheme_t ans_v34 = { tick, cleardown, retrain, moving, snr_den, rtd, NULL, 0 };
