/* Transmit history and echo delay measurement. See ecdly.h. */
#include <string.h>
#include "ecdly.h"

void ecdly_init(ecdly_t *d, int delay)
{
    memset(d, 0, sizeof *d);
    d->delay = delay > 0 ? delay : 0;
}

int ecdly_near_base(int rtd)
{
    int b = rtd + ECDLY_MAXLAG / 5 - ECDLY_MAXLAG;
    return b < 0 ? 0 : b > ECDLY_MAXBASE ? ECDLY_MAXBASE : b;
}

void ecdly_near(ecdly_t *d, int rtd) { d->base = ecdly_near_base(rtd); }

void ecdly_restart(ecdly_t *d)
{
    memset(d->win, 0, sizeof d->win);
    d->last = 0; d->n = 0; d->s_pos = 0; d->s_lag = 0; d->best = 0; d->best_lag = 0;
}

static int search(ecdly_t *d)
{
    int k, i;
    for (k = 0; k < ECDLY_STEP && d->s_lag <= ECDLY_MAXLAG; k++, d->s_lag++) {
        int64_t acc = 0;
        /* window sample i was received when pos was s_pos - ECDLY_WIN + 1 + i;
         * what we sent `lag` samples before it sits lag + 1 behind that write
         * position (the sample of the same tick is stored before the canceller runs) */
        int base = d->s_pos - ECDLY_WIN - d->base - d->s_lag;
        for (i = 0; i < ECDLY_WIN; i++)
            acc += (int32_t)d->win[i] * d->hist[(base + i) & (ECDLY_HIST - 1)];
        if (acc < 0) acc = -acc;
        if (acc > d->best) { d->best = acc; d->best_lag = d->base + d->s_lag; }
    }
    if (d->s_lag <= ECDLY_MAXLAG) return 0;
    d->delay = d->best_lag > 8 ? d->best_lag - 8 : 0;
    return 1;
}

int ecdly_measure(ecdly_t *d, int32_t k, int16_t rxs, int diff)
{
    const int32_t snap = ECDLY_MAXLAG + ECDLY_WIN;
    if (k == snap - ECDLY_WIN) {
        d->last = rxs;
    } else if (k > snap - ECDLY_WIN && k <= snap) {
        int32_t v = diff ? (int32_t)rxs - d->last : rxs;
        d->last = rxs;
        if (d->n < ECDLY_WIN) d->win[d->n++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        if (k == snap) { d->s_pos = d->pos; d->s_lag = 0; d->best = -1; }
    } else if (k > snap && d->s_lag <= ECDLY_MAXLAG) {
        return search(d);
    }
    return 0;
}
