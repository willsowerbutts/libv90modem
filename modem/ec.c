/* Echo canceller. See ec.h. */
#include "ec.h"

void ec_init(ec_t *e)
{
    int i;
    for (i = 0; i < 2 * EC_NTAP; i++) e->x[i] = 0;
    for (i = 0; i < EC_NTAP; i++) e->h[i] = 0;
    e->pos = 0; e->energy = 0;
}

int16_t ec_tick(ec_t *e, int16_t tx, int16_t rx, int mu_shift)
{
    const int16_t *x;
    int64_t acc = 0;
    int32_t err;
    int i, old;

    /* slide the history: the oldest sample leaves the energy sum */
    if (--e->pos < 0) e->pos = EC_NTAP - 1;
    old = e->x[e->pos];
    e->energy += (int64_t)tx * tx - (int64_t)old * old;
    e->x[e->pos] = e->x[e->pos + EC_NTAP] = tx;
    x = &e->x[e->pos];

    /* four taps a pass (EC_NTAP is a multiple of 4): the same sum */
    for (i = 0; i < EC_NTAP; i += 4)
    {
        acc += (int64_t)e->h[i] * x[i]; acc += (int64_t)e->h[i + 1] * x[i + 1];
        acc += (int64_t)e->h[i + 2] * x[i + 2]; acc += (int64_t)e->h[i + 3] * x[i + 3];
    }
    err = rx - (int32_t)((acc + (1 << 23)) >> 24);

    if (mu_shift >= 0 && e->energy > 65536) {
        /* NLMS: h += mu * err * x / energy. err/energy is tiny, so carry it
         * with 40 fractional bits and drop 16 after the product (h is Q24). */
        int64_t g = (int64_t)err * ((int64_t)1 << 40) / e->energy;
        for (i = 0; i < EC_NTAP; i++) e->h[i] += (int32_t)((g * x[i] + ((int64_t)1 << (15 + mu_shift))) >> (16 + mu_shift));
    }
    if (err > 32767) err = 32767;
    if (err < -32768) err = -32768;
    return (int16_t)err;
}

void ec_adapt(ec_t *e, int32_t err, int mu_shift)
{
    const int16_t *x = &e->x[e->pos];
    int i;
    if (mu_shift >= 0 && e->energy > 65536) {
        int64_t g = (int64_t)err * ((int64_t)1 << 40) / e->energy;
        for (i = 0; i < EC_NTAP; i++) e->h[i] += (int32_t)((g * x[i] + ((int64_t)1 << (15 + mu_shift))) >> (16 + mu_shift));
    }
}

int ec_peak(const ec_t *e)
{
    int32_t best = 0;
    int i, at = -1;
    for (i = 0; i < EC_NTAP; i++) {
        int32_t v = e->h[i] < 0 ? -e->h[i] : e->h[i];
        if (v > best) { best = v; at = i; }
    }
    return at;
}

int64_t ec_norm(const ec_t *e)
{
    int64_t n = 0;
    int i;
    for (i = 0; i < EC_NTAP; i++) n += (int64_t)e->h[i] * e->h[i];
    return n;
}
