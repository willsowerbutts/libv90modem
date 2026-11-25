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
#include "v90modem_fields.h"
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

typedef char v90m_check_stop[(SAME(V90M_SIDE_NEAR, DX_BY_NEAR) && SAME(V90M_SIDE_FAR, DX_BY_FAR)
                              && SAME(V90M_STOP_ASKED, DX_WHY_ASKED) && SAME(V90M_STOP_CLEARDOWN, DX_WHY_CLEARDOWN)
                              && SAME(V90M_STOP_SNR_DECLINED, DX_WHY_SNR_DECLINED) && SAME(V90M_STOP_SNR_STEP, DX_WHY_SNR_STEP)
                              && SAME(V90M_STOP_SNR_LOW, DX_WHY_SNR_LOW) && SAME(V90M_STOP_NO_SIGNAL, DX_WHY_NO_SIGNAL)
                              && SAME(V90M_STOP_B1, DX_WHY_B1) && SAME(V90M_STOP_TIMEOUT, DX_WHY_TIMEOUT)) ? 1 : -1];

/* the strictest alignment any member can need: where a v90m_align lands
 * after a char */
typedef struct { char c; v90m_align u; } v90m_align_probe;
#define V90M_ALIGN offsetof(v90m_align_probe, u)
static int aligned(const void *p) { return ((uintptr_t)p % V90M_ALIGN) == 0; }
static void seterr(int *err, int e) { if (err) *err = e; }

/* the header's numbers as "MAJOR.MINOR.PATCH" (make check-version holds
 * them to the Makefile's VERSION) */
#define V90M_STR_(x) #x
#define V90M_STR(x)  V90M_STR_(x)
const char *v90m_version(void)
{
    return V90M_STR(V90M_VERSION_MAJOR) "." V90M_STR(V90M_VERSION_MINOR) "." V90M_STR(V90M_VERSION_PATCH);
}

uint8_t v90m_g711_encode(int alaw, int16_t linear) { return pcm_encode(alaw != 0, linear); }
int16_t v90m_g711_decode(int alaw, uint8_t codeword) { return (int16_t)v90_octet_linear(alaw != 0, codeword); }

/* ------------------------------------------------------------------ modem */
struct v90m {
    v90m_cfg cfg;
    int      owned;             /* from v90m_create: v90m_destroy frees it */
    /* what the last sample left, to report changes */
    uint8_t  was_connected, was_moving, was_ended;
    uint8_t  busy;              /* inside v90m_process_*: a callback may be running */
    uint8_t  dead;              /* v90m_destroy came from a callback: stop after this sample */
    uint8_t  reneg_pending;     /* data stopped, not in a retrain: counts when it moves again, becomes
                                   a retrain, or the call ends other than by a cleardown */
    uint8_t  meas_tick;         /* samples since the SNR and round trip were last kept */
    int      was_mod, was_tx, was_rx;
    unsigned renegotiations;
    int16_t  last_snr, last_rtd;    /* the last measured while data moves; -1 before the first */
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
    if (!cfg) return;
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
    if (c->max_tx_code < 0 || c->max_tx_code > 31 || c->tx_level < 0 || c->tx_level > V90M_TX_LEVEL_MAX
        || c->max_down_bps < 0 || c->max_up_bps < 0 || c->echo_delay < -1)
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
    } else {
        /* answering, Bell 212A is the fallback of the V.22 answerer */
        if ((c->modulations & V90M_MOD_BELL212A) && !(c->modulations & V90M_MOD_V22)) return V90M_ENOTSUP;
        /* x2's and K56flex's upstream is V.34 from 2400 bit/s: a lower ceiling
         * leaves the client no rate after the point of no return */
        if ((c->modulations & (V90M_MOD_X2 | V90M_MOD_K56)) && c->max_up_bps > 0 && c->max_up_bps < 2400) return V90M_EINVAL;
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

/* a checked cfg into m, whatever m held but owned */
static void setup(v90m_t *m, const v90m_cfg *cfg)
{
    int owned = m->owned;
    /* the code underneath expects zeroed memory before answer_init */
    memset(m, 0, sizeof *m);
    m->owned = owned;
    m->cfg = *cfg;
    m->last_snr = m->last_rtd = -1;
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
        oc.retrain_on_step = cfg->retrain_on_step != 0;
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
        ac.retrain_on_step = cfg->retrain_on_step != 0;
        answer_init(&m->u.a, &ac);
        if (cfg->alaw) {
            /* the Phase 2 defaults with the law changed */
            p2d_cfg pc = m->u.a.p2cfg;
            pc.alaw = 1;
            answer_v90_cfg(&m->u.a, &pc);
        }
        answer_v90_line(&m->u.a, cfg->echo_delay, cfg->max_down_bps, cfg->max_up_bps);
    }
}

int v90m_reset(v90m_t *m, const v90m_cfg *cfg)
{
    int r;
    if (!m || !cfg) return V90M_EINVAL;
    /* from a callback: the block under way runs on the modem as it is */
    if (m->busy) return V90M_ESTATE;
    if ((r = check_cfg(cfg)) != V90M_OK) return r;
    setup(m, cfg);
    return V90M_OK;
}

size_t v90m_mem_size(void) { return sizeof(struct v90m); }

v90m_t *v90m_init(void *mem, size_t len, const v90m_cfg *cfg, int *err)
{
    v90m_t *m = mem;
    int r;
    if (!mem || !cfg || !aligned(mem)) { seterr(err, V90M_EINVAL); return NULL; }
    if (len < sizeof *m) { seterr(err, V90M_ENOMEM); return NULL; }
    if ((r = check_cfg(cfg)) != V90M_OK) { seterr(err, r); return NULL; }
    m->owned = 0;
    setup(m, cfg);
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
    setup(m, cfg);
    seterr(err, V90M_OK);
    return m;
}

void v90m_destroy(v90m_t *m)
{
    if (!m) return;
    /* from a callback: v90m_process_* ends the block and frees it */
    if (m->busy) { m->dead = 1; return; }
    if (m->owned) free(m);
}

/* the modem's state, on the side orig says (ORIGINATING(m)): a constant
 * where the role is known, as in after_answer and after_orig */
static int failed_of(const v90m_t *m, int orig) { return orig ? m->u.o.phase == ORIG_FAILED : m->u.a.phase == ANS_FAILED; }
static int connected_of(const v90m_t *m, int orig) { return orig ? m->u.o.connected : m->u.a.connected; }
static int moving_of(const v90m_t *m, int orig) { return orig ? orig_moving(&m->u.o) : answer_moving(&m->u.a); }
static int retraining_of(const v90m_t *m, int orig)
{
    if (orig) return orig_retraining(&m->u.o);
    return m->u.a.retraining;
}
static int mod_of(const v90m_t *m, int orig) { return orig ? m->u.o.modulation : m->u.a.modulation; }
static int tx_of(const v90m_t *m, int orig) { return orig ? m->u.o.bitrate : m->u.a.bitrate; }
static int rx_of(const v90m_t *m, int orig)
{
    if (orig) return m->u.o.rx_bitrate > 0 ? m->u.o.rx_bitrate : m->u.o.bitrate;
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
        /* ANS_END_* in order */
        static const v90m_end ans_end[] = {
            V90M_END_NONE, V90M_END_FAILED, V90M_END_LOST, V90M_END_CLEARDOWN_FAR, V90M_END_CLEARDOWN_OURS,
            V90M_END_NO_COMMON, V90M_END_RETRAIN_FAILED, V90M_END_X2_FAILED, V90M_END_K56_FAILED
        };
        int e = answer_end(&m->u.a);
        return e >= 0 && e < (int)(sizeof ans_end / sizeof ans_end[0]) ? ans_end[e] : V90M_END_FAILED;
    }
}

static void event(v90m_t *m, v90m_event ev) { if (m->cfg.on_event && !m->dead) m->cfg.on_event(m->cfg.user, ev); }

/* the SNR and the round trip as the engine says (-1: not known now) */
static int snr_of(const v90m_t *m, int orig)
{
    return orig ? orig_snr_db10(&m->u.o) : m->u.a.connected ? answer_snr_db10(&m->u.a) : -1;
}
static int rtd_of(const v90m_t *m, int orig)
{
    return orig ? orig_rtd(&m->u.o) : m->u.a.connected ? answer_rtd(&m->u.a) : -1;
}
/* keep what is measured while connected, for the status after the end of the call */
static void keep_measures(v90m_t *m, int orig)
{
    int v = snr_of(m, orig);
    if (v >= 0) m->last_snr = v;
    v = rtd_of(m, orig);
    if (v >= 0) m->last_rtd = v;
}

/* after each sample: only reads the modem, on the side orig says */
static inline void after(v90m_t *m, int orig)
{
    int moving, rx, connected = connected_of(m, orig), failed = failed_of(m, orig);
    if (!connected && !m->was_connected && !failed) return;   /* start-up: nothing to report */
    moving = moving_of(m, orig);
    rx = rx_of(m, orig);
    /* kept only while data moves: a retrain's figures are not the call's */
    if (moving && connected && !failed && (uint8_t)(++m->meas_tick & 127) == 0) keep_measures(m, orig);
    if (connected && !m->was_connected) {
        m->was_connected = 1;
        m->was_mod = mod_of(m, orig); m->was_tx = tx_of(m, orig); m->was_rx = rx;
        event(m, V90M_EV_CONNECTED);
    } else if (m->was_connected && (mod_of(m, orig) != m->was_mod || tx_of(m, orig) != m->was_tx || rx != m->was_rx)) {
        m->was_mod = mod_of(m, orig); m->was_tx = tx_of(m, orig); m->was_rx = rx;
        event(m, V90M_EV_RATES);
    }
    if (moving != m->was_moving) {
        /* a stop that is neither a retrain nor the end is a rate renegotiation;
         * it counts when data moves again, or when it becomes a retrain or
         * ends the call (below), unless that end is a cleardown, ours or the
         * far end's, which stops data for good: a cleardown never counts */
        if (!moving) m->reneg_pending = connected && !retraining_of(m, orig) && !failed;
        else if (m->reneg_pending) {
            m->renegotiations++;
            m->reneg_pending = 0;
        }
        m->was_moving = moving;
        event(m, moving ? V90M_EV_DATA_MOVING : V90M_EV_DATA_STOPPED);
    }
    if (m->reneg_pending) {
        if (failed) {
            v90m_end e = end_of(m);
            if (e != V90M_END_CLEARDOWN_OURS && e != V90M_END_CLEARDOWN_FAR) m->renegotiations++;
            m->reneg_pending = 0;
        } else if (retraining_of(m, orig)) {
            m->renegotiations++;     /* the renegotiation became a retrain: counted in both */
            m->reneg_pending = 0;
        }
    }
    if (failed && !m->was_ended) {
        m->was_ended = 1;
        event(m, V90M_EV_ENDED);
    }
}
/* one per role, for the loops of v90m_process_*, which know theirs */
static void after_answer(v90m_t *m) { after(m, 0); }
static void after_orig(v90m_t *m) { after(m, 1); }

/* The end of a block: when a callback destroyed the modem after sample i,
 * the rest of the block is silence and the modem is freed (when owned). */
static int block_end(v90m_t *m, size_t i, size_t n, uint8_t *tx8, int16_t *tx16)
{
    m->busy = 0;
    if (!m->dead) return V90M_OK;
    if (tx8) { if (n > i + 1) memset(tx8 + i + 1, m->cfg.alaw ? 0xD5 : 0xFF, n - i - 1); }
    else for (i++; i < n; i++) tx16[i] = 0;
    if (m->owned) free(m);
    return V90M_OK;
}

int v90m_process_g711(v90m_t *m, const uint8_t *rx, uint8_t *tx, size_t n)
{
    size_t i;
    if (!m || (n && (!rx || !tx))) return V90M_EINVAL;
    m->busy = 1;
    if (ORIGINATING(m)) {
        int alaw = m->cfg.alaw != 0;
        for (i = 0; i < n; i++) {
            tx[i] = pcm_encode(alaw, orig_tick(&m->u.o, (int16_t)v90_octet_linear(alaw, rx[i])));
            after_orig(m);
            if (m->dead) break;
        }
    } else
        for (i = 0; i < n; i++) { tx[i] = answer_tick_pcm(&m->u.a, rx[i]); after_answer(m); if (m->dead) break; }
    return block_end(m, i, n, tx, NULL);
}

int v90m_process_lin_rx(v90m_t *m, const int16_t *rx, uint8_t *tx, size_t n)
{
    size_t i;
    if (!m || (n && (!rx || !tx))) return V90M_EINVAL;
    m->busy = 1;
    if (ORIGINATING(m)) {
        int alaw = m->cfg.alaw != 0;
        for (i = 0; i < n; i++) { tx[i] = pcm_encode(alaw, orig_tick(&m->u.o, rx[i])); after_orig(m); if (m->dead) break; }
    } else
        for (i = 0; i < n; i++) { tx[i] = answer_tick_lin(&m->u.a, rx[i]); after_answer(m); if (m->dead) break; }
    return block_end(m, i, n, tx, NULL);
}

int v90m_process_linear(v90m_t *m, const int16_t *rx, int16_t *tx, size_t n)
{
    size_t i;
    if (!m || (n && (!rx || !tx))) return V90M_EINVAL;
    m->busy = 1;
    if (ORIGINATING(m))
        for (i = 0; i < n; i++) { tx[i] = orig_tick(&m->u.o, rx[i]); after_orig(m); if (m->dead) break; }
    else
        for (i = 0; i < n; i++) { tx[i] = answer_tick(&m->u.a, rx[i]); after_answer(m); if (m->dead) break; }
    return block_end(m, i, n, NULL, tx);
}

int v90m_retrain(v90m_t *m)
{
    if (!m) return V90M_EINVAL;
    if (ORIGINATING(m)) return orig_retrain(&m->u.o) == 0 ? V90M_OK : V90M_ESTATE;
    return answer_retrain(&m->u.a) == 0 ? V90M_OK : V90M_ESTATE;
}

int v90m_cleardown(v90m_t *m)
{
    if (!m) return V90M_EINVAL;
    /* V.34 (11.7), V.90 (9.7), V.92 (9.11), either role; V.21, V.22, V.22bis,
     * V.32bis and Bell have none in data mode */
    if (ORIGINATING(m)) return orig_cleardown(&m->u.o) == 0 ? V90M_OK : V90M_ESTATE;
    return answer_cleardown(&m->u.a) == 0 ? V90M_OK : V90M_ESTATE;
}

void v90m_get_state(const v90m_t *m, v90m_state *st)
{
    int orig;
    if (!m || !st) return;
    orig = ORIGINATING(m);
    st->connected = connected_of(m, orig);
    st->moving = moving_of(m, orig);
    st->retraining = retraining_of(m, orig);
    st->modulation = (v90m_conn)mod_of(m, orig);
    st->tx_bps = tx_of(m, orig);
    st->rx_bps = rx_of(m, orig);
    st->used_v8 = orig ? m->u.o.used_v8 : m->u.a.used_v8;
    st->odp_bypass = orig ? 0 : answer_odp_bypass(&m->u.a);
    st->end = end_of(m);
}

void v90m_get_stats(const v90m_t *m, v90m_stats *st)
{
    int orig;
    if (!m || !st) return;
    orig = ORIGINATING(m);
    st->modulation = (v90m_conn)mod_of(m, orig);
    st->tx_bps = tx_of(m, orig);
    st->rx_bps = rx_of(m, orig);
    st->renegotiations = m->renegotiations;
    /* the last measured while connected stays after the call, until the next one */
    if (orig) {
        const orig_t *o = &m->u.o;
        st->snr_db10 = orig_snr_db10(o);
        st->rtd = orig_rtd(o);
        st->echo_delay = -1;
        st->retrains = (unsigned)orig_retrains(o);
    } else {
        const answer_t *a = &m->u.a;
        st->snr_db10 = a->connected ? answer_snr_db10(a) : -1;
        st->rtd = a->connected ? answer_rtd(a) : -1;
        st->echo_delay = answer_echo_delay(a);
        st->retrains = (unsigned)a->retrains;
    }
    if (st->snr_db10 < 0) st->snr_db10 = m->last_snr;
    if (st->rtd < 0) st->rtd = m->last_rtd;
    st->ec_in_db10 = st->ec_res_db10 = -1;
    st->ec_near_delay = st->ec_near_peak = st->ec_near_loss_db10 = -1;
    st->ec_far_delay = st->ec_far_peak = st->ec_far_loss_db10 = -1;
    if (!orig) {
        v34dx_echo_t e;
        if (answer_v34_echo(&m->u.a, &e) == 0) {
            st->ec_in_db10 = e.in_db10; st->ec_res_db10 = e.res_db10;
            st->ec_near_delay = e.near_delay; st->ec_near_peak = e.near_peak; st->ec_near_loss_db10 = e.near_loss_db10;
            st->ec_far_delay = e.far_delay; st->ec_far_peak = e.far_peak; st->ec_far_loss_db10 = e.far_loss_db10;
        }
    }
    {
        const v34dx_stop_t *stop = orig ? &m->u.o.v34_stop : &m->u.a.v34_stop;
        int v34 = st->modulation == V90M_CONN_V34;
        st->stop_by = v34 ? (v90m_side)stop->by : V90M_SIDE_NONE;
        st->stop_why = v34 ? (v90m_stop)stop->why : V90M_STOP_NONE;
    }
}

answer_t *v90m__answer(v90m_t *m) { return &m->u.a; }
orig_t   *v90m__orig(v90m_t *m) { return &m->u.o; }

/* ------------------------------------------------------------ start-stop */
struct v90m_async { int owned; v14_t v; };

size_t v90m_async_mem_size(void) { return sizeof(struct v90m_async); }
void v90m_async_reset(v90m_async_t *a) { int o; if (!a) return; o = a->owned; memset(a, 0, sizeof *a); a->owned = o; v14_init(&a->v); }
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
    if (!a) return 0;
    return (size_t)v14_write(&a->v, buf, n > 0x7FFFFFFF ? 0x7FFFFFFF : (int)n);
}
size_t v90m_async_read(v90m_async_t *a, uint8_t *buf, size_t max)
{
    if (!a) return 0;
    return (size_t)v14_read(&a->v, buf, max > 0x7FFFFFFF ? 0x7FFFFFFF : (int)max);
}
size_t v90m_async_tx_pending(const v90m_async_t *a) { return a ? (size_t)v14_tx_pending(&a->v) : 0; }
unsigned long v90m_async_frame_errors(const v90m_async_t *a) { return a ? (unsigned long)a->v.rx_frame_errs : 0; }

/* ------------------------------------------------------------------ V.42 */
struct v90m_ec { int owned; v42ec_t e; };

/* check_ec_cfg, cfg_in, cfg_out and status_out: api/v90modem_fields.h,
 * from its lists of the fields */

void v90m_ec_cfg_default(v90m_ec_cfg *c, int originator)
{
    v42ec_cfg_t i;
    if (!c) return;
    v42ec_cfg_default(&i, originator);
    cfg_out(c, &i);
}

int v90m_ec_reset(v90m_ec_t *e, const v90m_ec_cfg *cfg)
{
    v42ec_cfg_t c;
    int o;
    if (!e || !cfg || check_ec_cfg(cfg) != V90M_OK) return V90M_EINVAL;
    o = e->owned;
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
    if (!mem || !cfg || !aligned(mem) || check_ec_cfg(cfg) != V90M_OK) { seterr(err, V90M_EINVAL); return NULL; }
    if (len < sizeof *e) { seterr(err, V90M_ENOMEM); return NULL; }
    e->owned = 0; v90m_ec_reset(e, cfg); seterr(err, V90M_OK);
    return e;
}

v90m_ec_t *v90m_ec_create(const v90m_ec_cfg *cfg, int *err)
{
    v90m_ec_t *e;
    if (!cfg || check_ec_cfg(cfg) != V90M_OK) { seterr(err, V90M_EINVAL); return NULL; }
    if (!(e = malloc(sizeof *e))) { seterr(err, V90M_ENOMEM); return NULL; }
    e->owned = 1; v90m_ec_reset(e, cfg); seterr(err, V90M_OK);
    return e;
}

void   v90m_ec_destroy(v90m_ec_t *e) { if (e && e->owned) free(e); }
int    v90m_ec_tx_bit(v90m_ec_t *e) { return v42ec_tx_bit(&e->e); }
void   v90m_ec_rx_bit(v90m_ec_t *e, int bit) { v42ec_rx_bit(&e->e, bit); }
/* NULL (the header): every function checks its object but the two per-bit ones */
void   v90m_ec_tick(v90m_ec_t *e, unsigned ms) { if (e) v42ec_tick(&e->e, ms); }
void   v90m_ec_line_stopped(v90m_ec_t *e) { if (e) v42ec_line_stopped(&e->e); }
void   v90m_ec_line_restored(v90m_ec_t *e) { if (e) v42ec_line_restored(&e->e); }
size_t v90m_ec_write(v90m_ec_t *e, const uint8_t *d, size_t n) { return e ? v42ec_write(&e->e, d, n) : 0; }
size_t v90m_ec_write_space(const v90m_ec_t *e) { return e ? v42ec_write_space(&e->e) : 0; }
size_t v90m_ec_read(v90m_ec_t *e, uint8_t *d, size_t max) { return e ? v42ec_read(&e->e, d, max) : 0; }
size_t v90m_ec_read_avail(const v90m_ec_t *e) { return e ? v42ec_read_avail(&e->e) : 0; }
int    v90m_ec_tx_idle(const v90m_ec_t *e) { return e ? v42ec_tx_idle(&e->e) : V90M_EINVAL; }
int    v90m_ec_tx_busy(const v90m_ec_t *e) { return e ? e->e.hdlc_tx_on && v42hdlc_tx_busy(&e->e.htx) : V90M_EINVAL; }
void   v90m_ec_release(v90m_ec_t *e) { if (e) v42ec_release(&e->e); }
void   v90m_ec_abort(v90m_ec_t *e) { if (e) v42ec_abort(&e->e); }
int    v90m_ec_send_break(v90m_ec_t *e, int opt, int len10ms)
{
    if (!e || (opt != V90M_BRK_NONDESTRUCTIVE_INSEQ && opt != V90M_BRK_NONDESTRUCTIVE_EXPEDITED
               && opt != V90M_BRK_DESTRUCTIVE_EXPEDITED)) return V90M_EINVAL;
    return v42ec_send_break(&e->e, opt, len10ms) == 0 ? V90M_OK : V90M_ESTATE;
}
int    v90m_ec_get_break(v90m_ec_t *e, int *opt, int *len10ms) { return e ? v42ec_get_break(&e->e, opt, len10ms) : V90M_EINVAL; }
int    v90m_ec_state(const v90m_ec_t *e) { return e ? v42ec_state(&e->e) : V90M_EINVAL; }

void v90m_ec_get_status(const v90m_ec_t *e, v90m_ec_status *st)
{
    v42ec_status_t s;
    if (!e || !st) return;
    v42ec_get_status(&e->e, &s);
    status_out(st, &s, &e->e);
}

v42ec_t *v90m__ec(v90m_ec_t *e) { return &e->e; }

/* the bounds in the header hold on this platform */
typedef char v90m_check_mem[sizeof(struct v90m) <= V90M_MEM_SIZE ? 1 : -1];
typedef char v90m_check_ec_mem[sizeof(struct v90m_ec) <= V90M_EC_MEM_SIZE ? 1 : -1];
typedef char v90m_check_async_mem[sizeof(struct v90m_async) <= V90M_ASYNC_MEM_SIZE ? 1 : -1];
