/* Echo canceller for the V.90 digital modem: our transmit signal (the linear
 * value of each PCM codeword sent) leaks back through the hybrid into the
 * upstream receive path. An FIR model of that path, adapted by NLMS while the
 * far end is silent (TRN1d) and frozen or crawling afterwards.
 * Fixed-point, no allocation. */
#ifndef V90D_MODEM_EC_H
#define V90D_MODEM_EC_H
#include <stdint.h>

#define EC_NTAP 96              /* 12 ms; a multiple of 8 (ec_tick) */

typedef struct {
    int16_t x[2 * EC_NTAP]; int pos;    /* transmit history, newest first, doubled */
    int32_t h[EC_NTAP];                 /* echo path estimate, Q24 */
    int64_t energy;                     /* sum of x^2 over the history */
} ec_t;

void ec_init(ec_t *e);
/* tx: the sample we are sending now; rx: the sample received now.
 * mu_shift: adaptation step 2^-mu_shift (0 = fastest, right while the far end
 * is silent; 7 or more under double talk), or negative to freeze.
 * Returns rx with the echo estimate removed. */
int16_t ec_tick(ec_t *e, int16_t tx, int16_t rx, int mu_shift);
/* The NLMS step alone, on the history as the last ec_tick() left it and an
 * error computed elsewhere: for two cancellers in a row (near and far echo),
 * which must both learn from what is left after both. */
void    ec_adapt(ec_t *e, int32_t err, int mu_shift);
/* The strongest tap, 0..EC_NTAP-1: where in the window the echo sits; -1 while
 * the model is empty. */
int     ec_peak(const ec_t *e);
/* Sum of the squared taps, Q42: the echo path's gain squared, as modelled (the
 * squares of EC_NTAP saturated taps would not fit in Q48). */
int64_t ec_norm(const ec_t *e);

#endif
