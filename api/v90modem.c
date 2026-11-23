/* libv90modem: the public API (include/v90modem.h) over the modem, V.14 and
 * V.42 code. A thin layer: it maps the configuration, runs the modem one
 * sample at a time exactly as the code underneath expects, and reads its
 * state after every sample to report changes. Nothing here touches the
 * signal path. */
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#pragma GCC visibility push(default)
#include "../include/v90modem.h"
#pragma GCC visibility pop
#include "v90modem_priv.h"
#include "../modem/pcmenc.h"
#include "../modem/v14.h"
#include "../v90/v90_pcm.h"

/* the public numbers are the internal ones (checked at compile time) */
#define SAME(a, b) ((int)(a) == (int)(b))
typedef char v90m_check_conn[(SAME(V90M_CONN_V21, ANS_MOD_V21) && SAME(V90M_CONN_V22, ANS_MOD_V22) && SAME(V90M_CONN_V22BIS, ANS_MOD_V22BIS)
                              && SAME(V90M_CONN_V90, ANS_MOD_V90) && SAME(V90M_CONN_V34, ANS_MOD_V34) && SAME(V90M_CONN_V32BIS, ANS_MOD_V32BIS)
                              && SAME(V90M_CONN_V92, ANS_MOD_V92) && SAME(V90M_CONN_BELL103, ANS_MOD_BELL103)
                              && SAME(V90M_CONN_BELL212A, ANS_MOD_BELL212A) && SAME(V90M_CONN_X2, ANS_MOD_X2)
                              && SAME(V90M_CONN_K56FLEX, ANS_MOD_K56)) ? 1 : -1];
typedef char v90m_check_ec[(SAME(V90M_EC_DETECT, V42EC_DETECT) && SAME(V90M_EC_ESTABLISH, V42EC_ESTABLISH) && SAME(V90M_EC_CONNECTED, V42EC_CONNECTED)
                            && SAME(V90M_EC_ASYNC, V42EC_ASYNC) && SAME(V90M_EC_RELEASING, V42EC_RELEASING) && SAME(V90M_EC_DISCONNECTED, V42EC_DISCONNECTED)
                            && SAME(V90M_EC_R_LOCAL, V42LAPM_R_LOCAL) && SAME(V90M_EC_R_NEGOTIATION, V42LAPM_R_NEGOTIATION)
                            && SAME(V90M_EC_R_NO_EC, V42EC_R_NO_EC) && SAME(V90M_EC_R_NO_SABME, V42EC_R_NO_SABME)
                            && SAME(V90M_EC_COMP_V42BIS, V42EC_COMP_V42BIS) && SAME(V90M_EC_COMP_V44, V42EC_COMP_V44)
                            && SAME(V90M_COMP_AUTO, V42COMP_AUTO) && SAME(V90M_COMP_ALWAYS, V42COMP_ALWAYS) && SAME(V90M_COMP_NEVER, V42COMP_NEVER)
                            && SAME(V90M_BRK_DESTRUCTIVE_EXPEDITED, V42_BRK_DESTRUCTIVE_EXPEDITED)
                            && SAME(V90M_BRK_NONDESTRUCTIVE_EXPEDITED, V42_BRK_NONDESTRUCTIVE_EXPEDITED)) ? 1 : -1];

/* the strictest alignment any member can need */
#define V90M_ALIGN offsetof(struct { char c; v90m_align u; }, u)
static int aligned(const void *p) { return ((uintptr_t)p % V90M_ALIGN) == 0; }
static void seterr(int *err, int e) { if (err) *err = e; }

const char *v90m_version(void) { return "0.5.0"; }

uint8_t v90m_g711_encode(int alaw, int16_t linear) { return pcm_encode(alaw != 0, linear); }
int16_t v90m_g711_decode(int alaw, uint8_t codeword) { return (int16_t)v90_octet_linear(alaw != 0, codeword); }

/* ------------------------------------------------------------------ modem */
struct v90m {
    v90m_cfg cfg;
    int      owned;             /* from v90m_create: v90m_destroy frees it */
    /* what the last sample left, to report changes */
    int      was_connected, was_moving, was_ended, was_mod, was_tx, was_rx;
    unsigned renegotiations;
    union { answer_t a; orig_t o; } u;     /* by cfg.role */
};

#define ORIGINATING(m) ((m)->cfg.role == V90M_ROLE_ORIGINATE)
#define V90M_MOD_ALL  (V90M_MOD_V21 | V90M_MOD_V22 | V90M_MOD_V32BIS | V90M_MOD_V34 | V90M_MOD_V90 | V90M_MOD_V92 \
                       | V90M_MOD_BELL103 | V90M_MOD_BELL212A | V90M_MOD_X2 | V90M_MOD_K56)
/* the defaults: all but V.92, Bell (which would change what a caller
 * without V.8 hears from us after 4 s), x2 and K56flex */
#define V90M_MOD_DEFAULT (V90M_MOD_ALL & ~(V90M_MOD_V92 | V90M_MOD_BELL103 | V90M_MOD_BELL212A | V90M_MOD_X2 | V90M_MOD_K56))
/* what the calling side can do: all but x2 and K56flex (V.90 and V.92 as
 * their analogue modem) */
#define V90M_MOD_ORIG (V90M_MOD_ALL & ~(V90M_MOD_X2 | V90M_MOD_K56))

typedef char v90m_check_orig[(SAME(ORIG_MOD_V21, ANS_MOD_V21) && SAME(ORIG_MOD_V22, ANS_MOD_V22) && SAME(ORIG_MOD_V22BIS, ANS_MOD_V22BIS)
                              && SAME(ORIG_MOD_V32BIS, ANS_MOD_V32BIS) && SAME(ORIG_MOD_V34, ANS_MOD_V34)
                              && SAME(ORIG_MOD_V90, ANS_MOD_V90) && SAME(ORIG_MODE_V90, ANS_MODE_V90)
                              && SAME(ORIG_MOD_V92, ANS_MOD_V92) && SAME(ORIG_MODE_V92, ANS_MODE_V92)
                              && SAME(ORIG_MOD_BELL103, ANS_MOD_BELL103) && SAME(ORIG_MODE_BELL103, ANS_MODE_BELL103)
                              && SAME(ORIG_MOD_BELL212A, ANS_MOD_BELL212A) && SAME(ORIG_MODE_BELL212A, ANS_MODE_BELL212A)) ? 1 : -1];

void v90m_cfg_default(v90m_cfg *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->role = V90M_ROLE_ANSWER;
    cfg->modulations = V90M_MOD_DEFAULT;
    cfg->tx_level = 3000;
    cfg->echo_delay = -1;
}

static int check_cfg(const v90m_cfg *c)
{
    if (c->role != V90M_ROLE_ANSWER && c->role != V90M_ROLE_ORIGINATE) return V90M_EINVAL;
    if (c->modulations & ~V90M_MOD_ALL) return V90M_EINVAL;
    if (c->max_tx_code < 0 || c->max_tx_code > 31 || c->tx_level < 0 || c->max_down_bps < 0 || c->max_up_bps < 0 || c->echo_delay < -1)
        return V90M_EINVAL;
    if (!c->modulations) return V90M_EINVAL;
    /* V.92 is V.90's start-up with PCM upstream on top, in either role */
    if ((c->modulations & V90M_MOD_V92) && !(c->modulations & V90M_MOD_V90)) return V90M_ENOTSUP;
    /* x2's upstream is V.34, and only its server (answering) side is here */
    if ((c->modulations & V90M_MOD_X2) && !(c->modulations & V90M_MOD_V34)) return V90M_ENOTSUP;
    /* K56flex's upstream is V.34 too, and only its server side is here */
    if ((c->modulations & V90M_MOD_K56) && !(c->modulations & V90M_MOD_V34)) return V90M_ENOTSUP;
    if (c->role == V90M_ROLE_ORIGINATE) {
        if (!(c->modulations & V90M_MOD_ORIG)) return (c->modulations ? V90M_ENOTSUP : V90M_EINVAL);
        if (c->modulations & ~V90M_MOD_ORIG) return V90M_ENOTSUP;
        /* V.92 is V.90's start-up with PCM upstream on top */
        if ((c->modulations & V90M_MOD_V92) && !(c->modulations & V90M_MOD_V90)) return V90M_ENOTSUP;
    }
    return V90M_OK;
}

static unsigned modes_of(unsigned mods)
{
    unsigned m = 0;
    if (mods & V90M_MOD_V21) m |= V8_MOD_V21;
    if (mods & V90M_MOD_V22) m |= V8_MOD_V22;
    if (mods & V90M_MOD_V32BIS) m |= V8_MOD_V32;
    if (mods & V90M_MOD_V34) m |= V8_MOD_V34;
    if (mods & V90M_MOD_V90) m |= ANS_MODE_V90;
    if (mods & V90M_MOD_V92) m |= ANS_MODE_V92;
    if (mods & V90M_MOD_BELL103) m |= ANS_MODE_BELL103;
    if (mods & V90M_MOD_BELL212A) m |= ANS_MODE_BELL212A;
    if (mods & V90M_MOD_X2) m |= ANS_MODE_X2;
    if (mods & V90M_MOD_K56) m |= ANS_MODE_K56;
    return m;
}

int v90m_reset(v90m_t *m, const v90m_cfg *cfg)
{
    int r, owned;
    if (!m || !cfg) return V90M_EINVAL;
    if ((r = check_cfg(cfg)) != V90M_OK) return r;
    owned = m->owned;
    /* the code underneath expects zeroed memory before answer_init */
    memset(m, 0, sizeof *m);
    m->owned = owned;
    m->cfg = *cfg;
    if (ORIGINATING(m)) {
        orig_cfg oc;
        memset(&oc, 0, sizeof oc);
        oc.level = cfg->tx_level;
        oc.modes = modes_of(cfg->modulations);
        oc.send_ci = cfg->send_ci != 0;
        /* V.32bis: one rate both ways, the lower ceiling */
        oc.max_bps = cfg->max_down_bps;
        if (cfg->max_up_bps > 0 && (oc.max_bps <= 0 || cfg->max_up_bps < oc.max_bps)) oc.max_bps = cfg->max_up_bps;
        /* V.34: max_down caps what we send, max_up what we receive (as answering);
         * V.90 likewise: max_down the V.34 upstream, max_up the PCM downstream */
        oc.max_tx_bps = cfg->max_down_bps; oc.max_rx_bps = cfg->max_up_bps;
        oc.echo_delay = cfg->echo_delay;
        /* V.34 sends codec values, as the answering side's does */
        oc.law = cfg->alaw ? 2 : 1;
        oc.getbit = cfg->getbit;
        oc.putbit = cfg->putbit;
        oc.user = cfg->user;
        orig_init(&m->u.o, &oc);
    } else {
        answer_cfg ac;
        memset(&ac, 0, sizeof ac);
        ac.level = cfg->tx_level;
        ac.modes = modes_of(cfg->modulations);
        ac.getbit = cfg->getbit;
        ac.putbit = cfg->putbit;
        ac.user = cfg->user;
        ac.rx_linear = cfg->codec_linear_rx != 0;
        ac.max_tx_code = cfg->max_tx_code;
        answer_init(&m->u.a, &ac);
        if (cfg->alaw) {
            /* the Phase 2 defaults with the law changed */
            p2d_cfg pc = m->u.a.p2cfg;
            pc.alaw = 1;
            answer_v90_cfg(&m->u.a, &pc);
        }
        answer_v90_line(&m->u.a, cfg->echo_delay, cfg->max_down_bps, cfg->max_up_bps);
    }
    return V90M_OK;
}

size_t v90m_mem_size(void) { return sizeof(struct v90m); }

v90m_t *v90m_init(void *mem, size_t len, const v90m_cfg *cfg, int *err)
{
    v90m_t *m = mem;
    int r;
    if (!mem || !cfg || !aligned(mem)) { seterr(err, V90M_EINVAL); return NULL; }
    if (len < sizeof *m) { seterr(err, V90M_ENOMEM); return NULL; }
    m->owned = 0;
    if ((r = v90m_reset(m, cfg)) != V90M_OK) { seterr(err, r); return NULL; }
    seterr(err, V90M_OK);
    return m;
}

v90m_t *v90m_create(const v90m_cfg *cfg, int *err)
{
    v90m_t *m;
    int r;
    if (!cfg) { seterr(err, V90M_EINVAL); return NULL; }
    if ((r = check_cfg(cfg)) != V90M_OK) { seterr(err, r); return NULL; }
    if (!(m = malloc(sizeof *m))) { seterr(err, V90M_ENOMEM); return NULL; }
    m->owned = 1;
    v90m_reset(m, cfg);
    seterr(err, V90M_OK);
    return m;
}

void v90m_destroy(v90m_t *m) { if (m && m->owned) free(m); }

/* the modem's state, whichever side it is */
static int failed_of(const v90m_t *m) { return ORIGINATING(m) ? m->u.o.phase == ORIG_FAILED : m->u.a.phase == ANS_FAILED; }
static int connected_of(const v90m_t *m) { return ORIGINATING(m) ? m->u.o.connected : m->u.a.connected; }
static int moving_of(const v90m_t *m) { return ORIGINATING(m) ? orig_moving(&m->u.o) : answer_moving(&m->u.a); }
static int retraining_of(const v90m_t *m)
{
    if (ORIGINATING(m)) return orig_retraining(&m->u.o);
    return m->u.a.retraining;
}
static int mod_of(const v90m_t *m) { return ORIGINATING(m) ? m->u.o.modulation : m->u.a.modulation; }
static int tx_of(const v90m_t *m) { return ORIGINATING(m) ? m->u.o.bitrate : m->u.a.bitrate; }
static int rx_of(const v90m_t *m)
{
    if (ORIGINATING(m)) return m->u.o.rx_bitrate > 0 ? m->u.o.rx_bitrate : m->u.o.bitrate;
    return m->u.a.rx_bitrate > 0 ? m->u.a.rx_bitrate : m->u.a.bitrate;
}

static v90m_end end_of(const v90m_t *m)
{
    if (ORIGINATING(m)) {
        switch (m->u.o.phase == ORIG_FAILED ? m->u.o.end : ORIG_END_NONE) {
        case ORIG_END_NONE: return V90M_END_NONE;
        case ORIG_END_NO_TONE: return V90M_END_NO_ANSWER_TONE;
        case ORIG_END_NO_JM: return V90M_END_NO_JM;
        case ORIG_END_NO_COMMON: return V90M_END_NO_COMMON;
        case ORIG_END_LOST: return V90M_END_LOST;
        case ORIG_END_CLEARDOWN: return V90M_END_CLEARDOWN_FAR;
        case ORIG_END_HANGUP: return V90M_END_CLEARDOWN_OURS;
        case ORIG_END_RETRAIN: return V90M_END_RETRAIN_FAILED;
        default: return V90M_END_FAILED;
        }
    } else {
        const answer_t *a = &m->u.a;
        if (a->phase != ANS_FAILED) return V90M_END_NONE;
        if (a->x2_failed == 2) return V90M_END_RETRAIN_FAILED;
        if (a->x2_failed) return V90M_END_X2_FAILED;
        if (a->k56_failed) return V90M_END_K56_FAILED;
        if (a->k56_lost) return V90M_END_LOST;
        if (a->cleared == 1) return V90M_END_CLEARDOWN_FAR;
        if (a->cleared == 2) return V90M_END_CLEARDOWN_OURS;
        return a->connected ? V90M_END_LOST : V90M_END_FAILED;
    }
}

static void event(v90m_t *m, v90m_event ev) { if (m->cfg.on_event) m->cfg.on_event(m->cfg.user, ev); }

#define RENEG_PENDING 2

/* after each sample: only reads the modem */
static void after(v90m_t *m)
{
    int moving, rx, connected = connected_of(m), failed = failed_of(m);
    if (!connected && !m->was_connected && !failed) return;   /* start-up: nothing to report */
    moving = moving_of(m) != 0;
    rx = rx_of(m);
    if (connected && !m->was_connected) {
        m->was_connected = 1;
        m->was_mod = mod_of(m); m->was_tx = tx_of(m); m->was_rx = rx;
        event(m, V90M_EV_CONNECTED);
    } else if (m->was_connected && (mod_of(m) != m->was_mod || tx_of(m) != m->was_tx || rx != m->was_rx)) {
        m->was_mod = mod_of(m); m->was_tx = tx_of(m); m->was_rx = rx;
        event(m, V90M_EV_RATES);
    }
    /* A stop is a rate renegotiation when data moves again with neither a
     * retrain nor the end in between: a cleardown also stops the data first.
     * Until then was_moving is RENEG_PENDING, not 0: the memory size is pinned. */
    if (m->was_moving == RENEG_PENDING && (retraining_of(m) || failed)) m->was_moving = 0;
    if (moving != (m->was_moving == 1)) {
        if (moving && m->was_moving == RENEG_PENDING) m->renegotiations++;
        m->was_moving = moving ? 1 : connected && !retraining_of(m) && !failed ? RENEG_PENDING : 0;
        event(m, moving ? V90M_EV_DATA_MOVING : V90M_EV_DATA_STOPPED);
    }
    if (failed && !m->was_ended) {
        m->was_ended = 1;
        event(m, V90M_EV_ENDED);
    }
}

int v90m_process_g711(v90m_t *m, const uint8_t *rx, uint8_t *tx, size_t n)
{
    size_t i;
    if (!m || (n && (!rx || !tx))) return V90M_EINVAL;
    if (ORIGINATING(m)) {
        int alaw = m->cfg.alaw != 0;
        for (i = 0; i < n; i++) {
            tx[i] = pcm_encode(alaw, orig_tick(&m->u.o, (int16_t)v90_octet_linear(alaw, rx[i])));
            after(m);
        }
        return V90M_OK;
    }
    for (i = 0; i < n; i++) { tx[i] = answer_tick_pcm(&m->u.a, rx[i]); after(m); }
    return V90M_OK;
}

int v90m_process_lin_rx(v90m_t *m, const int16_t *rx, uint8_t *tx, size_t n)
{
    size_t i;
    if (!m || (n && (!rx || !tx))) return V90M_EINVAL;
    if (ORIGINATING(m)) {
        int alaw = m->cfg.alaw != 0;
        for (i = 0; i < n; i++) { tx[i] = pcm_encode(alaw, orig_tick(&m->u.o, rx[i])); after(m); }
        return V90M_OK;
    }
    for (i = 0; i < n; i++) { tx[i] = answer_tick_lin(&m->u.a, rx[i]); after(m); }
    return V90M_OK;
}

int v90m_process_linear(v90m_t *m, const int16_t *rx, int16_t *tx, size_t n)
{
    size_t i;
    if (!m || (n && (!rx || !tx))) return V90M_EINVAL;
    if (ORIGINATING(m)) {
        for (i = 0; i < n; i++) { tx[i] = orig_tick(&m->u.o, rx[i]); after(m); }
        return V90M_OK;
    }
    for (i = 0; i < n; i++) { tx[i] = answer_tick(&m->u.a, rx[i]); after(m); }
    return V90M_OK;
}

int v90m_retrain(v90m_t *m)
{
    if (ORIGINATING(m)) return orig_retrain(&m->u.o) == 0 ? V90M_OK : V90M_ESTATE;
    return answer_retrain(&m->u.a) == 0 ? V90M_OK : V90M_ESTATE;
}

int v90m_cleardown(v90m_t *m)
{
    /* V.34 (11.7), V.90 (9.7), V.92 (9.11), either role; V.21, V.22, V.22bis,
     * V.32bis and Bell have none in data mode */
    if (ORIGINATING(m)) return orig_cleardown(&m->u.o) == 0 ? V90M_OK : V90M_ESTATE;
    return answer_cleardown(&m->u.a) == 0 ? V90M_OK : V90M_ESTATE;
}

void v90m_get_state(const v90m_t *m, v90m_state *st)
{
    st->connected = connected_of(m);
    st->moving = moving_of(m);
    st->retraining = retraining_of(m);
    st->modulation = (v90m_conn)mod_of(m);
    st->tx_bps = tx_of(m);
    st->rx_bps = rx_of(m);
    st->used_v8 = ORIGINATING(m) ? m->u.o.used_v8 : m->u.a.used_v8;
    st->odp_bypass = ORIGINATING(m) ? 0 : answer_odp_bypass(&m->u.a);
    st->end = end_of(m);
}

void v90m_get_stats(const v90m_t *m, v90m_stats *st)
{
    st->modulation = (v90m_conn)mod_of(m);
    st->tx_bps = tx_of(m);
    st->rx_bps = rx_of(m);
    st->renegotiations = m->renegotiations;
    if (ORIGINATING(m)) {
        const orig_t *o = &m->u.o;
        st->snr_db10 = orig_snr_db10(o);
        st->rtd = orig_rtd(o);
        st->echo_delay = -1;
        st->retrains = (unsigned)orig_retrains(o);
    } else {
        const answer_t *a = &m->u.a;
        st->snr_db10 = a->connected ? answer_snr_db10(a) : -1;
        st->rtd = a->connected ? answer_rtd(a) : -1;
        if (a->modulation == ANS_MOD_V92 || a->phase == ANS_P3X || a->phase == ANS_V92) st->echo_delay = a->u.v92.ecd.delay;
        else if (a->modulation == ANS_MOD_V90 || a->phase == ANS_P3 || a->phase == ANS_V90) st->echo_delay = a->p34.ecd.delay;
        else if (a->modulation == ANS_MOD_X2 || a->phase == ANS_X2) st->echo_delay = a->u.x2.ecd.delay;
        else if (a->modulation == ANS_MOD_K56 || a->phase == ANS_K56) st->echo_delay = a->u.k56.ecd.delay;
        else st->echo_delay = -1;
        st->retrains = (unsigned)a->retrains;
    }
    st->ec_in_db10 = st->ec_res_db10 = -1;
    st->ec_near_delay = st->ec_near_peak = st->ec_near_loss_db10 = -1;
    st->ec_far_delay = st->ec_far_peak = st->ec_far_loss_db10 = -1;
    if (!ORIGINATING(m)) {
        v34dx_echo_t e;
        if (answer_v34_echo(&m->u.a, &e) == 0) {
            st->ec_in_db10 = e.in_db10; st->ec_res_db10 = e.res_db10;
            st->ec_near_delay = e.near_delay; st->ec_near_peak = e.near_peak; st->ec_near_loss_db10 = e.near_loss_db10;
            st->ec_far_delay = e.far_delay; st->ec_far_peak = e.far_peak; st->ec_far_loss_db10 = e.far_loss_db10;
        }
    }
}

answer_t *v90m__answer(v90m_t *m) { return &m->u.a; }
orig_t   *v90m__orig(v90m_t *m) { return &m->u.o; }

/* ------------------------------------------------------------ start-stop */
struct v90m_async { int owned; v14_t v; };

size_t v90m_async_mem_size(void) { return sizeof(struct v90m_async); }
void v90m_async_reset(v90m_async_t *a) { int o = a->owned; memset(a, 0, sizeof *a); a->owned = o; v14_init(&a->v); }
v90m_async_t *v90m_async_init(void *mem, size_t len, int *err)
{
    v90m_async_t *a = mem;
    if (!mem || !aligned(mem)) { seterr(err, V90M_EINVAL); return NULL; }
    if (len < sizeof *a) { seterr(err, V90M_ENOMEM); return NULL; }
    a->owned = 0; v90m_async_reset(a); seterr(err, V90M_OK);
    return a;
}
v90m_async_t *v90m_async_create(int *err)
{
    v90m_async_t *a = malloc(sizeof *a);
    if (!a) { seterr(err, V90M_ENOMEM); return NULL; }
    a->owned = 1; v90m_async_reset(a); seterr(err, V90M_OK);
    return a;
}
void v90m_async_destroy(v90m_async_t *a) { if (a && a->owned) free(a); }
int  v90m_async_getbit(void *a) { return v14_getbit(&((v90m_async_t *)a)->v); }
void v90m_async_putbit(void *a, int bit) { v14_putbit(&((v90m_async_t *)a)->v, bit); }
size_t v90m_async_write(v90m_async_t *a, const uint8_t *buf, size_t n)
{
    return (size_t)v14_write(&a->v, buf, n > 0x7FFFFFFF ? 0x7FFFFFFF : (int)n);
}
size_t v90m_async_read(v90m_async_t *a, uint8_t *buf, size_t max)
{
    return (size_t)v14_read(&a->v, buf, max > 0x7FFFFFFF ? 0x7FFFFFFF : (int)max);
}
size_t v90m_async_tx_pending(const v90m_async_t *a) { return (size_t)v14_tx_pending(&a->v); }
unsigned long v90m_async_frame_errors(const v90m_async_t *a) { return (unsigned long)a->v.rx_frame_errs; }

/* ------------------------------------------------------------------ V.42 */
struct v90m_ec { int owned; v42ec_t e; };

static void cfg_in(v42ec_cfg_t *o, const v90m_ec_cfg *c)
{
    memset(o, 0, sizeof *o);
    o->originator = c->originator; o->detect = c->detect; o->fallback = c->fallback;
    o->t400_ms = c->t400_ms; o->establish_ms = c->establish_ms; o->comp_policy = c->comp_policy;
    o->lapm.use_xid = c->lapm.use_xid;
    o->lapm.k_tx = c->lapm.k_tx; o->lapm.k_rx = c->lapm.k_rx;
    o->lapm.n401_tx = c->lapm.n401_tx; o->lapm.n401_rx = c->lapm.n401_rx;
    o->lapm.fcs32 = c->lapm.fcs32;
    o->lapm.comp_tx = c->lapm.v42bis_tx; o->lapm.comp_rx = c->lapm.v42bis_rx;
    o->lapm.comp_p1 = c->lapm.v42bis_p1; o->lapm.comp_p2 = c->lapm.v42bis_p2;
    o->lapm.v44_tx = c->lapm.v44_tx; o->lapm.v44_rx = c->lapm.v44_rx;
    o->lapm.v44_p1_tx = c->lapm.v44_p1_tx; o->lapm.v44_p1_rx = c->lapm.v44_p1_rx;
    o->lapm.v44_p2_tx = c->lapm.v44_p2_tx; o->lapm.v44_p2_rx = c->lapm.v44_p2_rx;
    o->lapm.v44_p3_tx = c->lapm.v44_p3_tx; o->lapm.v44_p3_rx = c->lapm.v44_p3_rx;
    o->lapm.t401_ms = c->lapm.t401_ms; o->lapm.t403_ms = c->lapm.t403_ms; o->lapm.n400 = c->lapm.n400;
}

void v90m_ec_cfg_default(v90m_ec_cfg *c, int originator)
{
    v42ec_cfg_t i;
    v42ec_cfg_default(&i, originator);
    memset(c, 0, sizeof *c);
    c->originator = i.originator; c->detect = i.detect; c->fallback = i.fallback;
    c->t400_ms = i.t400_ms; c->establish_ms = i.establish_ms; c->comp_policy = i.comp_policy;
    c->lapm.use_xid = i.lapm.use_xid;
    c->lapm.k_tx = i.lapm.k_tx; c->lapm.k_rx = i.lapm.k_rx;
    c->lapm.n401_tx = i.lapm.n401_tx; c->lapm.n401_rx = i.lapm.n401_rx;
    c->lapm.fcs32 = i.lapm.fcs32;
    c->lapm.v42bis_tx = i.lapm.comp_tx; c->lapm.v42bis_rx = i.lapm.comp_rx;
    c->lapm.v42bis_p1 = i.lapm.comp_p1; c->lapm.v42bis_p2 = i.lapm.comp_p2;
    c->lapm.v44_tx = i.lapm.v44_tx; c->lapm.v44_rx = i.lapm.v44_rx;
    c->lapm.v44_p1_tx = i.lapm.v44_p1_tx; c->lapm.v44_p1_rx = i.lapm.v44_p1_rx;
    c->lapm.v44_p2_tx = i.lapm.v44_p2_tx; c->lapm.v44_p2_rx = i.lapm.v44_p2_rx;
    c->lapm.v44_p3_tx = i.lapm.v44_p3_tx; c->lapm.v44_p3_rx = i.lapm.v44_p3_rx;
    c->lapm.t401_ms = i.lapm.t401_ms; c->lapm.t403_ms = i.lapm.t403_ms; c->lapm.n400 = i.lapm.n400;
}

int v90m_ec_reset(v90m_ec_t *e, const v90m_ec_cfg *cfg)
{
    v42ec_cfg_t c;
    int o = e->owned;
    if (!cfg) return V90M_EINVAL;
    cfg_in(&c, cfg);
    memset(e, 0, sizeof *e);
    e->owned = o;
    v42ec_init(&e->e, &c);
    return V90M_OK;
}

size_t v90m_ec_mem_size(void) { return sizeof(struct v90m_ec); }

v90m_ec_t *v90m_ec_init(void *mem, size_t len, const v90m_ec_cfg *cfg, int *err)
{
    v90m_ec_t *e = mem;
    if (!mem || !cfg || !aligned(mem)) { seterr(err, V90M_EINVAL); return NULL; }
    if (len < sizeof *e) { seterr(err, V90M_ENOMEM); return NULL; }
    e->owned = 0; v90m_ec_reset(e, cfg); seterr(err, V90M_OK);
    return e;
}

v90m_ec_t *v90m_ec_create(const v90m_ec_cfg *cfg, int *err)
{
    v90m_ec_t *e;
    if (!cfg) { seterr(err, V90M_EINVAL); return NULL; }
    if (!(e = malloc(sizeof *e))) { seterr(err, V90M_ENOMEM); return NULL; }
    e->owned = 1; v90m_ec_reset(e, cfg); seterr(err, V90M_OK);
    return e;
}

void   v90m_ec_destroy(v90m_ec_t *e) { if (e && e->owned) free(e); }
int    v90m_ec_tx_bit(v90m_ec_t *e) { return v42ec_tx_bit(&e->e); }
void   v90m_ec_rx_bit(v90m_ec_t *e, int bit) { v42ec_rx_bit(&e->e, bit); }
void   v90m_ec_tick(v90m_ec_t *e, unsigned ms) { v42ec_tick(&e->e, ms); }
void   v90m_ec_line_stopped(v90m_ec_t *e) { v42ec_line_stopped(&e->e); }
void   v90m_ec_line_restored(v90m_ec_t *e) { v42ec_line_restored(&e->e); }
size_t v90m_ec_write(v90m_ec_t *e, const uint8_t *d, size_t n) { return v42ec_write(&e->e, d, n); }
size_t v90m_ec_write_space(const v90m_ec_t *e) { return v42ec_write_space(&e->e); }
size_t v90m_ec_read(v90m_ec_t *e, uint8_t *d, size_t max) { return v42ec_read(&e->e, d, max); }
size_t v90m_ec_read_avail(const v90m_ec_t *e) { return v42ec_read_avail(&e->e); }
int    v90m_ec_tx_idle(const v90m_ec_t *e) { return v42ec_tx_idle(&e->e); }
int    v90m_ec_tx_busy(const v90m_ec_t *e) { return e->e.hdlc_tx_on && v42hdlc_tx_busy(&e->e.htx); }
void   v90m_ec_release(v90m_ec_t *e) { v42ec_release(&e->e); }
void   v90m_ec_abort(v90m_ec_t *e) { v42lapm_abort(&e->e.lapm, V42LAPM_R_LOCAL); }
int    v90m_ec_send_break(v90m_ec_t *e, int opt, int len10ms) { return v42ec_send_break(&e->e, opt, len10ms) == 0 ? V90M_OK : V90M_ESTATE; }
int    v90m_ec_get_break(v90m_ec_t *e, int *opt, int *len10ms) { return v42ec_get_break(&e->e, opt, len10ms); }
int    v90m_ec_state(const v90m_ec_t *e) { return v42ec_state(&e->e); }

void v90m_ec_get_status(const v90m_ec_t *e, v90m_ec_status *st)
{
    v42ec_status_t s;
    v42ec_get_status(&e->e, &s);
    memset(st, 0, sizeof *st);
    st->state = s.state; st->reason = s.reason;
    st->k_tx = s.k_tx; st->k_rx = s.k_rx; st->n401_tx = s.n401_tx; st->n401_rx = s.n401_rx;
    st->comp_alg = s.comp_alg; st->comp_tx = s.comp_tx; st->comp_rx = s.comp_rx;
    st->enc_on = e->e.comp_tx; st->dec_on = e->e.comp_rx; st->alg_on = e->e.comp_alg;
    st->comp_n2 = s.comp_n2; st->comp_n7 = s.comp_n7;
    st->v44_n2_tx = s.v44_n2_tx; st->v44_n7_tx = s.v44_n7_tx; st->v44_n8_tx = s.v44_n8_tx;
    st->v44_n2_rx = s.v44_n2_rx; st->v44_n7_rx = s.v44_n7_rx; st->v44_n8_rx = s.v44_n8_rx;
    st->fcs32 = s.fcs32;
    st->tx_queued = s.tx_queued; st->rx_avail = s.rx_avail; st->unacked = s.unacked;
    st->rx_frames = s.rx_frames; st->rx_frames_bad = s.rx_frames_bad;
    st->tx_i = s.tx_i; st->tx_i_retx = s.tx_i_retx; st->rx_i = s.rx_i; st->rx_i_dropped = s.rx_i_dropped;
    st->tx_rej = s.tx_rej; st->rx_rej = s.rx_rej; st->t401_expiries = s.t401_expiries; st->violations = s.violations;
    st->comp_in = s.comp_in; st->comp_out = s.comp_out; st->decomp_in = s.decomp_in; st->decomp_out = s.decomp_out;
    st->async_overruns = s.async_overruns; st->async_framing = s.async_framing;
}

v42ec_t *v90m__ec(v90m_ec_t *e) { return &e->e; }

/* the bounds in the header hold on this platform */
typedef char v90m_check_mem[sizeof(struct v90m) <= V90M_MEM_SIZE ? 1 : -1];
typedef char v90m_check_ec_mem[sizeof(struct v90m_ec) <= V90M_EC_MEM_SIZE ? 1 : -1];
typedef char v90m_check_async_mem[sizeof(struct v90m_async) <= V90M_ASYNC_MEM_SIZE ? 1 : -1];
