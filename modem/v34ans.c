/* V.34 answer modem. See v34ans.h. */
#include <string.h>
#include "v34ans.h"

#define RETRAIN_SILENCE 560     /* 70 ms (11.5.2) */

static void end(v34ans_t *a, int err) { a->state = V34A_ENDED; a->error = err; }

static void p2_cfg(const v34ans_t *a, p2ans_cfg *c)
{
    c->level = a->cfg.level;
    c->io_delay = a->cfg.io_delay > 0 ? a->cfg.io_delay : 0;
    c->sr_mask = a->cfg.sr_mask;
    c->max_rate = a->cfg.max_rx_rate;
    c->x2 = a->cfg.x2;
    c->k56 = a->cfg.k56;
}

void v34ans_init(v34ans_t *a, const v34ans_cfg *cfg)
{
    p2ans_cfg c;
    memset(a, 0, sizeof *a);
    a->cfg = *cfg;
    p2_cfg(a, &c);
    p2ans_init(&a->p2, &c);
    a->state = V34A_PHASE2;
}

void v34ans_dx_cfg(const p2ans_t *p2, const v34ans_cfg *cfg, v34dx_cfg *dx)
{
    int32_t delay;
    memset(dx, 0, sizeof *dx);
    dx->answer = 1;
    dx->sym_rate = p2->sym_rate;
    v34_carrier(p2->sym_idx, p2->tx_hi_carrier, &dx->tx_car_num, &dx->tx_car_den);
    v34_carrier(p2->sym_idx, p2->rx_hi_carrier, &dx->rx_car_num, &dx->rx_car_den);
    dx->tx_preemph = p2->tx_preemph;
    dx->tx_pwr_red = p2->tx_pwr_red;
    dx->level = cfg->level;
    dx->law = cfg->law;
    dx->rtd = p2->rtd;
    dx->echo_delay = cfg->io_delay;
    dx->rtd_has_io = cfg->io_delay < 0;
    dx->md_len = p2->md_len;
    dx->retrain_on_step = cfg->retrain_on_step;
    /* 11.3.1.2.1: S begins 70 ms after INFO1a ended. p2ans stops 80 samples
     * after that end; the first symbol is on the line one pulse delay after
     * the tick that takes it, and begins half a symbol before its centre. */
    delay = ((int32_t)V34MOD_SPAN * 8000) / p2->sym_rate + (p2->tx_preemph ? V34MOD_NPRE / 2 : 0);
    dx->lead = 560 - 80 - delay + 4000 / p2->sym_rate + 32;    /* (+ 4 ms: the carrier of INFO1a
                                                                  and the first S pulse both ramp) */
    dx->peer_1664 = p2->info0c.c1664;
    dx->max_rx_rate = cfg->max_rx_rate;
    dx->max_tx_rate = cfg->max_tx_rate;
    dx->getbit = cfg->getbit; dx->putbit = cfg->putbit; dx->user = cfg->user;
}

int16_t v34ans_tick(v34ans_t *a, int16_t rx)
{
    int16_t out = 0;
    switch (a->state) {
    case V34A_PHASE2:
        out = p2ans_tick(&a->p2, rx);
        if (a->p2.state == P2N_DONE) {
            v34dx_cfg c;
            a->info0c = a->p2.info0c; a->have0c = 1;
            a->retrains += a->p2.retrains; a->retrains_ours += a->p2.retrains;
            v34ans_dx_cfg(&a->p2, &a->cfg, &c);
            if (v34dx_init(&a->dx, &c) != 0) end(a, V34A_ERR_TRAINING);
            else a->state = V34A_TRAINING;
        } else if (a->p2.state == P2N_FAILED) {
            end(a, V34A_ERR_PHASE2);
        } else if (a->p2.state == P2N_X2) {
            a->state = V34A_X2;
        } else if (a->p2.state == P2N_K56) {
            a->info0c = a->p2.info0c; a->have0c = 1;
            a->retrains += a->p2.retrains; a->retrains_ours += a->p2.retrains;
            a->state = V34A_K56;
        }
        break;
    case V34A_TRAINING:
    case V34A_DATA:
    case V34A_RENEG:
        out = v34dx_tick(&a->dx, rx);
        if (a->dx.state == DX_DATA) {
            if (a->state == V34A_TRAINING) { a->connects++; a->retrains_now = 0; }
            a->state = V34A_DATA;
        } else if (a->dx.state == DX_RETRAIN) {
            a->retrains++; a->retrains_now++;
            if (a->dx.retrain_ours) a->retrains_ours++;
            if (a->from_v90) { a->state = V34A_RETRAIN; a->t = 0; }     /* the owner's Phase 2 */
            else if (a->retrains_now > V34A_MAX_RETRAINS || !a->have0c) end(a, V34A_ERR_RETRAINS);
            else { a->state = V34A_RETRAIN; a->t = 0; }
        } else if (a->dx.state == DX_FAILED) {
            end(a, a->dx.error == DX_ERR_CLEARDOWN ? V34A_ERR_CLEARDOWN
                 : a->dx.error == DX_ERR_HANGUP ? V34A_ERR_HANGUP
                 : a->dx.error == DX_ERR_CARRIER ? V34A_ERR_CARRIER : V34A_ERR_TRAINING);
        } else if (a->state == V34A_DATA) {
            a->state = V34A_RENEG;
        }
        break;
    case V34A_RETRAIN:
        if (a->from_v90) break;
        if (++a->t >= RETRAIN_SILENCE) {
            p2ans_cfg c;
            p2_cfg(a, &c);
            p2ans_init_retrain(&a->p2, &c, &a->info0c);
            a->state = V34A_PHASE2;
        }
        break;
    default:
        break;
    }
    return out;
}

int v34ans_tx_rate(const v34ans_t *a) { return a->state == V34A_DATA ? a->dx.tx_rate : 0; }
int v34ans_rx_rate(const v34ans_t *a) { return a->state == V34A_DATA ? a->dx.rx_rate : 0; }

int v34ans_renegotiate(v34ans_t *a, int max_rx_rate)
{
    return a->state == V34A_DATA ? v34dx_renegotiate(&a->dx, max_rx_rate) : -1;
}

int v34ans_cleardown(v34ans_t *a)
{
    return a->state == V34A_DATA ? v34dx_cleardown(&a->dx) : -1;
}

int v34ans_retrain(v34ans_t *a)
{
    return (a->state == V34A_DATA || a->state == V34A_RENEG) ? v34dx_retrain(&a->dx) : -1;
}

int v34_call_cfg_from_v90(const info1a_t *i, int rx_hi_carrier, int c1664, int rtd,
                          const v34ans_cfg *cfg, v34dx_cfg *dx)
{
    int idx = i->down_sr;                   /* bits 37:39: call -> answer, our transmitter */
    memset(dx, 0, sizeof *dx);
    if (i->v90 || i->up_sr != i->down_sr || (idx != 0 && idx != 3 && idx != 4)) return -1;
    dx->answer = 0;
    dx->sym_rate = v34_symbol_rate(idx);
    v34_carrier(idx, i->hi_carrier, &dx->tx_car_num, &dx->tx_car_den);     /* Table 16 bit 25 */
    v34_carrier(idx, rx_hi_carrier, &dx->rx_car_num, &dx->rx_car_den);
    dx->tx_preemph = i->preemph > 10 ? 0 : i->preemph;                      /* bits 26:29 */
    dx->tx_pwr_red = i->min_pwr_red;                                        /* bits 12:14 */
    dx->md_len = i->md_len;
    dx->level = cfg->level;
    dx->law = cfg->law;
    dx->rtd = rtd;
    dx->echo_delay = cfg->io_delay;
    dx->rtd_has_io = cfg->io_delay < 0;
    dx->peer_1664 = c1664;
    dx->retrain_on_step = cfg->retrain_on_step;
    dx->max_rx_rate = cfg->max_rx_rate;
    dx->max_tx_rate = cfg->max_tx_rate;
    dx->getbit = cfg->getbit; dx->putbit = cfg->putbit; dx->user = cfg->user;
    return 0;
}

int v34ans_init_call(v34ans_t *a, const v34ans_cfg *cfg, const info1a_t *info1a,
                     int rx_hi_carrier, int c1664, int rtd)
{
    v34dx_cfg c;
    memset(a, 0, sizeof *a);
    a->cfg = *cfg;
    a->from_v90 = 1;
    if (v34_call_cfg_from_v90(info1a, rx_hi_carrier, c1664, rtd, cfg, &c) != 0 || v34dx_init(&a->dx, &c) != 0) {
        end(a, V34A_ERR_TRAINING);
        return -1;
    }
    a->state = V34A_TRAINING;
    return 0;
}
