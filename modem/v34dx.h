/* V.34 Phases 3 and 4, data mode, rate renegotiation and cleardown, full
 * duplex, for either role (V.34 11.3, 11.4, 11.6, 11.7); a retrain (11.5)
 * ends this module in DX_RETRAIN and its owner runs Phase 2 again.
 *
 *   answer modem: 70 ms of silence, S, S-bar, PP, TRN (our echo canceller
 *     trains: the caller is silent), J until the caller's S / S-bar; then
 *     silent while the caller's PP, TRN (equaliser training) and J come in;
 *     then S, S-bar, TRN, MP, MP', E, B1, data, while J, J', TRN, MP, MP',
 *     E, B1, data are received.
 *   call modem: silent while S, S-bar, PP, TRN, J come in; then S, S-bar,
 *     PP, TRN, J until the answerer's S / S-bar; then J', TRN, MP, MP', E,
 *     B1, data. (This is the part a V.90 digital modem takes when the caller
 *     selects V.34 in INFO1a, V.90 9.2.1.1.8, and it is the test peer.)
 *
 * One linear sample in, one out, at 8 kHz. Fixed-point, no allocation.
 * Transmitter: v34/ (bits to points) and v34mod; receiver: ec, v34up.
 *
 * The far receiver gets what it asks for: carrier, pre-emphasis and power
 * reduction (INFO1), 4 or 16-point TRN / MP / E (its J), trellis code,
 * non-linear encoder, shaping and precoder (its MP). What our own receiver
 * asks is in the cfg.ask_* fields; all zero is the plain choice (4-point,
 * 16 states, no non-linear encoder, minimum shaping, no precoding). */
#ifndef V90D_MODEM_V34DX_H
#define V90D_MODEM_V34DX_H
#include <stdint.h>
#include "seqhunt.h"
#include "ec.h"
#include "ecdly.h"
#include "tonedet.h"
#include "v34up.h"
#include "v34mod.h"
#include "v34mp.h"
#include "../v34/v34.h"

typedef int  (*v34dx_getbit_fn)(void *user);
typedef void (*v34dx_putbit_fn)(void *user, int bit);

typedef struct {
    int answer;             /* 1 answer modem, 0 call modem */
    int sym_rate;           /* 2400, 3000 or 3200, both directions */
    int tx_car_num, tx_car_den;     /* our carrier (the far receiver's choice) */
    int rx_car_num, rx_car_den;     /* the far transmitter's carrier (our choice) */
    int tx_preemph;         /* pre-emphasis filter index the far receiver asked, 0..10 */
    int tx_pwr_red;         /* power reduction it asked, dB */
    int level;              /* nominal transmit power: RMS in sample units */
    int law;                /* 0: samples go to the line as they are; 1 mu-law, 2 A-law:
                               they pass a G.711 codec, so we send codec values only and
                               the echo canceller's reference is exact */
    int rtd;                /* round-trip delay from Phase 2, samples */
    int rtd_has_io;         /* ... which still contains our own path's delay (Phase 2 did
                               not know it either): taken off once echo_delay is found */
    int echo_delay;         /* our own sample path, as p34d_cfg: samples until the echo
                               starts to come back; -1 = find it during TRN */
    int md_len;             /* the far modem's MD, units of 35 ms */
    int lead;               /* answer: samples of silence before S */
    int peer_1664;          /* the far modem's INFO0 bit 25 (rates above 28800) */
    int max_rx_rate, max_tx_rate;   /* ceilings in bit/s, 0 = what the line gives */
    int symmetric;          /* 1: MP bit 50 = 0 */
    /* asked of the far transmitter */
    int ask_16pt;           /* J: 16-point TRN / MP / E */
    int ask_trellis;        /* MP bits 29:30 */
    int ask_nonlinear;      /* MP bit 31 */
    int ask_shaping;        /* MP bit 32 */
    int16_t ask_h[3][2];    /* precoder coefficients, 2.14; non-zero = MP Type 1 */
    int reneg_type0;        /* send the MP of a rate renegotiation as Type 0 (coefficients not repeated) */
    v34dx_getbit_fn getbit; /* data to send */
    v34dx_putbit_fn putbit; /* data received */
    void *user;
} v34dx_cfg;

typedef enum {
    DX_P3_SIL,      /* answer: silence before S */
    DX_P3_WAIT_S,   /* call: silent, waiting for S / S-bar */
    DX_P3_TX,       /* our S, S-bar, PP, TRN: echo canceller training */
    DX_P3_J,        /* our J, until the far S / S-bar */
    DX_P3_RX,       /* the far PP, TRN, J: equaliser training */
    DX_P4_TRN,      /* (S, S-bar or J') and TRN of Phase 4 or of a rate renegotiation */
    DX_P4_MP,       /* MP, MP' */
    DX_P4_E,        /* E, B1; waiting for both directions */
    DX_DATA,
    DX_RN_S,        /* S heard in data mode: waiting for its S-bar */
    DX_RETRAIN,     /* over: a retrain is due (the far modem's tone, or our own wish) */
    DX_FAILED
} v34dx_state;

enum {
    DX_OK = 0,
    DX_ERR_CFG,         /* symbol rate / carrier / filter that does not exist */
    DX_ERR_NO_S,        /* call: the first S never came */
    DX_ERR_RATE,        /* no data signalling rate in common, or parameters we cannot do */
    DX_ERR_B1,          /* B1 wrong */
    DX_ERR_CLEARDOWN,   /* the far modem cleared down (11.7) */
    DX_ERR_CARRIER,     /* signal lost in data mode */
    DX_ERR_HANGUP       /* we cleared down (v34dx_cleardown) */
};

/* transmit signals, for tx_count[] / tx_first[] */
enum { DXS_SIL, DXS_S, DXS_SBAR, DXS_PP, DXS_TRN, DXS_J, DXS_JP, DXS_TRN4, DXS_MP, DXS_E, DXS_DATA, DXS_N };

#define DX_MAX_ECHO_DELAY 3839  /* the longest own-path delay that can be given */
#define DX_FBUF 256

typedef struct {
    v34dx_cfg   cfg;
    v34dx_state state;
    int         error;
    uint32_t    now, t;             /* sample clock; samples in this state (modem/clock.h) */
    int         retrain_ours;       /* DX_RETRAIN: we asked for it */
    uint32_t    t_neg;              /* when Phase 4 or the rate renegotiation began */
    /* ---- transmit ---- */
    v34mod_t    mod;
    int         txsig; int32_t txn; /* signal and symbol index in it */
    int         tx_next;            /* leave J / TRN at the next chance; data at the end of its
                                       mapping frame (a renegotiation's or cleardown's S) */
    int         phase4;             /* S-bar is followed by TRN, not PP */
    int         tx_pts; int32_t tx_g;   /* TRN / MP / E: 4 or 16 points, and their gain */
    int32_t     trn_syms, trn4_syms;
    v34_tmod_t  tmod;
    v34_mp_t    mp;                 /* ours */
    int16_t     far_h[3][2];        /* precoding coefficients of the last Type 1 MP heard: zero until
                                       then, and a Type 0 MP leaves them alone (10.1.3.9) */
    uint8_t     mpbits[V34_MP_MAX_BITS]; int mplen;
    int         mp_sent, mpa_sent;
    v34_params_t tpar; v34_tx_t vtx; int32_t tx_dg;
    uint32_t    tx_count[DXS_N];    /* symbols sent of each signal so far */
    uint32_t    tx_first[DXS_N];    /* sample clock at the first / at the latest start */
    /* ---- echo ---- */
    ec_t        ec, fec;            /* near; far (round trip later), when far_on */
    int         ec_mu, far_on;
    ecdly_t     ecd;                /* what we sent; ecd.delay is our own path's delay in use */
    int32_t     trn_t;              /* samples of Phase 3 TRN sent */
    int32_t     pw_in, pw_res;      /* smoothed power into and out of the cancellers */
    /* ---- receive ---- */
    sdet_t      sdet;
    v34up_t     up;                 /* the receiver; up.rx_ready: data */
    sres_t      sres;
    int         rxmode;
    int         md_done; int32_t md_wait;
    int         rx16;               /* 16-point decisions */
    /* sequences */
    uint16_t    jsr; int32_t jbits, j_at; int j_type;
    int         seq_j, seq_mp;
    int         got_j, got_jp, far_16pt; uint32_t t_jp;
    uint8_t     fbuf[DX_FBUF]; int fn; seqhunt_t sh;
    v34_mp_t    far_mp, cand; int cand_ok;
    int         got_far_mp, got_far_mpa, got_e, far_clear;
    int32_t     snr_db10;           /* receiver SNR when our MP was made, 0.1 dB */
    int         line_n;             /* what that allows, units of 2400 bit/s */
    /* data mode */
    int         rates_done;
    int         tx_rate, rx_rate;   /* bit/s */
    int         tx_ready;
    int         in_reneg, clearing;
    int         n_reneg;            /* rate renegotiations completed */
    int         tx_starts, rx_starts;   /* times data mode began in each direction: the first
                                       bit asked of getbit / given to putbit after each is
                                       the first bit of that stretch of data */
    /* the far modem's retrain tone, 10 ms blocks */
    tonedet_t   tone;               /* the far modem's retrain tone */
    int         rate_drops;
} v34dx_t;

/* Returns 0, or -1 (state DX_FAILED, DX_ERR_CFG). */
int     v34dx_init(v34dx_t *p, const v34dx_cfg *cfg);
int16_t v34dx_tick(v34dx_t *p, int16_t rx);
/* From data mode: rate renegotiation (11.6); max_rx_rate > 0 sets a new
 * ceiling for our receive direction first. Returns 0, or -1 when not in
 * data mode. */
int     v34dx_renegotiate(v34dx_t *p, int max_rx_rate);
/* From data mode: cleardown (11.7); ends in DX_FAILED / DX_ERR_HANGUP. */
int     v34dx_cleardown(v34dx_t *p);
/* From Phase 4 on: give up and ask for a retrain; the state goes to DX_RETRAIN. */
int     v34dx_retrain(v34dx_t *p);

/* The echo cancellers now. Delays are where a canceller's EC_NTAP window
 * starts, samples after we sent; a peak is ec_peak() within it. */
typedef struct {
    int in_db10, res_db10;      /* received power into them, and left after them:
                                   0.1 dB below 32767 squared */
    int near_delay, near_peak, near_loss_db10;  /* loss: of the echo path as modelled,
                                                   0.1 dB; -1 while the model is empty */
    int far_delay, far_peak, far_loss_db10;     /* all -1 when there is no far canceller */
} v34dx_echo_t;
void    v34dx_echo(const v34dx_t *p, v34dx_echo_t *e);

#endif
