/* V.34 call modem. See v34orig.h. */
#include <string.h>
#include "v34orig.h"

#define MS(x) ((x) * 8)
#define RETRAIN_SILENCE MS(70)  /* 11.5.1 */

static void end(v34orig_t *o, int err) { o->state = V34O_ENDED; o->error = err; }

static void p2_cfg(const v34orig_t *o, p2call_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->level = o->cfg.level;
    c->io_delay = o->cfg.io_delay > 0 ? o->cfg.io_delay : 0;
    c->sr_mask = o->cfg.sr_mask;
    c->max_rate = o->cfg.max_rx_rate;
}

void v34orig_init(v34orig_t *o, const v34orig_cfg *cfg)
{
    p2call_cfg c;
    memset(o, 0, sizeof *o);
    o->cfg = *cfg;
    p2_cfg(o, &c);
    p2call_init(&o->p2, &c);
    o->state = V34O_PHASE2;
}

void v34orig_dx_cfg(const p2call_t *p2, const v34orig_cfg *cfg, int peer_1664, v34dx_cfg *dx)
{
    memset(dx, 0, sizeof *dx);
    dx->answer = 0;
    dx->sym_rate = p2->sym_rate;
    /* our carrier, filter and power reduction: INFO1a's; the answerer's
     * carrier: what our INFO1c asked at that symbol rate */
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
    dx->peer_1664 = peer_1664;
    dx->max_rx_rate = cfg->max_rx_rate;
    dx->max_tx_rate = cfg->max_tx_rate;
    dx->getbit = cfg->getbit; dx->putbit = cfg->putbit; dx->user = cfg->user;
    dx->retrain_on_step = cfg->retrain_on_step;
}

/* a retrain is due, whoever began it: 70 ms of silence, then Phase 2 again */
static void retrain(v34orig_t *o, int ours)
{
    o->retrains++; o->retrains_now++;
    if (ours) o->retrains_ours++;
    if (o->retrains_now > V34O_MAX_RETRAINS || !o->have0a) end(o, V34O_ERR_RETRAINS);
    else { o->state = V34O_RETRAIN; o->t = 0; }
}

int16_t v34orig_tick(v34orig_t *o, int16_t rx)
{
    int16_t out = 0;
    switch (o->state) {
    case V34O_PHASE2:
        out = p2call_tick(&o->p2, rx);
        if (o->p2.state == P2L_DONE) {
            v34dx_cfg c;
            /* a retrain's Phase 2 has no INFO0: the first one's holds */
            if (!o->have0a || o->p2.n_info0c > 0) { o->info0a = o->p2.info0a; o->have0a = 1; }
            o->retrains += o->p2.retrains; o->retrains_ours += o->p2.retrains;
            v34orig_dx_cfg(&o->p2, &o->cfg, o->info0a.c1664, &c);
            /* 11.3.1.1.1: silent, waiting for the answerer's S */
            if (v34dx_init(&o->dx, &c) != 0) end(o, V34O_ERR_TRAINING);
            else o->state = V34O_TRAINING;
        } else if (o->p2.state == P2L_FAILED) {
            end(o, V34O_ERR_PHASE2);
        } else if (o->connects > 0 && ++o->t > MS(V34O_RT_P2_MS)) {
            /* a retrain's Phase 2 once the call has carried data (orig's
             * start-up limit is over): bounded whatever p2call does (D5) */
            end(o, V34O_ERR_RETRAINS);
        }
        break;
    case V34O_TRAINING:
    case V34O_DATA:
    case V34O_RENEG:
        out = v34dx_tick(&o->dx, rx);
        if (o->dx.state == DX_DATA) {
            if (o->state == V34O_TRAINING) { o->connects++; o->retrains_now = 0; }
            o->state = V34O_DATA;
        } else if (o->dx.state == DX_RETRAIN) {
            retrain(o, o->dx.retrain_ours);
        } else if (o->dx.state == DX_FAILED) {
            /* the first S never came: "the call modem may initiate a
             * retrain during Phase 3" (11.3.2.1) */
            if (o->dx.error == DX_ERR_NO_S) retrain(o, 1);
            else if (o->dx.error == DX_ERR_CLEARDOWN) end(o, V34O_ERR_CLEARDOWN);
            else if (o->dx.error == DX_ERR_HANGUP) end(o, V34O_ERR_HANGUP);
            /* a retrain's Phases 3 / 4 (the call has carried data): a
             * failed retrain, as the answering side's (spec D3) */
            else if (o->state == V34O_TRAINING && o->connects > 0) end(o, V34O_ERR_RETRAINS);
            else end(o, o->dx.error == DX_ERR_CARRIER ? V34O_ERR_CARRIER : V34O_ERR_TRAINING);
        } else if (o->state == V34O_DATA) {
            o->state = V34O_RENEG;
        }
        break;
    case V34O_RETRAIN:
        /* 11.5.1.1, 11.5.1.2: 70 ms of silence, then Tone B */
        if (++o->t >= RETRAIN_SILENCE) {
            p2call_cfg c;
            p2_cfg(o, &c);
            p2call_init_retrain(&o->p2, &c, &o->info0a);
            o->state = V34O_PHASE2; o->t = 0;
        }
        break;
    default:
        break;
    }
    return out;
}

int v34orig_tx_rate(const v34orig_t *o) { return o->state == V34O_DATA ? o->dx.tx_rate : 0; }
int v34orig_rx_rate(const v34orig_t *o) { return o->state == V34O_DATA ? o->dx.rx_rate : 0; }

int v34orig_renegotiate(v34orig_t *o, int max_rx_rate)
{
    return o->state == V34O_DATA ? v34dx_renegotiate(&o->dx, max_rx_rate) : -1;
}

int v34orig_cleardown(v34orig_t *o)
{
    return o->state == V34O_DATA ? v34dx_cleardown(&o->dx) : -1;
}

int v34orig_retrain(v34orig_t *o)
{
    return (o->state == V34O_DATA || o->state == V34O_RENEG) ? v34dx_retrain(&o->dx) : -1;
}
