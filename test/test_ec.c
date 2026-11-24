/* Tests for the echo canceller. */
#include <string.h>
#include "test.h"
#include "../modem/ec.h"
#include "../sim/noise.h"

#define NH 40
static double hpath[NH];
static double hist[NH];

static double echo(double tx)
{
    double y = 0; int k;
    memmove(hist + 1, hist, sizeof(double) * (NH - 1));
    hist[0] = tx;
    for (k = 0; k < NH; k++) y += hpath[k] * hist[k];
    return y;
}

int main(void)
{
    static ec_t ec; rng_t rng;
    double pe = 0, pr = 0, erle;
    int i, k;

    /* an echo path: 5 samples of delay, then a decaying oscillation; about -12 dB */
    for (k = 0; k < NH; k++) hpath[k] = k < 5 ? 0 : 0.2 * cos(0.9 * (k - 5)) * exp(-(k - 5) / 6.0);
    memset(hist, 0, sizeof hist);
    rng_seed(&rng, 11);

    /* before training nothing is removed */
    ec_init(&ec);
    CHECK_EQ_I(ec_tick(&ec, 1000, 321, -1), 321);

    /* training on a two-level signal (like TRN1d), far end silent */
    ec_init(&ec);
    for (i = 0; i < 2040; i++) {
        int16_t tx = rng_uniform(&rng) < 0.5 ? -3000 : 3000;
        double e = echo(tx);
        int16_t out = ec_tick(&ec, tx, (int16_t)floor(e + 0.5), 0);
        if (i >= 1500) { pe += e * e; pr += (double)out * out; }
    }
    erle = 10 * log10(pe / (pr + 1e-9));
    printf("  ec: ERLE after 2040 samples of two-level training %.1f dB\n", erle);
    CHECK(erle > 40.0);

    /* frozen, multi-level transmit signal, far-end signal present: the far
     * signal comes through and the echo stays down */
    {
        double pfar = 0, perr = 0;
        for (i = 0; i < 8000; i++) {
            int16_t tx = (int16_t)(8000 * (rng_uniform(&rng) * 2 - 1));
            double e = echo(tx), far = 500 * rng_gauss(&rng);
            int16_t out = ec_tick(&ec, tx, (int16_t)floor(e + far + 0.5), -1);
            pfar += far * far; perr += (out - far) * (out - far);
        }
        printf("  ec: frozen, residual echo %.1f dB below the far signal\n", 10 * log10(pfar / perr));
        CHECK(10 * log10(pfar / perr) > 25.0);      /* echo was 13 dB ABOVE the far signal */
    }

    /* crawling adaptation under double talk follows a changed echo path
     * without being thrown by the far signal */
    {
        double pfar = 0, perr = 0;
        for (k = 5; k < NH; k++) hpath[k] *= 1.15;
        for (i = 0; i < 80000; i++) {
            int16_t tx = (int16_t)(8000 * (rng_uniform(&rng) * 2 - 1));
            double e = echo(tx), far = 500 * rng_gauss(&rng);
            int16_t out = ec_tick(&ec, tx, (int16_t)floor(e + far + 0.5), 7);
            if (i >= 60000) { pfar += far * far; perr += (out - far) * (out - far); }
        }
        printf("  ec: after a 15 %% path change, slow adaptation: residual %.1f dB below the far signal\n",
               10 * log10(pfar / perr));
        CHECK(10 * log10(pfar / perr) > 22.0);
    }

    /* a hostile caller (security pass, I1): it picks a small UINFO, so our
     * TRN1d is +-64, and sends a full-scale copy of it while it should be
     * silent. NLMS drives the canceller toward an echo gain of 512, which
     * Q24 taps cannot hold: they saturate at the largest gain (128), and
     * never wrap (signed overflow before the fix). Both the update in
     * ec_tick and ec_adapt (two cancellers in a row). */
    {
        uint32_t r = 1;
        int wrapped = 0, pass;
        for (pass = 0; pass < 2; pass++) {
            ec_init(&ec);
            for (i = 0; i < 4800; i++) {
                int16_t sg;
                r = r * 1664525u + 1013904223u;
                sg = (int16_t)((r >> 31) ? -1 : 1);
                if (pass == 0) (void)ec_tick(&ec, (int16_t)(64 * sg), (int16_t)(32767 * sg), i < 3000 ? 0 : 3);
                else ec_adapt(&ec, ec_tick(&ec, (int16_t)(64 * sg), (int16_t)(32767 * sg), -1), i < 3000 ? 0 : 3);
                if (i >= 200 && ec.h[0] <= 0) wrapped++;
            }
            CHECK_EQ_I(ec.h[0], INT32_MAX);
        }
        CHECK_EQ_I(wrapped, 0);
    }

    /* such taps, then a loud transmit: trained on a quiet DC (+27, energy
     * just over 2^16) against a full-scale one, the taps sum to a gain of
     * about 1200; frozen while our transmit goes to full scale, the
     * estimate reaches about 2^25, and the error at the next adaptation
     * is past 2^23, where err * 2^40 would leave int64 (signed overflow
     * before; UBSan). Then it adapts on, both ways in: the taps stay in
     * range and fall back toward the silent line. */
    {
        int64_t hsum = 0;
        int16_t out;
        int pass;
        for (pass = 0; pass < 2; pass++) {
            ec_init(&ec);
            for (i = 0; i < 3000; i++) (void)ec_tick(&ec, 27, 32767, 0);
            for (i = 0, hsum = 0; i < EC_NTAP; i++) hsum += ec.h[i];
            CHECK(hsum > (int64_t)1000 << 24);      /* a gain over 1000 */
            for (i = 0; i < EC_NTAP; i++) (void)ec_tick(&ec, 32767, 0, -1);
            if (pass == 0) out = ec_tick(&ec, 32767, 0, 0);
            else { out = ec_tick(&ec, 32767, 0, -1); ec_adapt(&ec, -33554432, 0); }
            CHECK_EQ_I(out, -32768);
            for (i = 0; i < 2000; i++) (void)ec_tick(&ec, 32767, 0, 0);
            for (i = 0, hsum = 0; i < EC_NTAP; i++) hsum += ec.h[i];
            CHECK(hsum < (int64_t)1 << 24 && hsum > -((int64_t)1 << 24));  /* |gain| under 1 */
        }
    }

    /* ec_peak and ec_norm (what the API reports of the model): an empty
     * model, one tap, and every tap saturated, where Q48 squares would
     * overflow (UBSan) */
    ec_init(&ec);
    CHECK_EQ_I(ec_peak(&ec), -1); CHECK(ec_norm(&ec) == 0);
    ec.h[7] = -(1 << 23); ec.h[9] = 1 << 22;
    CHECK_EQ_I(ec_peak(&ec), 7); CHECK(ec_norm(&ec) == ((int64_t)5 << 38));
    for (i = 0; i < EC_NTAP; i++) ec.h[i] = INT32_MAX;
    ec.h[40] = INT32_MIN;
    CHECK_EQ_I(ec_peak(&ec), 40); CHECK(ec_norm(&ec) > (int64_t)95 << 55);

    /* silence in, silence out, no blow-up when the transmit history is empty */
    ec_init(&ec);
    for (i = 0; i < 500; i++) CHECK_EQ_I(ec_tick(&ec, 0, 0, 2) == 0, 1);

    T_REPORT("ec");
}
