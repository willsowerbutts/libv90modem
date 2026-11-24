/* V.34 answer modem, full duplex, primary channel: everything after V.8.
 *
 *   Phase 2 (p2ans) -> Phases 3 and 4, data, rate renegotiation, cleardown
 *   (v34dx in the answer role) -> on a retrain, 70 ms of silence and Phase 2
 *   again (11.5.2), as often as V34A_MAX_RETRAINS in a row without data.
 *
 * Also the V.34 that a V.90 caller may select at the end of V.90 Phase 2
 * (v34ans_init_call below): then only the second line, in the call role.
 *
 * Start it when the 75 ms of silence after JM / CJ are over (modem/v8ans in
 * V8A_DONE) and V.8 selected V.34 without PCM. One linear sample in, one
 * out, 8 kHz. Fixed-point, no allocation. */
#ifndef V90D_MODEM_V34ANS_H
#define V90D_MODEM_V34ANS_H
#include <stdint.h>
#include "p2ans.h"
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
    int x2;                 /* offer x2 in Phase 2 (p2ans_cfg.x2): V.8 made our JM x2's */
    int k56;                /* K56flex was agreed in V.8bis (answer_t.k56_agreed): Phase 2
                               (p2ans_cfg.k56) may flag INFO1a, first and in a retrain */
    int retrain_on_step;    /* v34dx_cfg.retrain_on_step */
} v34ans_cfg;

typedef enum {
    V34A_PHASE2,            /* probing / ranging */
    V34A_TRAINING,          /* Phases 3 and 4 */
    V34A_DATA,              /* both directions carry data */
    V34A_RENEG,             /* rate renegotiation under way (data interrupted) */
    V34A_RETRAIN,           /* 70 ms of silence before Phase 2 of a retrain */
    V34A_ENDED,             /* see error */
    V34A_X2,                /* Phase 2 ended with the x2 marker (p2.x2_marker): silent, and
                               the owner goes on as x2 */
    V34A_K56                /* Phase 2 ended with our flagged INFO1a (p2.k56): silent, and the
                               owner goes on as K56flex; there is no way back to V.34 */
} v34ans_state;

enum {
    V34A_OK = 0,
    V34A_ERR_PHASE2,        /* Phase 2 failed: p2.error says how */
    V34A_ERR_TRAINING,      /* Phase 3 / 4 failed: dx.error says how */
    V34A_ERR_RETRAINS,      /* too many retrains in a row */
    V34A_ERR_CLEARDOWN,     /* the caller cleared down */
    V34A_ERR_CARRIER,       /* the caller's signal went away */
    V34A_ERR_HANGUP         /* we cleared down (v34ans_cleardown) */
};

#define V34A_MAX_RETRAINS 4

typedef struct {
    v34ans_cfg   cfg;
    v34ans_state state;
    int          error;
    int32_t      t;
    p2ans_t      p2;
    v34dx_t      dx;
    info0_t      info0c; int have0c;
    int          retrains;          /* all, since the call began */
    int          retrains_now;      /* in a row, without data in between */
    int          retrains_ours;     /* ... of all, those we began */
    int          connects;          /* times data mode was reached (start-up, retrains) */
    int          from_v90;          /* started by v34ans_init_call: no Phase 2 of ours */
} v34ans_t;

void    v34ans_init(v34ans_t *a, const v34ans_cfg *cfg);
int16_t v34ans_tick(v34ans_t *a, int16_t rx);
/* data signalling rates in bit/s, 0 before data mode: tx = answer -> call */
int     v34ans_tx_rate(const v34ans_t *a);
int     v34ans_rx_rate(const v34ans_t *a);
/* From data mode; each returns 0, or -1 when not in data mode. */
int     v34ans_renegotiate(v34ans_t *a, int max_rx_rate);
int     v34ans_cleardown(v34ans_t *a);
int     v34ans_retrain(v34ans_t *a);

/* Configuration of the Phase 3 / 4 engine from the outcome of p2ans: used
 * by v34ans itself and by tests that drive the two parts separately. */
void    v34ans_dx_cfg(const p2ans_t *p2, const v34ans_cfg *cfg, v34dx_cfg *dx);


/* V.90 9.2.1.1.8: the caller's INFO1a at the end of V.90 Phase 2 (modem/p2d)
 * has bits 37:39 in 0..5: it selects V.34, and the digital modem goes on
 * "in accordance with 11.3.1.1/V.34 assuming the role of a call modem".
 * This fills the engine's configuration for that: info1a as parsed with
 * info1a.v90 == 0; rx_hi_carrier: the carrier our INFO1d asked for at the
 * symbol rate chosen (p2d_cfg.up_hi_carrier bit); c1664: the caller's
 * INFO0a bit 25; rtd from p2d. Only cfg's level, law, io_delay, ceilings
 * and bit callbacks are used. Returns 0, or -1 when INFO1a asks for
 * something this modem does not do (different symbol rates in the two
 * directions, or one outside 2400 / 3000 / 3200). */
int     v34_call_cfg_from_v90(const info1a_t *info1a, int rx_hi_carrier, int c1664, int rtd,
                              const v34ans_cfg *cfg, v34dx_cfg *dx);
/* The same case as a whole: start here when modem/p2d is done and INFO1a
 * selected V.34, with the arguments as above. Phases 3 and 4 in the call
 * role begin at once (silent, waiting for the caller's S), then data, rate
 * renegotiation and cleardown as after v34ans_init. A retrain is not run
 * here, because it "shall use Phase 2 of V.90" (9.2.1.1.8): the state goes
 * to V34A_RETRAIN and stays there, sending silence, and the owner runs p2d
 * again (and starts this again if INFO1a says V.34 once more). Returns 0, or
 * -1 with the state V34A_ENDED / V34A_ERR_TRAINING. */
int     v34ans_init_call(v34ans_t *a, const v34ans_cfg *cfg, const info1a_t *info1a,
                         int rx_hi_carrier, int c1664, int rtd);

#endif
