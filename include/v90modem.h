/* libv90modem: a software voice-band data modem (V.92, V.90, V.34, V.32bis,
 * V.22bis, V.22, V.21, Bell 212A, Bell 103, V.8) with V.42 error control
 * (LAPM, V.42bis, V.44). Both roles: answering (for V.90 / V.92 the digital,
 * server side) and calling (for V.90 / V.92 the analogue, client side). The
 * calling role has been tried in simulation and against this library's own
 * answerer only, never against a real modem.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The API is NOT stable before version 1.0: any 0.x release may change it.
 *
 * Rules the library keeps: C99, fixed-point arithmetic in the signal path, no
 * memory allocation after v90m_create() / v90m_ec_create(), no global state
 * (any number of modems in one process, one thread per object at a time).
 *
 * The line: 8000 samples per second, at a G.711 codec. The digital (V.90 /
 * V.92 server) side needs the codec's exact codewords on the line, so the
 * modem sends codewords, not linear samples, wherever a PCM modulation may
 * be offered.
 *
 * Data: the modem is a synchronous bit pump. Once connected, it calls getbit
 * for every bit it transmits and putbit for every bit it receives, from
 * inside v90m_process_*(), at the moment the bit is needed or decided. V.42
 * (below) or a start-stop framer sits on that pump. Callbacks (getbit,
 * putbit, on_event) are only ever made from inside v90m_process_*() on the
 * calling thread.
 *
 * What a callback may do to the modem that calls it: v90m_retrain,
 * v90m_cleardown, v90m_get_state, v90m_get_stats (and anything on another
 * object: every v90m_ec_* and v90m_async_* function). v90m_reset returns
 * V90M_ESTATE and changes nothing. v90m_destroy takes effect at the end of
 * the sample under way: no event after it, the rest of the block is
 * silence (linear 0, mu-law 0xFF, A-law 0xD5), the modem is freed (when it
 * came from v90m_create) and v90m_process_* returns V90M_OK. After a
 * v90m_destroy from getbit or putbit the rest of that sample still calls
 * getbit and putbit with the host's user pointer: free the user object
 * (a v90m_ec_t, a v90m_async_t) only after v90m_process_* returns, and
 * never from a callback of a modem that goes on. v90m_init on the modem's
 * own memory is not allowed.
 *
 * NULL: every function checks the object it is given (and the struct that
 * v90m_get_state, v90m_get_stats and v90m_ec_get_status fill): with NULL a
 * function returning int returns V90M_EINVAL, one returning size_t or
 * unsigned long 0, a void one does nothing. Except the four called for
 * every bit, v90m_ec_tx_bit, v90m_ec_rx_bit, v90m_async_getbit and
 * v90m_async_putbit: their object must not be NULL. */
#ifndef V90MODEM_H
#define V90MODEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define V90M_VERSION_MAJOR 0
#define V90M_VERSION_MINOR 6
#define V90M_VERSION_PATCH 0
/* "0.6.0": the version of the library linked in (may differ from the header) */
const char *v90m_version(void);

/* error codes (negative returns) */
#define V90M_OK        0
#define V90M_EINVAL   -1    /* a NULL or misaligned argument, or a configuration value out of range */
#define V90M_ENOTSUP  -2    /* a combination not implemented in this version (see v90m_role and
                               the V90M_MOD_* rules) */
#define V90M_ENOMEM   -3    /* create could not allocate, or init was given too little memory */
#define V90M_ESTATE   -4    /* not possible in the present state of the call */

/* ------------------------------------------------------------------ G.711
 * Codeword <-> linear on the scale of V.90 Table 1 (16-bit, the scale the
 * modem works in). alaw: 0 = mu-law, 1 = A-law. */
uint8_t v90m_g711_encode(int alaw, int16_t linear);    /* nearest codeword */
int16_t v90m_g711_decode(int alaw, uint8_t codeword);

/* ------------------------------------------------------------------ modem */
typedef struct v90m v90m_t;

/* The calling role (V90M_ROLE_ORIGINATE) starts when the far end has
 * answered; dialling and call progress are the owner's. Every modulation of
 * the calling role has been tried in simulation and against this library's
 * own answerer only.
 *  - V.21, V.22, V.22bis, V.32bis, V.34: after V.8, or without it after a
 *    pre-V.8 answer tone (V.22bis, V.21; V.32bis Annex A).
 *  - V.90 (V90M_MOD_V90) as the analogue modem. It needs the received samples
 *    linear as the line delivers them (v90m_process_linear or _lin_rx): a
 *    G.711 quantiser on our side would bury the PCM downstream in its noise.
 *    It retrains (either end asks: v90m_retrain, or the answerer's Tone B)
 *    and renegotiates the rate (the answerer's, and its own when its
 *    receiver's error moves far from what the rate was chosen for; a retrain
 *    when the line's level moved or the receiver is lost).
 *  - V.92 (V90M_MOD_V92, only with V90M_MOD_V90: V90M_ENOTSUP without it):
 *    PCM upstream when the answerer offers it in Phase 2, V.90 otherwise;
 *    full start-up, retrains and rate renegotiation as V.90's. No quick
 *    connect, short Phase 2 or modem-on-hold.
 *  - Bell 103 and Bell 212A: see V90M_MOD_BELL103 below. */
typedef enum {
    V90M_ROLE_ANSWER = 0,       /* the called modem */
    V90M_ROLE_ORIGINATE = 1     /* the calling modem (see above) */
} v90m_role;

/* modulations offered (v90m_cfg.modulations) */
#define V90M_MOD_V21     0x0001u
#define V90M_MOD_V22     0x0002u     /* V.22 and V.22bis */
#define V90M_MOD_V32BIS  0x0004u     /* V.32bis and V.32 (both roles) */
#define V90M_MOD_V34     0x0008u     /* (both roles) */
#define V90M_MOD_V90     0x0010u     /* answering: the digital (server) side; calling: the analogue
                                        (client) side */
#define V90M_MOD_V92     0x0020u     /* with V90M_MOD_V90 (alone: V90M_ENOTSUP): PCM upstream on
                                        offer (answering), or asked for (calling); not in the
                                        defaults */
/* Not in the defaults, either role. Answering: Bell 212A only with V90M_MOD_V22
 * (without: V90M_ENOTSUP). Bell 103: a caller without V.8
 * that sends 1270 Hz (the Bell 103 mark) gets 300 bit/s FSK on 2225 / 2025
 * Hz, start-stop and no error control as in V.21. Bell 212A, with
 * V90M_MOD_V22: when the V.22bis answer sequence has gone unanswered for 4 s
 * by a caller without V.8, 2225 Hz (the Bell answer tone) in place of the
 * unscrambled ones for 4 s; a caller that then starts as V.22 does is a
 * Bell 212A call at 1200 bit/s. */
/* Calling (tried in simulation and against this library's own answerer
 * only; the 212A handshake follows V.22 and was not checked against a Bell
 * 212A specification): V.8 cannot offer Bell, so it is what the caller does
 * when no V.8 comes about; a V.8 exchange that finds nothing in common does
 * not fall back to Bell. In this order: ANSam with a V.8 modulation also on
 * offer -> V.8 as ever. 2225 Hz (the Bell answer tone, within some 10 Hz) before any
 * 2100 Hz tone, or after ANS (V.25) -> Bell 212A if offered, else Bell 103.
 * Only Bell on offer: after ANSam or ANS we stay silent and listen; 2225 Hz
 * -> Bell 212A (else 103); with 103 alone, 150 ms of a carrier in the Bell
 * answer channel (2025..2250 Hz, a V.22 answerer's unscrambled ones
 * included, 2100 Hz not) -> Bell 103; with 212A too, 6 s of that carrier
 * without 2225 Hz -> Bell 103. A Bell 212A start that gets no 1200 bit/s
 * answer in 3 s -> Bell 103, if offered. Bell 212A is V.22 at 1200 bit/s
 * (no retrain); Bell 103 is 300 bit/s FSK (mark 1270 / space 1070 Hz out,
 * 2225 / 2025 Hz in), start-stop and no error control as in V.21. Neither
 * has a cleardown signal. */
#define V90M_MOD_BELL103  0x0040u
#define V90M_MOD_BELL212A 0x0080u
/* Answering only (calling: V90M_ENOTSUP), with V90M_MOD_V34 (without:
 * V90M_ENOTSUP), not in the defaults: x2, U.S. Robotics' 56k of 1997, as
 * its server. Offered on a mu-law line to a caller whose V.8 CM offers V.34
 * and no PCM: the JM and INFO0a of an x2 server. A caller that does not
 * take it up still gets V.34, but sees that JM (carrier inverted after
 * each JM) and INFO0a, not those it would get without x2; not yet tried
 * on a live non-x2 V.34 call. A
 * client that does take it up (its marker after our L1) has no way back to
 * V.34: if the x2 start-up does not reach data mode, or a rate change the
 * client asks for in data mode cannot be served (an index outside 1..15,
 * a pad other than none, 3 or 6 dB) or does not complete, the call ends
 * with V90M_END_X2_FAILED. In data mode (V90M_CONN_X2) tx_bps is the
 * downstream payload rate (32000 to 56000: what the client asks for, at
 * the start-up and in each rate change) and rx_bps the client's V.34
 * upstream (it may change in the client's rate change); USR's CONNECT
 * figure is one step of 1333 above it. */
#define V90M_MOD_X2       0x0100u
/* Answering only (calling: V90M_ENOTSUP), with V90M_MOD_V34 (without:
 * V90M_ENOTSUP), not in the defaults; experimental: K56flex (Rockwell and
 * Lucent, 1997) as its server. Offered on a mu-law line, in V.8bis before
 * V.8: at least 400 ms of silence after answering, CRe, and to a caller
 * that answers with CRd, a CL with the K56flex block; a caller whose MS
 * carries the block gets ACK(1), and K56flex is agreed (x2, offered too, is
 * then not offered in that call). V.8 follows in every case and chooses the
 * modulation as without K56flex: a caller whose CM offers PCM while V.90
 * is offered gets V.90. Every caller hears our answer tone later than
 * without K56flex: 1.79 s for a caller without V.8bis, about 3.8 s for a
 * V.8bis caller without K56flex (CRd, no MS), about 3.4 s for a K56flex
 * client (measured in the simulator). An agreed client whose INFO1c asks
 * for K56flex, with 3200 baud chosen for its upstream, gets our INFO1a
 * flagged: there is no way back to V.34 after it. If the K56flex start-up,
 * or a rate renegotiation or retrain of the client's, does not complete,
 * or the client retrains a third time in a row, the call ends with
 * V90M_END_K56_FAILED; we start neither a renegotiation nor a retrain.
 * Only the client's tone B in data mode is followed as a retrain: one sent
 * during a training (after a failed training) is not, and that training
 * ends at its time limit with V90M_END_K56_FAILED. In
 * data mode (V90M_CONN_K56FLEX) tx_bps is the downstream payload rate
 * (32000 to 56000 in steps of 2000, the figure a Rockwell modem prints)
 * and rx_bps the client's V.34 upstream. */
#define V90M_MOD_K56      0x0200u

/* the modulation of a connection (v90m_state.modulation) */
typedef enum {
    V90M_CONN_NONE = 0, V90M_CONN_V21, V90M_CONN_V22, V90M_CONN_V22BIS,
    V90M_CONN_V90, V90M_CONN_V34, V90M_CONN_V32BIS, V90M_CONN_V92,
    V90M_CONN_BELL103,          /* appended: the values above stay as they were */
    V90M_CONN_BELL212A,         /* V.22 at 1200 bit/s begun on the 2225 Hz Bell answer tone */
    V90M_CONN_X2,               /* (appended) x2: PCM downstream, V.34 upstream */
    V90M_CONN_K56FLEX           /* (appended) K56flex: PCM downstream, V.34 upstream */
} v90m_conn;

typedef int  (*v90m_getbit_fn)(void *user);             /* next bit to send, 0 or 1 */
typedef void (*v90m_putbit_fn)(void *user, int bit);    /* a bit received */

/* State changes, reported from inside v90m_process_*() right after the sample
 * at which they happened, before the next one is processed. */
typedef enum {
    V90M_EV_CONNECTED = 1,      /* data mode reached: v90m_state has the modulation and rates */
    V90M_EV_RATES,              /* the rates or the modulation changed (renegotiation, retrain) */
    V90M_EV_DATA_STOPPED,       /* data stopped moving (retrain, rate renegotiation) */
    V90M_EV_DATA_MOVING,        /* data moves again both ways (error control: say so to V.42) */
    V90M_EV_ENDED               /* the modem stopped: see v90m_state.end */
} v90m_event;
typedef void (*v90m_event_fn)(void *user, v90m_event ev);

#define V90M_TX_LEVEL_MAX 16384     /* v90m_cfg.tx_level: x 181/128 (the tones' peak) fits 16 bits */

typedef struct {
    v90m_role      role;
    unsigned       modulations;     /* V90M_MOD_*; default all but V.92 and Bell. Either role:
                                       at least one (0 gives V90M_EINVAL), and V.92 only with
                                       V.90 (else V90M_ENOTSUP) */
    int            alaw;            /* the codec's law: 0 mu-law, 1 A-law */
    int            codec_linear_rx; /* answering: 1 = the codec receives in 16-bit linear, with no
                                       G.711 quantiser on the receive path (V.92 then designs the
                                       upstream constellation without its share of the noise);
                                       0 = the received samples went through G.711, also when
                                       they are handed over as linear values. Calling: unused */
    int            tx_level;        /* transmit RMS outside PCM modes, 16-bit units, 0 ..
                                       V90M_TX_LEVEL_MAX (else V90M_EINVAL); default 3000. Near
                                       the top the peaks of V.34-family signals clip: a calling
                                       V.90 modem's V.34 upstream at 15360 or 16384 goes over
                                       full scale some tens of samples a call, and whether its
                                       B1 gets through depends on the upstream rate the answerer
                                       picks (docs/api.md); 15360 is the level tested to connect */
    int            max_tx_code;     /* answering, V.90 / V.92: the most downstream power the caller
                                       may ask for, -0.5 dBm0 less 0.5 dB per step, 1..31;
                                       0 = 23 (-12 dBm0). Calling: unused (0..31 accepted) */
    int            max_down_bps;    /* rate ceilings, bit/s, >= 0; 0 = none. max_down caps what we */
    int            max_up_bps;      /* send, max_up what we receive (named from the answering, server
                                       side), in V.34, V.90 and V.92, either role: V.92's PCM upstream
                                       (up to 48000) under max_up answering, max_down calling; 33600
                                       holds it at 33333. V.32bis (either role): one rate both ways,
                                       under the lower of the two. V.21, V.22, V.22bis, Bell: unused.
                                       With x2 or K56flex, max_up 1..2399 is V90M_EINVAL (their
                                       V.34 upstream starts at 2400) */
    int            echo_delay;      /* samples from a sample leaving to its echo starting back (the
                                       driver's buffering), >= 0; -1 = measure it in every start-up.
                                       Calling V.90 / V.92: never measured, used as given, 0 .. 511
                                       (-1 is taken as 0, more as 511) */
    int            retrain_on_step; /* V.34, either role: when what we receive falls in a step (6 dB
                                       below what it was a second or so before), retrain rather
                                       than renegotiate down. The echo cancellers do not
                                       adapt in data mode and only a retrain trains them again: for
                                       a path whose echo can move mid-call, as through a VoIP
                                       jitter buffer that grows by a packet. Also retrain when a
                                       rate renegotiation's training fails, rather than end the
                                       call. Answering, also when the SNR has stayed 6 dB or more
                                       below the call's best for 30 s. Default 0 */
    v90m_getbit_fn getbit;
    v90m_putbit_fn putbit;
    v90m_event_fn  on_event;        /* may be NULL: poll v90m_get_state() instead */
    int            send_ci;         /* calling: send CI before the answer tone (V.8 7.1) */
    void          *user;            /* passed to getbit, putbit and on_event */
} v90m_cfg;

/* Memory for v90m_init() / v90m_ec_init() / v90m_async_init() as part of a
 * struct of the caller's (nothing allocated at all): upper bounds of
 * v90m_mem_size() and the others for this version, on every platform. They
 * change with the version; the init functions check the length given. */
#define V90M_MEM_SIZE        (108u * 1024u)
#define V90M_EC_MEM_SIZE     (50u * 1024u)
#define V90M_ASYNC_MEM_SIZE  2048u
/* alignment for that memory:  union { v90m_align a; uint8_t b[V90M_MEM_SIZE]; } mem; */
typedef union { long double ld; double d; int64_t i; void *p; void (*f)(void); } v90m_align;

/* Answering, the default modulations, tx_level 3000, echo_delay -1, the
 * rest 0 or NULL. */
void     v90m_cfg_default(v90m_cfg *cfg);
/* One modem. The only allocation the library makes for it. NULL on failure,
 * with *err (err may be NULL): V90M_EINVAL (cfg NULL or a value out of
 * range), V90M_ENOTSUP, V90M_ENOMEM; V90M_OK otherwise. */
v90m_t  *v90m_create(const v90m_cfg *cfg, int *err);
/* Or in memory of the caller's, at least v90m_mem_size() octets, aligned for
 * any type (nothing is allocated; v90m_destroy() then does nothing). Errors
 * as v90m_create, and V90M_EINVAL for mem NULL or misaligned, V90M_ENOMEM
 * for len too small. */
size_t   v90m_mem_size(void);
v90m_t  *v90m_init(void *mem, size_t len, const v90m_cfg *cfg, int *err);
/* Back to the start of a new call (the line went off-hook), same memory, the
 * role and everything else from cfg. V90M_OK; or V90M_EINVAL (m or cfg NULL,
 * a value out of range), V90M_ENOTSUP, or V90M_ESTATE (from a callback of
 * m's), the modem left as it was. */
int      v90m_reset(v90m_t *m, const v90m_cfg *cfg);
void     v90m_destroy(v90m_t *m);           /* NULL is fine; from a callback: see the top */

/* The line: n received samples in, n samples to send out. Any n, 1
 * included: the result does not depend on how the samples are cut into
 * blocks. Use one of the three for a call:
 *   _g711     codewords in, codewords out: the interface for a codec, and the
 *             one V.90 / V.92 need (the far end decodes our exact codewords)
 *   _lin_rx   a receive side in 16-bit linear (Table 1 scale: what
 *             v90m_g711_decode gives), codewords out; V.90 / V.92 work too
 *   _linear   linear both ways (a PCM modulation then sends the linear value
 *             of its codewords)
 * Return V90M_OK, or V90M_EINVAL (m NULL, or rx or tx NULL with n > 0).
 * A v90m_destroy from a callback ends the block early (see the top). */
int      v90m_process_g711(v90m_t *m, const uint8_t *rx, uint8_t *tx, size_t n);
int      v90m_process_lin_rx(v90m_t *m, const int16_t *rx, uint8_t *tx, size_t n);
int      v90m_process_linear(v90m_t *m, const int16_t *rx, int16_t *tx, size_t n);

/* Ask for a retrain. Answering: in data mode of V.92, V.90, V.34, V.32bis
 * (x2, K56flex: V90M_ESTATE). Calling: once connected in V.22bis at 2400 bit/s, V.32bis, V.34, V.90,
 * V.92. V90M_OK, or V90M_ESTATE (none of those, or one under way).
 * Calling V.90 / V.92: 70 ms of silence and Tone A, Phase 2 to data mode
 * again (some 7 to 11 s); also during a rate renegotiation; V90M_ESTATE when
 * one is under way or three have run without a minute of data mode between
 * them. A retrain that fails, or one more due past that limit (the far end's,
 * or one the receiver needs), ends the call with V90M_END_RETRAIN_FAILED
 * (the receiver's after a cleardown, V.90 9.7 / V.92 9.11). */
int      v90m_retrain(v90m_t *m);
/* Hang up, telling the far modem where the modulation has a way to (V.90
 * 9.7, V.92 9.11, V.34 11.7, either role): V90M_OK when that has begun
 * (V90M_EV_ENDED follows within a fraction of a second; V.92 answering: the
 * cleardown is a rate sequence with data rate 0, drn = 0, and a caller that
 * does not acknowledge it is given up to 3.5 s); V90M_ESTATE when
 * there is nothing to send (V.21, V.22, V.22bis, V.32bis, Bell, or not in
 * data mode): just stop calling v90m_process_*. x2 and K56flex (answering,
 * in data mode or a rate change): V90M_OK, and the call ends at once with
 * nothing sent. */
int      v90m_cleardown(v90m_t *m);

typedef enum {
    V90M_END_NONE = 0,
    V90M_END_FAILED,            /* no connection was made (start-up failed) */
    V90M_END_LOST,              /* the connection was lost (any loss once connected but
                                   those below) */
    V90M_END_CLEARDOWN_FAR,     /* the far modem cleared down */
    V90M_END_CLEARDOWN_OURS,    /* after v90m_cleardown() */
    V90M_END_NO_ANSWER_TONE,    /* calling only: no answer tone came */
    V90M_END_NO_JM,             /* calling only: ANSam came, but no JM in answer to CM (V.8) */
    V90M_END_NO_COMMON,         /* V.8 found no modulation in common with the far modem */
    V90M_END_RETRAIN_FAILED,    /* (appended) V.90 / V.92, V.34 (either role):
                                   a retrain failed (in its Phase 2, 3 or 4), or one
                                   more was due past the limit
                                   (see v90m_retrain); answering x2: the client asked for
                                   a retrain in data mode or in its rate change (its
                                   Tone A), which x2 cannot serve */
    V90M_END_X2_FAILED,         /* (appended) answering: the client took up x2 (its marker
                                   came) and the x2 start-up, or a rate change of
                                   the client's, did not complete */
    V90M_END_K56_FAILED         /* (appended) answering: K56flex was agreed and its start-up
                                   went past the point of no return (our INFO1a), and
                                   the start-up, or a rate change or retrain of the
                                   client's, did not complete, or the client retrained
                                   a third time in a row */
} v90m_end;

/* Cheap; fine to read after every sample. Also after the end: end says why. */
typedef struct {
    int        connected;       /* data mode reached, and kept through retrains */
    int        moving;          /* user data moves both ways now */
    int        retraining;      /* a retrain is under way; 0 in a rate renegotiation */
    v90m_conn  modulation;      /* in force (during a retrain: the one before) */
    int        tx_bps, rx_bps;  /* our transmit / receive rate (rx = tx where symmetric) */
    int        used_v8;         /* the far modem spoke V.8 */
    int        odp_bypass;      /* answering, V.92 9.3.1: the caller skips V.42's ODP / ADP
                                   (start the V.42 endpoint without detection); calling: 0 */
    v90m_end   end;
} v90m_state;
void     v90m_get_state(const v90m_t *m, v90m_state *st);

typedef struct {
    v90m_conn  modulation;
    int        tx_bps, rx_bps;
    int        snr_db10;        /* receiver signal to error, 0.1 dB, 0..600; -1 = none.
                                   While connected, the engine's figure, in a retrain too;
                                   where it has none, the last measured while data moved.
                                   After the end of the call, that last value; -1 only if
                                   none was measured this call. Until v90m_reset */
    int        rtd;             /* round-trip delay measured in the handshake, samples; -1 = none
                                   (answering: V.34, V.90, V.92, x2, K56flex; calling: V.32bis,
                                   V.34, V.90, V.92); live or kept as snr_db10 is */
    int        echo_delay;      /* answering V.90 / V.92 / x2 / K56flex: in use (configured
                                   or measured) by the engine running now, during a retrain
                                   or after the end the call's; samples; -1 = none, and
                                   always -1 calling */
    unsigned   retrains;        /* begun, this call */
    unsigned   renegotiations;  /* rate renegotiations that stopped the data, this call:
                                   counted when data moves again, when it becomes a retrain,
                                   or when the call ends in it (not a cleardown's stop, ours
                                   or the far end's) (answering: V.34, V.90, V.92, x2, K56flex; calling:
                                   V.32bis, V.34, V.90, V.92) */
    /* (appended) answering V.34, in data mode or a rate renegotiation: the echo
     * cancellers; all -1 otherwise. Each covers a 12 ms window of what we sent:
     * a delay is where its window starts and a peak the strongest tap in it,
     * samples; a loss is the echo path's as modelled, 0.1 dB (-1 while empty).
     * "far" cancels the echo a round trip later (-1 when not in use). An echo
     * whose delay moves out of its window is no longer cancelled: its model
     * fades (the loss rises) and the SNR falls. */
    int        ec_in_db10, ec_res_db10;    /* received power into the cancellers and left after
                                              them, 0.1 dB below full scale (32767 squared) */
    int        ec_near_delay, ec_near_peak, ec_near_loss_db10;
    int        ec_far_delay, ec_far_peak, ec_far_loss_db10;
} v90m_stats;
void     v90m_get_stats(const v90m_t *m, v90m_stats *st);

/* ------------------------------------- start-stop characters (V.14 basic)
 * Characters (1 start, 8 data LSB first, 1 stop, mark idle) on the bit pump
 * of a connected modem, without error control: pass v90m_async_getbit and
 * v90m_async_putbit as the modem's getbit / putbit with the object as user.
 * Fixed 512-octet queues each way; a character received with the receive
 * queue full is dropped, and not counted. */
typedef struct v90m_async v90m_async_t;
/* create / init: NULL on failure, *err as for v90m_create / v90m_init */
v90m_async_t *v90m_async_create(int *err);
size_t        v90m_async_mem_size(void);
v90m_async_t *v90m_async_init(void *mem, size_t len, int *err);
void          v90m_async_reset(v90m_async_t *a);
void          v90m_async_destroy(v90m_async_t *a);     /* NULL is fine */
int           v90m_async_getbit(void *a);
void          v90m_async_putbit(void *a, int bit);
size_t        v90m_async_write(v90m_async_t *a, const uint8_t *buf, size_t n);  /* octets accepted */
size_t        v90m_async_read(v90m_async_t *a, uint8_t *buf, size_t max);       /* octets delivered */
size_t        v90m_async_tx_pending(const v90m_async_t *a);   /* queued or being sent */
unsigned long v90m_async_frame_errors(const v90m_async_t *a);

/* -------------------------------------------- V.42 error control (LAPM)
 * One endpoint, on the bit pump of a connected modem: feed it the modem's
 * bits (v90m_ec_tx_bit from getbit, v90m_ec_rx_bit from putbit), tick it
 * every millisecond, and move user octets with write / read. With V.42bis or
 * V.44 compression when both ends agree; falls back to start-stop (no error
 * control) when the far end has no V.42 and fallback is on. */
typedef struct v90m_ec v90m_ec_t;

/* The ranges: a value outside them is V90M_EINVAL from create, init and
 * reset. Flags are 0 or 1. */
typedef struct {
    int originator;         /* 1 = the calling end, 0 = the answering end */
    int detect;             /* run the detection phase (V.42 7.2.1), 0..1 */
    int fallback;           /* when detection fails: 1 = start-stop, 0 = disconnect */
    int t400_ms;            /* detection phase timer, >= 0; 0 = the default, 750 */
    int establish_ms;       /* answerer: give up when no SABME comes this long after detection,
                               >= 0; 0 = never */
    int comp_policy;        /* V90M_COMP_AUTO / ALWAYS / NEVER (0..2) for our encoder */
    struct {
        int use_xid;            /* originator: negotiate with XID before SABME, 0..1 */
        int k_tx, k_rx;         /* window, 1..32 (k_tx), 1..127 (k_rx) */
        int n401_tx, n401_rx;   /* information field, octets, 1..256 */
        int fcs32;              /* 0..1 */
        int v42bis_tx, v42bis_rx;   /* V.42bis wanted, our send / receive direction, 0..1 */
        int v42bis_p1, v42bis_p2;   /* codewords 512..2048, string length 6..250 */
        int v44_tx, v44_rx;         /* V.44 wanted (preferred over V.42bis), 0..1 */
        int v44_p1_tx, v44_p1_rx;   /* codewords 256..2048 */
        int v44_p2_tx, v44_p2_rx;   /* string length 32..255 */
        int v44_p3_tx, v44_p3_rx;   /* history, octets 512..4096 */
        int t401_ms, t403_ms, n400; /* t401_ms >= 1; t403_ms >= 0, 0 = off; n400 >= 1 */
    } lapm;
} v90m_ec_cfg;
#define V90M_COMP_AUTO   0
#define V90M_COMP_ALWAYS 1
#define V90M_COMP_NEVER  2

enum { V90M_EC_DETECT = 0, V90M_EC_ESTABLISH, V90M_EC_CONNECTED, V90M_EC_ASYNC,
       V90M_EC_RELEASING, V90M_EC_DISCONNECTED };
/* why an endpoint is V90M_EC_DISCONNECTED (v90m_ec_status.reason).
 * V90M_EC_R_N400 also ends a link that made no progress through n400
 * recoveries (docs/error-control.md "Bounded waits"; a busy peer, RNR,
 * is not counted). */
enum { V90M_EC_R_NONE = 0, V90M_EC_R_LOCAL, V90M_EC_R_PEER_DISC, V90M_EC_R_PEER_DM,
       V90M_EC_R_N400, V90M_EC_R_PROTOCOL, V90M_EC_R_NEGOTIATION,
       V90M_EC_R_NO_EC = 16, V90M_EC_R_COMPRESSION, V90M_EC_R_NO_SABME };
enum { V90M_EC_COMP_NONE = 0, V90M_EC_COMP_V42BIS, V90M_EC_COMP_V44 };

typedef struct {
    int state, reason;
    int k_tx, k_rx, n401_tx, n401_rx;
    int comp_alg;                   /* V90M_EC_COMP_*, agreed */
    int comp_tx, comp_rx;           /* agreed, per direction */
    int enc_on, dec_on;             /* our encoder / decoder in use now */
    int alg_on;                     /* V90M_EC_COMP_* of the encoder / decoder in use */
    int comp_n2, comp_n7;
    int v44_n2_tx, v44_n7_tx, v44_n8_tx, v44_n2_rx, v44_n7_rx, v44_n8_rx;
    int fcs32;
    size_t tx_queued, rx_avail;
    int unacked;
    unsigned long rx_frames, rx_frames_bad;
    unsigned long tx_i, tx_i_retx, rx_i, rx_i_dropped;
    unsigned long tx_rej, rx_rej, t401_expiries, violations;
    unsigned long comp_in, comp_out, decomp_in, decomp_out;
    unsigned long async_overruns, async_framing;
} v90m_ec_status;

void        v90m_ec_cfg_default(v90m_ec_cfg *cfg, int originator);
/* create / init: NULL on failure, *err as for v90m_create / v90m_init
 * (V90M_EINVAL for a value outside the ranges above); reset: V90M_OK, or
 * V90M_EINVAL for e or cfg NULL or a value outside them, the endpoint left
 * as it was */
v90m_ec_t  *v90m_ec_create(const v90m_ec_cfg *cfg, int *err);
size_t      v90m_ec_mem_size(void);
v90m_ec_t  *v90m_ec_init(void *mem, size_t len, const v90m_ec_cfg *cfg, int *err);
int         v90m_ec_reset(v90m_ec_t *e, const v90m_ec_cfg *cfg);
void        v90m_ec_destroy(v90m_ec_t *e);           /* NULL is fine */

int         v90m_ec_tx_bit(v90m_ec_t *e);               /* from the modem's getbit */
void        v90m_ec_rx_bit(v90m_ec_t *e, int bit);      /* from the modem's putbit */
void        v90m_ec_tick(v90m_ec_t *e, unsigned ms);    /* timers */
void        v90m_ec_line_stopped(v90m_ec_t *e);         /* on V90M_EV_DATA_STOPPED (moving 1 -> 0) */
void        v90m_ec_line_restored(v90m_ec_t *e);        /* on V90M_EV_DATA_MOVING (moving 0 -> 1) */

size_t      v90m_ec_write(v90m_ec_t *e, const uint8_t *data, size_t len);
size_t      v90m_ec_write_space(const v90m_ec_t *e);
size_t      v90m_ec_read(v90m_ec_t *e, uint8_t *data, size_t max);
size_t      v90m_ec_read_avail(const v90m_ec_t *e);
int         v90m_ec_tx_idle(const v90m_ec_t *e);        /* everything written was acknowledged */
int         v90m_ec_tx_busy(const v90m_ec_t *e);        /* a frame is still leaving the transmitter */

void        v90m_ec_release(v90m_ec_t *e);              /* orderly: DISC, wait for UA */
void        v90m_ec_abort(v90m_ec_t *e);                /* the line is gone: stop without a word, in
                                                           every state: V90M_EC_DISCONNECTED on return */
/* break signals (V.42 12.3.1): opt is one of these, len10ms the length in
 * 10 ms units. send_break: V90M_OK when the break is on its way;
 * V90M_EINVAL for any other opt; V90M_ESTATE when error control is not up
 * (V90M_EC_CONNECTED) or a break of ours is still pending. get_break: 1 with *opt and *len10ms when a break came
 * (in its place among the octets read), else 0. */
#define V90M_BRK_NONDESTRUCTIVE_INSEQ      0x00
#define V90M_BRK_NONDESTRUCTIVE_EXPEDITED  0x40
#define V90M_BRK_DESTRUCTIVE_EXPEDITED     0xC0
int         v90m_ec_send_break(v90m_ec_t *e, int opt, int len10ms);
int         v90m_ec_get_break(v90m_ec_t *e, int *opt, int *len10ms);

int         v90m_ec_state(const v90m_ec_t *e);       /* V90M_EC_DETECT ... */
void        v90m_ec_get_status(const v90m_ec_t *e, v90m_ec_status *st);

#ifdef __cplusplus
}
#endif
#endif
