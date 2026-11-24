/* A two-wire-to-network path for the whole-call V.34 tests: the answer
 * modem sits behind a G.711 codec (or none) and a hybrid; the calling modem
 * at the far end of a subscriber loop with its own hybrid and its own
 * sample clock. Delay, loop filter, near and far echo, noise, clock offset
 * of the caller, frequency offset in each direction. Test side (double). */
#ifndef V90D_TEST_LINE34_H
#define V90D_TEST_LINE34_H
#include <string.h>
#include <math.h>
#include "../sim/noise.h"
#include "../sim/resamp.h"
#include "../modem/pcmenc.h"
#include "../v90/v90_pcm.h"

#define L34_MAXD 4096
#define L34_HILB 31
typedef struct {
    int law;                    /* codec at the answer modem: 0 linear, 1 mu-law, 2 A-law */
    int rx_lin;                 /* with law 1 or 2: that codec's receive side is 16-bit linear
                                   (its transmit side still puts G.711 levels on the line) */
    int d_ca, d_ac;             /* one-way delays, samples */
    const double *loop; int nloop;  /* the subscriber loop, each direction (0 = flat) */
    double gain;                /* loss of the path, each direction */
    double echo_a;              /* answer side hybrid */
    int io_delay;               /* delay of the answer modem's own sample path to the line
                                   terminals (a driver's buffers) */
    double echo_c;              /* caller's hybrid */
    double far_a;               /* answer side: echo from the far hybrid, a round trip late */
    int far_lag;                /* ... and this many samples later again (0 = right at the hybrid; an
                                   ATA's response peaks near the round trip the modems measure) */
    double noise;               /* RMS at each receiver */
    double ppm;                 /* the caller's clock against ours */
    double foff_ca, foff_ac;    /* frequency offset of each direction, Hz */
    int mute_ca_from, mute_ca_to;   /* caller -> answer dead in this window (samples) */
    long click_at;              /* > 0: from this sample, six samples to the answer modem
                                   replaced by full scale, + and - alternated (0.75 ms) */
} line34_cfg;

typedef int16_t (*line34_tick_fn)(void *modem, int16_t rx);

typedef struct {
    line34_cfg c; rng_t rng; long n;
    double la[L34_MAXD], lc[L34_MAXD]; int pos;     /* what each side put on the line */
    double za[2][64], zc[2][64];                    /* loop filter states */
    double ha[L34_HILB], hc[L34_HILB]; double pha, phc;
    sresamp_t n2c, c2n; double fifo[8]; int nfifo;
    double c_last;              /* the caller's latest output at its own clock */
    double crx_net;             /* what reaches the caller, at our clock */
    int16_t arx;
} line34_t;

static void line34_init(line34_t *l, const line34_cfg *c)
{
    memset(l, 0, sizeof *l); l->c = *c; rng_seed(&l->rng, 97531);
    sresamp_init(&l->n2c, 1.0 + c->ppm * 1e-6); sresamp_init(&l->c2n, 1.0 / (1.0 + c->ppm * 1e-6));
}
static double l34_at(const double *b, int pos, int d) { return b[(pos - d) & (L34_MAXD - 1)]; }
static double l34_codec(int law, double v)
{
    int16_t s = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : floor(v + 0.5));
    return law ? (double)v90_octet_linear(law == 2, pcm_encode(law == 2, s)) : (double)s;
}
static double l34_fir(const double *taps, int n, double *z, double x)
{
    double y = 0; int i;
    if (n <= 0) return x;
    for (i = n - 1; i > 0; i--) z[i] = z[i - 1];
    z[0] = x;
    for (i = 0; i < n; i++) y += taps[i] * z[i];
    return y;
}
/* shift the spectrum by f Hz: the analytic signal turned */
static double l34_shift(double *h, double *ph, double f, double x)
{
    double q = 0; int i, m = L34_HILB / 2;
    if (f == 0) return x;
    for (i = L34_HILB - 1; i > 0; i--) h[i] = h[i - 1];
    h[0] = x;
    for (i = -m; i <= m; i++)
        if (i & 1) q += h[m + i] * (2.0 / (3.14159265358979 * i)) * (0.54 + 0.46 * cos(3.14159265358979 * i / (m + 1)));
    *ph += 6.283185307179586 * f / 8000.0;
    return h[m] * cos(*ph) - q * sin(*ph);
}

/* One tick of our clock: the answer modem runs once, the caller as often as
 * its clock says (once, with now and then none or two). */
static int16_t line34_step(line34_t *l, line34_tick_fn atick, void *am, line34_tick_fn ctick, void *cm)
{
    const line34_cfg *c = &l->c;
    double out[4], a_line, c_net, v;
    int i, k, nc;
    int16_t atx = atick(am, l->arx);

    /* answer side: codec, then the hybrid and the network */
    a_line = l34_codec(c->law, atx);
    l->la[l->pos] = a_line;
    /* towards the caller */
    v = l34_at(l->la, l->pos, c->io_delay + c->d_ac);
    v = l34_shift(l->ha, &l->pha, c->foff_ac, v);
    v = c->gain * l34_fir(c->loop, c->nloop, l->za[0], v);
    v += c->noise * rng_gauss(&l->rng);
    l->crx_net = v;
    /* the caller, at its clock */
    nc = c->ppm != 0 ? sresamp_push(&l->n2c, l->crx_net, out, 4) : (out[0] = l->crx_net, 1);
    for (i = 0; i < nc; i++) {
        double o2[4]; int n2;
        double e = out[i] + c->echo_c * 0.6 * l->c_last;    /* the caller's hybrid, at its clock */
        int16_t s = (int16_t)(e > 32767 ? 32767 : e < -32768 ? -32768 : floor(e + 0.5));
        l->c_last = ctick(cm, s);
        n2 = c->ppm != 0 ? sresamp_push(&l->c2n, l->c_last, o2, 4) : (o2[0] = l->c_last, 1);
        for (k = 0; k < n2 && l->nfifo < 8; k++) l->fifo[l->nfifo++] = o2[k];
    }
    c_net = 0;
    if (l->nfifo > 0) { c_net = l->fifo[0]; l->nfifo--; for (k = 0; k < l->nfifo; k++) l->fifo[k] = l->fifo[k + 1]; }
    /* towards the answer modem */
    v = c->gain * l34_fir(c->loop, c->nloop, l->zc[0], c_net);
    l->lc[l->pos] = v;
    v = l34_at(l->lc, l->pos, c->d_ca);
    v = l34_shift(l->hc, &l->phc, c->foff_ca, v);
    if (l->n >= c->mute_ca_from && l->n < c->mute_ca_to) v = 0;
    v += c->echo_a * (0.6 * l34_at(l->la, l->pos, c->io_delay) + 0.5 * l34_at(l->la, l->pos, c->io_delay + 2)
                      - 0.3 * l34_at(l->la, l->pos, c->io_delay + 6));
    v += c->far_a * (0.7 * l34_at(l->la, l->pos, c->io_delay + c->d_ac + c->d_ca + c->far_lag + 1)
                     + 0.4 * l34_at(l->la, l->pos, c->io_delay + c->d_ac + c->d_ca + c->far_lag + 4));
    v += c->noise * rng_gauss(&l->rng);
    if (c->click_at > 0 && l->n >= c->click_at && l->n < c->click_at + 6) v = (l->n - c->click_at) & 1 ? -32124.0 : 32124.0;
    l->arx = (int16_t)l34_codec(c->rx_lin ? 0 : c->law, v);
    l->pos = (l->pos + 1) & (L34_MAXD - 1);
    l->n++;
    return atx;
}
#endif
