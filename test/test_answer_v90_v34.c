/* The answering modem with a V.90 caller that selects V.34 at the end of
 * V.90 Phase 2 (V.90 9.2.1.1.8, 9.2.2.1.9): V.8 selects V.90, Phase 2 runs,
 * INFO1a has bits 37:39 in 0..5, and from there the caller is the ANSWER
 * modem of V.34 Phase 3 (11.3.1.2/V.34: it sends S first) and we are the
 * call modem. The call must reach V.34 data mode and carry data both ways;
 * retrains go through V.90 Phase 2 again; cleardown either way ends it.
 * The caller is sim/v8call, sim/p2a and modem/v34dx in the answer role over
 * test/line34.h (mu-law codec at our side). */
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "../modem/answer.h"
#include "../sim/v8call.h"
#include "../sim/p2a.h"
#include "line34.h"

/* x^7 + x^6 + 1 sent; the receiver predicts each bit from the seven before */
typedef struct { unsigned tx, rx; long n, bad; } prbs_t;
static int p_get(void *u)
{
    prbs_t *p = (prbs_t *)u;
    int b = ((p->tx >> 6) ^ (p->tx >> 5)) & 1;
    p->tx = ((p->tx << 1) | (unsigned)b) & 127;
    return b;
}
static void p_put(void *u, int bit)
{
    prbs_t *p = (prbs_t *)u;
    int want = ((p->rx >> 6) ^ (p->rx >> 5)) & 1;
    if (p->n >= 7 && want != (bit & 1)) p->bad++;
    p->rx = ((p->rx << 1) | (unsigned)(bit & 1)) & 127;
    p->n++;
}

/* the caller: a V.90 analogue modem that asks for V.34 in INFO1a */
typedef enum { C_V8, C_PHASE2, C_TRAINING, C_DATA, C_RETRAIN, C_ENDED } c_state;
typedef struct {
    c_state state;
    int v90_next;           /* the next INFO1a asks for V.90 (nobody trains that here) */
    int32_t t;
    v8call_t v8; p2a_t p2; p2a_cfg pc; v34dx_t dx;
    int retrains, connects;
    void *user;
} caller_t;

static void c_init(caller_t *c, void *user, int hi_carrier, int preemph)
{
    v8_menu_t cm;
    memset(c, 0, sizeof *c);
    c->user = user;
    memset(&cm, 0, sizeof cm);
    cm.call_function = V8_CALLF_DATA;
    cm.modes = V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21; cm.n_mod_octets = 3;
    cm.has_pcm = 1; cm.pcm = V8_PCM_ANALOG;
    cm.has_prot = 1; cm.prot = V8_PROT_LAPM; cm.has_access = 1; cm.access = 0;
    v8call_init(&c->v8, 8000, 3000, &cm);
    c->pc.level = 3000; c->pc.v90 = 0; c->pc.up_sr = -1;
    c->pc.info0a.lo3000 = c->pc.info0a.hi3000 = 1; c->pc.info0a.lo3200 = c->pc.info0a.hi3200 = 1;
    c->pc.info0a.pwr_reduce = 1; c->pc.info0a.max_sr_diff = 5; c->pc.info0a.c1664 = 1;
    c->pc.v34_hi_carrier = hi_carrier; c->pc.v34_preemph = preemph;
    c->state = C_V8;
}

/* 11.3.1.2/V.34 as the answer modem, from what Phase 2 exchanged */
static void c_start_dx(caller_t *c)
{
    v34dx_cfg d;
    int idx = c->p2.info1a.up_sr;
    int32_t delay;
    memset(&d, 0, sizeof d);
    d.answer = 1;
    d.sym_rate = v34_symbol_rate(idx);
    /* our carrier and filter are INFO1d's for this symbol rate; theirs what INFO1a asked */
    v34_carrier(idx, c->p2.info1d.sr[idx].hi_carrier, &d.tx_car_num, &d.tx_car_den);
    v34_carrier(idx, c->p2.info1a.hi_carrier, &d.rx_car_num, &d.rx_car_den);
    d.tx_preemph = c->p2.info1d.sr[idx].preemph;
    d.tx_pwr_red = c->p2.info1d.min_pwr_red;
    d.level = 3000;
    d.rtd = c->p2.rtd;
    d.md_len = c->p2.info1d.md_len;
    /* S begins 70 ms after INFO1a ended; p2a stops 60 samples after that end */
    delay = ((int32_t)V34MOD_SPAN * 8000) / d.sym_rate + (d.tx_preemph ? V34MOD_NPRE / 2 : 0);
    d.lead = 560 - 60 - delay + 4000 / d.sym_rate + 32;
    d.peer_1664 = c->p2.info0d.c1664;
    d.getbit = p_get; d.putbit = p_put; d.user = c->user;
    c->state = v34dx_init(&c->dx, &d) != 0 ? C_ENDED : C_TRAINING;
}

static int16_t c_tick(void *u, int16_t rx)
{
    caller_t *c = (caller_t *)u;
    int16_t out = 0;
    switch (c->state) {
    case C_V8:
        if (v8call_tick(&c->v8, rx, &out) == V8C_DONE) { p2a_init(&c->p2, &c->pc); c->state = C_PHASE2; }
        break;
    case C_PHASE2:
        out = p2a_tick(&c->p2, rx);
        if (c->p2.state == P2A_DONE) {
            if (c->p2.info1a.v90) c->state = C_ENDED; else c_start_dx(c);
        } else if (c->p2.state == P2A_FAILED) c->state = C_ENDED;
        break;
    case C_TRAINING:
    case C_DATA:
        out = v34dx_tick(&c->dx, rx);
        if (c->dx.state == DX_DATA) {
            if (c->state == C_TRAINING) c->connects++;
            c->state = C_DATA;
        } else if (c->dx.state == DX_RETRAIN) {
            c->retrains++; c->state = C_RETRAIN; c->t = 0;
        } else if (c->dx.state == DX_FAILED) c->state = C_ENDED;
        else if (c->dx.state < DX_P4_TRN) c->state = C_TRAINING;
        break;
    case C_RETRAIN:
        /* V.90 9.5.2 whatever the mode: 70 ms of silence, then Tone A and Phase 2 of V.90 */
        if (++c->t >= 560) {
            c->pc.v90 = c->v90_next;
            p2a_init_retrain(&c->p2, &c->pc);
            c->state = C_PHASE2;
        }
        break;
    default:
        break;
    }
    return out;
}

static answer_t A; static caller_t C; static line34_t L; static prbs_t UA, UC;
static int retrain_on_step;

static int16_t a_tick(void *u, int16_t rx) { return answer_tick((answer_t *)u, rx); }

static void start(unsigned modes, int max_down, int max_up, int hi_carrier, int preemph, long mute_from, long mute_to)
{
    answer_cfg ac; line34_cfg lc;
    memset(&lc, 0, sizeof lc);
    lc.law = 1; lc.d_ca = 24; lc.d_ac = 20; lc.gain = 0.5; lc.echo_a = 0.3; lc.echo_c = 0.3;
    lc.mute_ca_from = (int)mute_from; lc.mute_ca_to = (int)mute_to;
    memset(&UA, 0, sizeof UA); memset(&UC, 0, sizeof UC); UA.tx = 1; UC.tx = 77;
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = modes; ac.getbit = p_get; ac.putbit = p_put; ac.user = &UA;
    ac.retrain_on_step = retrain_on_step;
    answer_init(&A, &ac);
    answer_v90_line(&A, 0, max_down, max_up);
    c_init(&C, &UC, hi_carrier, preemph); line34_init(&L, &lc);
}

static void run(long samples) { while (samples-- > 0) (void)line34_step(&L, a_tick, &A, c_tick, &C); }

static int to_data(int max_ms)
{
    long end = L.n + 8L * max_ms;
    while (L.n < end) {
        run(8);
        if (A.connected && !A.retraining && C.state == C_DATA) return 1;
        if (A.phase == ANS_FAILED || C.state == C_ENDED) return 0;
    }
    return 0;
}

/* three seconds of data, both ways, without an error */
static void data_ok(const char *what)
{
    UA.n = UA.bad = UC.n = UC.bad = 0;
    run(8000L * 3);
    CHECK_EQ_I(A.connected, 1); CHECK_EQ_I(A.retraining, 0);
    CHECK_EQ_I(A.phase, ANS_V34); CHECK_EQ_I(A.modulation, ANS_MOD_V34);
    CHECK(UA.n > 2L * A.rx_bitrate); CHECK(UC.n > 2L * A.bitrate);
    CHECK_EQ_I(UA.bad, 0); CHECK_EQ_I(UC.bad, 0);
    printf("  %s: %d bit/s to it, %d bit/s from it; %ld / %ld bits, %ld / %ld bad; SNR %d.%d dB, rtd %d\n",
           what, A.bitrate, A.rx_bitrate, UC.n, UA.n, UC.bad, UA.bad,
           answer_snr_db10(&A) / 10, answer_snr_db10(&A) % 10, answer_rtd(&A));
}

#define ALL (ANS_MODE_V90 | V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21)

int main(void)
{
    long t;

    /* ---- the call: V.8 selects V.90, INFO1a selects V.34 ---- */
    start(ALL, 0, 0, 0, 0, 0, 0);
    CHECK_EQ_I(answer_snr_db10(&A), -1); CHECK_EQ_I(answer_rtd(&A), -1);
    CHECK_EQ_I(to_data(25000), 1);
    printf("  V.90 caller that selects V.34: data mode at %.2f s\n", L.n / 8000.0);
    if (A.phase == ANS_FAILED) printf("    failed: p2 state %d err %d\n", (int)A.p2.state, A.p2.error);
    CHECK_EQ_I(A.v8.selected, V8_SEL_V90);
    CHECK_EQ_I(A.p2.state, P2D_DONE); CHECK_EQ_I(A.p2.info1a.v90, 0);
    CHECK_EQ_I(A.phase, ANS_V34);
    CHECK_EQ_I(A.modulation, ANS_MOD_V34);
    CHECK_EQ_I(A.v34.dx.cfg.answer, 0);                 /* we are the call modem */
    CHECK_EQ_I(C.dx.cfg.answer, 1);
    CHECK(C.dx.tx_first[DXS_S] > 0);
    CHECK(A.bitrate >= 24000 && A.bitrate <= 33600);
    CHECK(A.rx_bitrate >= 24000 && A.rx_bitrate <= 33600);
    CHECK_EQ_I(A.bitrate, C.dx.rx_rate); CHECK_EQ_I(A.rx_bitrate, C.dx.tx_rate);
    CHECK_EQ_I(A.retrains, 0);
    data_ok("data");
    /* status: the engine's SNR, the round trip that V.90 Phase 2 measured (24 + 20 samples) */
    CHECK(answer_snr_db10(&A) >= 250 && answer_snr_db10(&A) <= 600);
    CHECK_NEAR(answer_rtd(&A), 44, 6);

    /* a retrain of ours: Phase 2 of V.90 again (9.2.1.1.8), the caller selects V.34 again */
    CHECK_EQ_I(answer_retrain(&A), 0);
    run(8000);
    CHECK_EQ_I(A.connected, 1); CHECK_EQ_I(A.retraining, 1); CHECK_EQ_I(A.retrains, 1);
    CHECK(A.phase == ANS_RETRAIN || A.phase == ANS_P2 || A.phase == ANS_V34);
    CHECK_EQ_I(answer_retrain(&A), -1);                 /* not from here */
    CHECK_EQ_I(to_data(15000), 1);
    CHECK_EQ_I(C.retrains, 1); CHECK_EQ_I(C.connects, 2);
    CHECK_EQ_I(A.p2.n_info0d, 0);                       /* that Phase 2 had no INFO0 */
    data_ok("after our retrain");
    CHECK_NEAR(answer_rtd(&A), 44, 6);

    /* the caller's retrain: its Tone A in data mode */
    CHECK_EQ_I(v34dx_retrain(&C.dx), 0);
    run(8000);
    CHECK_EQ_I(A.connected, 1); CHECK_EQ_I(A.retraining, 1); CHECK_EQ_I(A.retrains, 2);
    CHECK_EQ_I(to_data(15000), 1);
    CHECK_EQ_I(C.connects, 3);
    data_ok("after the caller's retrain");

    /* our cleardown */
    CHECK_EQ_I(answer_cleardown(&A), 0);
    for (t = 0; t < 2000 && A.phase != ANS_FAILED; t += 10) run(80);
    CHECK_EQ_I(A.phase, ANS_FAILED); CHECK_EQ_I(answer_end(&A), ANS_END_CLEARDOWN_OURS); CHECK_EQ_I(A.connected, 0);
    run(4000);
    CHECK_EQ_I(C.dx.state, DX_FAILED); CHECK_EQ_I(C.dx.error, DX_ERR_CLEARDOWN);
    CHECK_EQ_I(answer_snr_db10(&A), -1); CHECK_EQ_I(answer_rtd(&A), -1);

    /* ---- retrain_on_step: the call's best SNR outlives a retrain through
     * V.90's Phase 2: one that trained badly (here, noise that came with it)
     * is seen against it ---- */
    retrain_on_step = 1;
    start(ALL, 0, 0, 0, 0, 0, 0);
    CHECK_EQ_I(to_data(25000), 1);
    run(8000L * 25);
    CHECK(A.v34.w.best > 0);
    CHECK_EQ_I(answer_retrain(&A), 0);
    L.c.noise = 60;
    CHECK_EQ_I(to_data(15000), 1);
    for (t = 0; t < 8000L * 70 && A.retrains == 1; t += 800) run(800);
    printf("  a retrain that trained badly: the watch retrained %.0f s later\n", t / 8000.0);
    CHECK_EQ_I(A.retrains, 2);
    CHECK_EQ_I(to_data(15000), 1);
    CHECK_EQ_I(A.v34.w.fails, 1); CHECK_EQ_I(A.v34.w.hold, 480000);
    retrain_on_step = 0;

    /* ---- the caller asks for the high carrier and a pre-emphasis filter;
     * rate ceilings; then the caller clears down ---- */
    start(ALL, 14400, 9600, 1, 3, 0, 0);
    CHECK_EQ_I(to_data(25000), 1);
    CHECK_EQ_I(A.modulation, ANS_MOD_V34);
    CHECK_EQ_I(A.p2.info1a.hi_carrier, 1); CHECK_EQ_I(A.p2.info1a.preemph, 3);
    CHECK_EQ_I(A.v34.dx.cfg.tx_preemph, 3);
    CHECK_EQ_I(A.bitrate, 14400); CHECK_EQ_I(A.rx_bitrate, 9600);
    data_ok("high carrier, filter 3, ceilings");
    CHECK_EQ_I(v34dx_cleardown(&C.dx), 0);
    for (t = 0; t < 2000 && A.phase != ANS_FAILED; t += 10) run(80);
    CHECK_EQ_I(A.phase, ANS_FAILED); CHECK_EQ_I(answer_end(&A), ANS_END_CLEARDOWN_FAR); CHECK_EQ_I(A.connected, 0);

    /* ---- a retrain after which the caller wants V.90: Phase 3 of V.90 begins ---- */
    start(ALL, 0, 0, 0, 0, 0, 0);
    CHECK_EQ_I(to_data(25000), 1);
    C.v90_next = 1;
    CHECK_EQ_I(answer_retrain(&A), 0);
    for (t = 0; t < 15000 && A.phase != ANS_P3 && A.phase != ANS_FAILED; t += 10) run(80);
    CHECK_EQ_I(A.phase, ANS_P3); CHECK_EQ_I(A.p2.info1a.v90, 1);
    CHECK_EQ_I(A.connected, 1); CHECK_EQ_I(A.retraining, 1);

    /* ---- the caller's signal stops in data mode: the call ends or a retrain
     * begins, as with a V.34 caller; nothing stays "connected" on a dead line ---- */
    start(ALL, 0, 0, 0, 0, 0, 0);
    CHECK_EQ_I(to_data(25000), 1);
    run(4000);
    L.c.mute_ca_from = (int)L.n; L.c.mute_ca_to = 0x7fffffff;
    for (t = 0; t < 30000 && A.phase != ANS_FAILED; t += 10) run(80);
    printf("  caller's signal lost in data mode: over after %ld ms, %d retrain(s)\n", t, A.retrains);
    CHECK_EQ_I(A.phase, ANS_FAILED); CHECK_EQ_I(A.connected, 0); CHECK(answer_end(&A) != ANS_END_CLEARDOWN_FAR && answer_end(&A) != ANS_END_CLEARDOWN_OURS);
    CHECK(t < 12000);

    /* ---- V.34 not offered here: INFO1a's choice cannot be served ---- */
    start(ANS_MODE_V90 | V8_MOD_V22 | V8_MOD_V21, 0, 0, 0, 0, 0, 0);
    CHECK_EQ_I(to_data(25000), 0);
    CHECK_EQ_I(A.v8.selected, V8_SEL_V90); CHECK_EQ_I(A.p2.info1a.v90, 0);
    CHECK_EQ_I(A.phase, ANS_FAILED); CHECK_EQ_I(A.connected, 0);

    T_REPORT("answer_v90_v34");
}
