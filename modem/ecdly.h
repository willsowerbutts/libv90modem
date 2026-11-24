/* What we sent, kept for the echo canceller's delayed reference, and the
 * measurement of how late our echo comes back.
 *
 * Between handing a sample to the line and its echo coming back lie the
 * driver's buffers: tens of milliseconds the canceller (ec.h, 12 ms) cannot
 * span. The history gives the canceller what was sent `delay` samples ago;
 * the measurement finds `delay` when the owner does not know it.
 *
 * The measurement: while we send a white training signal and the far end is
 * silent, a short window of received samples, correlated with what we sent
 * at every candidate distance before them, peaks where the echo path is
 * strongest. A few lags per sample keep the cost of any one sample small;
 * the history still holds what is needed when the search gets to it
 * (ECDLY_MAXLAG + ECDLY_WIN + the samples the search takes < ECDLY_HIST).
 *
 * With `diff` the window holds the change from each sample to the next: a
 * modem's line transformer can return the low frequencies of the training
 * signal as a tail tens of milliseconds long, larger in sum than the start
 * of the echo, and on plain samples that tail put the peak anywhere (U.S.
 * Robotics 5686: found 37 ms for an echo at 34, the canceller then sat
 * beside the echo). The difference takes a flat tail away and leaves the
 * start standing.
 *
 * Fixed-point, no allocation. Used by p34d (V.90), ../v92/v92_p34d (V.92),
 * x2d (x2), k56d (K56flex), each searching near the round trip
 * (ecdly_near), and v34dx (V.34), which searches for its own path's delay
 * and puts a second canceller a round trip after it, found by a second
 * search when the round trip is long. */
#ifndef V90D_MODEM_ECDLY_H
#define V90D_MODEM_ECDLY_H
#include <stdint.h>

#define ECDLY_HIST   4096   /* history of what we sent, a power of two */
#define ECDLY_MAXLAG 1280   /* longest echo delay the search covers: 160 ms */
#define ECDLY_WIN    256    /* received samples the search correlates */
#define ECDLY_STEP   4      /* lags tried per sample */
/* ecdly_measure() is to be called for k = 1 .. ECDLY_SAMPLES */
#define ECDLY_SAMPLES (ECDLY_MAXLAG + ECDLY_WIN + ECDLY_MAXLAG / ECDLY_STEP + 2)
/* the furthest the search can start (ecdly_near): the history must still
 * hold base + everything above when the search gets to its last lag */
#define ECDLY_MAXBASE (ECDLY_HIST - ECDLY_SAMPLES - 8)

typedef struct {
    int16_t hist[ECDLY_HIST]; int pos;  /* what we sent; where the next sample goes */
    int     delay;                      /* in use: given or measured */
    int16_t win[ECDLY_WIN];             /* the search's received samples, newest last */
    int16_t last;                       /* the sample before, while the window fills (diff) */
    int     n;                          /* samples collected */
    int     s_pos, s_lag;               /* pos at the snapshot; next lag to try */
    int64_t best; int best_lag;
    int     base;                       /* the search covers base .. base + ECDLY_MAXLAG */
} ecdly_t;

/* delay: what the owner knows (0 when it will be measured) */
void ecdly_init(ecdly_t *d, int delay);

/* Before the measurement: the echo is the far hybrid's, about a round trip
 * of rtd samples away (a digital modem whose codec sits behind a packet
 * network: an ATA's jitter buffers put it some 300 ms out, beyond
 * ECDLY_MAXLAG). Moves the search to end ECDLY_MAXLAG / 5 past rtd; a
 * shorter rtd leaves it at 0 .. ECDLY_MAXLAG. */
void ecdly_near(ecdly_t *d, int rtd);
/* where that search starts: the measurement needs what was sent from this
 * many samples before its first sample on (with ECDLY_SAMPLES after) */
int  ecdly_near_base(int rtd);

/* A second measurement on the same history (v34dx: the far echo after the
 * near one): the search starts afresh; delay, base and what was sent stay. */
void ecdly_restart(ecdly_t *d);

/* One sample period, in this order: ecdly_put() with what is being sent
 * now; ecdly_ref() for the canceller (what was sent `delay` + back samples
 * ago); ecdly_next(). */
static inline void    ecdly_put(ecdly_t *d, int16_t y) { d->hist[d->pos] = y; }
static inline int16_t ecdly_ref(const ecdly_t *d, int back) { return d->hist[(d->pos - d->delay - back) & (ECDLY_HIST - 1)]; }
static inline void    ecdly_next(ecdly_t *d) { d->pos = (d->pos + 1) & (ECDLY_HIST - 1); }

/* The measurement, once per sample after ecdly_next(), k counting from 1
 * when the far end has fallen silent; rxs is the received sample (not what
 * the canceller left). Returns 1 on the sample that completes it: `delay`
 * is then set, a little before the peak to leave room for the start of the
 * echo path, and a canceller that ran on the old value starts afresh. */
int ecdly_measure(ecdly_t *d, int32_t k, int16_t rxs, int diff);

#endif
