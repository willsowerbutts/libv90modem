/* modem/v34dx: the V.34 Phase 3 / Phase 4 / data engine, answer role against
 * call role, over a two-way path. Segment lengths and time limits of V.34
 * 11.3 and 11.4, the rates reached, bit errors in both directions, what the
 * far receiver may ask of our transmitter, rate renegotiation, cleardown,
 * and the conditions that must end in a retrain. */
#include "test.h"
#include "dxline.h"
#include "../modem/v34ans.h"

typedef struct {
    v34dx_t a, c; bend_t ua, uc; dxline_t line;
    int16_t arx, crx;
    double erle_a, erle_c;              /* echo attenuation measured during J */
    double pw_a, pw_c; long pw_n;       /* transmit power, since the last sinks_reset() */
    long n; long data_a, data_c;        /* sample clock; when each side reached data mode */
} pair_t;
static pair_t P;

static void mkcfg(v34dx_cfg *a, v34dx_cfg *c, int sym_idx, int a_hi, int c_hi, const dxline_cfg *lc)
{
    memset(a, 0, sizeof *a); memset(c, 0, sizeof *c);
    a->answer = 1; c->answer = 0;
    a->sym_rate = c->sym_rate = v34_symbol_rate(sym_idx);
    v34_carrier(sym_idx, a_hi, &a->tx_car_num, &a->tx_car_den);
    c->rx_car_num = a->tx_car_num; c->rx_car_den = a->tx_car_den;
    v34_carrier(sym_idx, c_hi, &c->tx_car_num, &c->tx_car_den);
    a->rx_car_num = c->tx_car_num; a->rx_car_den = c->tx_car_den;
    a->level = c->level = 3000;
    a->rtd = c->rtd = lc->d_ca + lc->d_ac;
    a->echo_delay = lc->echo_a_delay; c->echo_delay = 0;
    a->lead = 456;
    a->peer_1664 = c->peer_1664 = 1;
    a->getbit = c->getbit = bend_get; a->putbit = c->putbit = bend_put;
    a->user = &P.ua; c->user = &P.uc;
}

static void pair_init(const v34dx_cfg *a, const v34dx_cfg *c, const dxline_cfg *lc)
{
    memset(&P, 0, sizeof P);
    bend_init(&P.ua, &P.a, 0x1234567, 0x7654321); bend_init(&P.uc, &P.c, 0x7654321, 0x1234567);
    dxline_init(&P.line, lc);
    CHECK_EQ_I(v34dx_init(&P.a, a), 0);
    CHECK_EQ_I(v34dx_init(&P.c, c), 0);
    P.data_a = P.data_c = -1;
}

static void pair_run(long samples)
{
    long i;
    for (i = 0; i < samples; i++) {
        int16_t atx = v34dx_tick(&P.a, P.arx), ctx = v34dx_tick(&P.c, P.crx);
        dxline_step(&P.line, atx, ctx, &P.arx, &P.crx);
        P.n++; P.pw_a += (double)atx * atx; P.pw_c += (double)ctx * ctx; P.pw_n++;
        if (P.a.state == DX_P3_J && P.a.t == 200) P.erle_a = 10 * log10((P.a.pw_in + 1.0) / (P.a.pw_res + 1.0));
        if (P.c.state == DX_P3_J && P.c.t == 200) P.erle_c = 10 * log10((P.c.pw_in + 1.0) / (P.c.pw_res + 1.0));
        if (P.data_a < 0 && P.a.state == DX_DATA) P.data_a = P.n;
        if (P.data_c < 0 && P.c.state == DX_DATA) P.data_c = P.n;
    }
}

/* run until both are in data mode (1), or one has stopped / time is up (0) */
static int pair_to_data(int max_ms)
{
    long end = P.n + 8L * max_ms;
    while (P.n < end) {
        pair_run(8);
        if (P.a.state == DX_DATA && P.c.state == DX_DATA) return 1;
        if (P.a.state >= DX_RETRAIN || P.c.state >= DX_RETRAIN) return 0;
    }
    return 0;
}

static void sinks_reset(void) { P.ua.sink.n = P.ua.sink.errs = P.uc.sink.n = P.uc.sink.errs = 0; P.pw_a = P.pw_c = 0; P.pw_n = 0; }

static const dxline_cfg CLEAN = { 20, 24, 0.5, 0.5, 0.3, 0.3, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static void report(const char *what)
{
    printf("  %-34s %d sym/s  A->C %5d bit/s %7ld bits %ld errors   C->A %5d bit/s %7ld bits %ld errors\n",
           what, P.a.cfg.sym_rate, P.a.tx_rate, P.uc.sink.n, P.uc.sink.errs, P.a.rx_rate, P.ua.sink.n, P.ua.sink.errs);
}

int main(void)
{
    v34dx_cfg a, c;
    dxline_cfg lc;
    int k;

    /* ---- clean line, each symbol rate, both carriers: lengths, limits, data ---- */
    for (k = 0; k < 6; k++) {
        static const int idx[3] = { 4, 3, 0 }, top[3] = { 31200, 28800, 21600 };
        int si = idx[k / 2], hi = k & 1, S = v34_symbol_rate(si);
        long t_sbar_c;
        mkcfg(&a, &c, si, hi, !hi, &CLEAN);
        pair_init(&a, &c, &CLEAN);
        CHECK_EQ_I(pair_to_data(8000), 1);
        /* 11.3.1.2: S 128 T, S-bar 16 T, PP 288 T, TRN >= 512 T and (no MD) within 2000 ms */
        CHECK_EQ_I(P.a.tx_count[DXS_PP], 288); CHECK_EQ_I(P.c.tx_count[DXS_PP], 288);
        CHECK(P.a.tx_count[DXS_TRN] >= 512 && P.a.tx_count[DXS_TRN] * 1000L / S <= 2000);
        CHECK(P.c.tx_count[DXS_TRN] >= 512 && P.c.tx_count[DXS_TRN] * 1000L / S <= 2000);
        /* Phase 3 and Phase 4 each have one S / S-bar from the answer modem; the call modem has one */
        CHECK_EQ_I(P.a.tx_count[DXS_S], 2 * 128); CHECK_EQ_I(P.a.tx_count[DXS_SBAR], 2 * 16);
        CHECK_EQ_I(P.c.tx_count[DXS_S], 128); CHECK_EQ_I(P.c.tx_count[DXS_SBAR], 16);
        /* J in whole repetitions; one J' from the call modem, none from the answer modem */
        CHECK_EQ_I(P.a.tx_count[DXS_J] % 8, 0); CHECK_EQ_I(P.c.tx_count[DXS_J] % 8, 0);
        CHECK_EQ_I(P.c.tx_count[DXS_JP], 8); CHECK_EQ_I(P.a.tx_count[DXS_JP], 0);
        CHECK_EQ_I(P.a.got_jp, 1);
        /* 11.3.2.2.1: the caller's S / S-bar within 600 ms + a round trip of the start of our J
         * (the call modem may wait 500 ms; this one does not) */
        t_sbar_c = P.c.tx_first[DXS_PP];
        CHECK(t_sbar_c - P.a.tx_first[DXS_J] < 8 * 600);
        /* 11.4.2.2.1: J' within 100 ms + a round trip of our S / S-bar transition */
        CHECK(P.a.t_jp - (P.a.tx_first[DXS_TRN4]) < 8 * 100 + P.a.cfg.rtd + 8 * 20);
        /* 11.4.1.x.2: TRN of Phase 4 at least 512 T, at most 2000 ms */
        CHECK(P.a.tx_count[DXS_TRN4] >= 512 && P.a.tx_count[DXS_TRN4] * 1000L / S <= 2000);
        CHECK(P.c.tx_count[DXS_TRN4] >= 512 && P.c.tx_count[DXS_TRN4] * 1000L / S <= 2000);
        /* MP in whole sequences of 88 bits, E 20 bits, with four points */
        CHECK_EQ_I(P.a.tx_count[DXS_MP] % 44, 0); CHECK_EQ_I(P.c.tx_count[DXS_MP] % 44, 0);
        CHECK(P.a.mpa_sent >= 1 && P.c.mpa_sent >= 1);
        CHECK_EQ_I(P.a.tx_count[DXS_E], 10); CHECK_EQ_I(P.c.tx_count[DXS_E], 10);
        /* 11.4.2.x.2: E within 2500 ms of the start of Phase 4 */
        CHECK(P.data_a - P.a.tx_first[DXS_S] < 8 * 2500 + 3 * P.a.cfg.rtd);
        /* the rates: the same at both ends, the top of Table 8 for this symbol rate */
        CHECK_EQ_I(P.a.tx_rate, P.c.rx_rate); CHECK_EQ_I(P.a.rx_rate, P.c.tx_rate);
        if (S == 2400) {
            /* the echo canceller learns less from the narrow TRN: the last step may be missing */
            CHECK(P.a.tx_rate >= 19200 && P.a.rx_rate >= 19200);
        } else {
            CHECK_EQ_I(P.a.tx_rate, top[k / 2]); CHECK_EQ_I(P.a.rx_rate, top[k / 2]);
        }
        CHECK(P.erle_a > (S == 2400 ? 27 : 45)); CHECK(P.erle_c > (S == 2400 ? 27 : 45));
        sinks_reset();
        pair_run(8000L * 4);
        CHECK_EQ_I(P.a.state, DX_DATA); CHECK_EQ_I(P.c.state, DX_DATA);
        CHECK(P.ua.sink.n > 3 * P.a.rx_rate); CHECK(P.uc.sink.n > 3 * P.a.tx_rate);
        CHECK_EQ_I(P.ua.sink.errs, 0); CHECK_EQ_I(P.uc.sink.errs, 0);
        printf("  data after %.2f s (Phase 3 + 4), SNR at MP: answer %.1f dB, call %.1f dB; echo down %.0f / %.0f dB\n",
               P.data_a / 8000.0, P.a.snr_db10 / 10.0, P.c.snr_db10 / 10.0, P.erle_a, P.erle_c);
        report(hi ? "clean, answer on the high carrier" : "clean, answer on the low carrier");
    }

    /* ---- noise: the rate follows the line, the data stays clean ---- */
    {
        static const int snr[5] = { 38, 32, 26, 20, 14 };
        int last = 99999;
        for (k = 0; k < 5; k++) {
            lc = CLEAN;
            lc.noise_a = lc.noise_c = 1500.0 / pow(10, snr[k] / 20.0);     /* the signal arrives at RMS 1500 */
            mkcfg(&a, &c, 4, 1, 0, &lc);
            pair_init(&a, &c, &lc);
            CHECK_EQ_I(pair_to_data(8000), 1);
            sinks_reset();
            pair_run(8000L * 8);
            CHECK_EQ_I(P.a.state, DX_DATA); CHECK_EQ_I(P.c.state, DX_DATA);
            CHECK(P.ua.sink.n > 7 * P.a.rx_rate); CHECK(P.uc.sink.n > 7 * P.a.tx_rate);
            CHECK(P.ua.sink.errs * 100000 <= P.ua.sink.n); CHECK(P.uc.sink.errs * 100000 <= P.uc.sink.n);
            CHECK(P.a.rx_rate <= last && P.a.rx_rate >= 2400 * (snr[k] - 12) * 4 / 9 - 2400);
            CHECK_EQ_I(P.a.rate_drops + P.c.rate_drops + P.a.n_reneg, 0);
            last = P.a.rx_rate;
            printf("  noise %d dB below the signal:", snr[k]); report("");
        }
    }

    /* ---- what a far receiver may ask of our transmitter ---- */
    for (k = 0; k < 9; k++) {
        static const char *name[9] = { "16-point TRN / MP / E", "32-state trellis", "64-state trellis", "non-linear encoder",
            "expanded shaping", "precoder (MP Type 1)", "pre-emphasis 7, 3 dB less power", "pre-emphasis 3, all of the above", "all asked of the caller too" };
        double ref_pw;
        mkcfg(&a, &c, 4, 1, 0, &CLEAN);
        pair_init(&a, &c, &CLEAN);
        CHECK_EQ_I(pair_to_data(8000), 1); sinks_reset(); pair_run(8000);
        ref_pw = P.pw_a / P.pw_n;
        mkcfg(&a, &c, 4, 1, 0, &CLEAN);
        if (k == 0 || k >= 7) c.ask_16pt = 1;
        if (k == 1) c.ask_trellis = 1;
        if (k == 2 || k >= 7) c.ask_trellis = 2;
        if (k == 3 || k >= 7) c.ask_nonlinear = 1;
        if (k == 4 || k >= 7) c.ask_shaping = 1;
        if (k == 5 || k >= 7) { c.ask_h[0][0] = 4096; c.ask_h[0][1] = -1638; c.ask_h[1][0] = -2048; c.ask_h[2][1] = 819; }
        if (k == 6) { a.tx_preemph = 7; a.tx_pwr_red = 3; }
        if (k >= 7) { a.tx_preemph = 3; a.tx_pwr_red = 1; }
        if (k == 8) {
            a.ask_16pt = 1; a.ask_trellis = 1; a.ask_nonlinear = 1; a.ask_shaping = 1;
            a.ask_h[0][0] = 3000; a.ask_h[1][1] = 1500; c.tx_preemph = 10; c.tx_pwr_red = 2;
        }
        pair_init(&a, &c, &CLEAN);
        CHECK_EQ_I(pair_to_data(8000), 1);
        if (c.ask_16pt) {
            CHECK_EQ_I(P.a.tx_count[DXS_E], 5);
            CHECK_EQ_I(P.a.tx_count[DXS_MP] % (a.ask_h[0][0] ? 47 : 22), 0);    /* 88 or 188 bits, 4 a symbol */
        } else {
            CHECK_EQ_I(P.a.tx_count[DXS_E], 10);
        }
        CHECK_EQ_I(P.a.tpar.cfg.trellis, c.ask_trellis); CHECK_EQ_I(P.a.tpar.cfg.nonlinear, c.ask_nonlinear);
        CHECK_EQ_I(P.a.tpar.cfg.expanded, c.ask_shaping); CHECK_EQ_I(P.a.tpar.precode, c.ask_h[0][0] != 0);
        CHECK_EQ_I(P.a.tpar.cfg.h[0][1], c.ask_h[0][1]);
        CHECK_EQ_I(P.c.tpar.cfg.trellis, a.ask_trellis); CHECK_EQ_I(P.c.tpar.precode, a.ask_h[0][0] != 0);
        sinks_reset();
        pair_run(8000L * 4);
        CHECK_EQ_I(P.a.state, DX_DATA); CHECK_EQ_I(P.c.state, DX_DATA);
        CHECK(P.ua.sink.n > 3 * P.a.rx_rate); CHECK(P.uc.sink.n > 3 * P.a.tx_rate);
        CHECK_EQ_I(P.ua.sink.errs, 0); CHECK_EQ_I(P.uc.sink.errs, 0);
        CHECK(P.a.tx_rate >= 24000 && P.a.rx_rate >= 24000);
        /* transmit power: nominal, less the reduction asked, with the non-linear encoder too
         * (a real modem scales its encoded signal back and expects the same of us) */
        CHECK_NEAR(10 * log10(P.pw_a / P.pw_n / ref_pw), -a.tx_pwr_red, 0.3);
        report(name[k]);
    }

    /* ---- our own path delayed by a driver and not known; a far echo a round trip late ---- */
    {
        lc = CLEAN; lc.echo_a_delay = 160;
        mkcfg(&a, &c, 4, 1, 0, &lc); a.echo_delay = -1;
        pair_init(&a, &c, &lc);
        CHECK_EQ_I(pair_to_data(8000), 1);
        CHECK(P.a.ecd.delay >= 148 && P.a.ecd.delay <= 161);
        CHECK(P.a.tx_count[DXS_TRN] * 1000L / 3200 <= 2000);
        sinks_reset(); pair_run(8000L * 4);
        CHECK_EQ_I(P.ua.sink.errs, 0); CHECK_EQ_I(P.uc.sink.errs, 0); CHECK(P.a.rx_rate >= 28800);
        printf("  echo delay found: %d samples (true 160);", P.a.ecd.delay); report("");

        lc = CLEAN; lc.d_ca = 300; lc.d_ac = 340; lc.far_a = 0.15; lc.echo_a_delay = 40;
        mkcfg(&a, &c, 4, 1, 0, &lc); a.rtd = 640 + 5;     /* Phase 2 is a few samples out */
        pair_init(&a, &c, &lc);
        CHECK_EQ_I(pair_to_data(9000), 1);
        CHECK_EQ_I(P.a.far_on, 1); CHECK_EQ_I(P.c.far_on, 1);
        sinks_reset(); pair_run(8000L * 4);
        CHECK_EQ_I(P.ua.sink.errs, 0); CHECK_EQ_I(P.uc.sink.errs, 0); CHECK(P.a.rx_rate >= 28800);
        printf("  far echo 80 ms late, 16 dB down: echo down %.0f dB;", P.erle_a); report("");
        CHECK(P.erle_a > 40);
    }

    /* ---- ceilings, symmetric and asymmetric rates (MP bits 20:27, 50) ---- */
    {
        mkcfg(&a, &c, 4, 1, 0, &CLEAN); c.max_rx_rate = 19200; a.max_rx_rate = 26400;
        pair_init(&a, &c, &CLEAN);
        CHECK_EQ_I(pair_to_data(8000), 1);
        CHECK_EQ_I(P.a.tx_rate, 19200); CHECK_EQ_I(P.a.rx_rate, 26400);
        CHECK_EQ_I(P.c.tx_rate, 26400); CHECK_EQ_I(P.c.rx_rate, 19200);
        sinks_reset(); pair_run(8000L * 2);
        CHECK_EQ_I(P.ua.sink.errs, 0); CHECK_EQ_I(P.uc.sink.errs, 0);
        report("asymmetric: ceilings 19200 / 26400");
        mkcfg(&a, &c, 4, 1, 0, &CLEAN); c.max_rx_rate = 19200; a.max_rx_rate = 26400; c.symmetric = 1;
        pair_init(&a, &c, &CLEAN);
        CHECK_EQ_I(pair_to_data(8000), 1);
        CHECK_EQ_I(P.a.tx_rate, 19200); CHECK_EQ_I(P.a.rx_rate, 19200);
        mkcfg(&a, &c, 4, 1, 0, &CLEAN); a.max_tx_rate = 9600; a.peer_1664 = c.peer_1664 = 0;
        pair_init(&a, &c, &CLEAN);
        CHECK_EQ_I(pair_to_data(8000), 1);
        CHECK_EQ_I(P.a.tx_rate, 9600); CHECK_EQ_I(P.a.rx_rate, 28800);    /* no 1664-point constellations: 28800 at most */
    }

    /* ---- rate renegotiation (11.6) ---- */
    for (k = 0; k < 3; k++) {
        long t0, s0 = 0;
        mkcfg(&a, &c, 4, 1, 0, &CLEAN);
        pair_init(&a, &c, &CLEAN);
        CHECK_EQ_I(pair_to_data(8000), 1); pair_run(8000);
        if (k == 1) { CHECK_EQ_I(v34dx_renegotiate(&P.c, 0), 0); (void)v34dx_renegotiate(&P.a, 0); s0 = P.c.tx_count[DXS_S]; }
        else        { CHECK_EQ_I(v34dx_renegotiate(&P.a, k == 2 ? 14400 : 0), 0); s0 = P.a.tx_count[DXS_S]; }
        (void)s0;
        t0 = P.n;
        pair_run(80);
        CHECK(P.a.state != DX_DATA);
        CHECK_EQ_I(pair_to_data(3000), 1);
        CHECK_EQ_I(P.a.n_reneg, 1); CHECK_EQ_I(P.c.n_reneg, 1);
        CHECK_EQ_I(P.a.tx_count[DXS_S], 3 * 128); CHECK_EQ_I(P.c.tx_count[DXS_S], 2 * 128);
        CHECK_EQ_I(P.a.tx_count[DXS_SBAR], 3 * 16); CHECK_EQ_I(P.c.tx_count[DXS_SBAR], 2 * 16);
        CHECK(P.n - t0 < 8 * 1000);
        CHECK_EQ_I(P.a.rx_rate, k == 2 ? 14400 : 31200); CHECK_EQ_I(P.a.tx_rate, 31200);
        CHECK_EQ_I(P.c.tx_rate, P.a.rx_rate);
        sinks_reset(); pair_run(8000L * 3);
        CHECK(P.ua.sink.n > 2 * P.a.rx_rate); CHECK(P.uc.sink.n > 2 * P.a.tx_rate);
        CHECK_EQ_I(P.ua.sink.errs, 0); CHECK_EQ_I(P.uc.sink.errs, 0);
        printf("  renegotiation took %ld ms;", (P.n - 8000L * 3 - t0) / 8);
        report(k == 0 ? "begun by the answer modem" : k == 1 ? "begun by both at once" : "answer modem, new ceiling 14400");
    }
    {
        /* A renegotiation whose MP is Type 0 (a Conexant CX93001 does that) leaves the
         * precoding coefficients of the first MP in force (10.1.3.9). */
        mkcfg(&a, &c, 4, 1, 0, &CLEAN);
        c.ask_h[0][0] = 4096; c.ask_h[0][1] = -1638; c.ask_h[1][0] = -2048; c.ask_h[2][1] = 819; c.reneg_type0 = 1;
        pair_init(&a, &c, &CLEAN);
        CHECK_EQ_I(pair_to_data(8000), 1); pair_run(8000);
        CHECK_EQ_I(P.a.tpar.cfg.h[0][1], -1638);
        CHECK_EQ_I(v34dx_renegotiate(&P.c, 0), 0);
        pair_run(80);
        CHECK_EQ_I(pair_to_data(3000), 1);
        CHECK_EQ_I(P.c.n_reneg, 1); CHECK_EQ_I(P.a.far_mp.type, 0);
        CHECK_EQ_I(P.a.tpar.cfg.h[0][0], 4096); CHECK_EQ_I(P.a.tpar.cfg.h[0][1], -1638); CHECK_EQ_I(P.a.tpar.cfg.h[2][1], 819);
        sinks_reset(); pair_run(8000L * 3);
        CHECK(P.uc.sink.n > 2 * P.a.tx_rate);
        CHECK_EQ_I(P.ua.sink.errs, 0); CHECK_EQ_I(P.uc.sink.errs, 0);
        report("renegotiation with a Type 0 MP: precoder kept");
    }
    {
        /* the line gets worse in the middle of the call: the answer modem lowers its receive rate */
        mkcfg(&a, &c, 4, 1, 0, &CLEAN);
        pair_init(&a, &c, &CLEAN);
        CHECK_EQ_I(pair_to_data(8000), 1); pair_run(8000);
        CHECK_EQ_I(P.a.rx_rate, 31200);
        P.line.c.noise_a = 1500.0 / pow(10, 27 / 20.0);
        pair_run(8000L * 4);
        CHECK_EQ_I(P.a.state, DX_DATA); CHECK_EQ_I(P.c.state, DX_DATA);
        CHECK(P.a.rate_drops >= 1); CHECK(P.a.n_reneg >= 1);
        CHECK(P.a.rx_rate <= 21600 && P.a.rx_rate >= 14400); CHECK_EQ_I(P.a.tx_rate, 31200);
        sinks_reset(); pair_run(8000L * 4);
        CHECK_EQ_I(P.a.state, DX_DATA);
        CHECK(P.ua.sink.errs * 100000 <= P.ua.sink.n); CHECK_EQ_I(P.uc.sink.errs, 0);
        printf("  noise rises to -27 dB in data mode: %d renegotiation(s) of our own;", P.a.rate_drops); report("");
    }
    {
        /* retrain_on_step: the same step, far past what the rate needs, is
         * taken for a step in the path, which a renegotiation would not mend
         * if it were our echo moving. The answer modem retrains (here that
         * ends the engine: Phase 2 is its owner's) */
        mkcfg(&a, &c, 4, 1, 0, &CLEAN);
        a.retrain_on_step = 1;
        pair_init(&a, &c, &CLEAN);
        CHECK_EQ_I(pair_to_data(8000), 1); pair_run(8000);
        CHECK_EQ_I(P.a.rx_rate, 31200);
        P.line.c.noise_a = 1500.0 / pow(10, 27 / 20.0);
        pair_run(8000L * 4);
        CHECK_EQ_I(P.a.state, DX_RETRAIN); CHECK_EQ_I(P.a.retrain_ours, 1);
        CHECK_EQ_I(P.a.rate_drops, 0); CHECK_EQ_I(P.a.n_reneg, 0);
        printf("  retrain_on_step, noise steps up to -27 dB in data mode: we retrain\n");
    }
    {
        /* retrain_on_step, but the line grows worse over 30 s: the answer
         * modem lowers its receive rate as without it */
        int k;
        mkcfg(&a, &c, 4, 1, 0, &CLEAN);
        a.retrain_on_step = 1;
        pair_init(&a, &c, &CLEAN);
        CHECK_EQ_I(pair_to_data(8000), 1); pair_run(8000);
        CHECK_EQ_I(P.a.rx_rate, 31200);
        for (k = 0; k <= 300; k++) {
            P.line.c.noise_a = 1500.0 / pow(10, (50 - 23 * k / 300.0) / 20.0);
            pair_run(800);
        }
        CHECK_EQ_I(P.a.state, DX_DATA); CHECK_EQ_I(P.c.state, DX_DATA);
        CHECK(P.a.rate_drops >= 1); CHECK(P.a.n_reneg >= 1);
        CHECK(P.a.rx_rate <= 21600 && P.a.rx_rate >= 14400); CHECK_EQ_I(P.a.tx_rate, 31200);
        sinks_reset(); pair_run(8000L * 4);
        CHECK_EQ_I(P.a.state, DX_DATA);
        CHECK(P.ua.sink.errs * 100000 <= P.ua.sink.n); CHECK_EQ_I(P.uc.sink.errs, 0);
        printf("  retrain_on_step, noise rises to -27 dB over 30 s: %d renegotiation(s) of our own;", P.a.rate_drops); report("");
    }

    /* ---- cleardown (11.7), both ways ---- */
    for (k = 0; k < 2; k++) {
        v34dx_t *me = k ? &P.c : &P.a, *far = k ? &P.a : &P.c;
        mkcfg(&a, &c, 4, 1, 0, &CLEAN);
        pair_init(&a, &c, &CLEAN);
        CHECK_EQ_I(pair_to_data(8000), 1); pair_run(4000);
        CHECK_EQ_I(v34dx_cleardown(me), 0);
        pair_run(8000);
        CHECK_EQ_I(me->state, DX_FAILED); CHECK_EQ_I(me->error, DX_ERR_HANGUP);
        CHECK_EQ_I(far->state, DX_FAILED); CHECK_EQ_I(far->error, DX_ERR_CLEARDOWN);
        CHECK(me->mp_sent >= 1 && far->mpa_sent >= 1);
    }

    /* ---- what must end in a retrain ---- */
    {
        long t;
        /* the far modem's tone for more than 50 ms: Tone B to the answer modem, Tone A to the call modem */
        mkcfg(&a, &c, 4, 1, 0, &CLEAN);
        pair_init(&a, &c, &CLEAN);
        CHECK_EQ_I(pair_to_data(8000), 1); pair_run(4000);
        P.line.c.mute_ca = 1; P.line.c.tone_a_hz = 1200; P.line.c.tone_amp = 1500;
        t = P.n; while (P.n < t + 4000 && P.a.state == DX_DATA) pair_run(8);
        CHECK_EQ_I(P.a.state, DX_RETRAIN); CHECK_EQ_I(P.a.retrain_ours, 0);
        CHECK(P.n - t > 8 * 50 && P.n - t < 8 * 110);
        printf("  Tone B in data mode: retrain after %ld ms\n", (P.n - t) / 8);
        pair_init(&a, &c, &CLEAN);
        CHECK_EQ_I(pair_to_data(8000), 1); pair_run(4000);
        P.line.c.mute_ac = 1; P.line.c.tone_c_hz = 2400; P.line.c.tone_amp = 1500;
        t = P.n; while (P.n < t + 4000 && P.c.state == DX_DATA) pair_run(8);
        CHECK_EQ_I(P.c.state, DX_RETRAIN); CHECK_EQ_I(P.c.retrain_ours, 0);
        CHECK(P.n - t > 8 * 50 && P.n - t < 8 * 110);
        /* the data signal itself must not look like the tone: covered by every run above */

        /* our own wish */
        pair_init(&a, &c, &CLEAN);
        CHECK_EQ_I(v34dx_retrain(&P.a), -1);
        CHECK_EQ_I(pair_to_data(8000), 1);
        CHECK_EQ_I(v34dx_retrain(&P.a), 0);
        CHECK_EQ_I(P.a.state, DX_RETRAIN); CHECK_EQ_I(P.a.retrain_ours, 1);
        CHECK_EQ_I(v34dx_tick(&P.a, 0), 0);

        /* 11.3.2.2.1: no S from the caller within 600 ms + a round trip of the start of J */
        lc = CLEAN; lc.mute_ca = 1;
        pair_init(&a, &c, &lc);
        t = 0; while (P.n < 8000L * 6 && P.a.state != DX_RETRAIN) pair_run(8);
        CHECK_EQ_I(P.a.state, DX_RETRAIN); CHECK_EQ_I(P.a.retrain_ours, 1);
        CHECK_NEAR((P.n - P.a.tx_first[DXS_J]) / 8.0, 600 + 44 / 8.0, 4);
        /* 11.3.2.2.2: the caller's S comes but its J does not */
        pair_init(&a, &c, &CLEAN);
        while (P.n < 8000L * 6 && P.a.state != DX_P3_RX) pair_run(8);
        CHECK_EQ_I(P.a.state, DX_P3_RX);
        P.line.c.mute_ca = 1; t = P.n;
        while (P.n < t + 8000L * 6 && P.a.state != DX_RETRAIN) pair_run(8);
        CHECK_EQ_I(P.a.state, DX_RETRAIN); CHECK_EQ_I(P.a.retrain_ours, 1);
        CHECK(P.n - t < 8 * 2900);
        /* Phase 4: the caller goes away, no E within 2500 ms + three round trips */
        pair_init(&a, &c, &CLEAN);
        while (P.n < 8000L * 6 && P.a.state != DX_P4_TRN) pair_run(8);
        CHECK_EQ_I(P.a.state, DX_P4_TRN);
        P.line.c.mute_ca = 1; t = P.n;
        while (P.n < t + 8000L * 6 && P.a.state != DX_RETRAIN) pair_run(8);
        CHECK_EQ_I(P.a.state, DX_RETRAIN);
        CHECK(P.n - t < 8 * 2600 + 3 * 44);
        /* data mode: the caller's signal stops */
        pair_init(&a, &c, &CLEAN);
        CHECK_EQ_I(pair_to_data(8000), 1); pair_run(4000);
        P.line.c.mute_ca = 1; t = P.n;
        while (P.n < t + 8000L * 6 && P.a.state == DX_DATA) pair_run(8);
        CHECK(P.a.state == DX_RETRAIN || (P.a.state == DX_FAILED && P.a.error == DX_ERR_CARRIER));
        printf("  caller's signal lost in data mode: %s after %ld ms\n", P.a.state == DX_RETRAIN ? "retrain" : "carrier lost", (P.n - t) / 8);
        CHECK(P.n - t < 8 * 2000);
        /* the call modem alone: no S at all */
        lc = CLEAN; lc.mute_ac = 1;
        pair_init(&a, &c, &lc);
        pair_run(8000L * 7);
        CHECK_EQ_I(P.c.state, DX_FAILED); CHECK_EQ_I(P.c.error, DX_ERR_NO_S);
        /* a configuration that does not exist */
        mkcfg(&a, &c, 4, 1, 0, &CLEAN); a.sym_rate = 2743;
        CHECK_EQ_I(v34dx_init(&P.a, &a), -1); CHECK_EQ_I(P.a.error, DX_ERR_CFG);
    }

    /* ---- V.90 9.2.1.1.8: after V.90 Phase 2 the caller's INFO1a selects V.34; we
     * are then the CALL modem of V.34 Phase 3. The configuration from INFO1a. ---- */
    {
        info1a_t i; v34ans_cfg ac; v34dx_cfg d;
        memset(&i, 0, sizeof i); memset(&ac, 0, sizeof ac);
        ac.level = 3000; ac.law = 1; ac.getbit = bend_get; ac.putbit = bend_put; ac.user = &P.uc;
        i.v90 = 1; i.up_sr = i.down_sr = 4;
        CHECK_EQ_I(v34_call_cfg_from_v90(&i, 1, 1, 44, &ac, &d), -1);  /* that is a V.90 INFO1a */
        i.v90 = 0; i.up_sr = 4; i.down_sr = 3;
        CHECK_EQ_I(v34_call_cfg_from_v90(&i, 1, 1, 44, &ac, &d), -1);  /* two symbol rates: not done */
        i.up_sr = i.down_sr = 5;
        CHECK_EQ_I(v34_call_cfg_from_v90(&i, 1, 1, 44, &ac, &d), -1);  /* 3429 */
        i.up_sr = i.down_sr = 4; i.hi_carrier = 0; i.preemph = 3; i.min_pwr_red = 2;
        CHECK_EQ_I(v34_call_cfg_from_v90(&i, 1, 1, 44, &ac, &d), 0);
        CHECK_EQ_I(d.answer, 0); CHECK_EQ_I(d.sym_rate, 3200); CHECK_EQ_I(d.law, 1);
        CHECK_EQ_I(d.tx_car_num, 4); CHECK_EQ_I(d.tx_car_den, 7); CHECK_EQ_I(d.rx_car_num, 3); CHECK_EQ_I(d.rx_car_den, 5);
        CHECK_EQ_I(d.tx_preemph, 3); CHECK_EQ_I(d.tx_pwr_red, 2); CHECK_EQ_I(d.rtd, 44);
        /* and a call with it: the other end is this module's answer role, standing in for the analogue modem */
        mkcfg(&a, &c, 4, 1, 0, &CLEAN);
        pair_init(&a, &d, &CLEAN);
        CHECK_EQ_I(pair_to_data(8000), 1);
        sinks_reset(); pair_run(8000L * 3);
        CHECK_EQ_I(P.ua.sink.errs, 0); CHECK_EQ_I(P.uc.sink.errs, 0);
        CHECK(P.c.tx_rate >= 24000 && P.c.rx_rate >= 24000);
        report("as call modem after V.90 Phase 2");
    }
    T_REPORT("v34dx");
}
