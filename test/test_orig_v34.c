/* modem/orig: the calling modem up to V.34 data mode. Against the library's
 * answering modem (modem/answer offering V.34, V.32bis, V.22bis, V.21, no
 * V.90) over test/line34.h: the answerer behind a G.711 codec (or none) and
 * a hybrid, the caller at the far end of a loop with its own hybrid and its
 * own clock. V.8, Phase 2 (modem/p2call), Phases 3 and 4 and data (v34dx in
 * the call role); pseudo-random data both ways, every bit checked; retrains,
 * rate renegotiations and cleardowns begun by each side; the answerer going
 * away; rate ceilings; poorer lines at lower rates. There is no independent
 * V.34 answer modem to call (spandsp has no V.34): the answerer is the
 * library's, itself checked against the test peer sim/v34peer
 * (test_v34call, test_answer_v34). */
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "dxline.h"
#include "line34.h"
#include "../modem/orig.h"
#include "../modem/answer.h"

static answer_t A; static orig_t O; static line34_t L; static bend_t UA, UO;
static int a_mute, retrain_on_step;

static int16_t a_tick(void *u, int16_t rx) { int16_t y = answer_tick((answer_t *)u, rx); return a_mute ? 0 : y; }
static int16_t o_tick(void *u, int16_t rx) { return orig_tick((orig_t *)u, rx); }

#define MODES (V8_MOD_V34 | V8_MOD_V32 | V8_MOD_V22 | V8_MOD_V21)

static void start(const line34_cfg *lc, int max_tx, int max_rx, int echo_delay)
{
    answer_cfg ac; orig_cfg oc;
    bend_init(&UA, &A.v34.dx, 0xACE1, 0xBEEF1); bend_init(&UO, &O.v34.dx, 0xBEEF1, 0xACE1);
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = MODES; ac.getbit = bend_get; ac.putbit = bend_put; ac.user = &UA;
    ac.retrain_on_step = retrain_on_step;
    answer_init(&A, &ac);
    answer_v90_line(&A, lc->io_delay, 0, 0);
    memset(&oc, 0, sizeof oc);
    oc.level = 3000; oc.modes = MODES; oc.getbit = bend_get; oc.putbit = bend_put; oc.user = &UO;
    oc.max_tx_bps = max_tx; oc.max_rx_bps = max_rx; oc.echo_delay = echo_delay;
    oc.retrain_on_step = retrain_on_step;
    orig_init(&O, &oc);
    line34_init(&L, lc);
    a_mute = 0;
}

static void run(long samples)
{
    long i;
    for (i = 0; i < samples; i++) {
        (void)line34_step(&L, a_tick, &A, o_tick, &O);
        /* a retrain starts the engines afresh: the data sequences start afresh with them */
        if (A.v34.dx.tx_starts == 0) UA.seen_tx = 0;
        if (A.v34.dx.rx_starts == 0) UA.seen_rx = 0;
        if (O.v34.dx.tx_starts == 0) UO.seen_tx = 0;
        if (O.v34.dx.rx_starts == 0) UO.seen_rx = 0;
    }
}

static int both_moving(void) { return O.connected && orig_moving(&O) && A.connected && answer_moving(&A); }

static int to_data(int max_ms)
{
    long end = L.n + 8L * max_ms;
    while (L.n < end) {
        run(8);
        if (both_moving()) return 1;
        if (A.phase == ANS_FAILED || O.phase == ORIG_FAILED) return 0;
    }
    return 0;
}

static void sinks_reset(void) { UA.sink.n = UA.sink.errs = UO.sink.n = UO.sink.errs = 0; }

static void report(const char *what)
{
    printf("  %-34s %d sym/s  C->A %5d bit/s %7ld bits %ld err  A->C %5d bit/s %7ld bits %ld err  SNR %.1f dB, rtd %d\n",
           what, O.v34.dx.cfg.sym_rate, O.bitrate, UA.sink.n, UA.sink.errs, O.rx_bitrate, UO.sink.n, UO.sink.errs,
           orig_snr_db10(&O) / 10.0, orig_rtd(&O));
}

/* data for secs seconds both ways: none lost, none wrong */
static void data_ok(int secs)
{
    sinks_reset();
    run(8000L * secs);
    CHECK_EQ_I(both_moving(), 1);
    CHECK(UA.sink.n > (long)(secs - 1) * O.bitrate); CHECK(UO.sink.n > (long)(secs - 1) * O.rx_bitrate);
    CHECK_EQ_I(UA.sink.errs, 0); CHECK_EQ_I(UO.sink.errs, 0);
}

/* a call that reaches V.34 data mode: the two ends agree */
static int call_up(const char *what, const line34_cfg *lc, int max_tx, int max_rx, int min_rate)
{
    int ok;
    start(lc, max_tx, max_rx, 0);
    ok = to_data(30000);
    CHECK_EQ_I(ok, 1);
    if (!ok) { printf("  %s: no data mode (orig phase %d end %d, answer phase %d)\n", what, O.phase, O.end, A.phase); return 0; }
    CHECK_EQ_I(O.phase, ORIG_V34); CHECK_EQ_I(O.modulation, ORIG_MOD_V34); CHECK_EQ_I(O.used_v8, 1);
    CHECK_EQ_I(A.phase, ANS_V34); CHECK_EQ_I(A.modulation, ANS_MOD_V34);
    CHECK_EQ_I(O.bitrate, A.rx_bitrate); CHECK_EQ_I(O.rx_bitrate, A.bitrate);
    CHECK_EQ_I(O.bitrate, O.v34.dx.tx_rate); CHECK_EQ_I(O.rx_bitrate, O.v34.dx.rx_rate);
    CHECK(O.bitrate >= min_rate); CHECK(O.rx_bitrate >= min_rate);
    CHECK_EQ_I(orig_retraining(&O), 0); CHECK_EQ_I(orig_retrains(&O), 0);
    /* Phase 2 agreed: one symbol rate; what our INFO1c asked of the answerer is what it does */
    CHECK_EQ_I(O.v34.p2.sym_rate, A.v34.p2.sym_rate);
    CHECK_EQ_I(A.v34.dx.cfg.tx_preemph, O.v34.p2.info1c.sr[O.v34.p2.sym_idx].preemph);
    CHECK_EQ_I(A.v34.dx.cfg.tx_pwr_red, O.v34.p2.info1c.min_pwr_red);
    CHECK_EQ_I(A.v34.p2.rx_hi_carrier, O.v34.p2.tx_hi_carrier); CHECK_EQ_I(A.v34.p2.tx_hi_carrier, O.v34.p2.rx_hi_carrier);
    data_ok(4);
    report(what);
    return 1;
}

static const double LOOP_MILD[3] = { 0.1, 0.8, 0.1 };              /* -4 dB at 3.4 kHz */
static const double LOOP_LONG[3] = { 0.25, 0.6, 0.25 };            /* -5 dB at 1.9 kHz, -13 dB at 3 kHz */

int main(void)
{
    line34_cfg good, real, lc;
    int good_tx, good_rx, k;
    long t;

    /* a good line: no codec, 20 ms each way, an echo at each end, noise
     * 50 dB down, the caller's clock 30 ppm fast */
    memset(&good, 0, sizeof good);
    good.law = 0; good.d_ca = 160; good.d_ac = 160; good.gain = 0.5; good.echo_a = 0.3; good.echo_c = 0.3;
    good.noise = 1500.0 / 316.0; good.ppm = 30;
    /* a real one: a mu-law codec at the answerer, 100 ppm, carrier shift
     * 1 Hz each way, noise 40 dB down, a mild loop */
    real = good;
    real.law = 1; real.ppm = -100; real.foff_ca = 1.0; real.foff_ac = -1.0; real.noise = 15.0;
    real.loop = LOOP_MILD; real.nloop = 3;

    /* ---- the best the line gives ---- */
    if (call_up("good line, no codec", &good, 0, 0, 28800)) {
        good_tx = O.bitrate; good_rx = O.rx_bitrate;
        CHECK_EQ_I(O.v34.dx.cfg.sym_rate, 3200);
        /* 31200 is the most the V.34 engine (v34dx) takes from any line in
         * the library's tests; what we receive from modem/answer stops at
         * 28800, because its V.34 sends G.711 codec values always (as
         * behind a codec: v34_cfg in answer.c), and a mu-law quantiser is
         * about 38 dB */
        CHECK(good_tx >= 31200); CHECK(good_rx >= 28800);
        CHECK(orig_snr_db10(&O) > 360);
        /* RTDEc: the line's 40 ms and what the 30 ppm resampler of the model
         * adds; the answerer measures the same loop */
        CHECK(orig_rtd(&O) >= 320 && orig_rtd(&O) < 400);
        CHECK_NEAR(orig_rtd(&O), answer_rtd(&A), 4);
        CHECK_EQ_I(O.v34.p2.retrains, 0); CHECK_EQ_I(O.v34.p2.restarts, 0);
    } else good_tx = good_rx = 31200;

    /* ---- retrain begun by the caller (11.5.1.1), then by the answerer (11.5.1.2) ---- */
    for (k = 0; k < 2; k++) {
        long t0;
        if (!call_up(k ? "realistic line, again" : "realistic line (mu-law, -100 ppm, 1 Hz)", &real, 0, 0, 21600)) continue;
        t0 = L.n;
        if (k == 0) CHECK_EQ_I(orig_retrain(&O), 0); else CHECK_EQ_I(answer_retrain(&A), 0);
        run(1600);
        CHECK_EQ_I(O.connected, 1); CHECK_EQ_I(orig_moving(&O), 0); CHECK_EQ_I(orig_retraining(&O), 1);
        CHECK_EQ_I(orig_renegotiate(&O, 0), -1); CHECK_EQ_I(orig_cleardown(&O), -1);
        CHECK_EQ_I(to_data(25000), 1);
        printf("  retrain begun by the %s: data again %.1f s later;", k ? "answerer" : "caller", (L.n - t0) / 8000.0);
        CHECK_EQ_I(orig_retraining(&O), 0);
        CHECK_EQ_I(orig_retrains(&O), 1); CHECK_EQ_I(O.v34.retrains_ours, k ? 0 : 1);
        CHECK_EQ_I(A.retrains, 1); CHECK_EQ_I(A.v34.retrains_ours, k ? 1 : 0);
        CHECK_EQ_I(O.v34.connects, 2);
        CHECK(O.bitrate >= 21600 && O.rx_bitrate >= 21600);
        CHECK_EQ_I(O.bitrate, A.rx_bitrate); CHECK_EQ_I(O.rx_bitrate, A.bitrate);
        data_ok(4);
        report(k ? "after the answerer's retrain" : "after our retrain");
    }

    /* ---- rate renegotiation (11.6) begun by each side ---- */
    if (call_up("good line, renegotiations", &good, 0, 0, 28800)) {
        int was_tx = O.bitrate;
        v34dx_echo_t e;
        CHECK_EQ_I(A.v34.dx.cfg.retrain_on_step, 0); CHECK_EQ_I(O.v34.dx.cfg.retrain_on_step, 0);   /* off by default */
        CHECK_EQ_I(answer_v34_echo(&A, &e), 0);
        printf("  answerer's echo cancellers: %.1f dB in, %.1f dB left; near at %d+%d loss %.1f dB, far at %d+%d loss %.1f dB\n",
               -e.in_db10 / 10.0, -e.res_db10 / 10.0, e.near_delay, e.near_peak, e.near_loss_db10 / 10.0,
               e.far_delay, e.far_peak, e.far_loss_db10 / 10.0);
        CHECK(e.res_db10 > e.in_db10); CHECK(e.near_peak >= 0); CHECK(e.near_loss_db10 > 0);
        CHECK_EQ_I(orig_renegotiate(&O, 16800), 0);         /* we take no more than 16800 */
        run(80);
        CHECK_EQ_I(orig_moving(&O), 0); CHECK_EQ_I(orig_retraining(&O), 0); CHECK_EQ_I(O.connected, 1);
        CHECK_EQ_I(to_data(4000), 1);
        CHECK_EQ_I(O.rx_bitrate, 16800); CHECK_EQ_I(A.bitrate, 16800);
        CHECK_EQ_I(O.bitrate, was_tx);
        data_ok(3);
        report("after our renegotiation (16800 in)");
        CHECK_EQ_I(v34ans_renegotiate(&A.v34, 14400), 0);  /* the answerer takes no more than 14400 */
        for (t = 0; t < 400 && orig_moving(&O); t += 10) run(80);
        CHECK_EQ_I(orig_moving(&O), 0); CHECK_EQ_I(orig_retraining(&O), 0);
        CHECK_EQ_I(answer_moving(&A), 0); CHECK_EQ_I(A.retraining, 0);   /* not a retrain */
        CHECK_EQ_I(to_data(4000), 1);
        CHECK_EQ_I(O.bitrate, 14400); CHECK_EQ_I(A.rx_bitrate, 14400);
        CHECK_EQ_I(O.v34.dx.n_reneg, 2); CHECK_EQ_I(orig_retrains(&O), 0);
        data_ok(3);
        report("after the answerer's (14400 out)");
        /* ---- cleardown (11.7) begun by the caller ---- */
        CHECK_EQ_I(orig_cleardown(&O), 0);
        for (t = 0; t < 2000 && (O.phase != ORIG_FAILED || A.phase != ANS_FAILED); t += 10) run(80);
        printf("  our cleardown: over in %ld ms; answerer cleared %d\n", t, A.cleared);
        CHECK_EQ_I(O.phase, ORIG_FAILED); CHECK_EQ_I(O.end, ORIG_END_HANGUP); CHECK_EQ_I(O.connected, 0);
        CHECK_EQ_I(A.phase, ANS_FAILED); CHECK_EQ_I(A.cleared, 1);
        CHECK(t < 1000);
        CHECK_EQ_I(orig_tick(&O, 0), 0);
    }
    /* ---- cleardown begun by the answerer ---- */
    if (call_up("good line, the answerer clears", &good, 0, 0, 28800)) {
        CHECK_EQ_I(answer_cleardown(&A), 0);
        for (t = 0; t < 2000 && (O.phase != ORIG_FAILED || A.phase != ANS_FAILED); t += 10) run(80);
        printf("  the answerer's cleardown: over in %ld ms\n", t);
        CHECK_EQ_I(O.phase, ORIG_FAILED); CHECK_EQ_I(O.end, ORIG_END_CLEARDOWN);
        CHECK_EQ_I(A.phase, ANS_FAILED); CHECK_EQ_I(A.cleared, 2);
    }
    /* ---- the answerer goes away in data mode ---- */
    if (call_up("good line, the answerer goes away", &good, 0, 0, 28800)) {
        a_mute = 1;
        for (t = 0; t < 30000 && O.phase != ORIG_FAILED; t += 10) run(80);
        printf("  answerer gone: over after %ld ms, %d retrain(s)\n", t, orig_retrains(&O));
        CHECK_EQ_I(O.phase, ORIG_FAILED); CHECK_EQ_I(O.end, ORIG_END_LOST); CHECK_EQ_I(O.connected, 0);
        CHECK(t < 15000);
    }

    /* ---- the answerer goes away in training: retrains (11.3.2.1), then the end ---- */
    start(&good, 0, 0, 0);
    while (L.n < 8000L * 30 && O.v34.state != V34O_TRAINING) run(8);
    CHECK_EQ_I(O.v34.state, V34O_TRAINING);
    a_mute = 1;
    for (t = 0; t < 60000 && O.phase != ORIG_FAILED; t += 10) run(80);
    printf("  answerer gone in training: over after %.1f s, %d retrain(s); Phase 2 error %d\n", t / 1000.0, O.v34.retrains, O.v34.p2.error);
    CHECK_EQ_I(O.phase, ORIG_FAILED); CHECK_EQ_I(O.end, ORIG_END_NO_CARRIER); CHECK_EQ_I(O.connected, 0);
    CHECK(O.v34.retrains >= 1);
    CHECK_EQ_I(O.v34.error, V34O_ERR_PHASE2); CHECK_EQ_I(O.v34.p2.error, P2L_ERR_REV);

    /* ---- ceilings: what we send, what we receive ---- */
    start(&good, 14400, 9600, 0);
    CHECK_EQ_I(to_data(30000), 1);
    CHECK_EQ_I(O.bitrate, 14400); CHECK_EQ_I(O.rx_bitrate, 9600);
    CHECK_EQ_I(A.rx_bitrate, 14400); CHECK_EQ_I(A.bitrate, 9600);
    data_ok(3);
    report("ceilings 14400 out, 9600 in");
    /* our own echo's delay unknown: found in training */
    start(&good, 0, 0, -1);
    CHECK_EQ_I(to_data(30000), 1);
    CHECK(O.bitrate >= 28800 && O.rx_bitrate >= 28800);
    CHECK(O.v34.dx.ecd.delay >= 0 && O.v34.dx.ecd.delay <= 4);
    data_ok(3);
    report("own echo delay found");

    /* ---- poorer lines, lower rates ---- */
    {
        static const int snr[2] = { 30, 22 };
        int last = good_tx + good_rx;
        for (k = 0; k < 2; k++) {
            char name[48];
            lc = good; lc.noise = 1500.0 / pow(10, snr[k] / 20.0);
            sprintf(name, "noise %d dB down", snr[k]);
            if (!call_up(name, &lc, 0, 0, k ? 7200 : 14400)) continue;
            CHECK(O.bitrate + O.rx_bitrate < last);
            CHECK(O.v34.p2.info1c.sr[4].max_rate < 12);
            last = O.bitrate + O.rx_bitrate;
        }
        /* an echo from the far hybrid as well: the answerer's receiver
         * takes less from us (sim/v34peer as the caller gets the same) */
        lc = good; lc.far_a = 0.03;
        if (call_up("far echo 3 %, 40 ms late", &lc, 0, 0, 9600)) CHECK(O.rx_bitrate >= 24000);
        /* the path towards the caller grows by one 20 ms packet mid-call (a
         * jitter buffer that took one more), so the echo from the far hybrid
         * comes 160 samples later than the answerer's far canceller, trained
         * in Phase 3, looks for it: with retrain_on_step the answerer
         * retrains, which trains the cancellers again, rather than
         * renegotiate down, which does not.
         * The echo about where an ATA puts it (far_lag): inside the far
         * canceller's window, so that the call starts as fast as the lab's */
        lc = good; lc.far_a = 0.06; lc.d_ac = 1160; lc.d_ca = 1160; lc.far_lag = 56;
        retrain_on_step = 1;
        if (call_up("far echo, 290 ms round trip", &lc, 0, 0, 24000)) {
            int rx0 = A.rx_bitrate, rtd0 = answer_rtd(&A);
            CHECK_EQ_I(A.v34.dx.cfg.retrain_on_step, 1); CHECK_EQ_I(O.v34.dx.cfg.retrain_on_step, 1);
            L.c.d_ac += 160;
            run(8000L * 3);
            CHECK_EQ_I(to_data(30000), 1);
            printf("  path 20 ms longer: answerer %d retrains, %d renegotiations, receives %d bit/s (was %d)\n",
                   A.retrains, A.v34.dx.n_reneg, A.rx_bitrate, rx0);
            CHECK_EQ_I(A.retrains, 1); CHECK_EQ_I(A.v34.dx.n_reneg, 0);
            CHECK(A.rx_bitrate >= rx0 - 2400);
            CHECK(answer_rtd(&A) >= rtd0 + 150 && answer_rtd(&A) <= rtd0 + 170);   /* measured again */
            data_ok(3);
            report("after the retrain");
        }
        /* the same step, and the caller renegotiates at once, before the
         * answerer's own watch sees it (as a real one did): the answerer's
         * B1 fails, its echo uncancelled, and it retrains rather than end
         * the call */
        if (call_up("far echo, the caller renegotiates", &lc, 0, 0, 24000)) {
            int rx0 = A.rx_bitrate;
            L.c.d_ac += 160;
            run(2400);
            CHECK_EQ_I(orig_renegotiate(&O, 0), 0);
            run(8000L * 3);
            CHECK_EQ_I(to_data(30000), 1);
            printf("  path 20 ms longer, the caller renegotiates: answerer %d retrains, phase %d; receives %d bit/s (was %d)\n",
                   A.retrains, A.phase, A.rx_bitrate, rx0);
            CHECK_EQ_I(A.retrains, 1);
            CHECK(A.rx_bitrate >= rx0 - 2400);
            data_ok(3);
            report("after the retrain");
        }
        /* the path moves again between the retrain's Phase 2, which measures
         * the round trip, and its Phase 3, which trains the cancellers (a
         * burst of late packets, as a real ATA gave): Phase 3 searches for
         * the far echo near that round trip and finds where it is now */
        if (call_up("far echo, the path moves twice", &lc, 0, 0, 24000)) {
            int rx0 = A.rx_bitrate, echo_at;
            v34dx_echo_t e;
            L.c.d_ac += 160;
            for (t = 0; t < 8000L * 15 && A.v34.state != V34A_TRAINING; t += 8) run(8);
            CHECK_EQ_I(A.v34.state, V34A_TRAINING);
            L.c.d_ac += 160;
            CHECK_EQ_I(to_data(30000), 1);
            CHECK_EQ_I(answer_v34_echo(&A, &e), 0);
            echo_at = L.c.d_ac + L.c.d_ca + L.c.far_lag + 1;        /* line34's far echo, its larger tap */
            printf("  path 20 ms longer twice, the second after Phase 2: answerer %d retrains, far canceller at %d+%d for an echo at %d; receives %d bit/s (was %d)\n",
                   A.retrains, e.far_delay, e.far_peak, echo_at, A.rx_bitrate, rx0);
            CHECK(e.far_delay + e.far_peak >= echo_at - 4 && e.far_delay + e.far_peak <= echo_at + 4);
            CHECK(A.rx_bitrate >= rx0 - 2400);
            data_ok(3);
            report("after the retrain");
        }
        /* the path moves again after the retrain's far search, during the rest
         * of its training (a burst of late packets): the cancellers are
         * trained on an echo that is gone, the call goes on at a low rate,
         * and nothing falls further for rate_down to see. The watch on the
         * long run sees the SNR held 6 dB or more below the call's best for
         * 30 s and retrains */
        if (call_up("far echo, moves during training", &lc, 0, 0, 24000)) {
            int rx0 = A.rx_bitrate, low;
            run(8000L * 25);                                /* the watch settles its best */
            CHECK(A.v34.w_best > 0);
            L.c.d_ac += 160;
            for (t = 0; t < 8000L * 15 && !(A.v34.state == V34A_TRAINING && A.v34.dx.state >= DX_P3_J); t += 8) run(8);
            CHECK_EQ_I(A.v34.dx.state >= DX_P3_J, 1);
            L.c.d_ac += 160;
            CHECK_EQ_I(to_data(30000), 1);
            run(8000L * 5);
            low = A.rx_bitrate;
            CHECK_EQ_I(A.retrains, 1);
            for (t = 0; t < 8000L * 90 && A.retrains == 1; t += 800) run(800);
            CHECK_EQ_I(to_data(30000), 1);
            printf("  path moved during the retrain's training: answerer received %d bit/s, the watch retrained %.0f s later; receives %d bit/s (was %d)\n",
                   low, t / 8000.0 + 5, A.rx_bitrate, rx0);
            CHECK(low < rx0 - 2400);
            CHECK_EQ_I(A.retrains, 2);
            CHECK(A.rx_bitrate >= rx0 - 2400);
            data_ok(3);
            report("after the watch's retrain");
        }
        /* the same, and renegotiations every 20 s as the SNR slides (a real
         * one did, well within the 50 s the watch needs to settle and see
         * it low): a renegotiation only pauses the watch */
        if (call_up("far echo, moves during training, renegotiations", &lc, 0, 0, 24000)) {
            int rx0 = A.rx_bitrate, renegs = 0;
            run(8000L * 25);
            L.c.d_ac += 160;
            for (t = 0; t < 8000L * 15 && !(A.v34.state == V34A_TRAINING && A.v34.dx.state >= DX_P3_J); t += 8) run(8);
            L.c.d_ac += 160;
            CHECK_EQ_I(to_data(30000), 1);
            CHECK_EQ_I(A.retrains, 1);
            for (t = 0; t < 8000L * 90 && A.retrains == 1; t += 800) {
                if (t % (8000L * 20) == 0 && O.v34.dx.state == DX_DATA) { CHECK_EQ_I(orig_renegotiate(&O, 0), 0); renegs++; }
                run(800);
            }
            CHECK_EQ_I(to_data(30000), 1);
            printf("  path moved during the retrain's training, renegotiations every 20 s: the watch retrained %.0f s later after %d renegotiations; receives %d bit/s (was %d)\n",
                   t / 8000.0, renegs, A.rx_bitrate, rx0);
            CHECK_EQ_I(A.retrains, 2);
            CHECK(renegs >= 2);
            CHECK(t < 8000L * 60);
            CHECK(A.rx_bitrate >= rx0 - 2400);
            data_ok(3);
            report("after the watch's retrain");
        }
        retrain_on_step = 0;
        lc = good; lc.loop = LOOP_LONG; lc.nloop = 3;
        if (call_up("long loop (13 dB down at 3 kHz)", &lc, 0, 0, 9600)) {
            CHECK(O.bitrate < good_tx); CHECK(O.rx_bitrate < good_rx);
            /* the slope: pre-emphasis asked of the answerer (it was checked to do as asked above) */
            CHECK(O.v34.p2.info1c.sr[O.v34.p2.sym_idx].preemph > 0);
        }
    }

    T_REPORT("orig_v34");
}
