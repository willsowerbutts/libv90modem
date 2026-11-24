/* V.34 call modem, full duplex, primary channel: everything after V.8.
 *
 *   Phase 2 (p2call) -> Phases 3 and 4, data, rate renegotiation, cleardown
 *   (v34dx in the call role) -> on a retrain, 70 ms of silence and Phase 2
 *   again (11.5.1), as often as V34O_MAX_RETRAINS in a row without data.
 *
 * The mirror of modem/v34ans (which stays the answer modem, unchanged).
 * Start it when the 75 ms of silence after CJ are over (modem/v8orig done)
 * and JM selected V.34. One linear sample in, one out, 8 kHz. Fixed-point,
 * no allocation. */
#ifndef V90D_MODEM_V34ORIG_H
#define V90D_MODEM_V34ORIG_H
#include <stdint.h>
#include "p2call.h"
#include "v34dx.h"

typedef struct {
    int level;              /* nominal transmit power: RMS in sample units */
    int law;                /* codec between us and the line: 0 none (linear), 1 mu-law, 2 A-law */
    int io_delay;           /* samples from a sample handed out until its echo starts to
                               come back (a driver's buffering); -1 = not known: found
                               during training, up to 160 ms */
    int sr_mask;            /* symbol rates allowed (P2N_SR_*), 0 = 2400, 3000 and 3200 */
    int max_rx_rate;        /* ceilings in bit/s, 0 = what the line gives */
    int max_tx_rate;
    v34dx_getbit_fn getbit; /* data to send */
    v34dx_putbit_fn putbit; /* data received */
    void *user;
    int retrain_on_step;    /* v34dx_cfg.retrain_on_step */
} v34orig_cfg;

typedef enum {
    V34O_PHASE2,            /* probing / ranging */
    V34O_TRAINING,          /* Phases 3 and 4 */
    V34O_DATA,              /* both directions carry data */
    V34O_RENEG,             /* rate renegotiation under way (data interrupted) */
    V34O_RETRAIN,           /* 70 ms of silence before Phase 2 of a retrain */
    V34O_ENDED              /* see error */
} v34orig_state;

enum {
    V34O_OK = 0,
    V34O_ERR_PHASE2,        /* Phase 2 failed: p2.error says how */
    V34O_ERR_TRAINING,      /* Phase 3 / 4 failed: dx.error says how */
    V34O_ERR_RETRAINS,      /* too many retrains in a row */
    V34O_ERR_CLEARDOWN,     /* the answerer cleared down */
    V34O_ERR_CARRIER,       /* the answerer's signal went away */
    V34O_ERR_HANGUP         /* we cleared down (v34orig_cleardown) */
};

#define V34O_MAX_RETRAINS 4

typedef struct {
    v34orig_cfg   cfg;
    v34orig_state state;
    int           error;
    int32_t       t;
    p2call_t      p2;
    v34dx_t       dx;
    info0_t       info0a; int have0a;
    int           retrains;         /* all, since the call began */
    int           retrains_now;     /* in a row, without data in between */
    int           retrains_ours;    /* ... of all, those we began */
    int           connects;         /* times data mode was reached (start-up, retrains) */
} v34orig_t;

void    v34orig_init(v34orig_t *o, const v34orig_cfg *cfg);
int16_t v34orig_tick(v34orig_t *o, int16_t rx);
/* data signalling rates in bit/s, 0 outside data mode: tx = call -> answer */
int     v34orig_tx_rate(const v34orig_t *o);
int     v34orig_rx_rate(const v34orig_t *o);
/* From data mode; each returns 0, or -1 when not in data mode (a retrain
 * also during a rate renegotiation). */
int     v34orig_renegotiate(v34orig_t *o, int max_rx_rate);
int     v34orig_cleardown(v34orig_t *o);
int     v34orig_retrain(v34orig_t *o);

/* Configuration of the Phase 3 / 4 engine from the outcome of p2call. */
void    v34orig_dx_cfg(const p2call_t *p2, const v34orig_cfg *cfg, int peer_1664, v34dx_cfg *dx);

#endif
