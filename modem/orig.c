/* See orig.h. */
#include <string.h>
#include "orig.h"
#include "v21h_bpf.h"
#include "bell103h_bpf.h"
#include "probe.h"
#include "sintab.h"

#define FS 8000
#define MS(ms) ((int32_t)(ms) * 8)

#define LEGACY_MS      10000   /* after ANS: how long to wait for V.22bis or V.21 */
#define V22_TOTAL_MS   30000   /* give up on a V.22bis start-up */
#define V21_WAIT_MS    20000
#define V21_CD_ON_MS     100   /* as in modem/answer.c */
#define V21_MARK_MS      300   /* after ANS: a steady channel 2 mark this long is a V.21 answerer */
#define CD_LOSS_MS       400
#define V21_CD_AMP       110   /* carrier detect, peak amplitude (about -43 dBm0) */
#define V32_TOTAL_MS   60000   /* give up on a V.32bis start-up (its every wait has its own time-out too) */
#define V34_TOTAL_MS   60000   /* the same for V.34, retrains in Phase 2 included */
#define V90_TOTAL_MS   60000   /* the same for V.90 (its own limits come first) */
/* V.32bis Annex A (see orig.h) */
#define AA_AFTER_MS      600   /* AA once ANS has been heard 1 s: v8orig needed 400 ms or more of it to tell ANS */
#define ANS_OFF_MS        40   /* ANS is over when away this long (the gaps of its phase reversals are shorter) */
#define AA_GIVEUP_MS    1500   /* after ANS, nothing but perhaps V.21: AA stops (V.21 on offer) */
#define ANS_LONG_MS      800   /* A.2.1.3 */
#define TC_MS           3100   /* A.2.1.2: Tc, more than 3100 ms */
/* Bell (see orig.h) */
#define BELL_WAIT_MS   30000   /* ORIG_BELL: give up */
#define BELL103_CD_MS    150   /* only 103 offered: a carrier this long in its answer channel */
#define BELL103_LATE_MS 6000   /* 212A offered too: the same carrier this long without 2225 Hz */
#define B212_WAIT_MS    3000   /* Bell 212A started: no 1200 bit/s answer by then -> Bell 103 */
#define BELL_2100 ((uint32_t)(((uint64_t)2100 << 32) / FS))
#if V21H_BPF_N != BELL103H_BPF_N
#error "the two FSK receive band-passes share one loop"
#endif

static void fail(orig_t *o, int why) { o->phase = ORIG_FAILED; o->connected = 0; o->end = why; o->t = 0; }

static void start_v22(orig_t *o)
{
    v22m_init(&o->v22, V22_CALLER, 1, o->cfg.level, o->cfg.getbit, o->cfg.putbit, o->cfg.user);
    o->cd_run = 0;
    o->phase = ORIG_V22; o->t = 0;
}

/* V.32bis (6.1): the calling modem, sending AA from now on. One rate both
 * ways: the ceiling of our configuration. */
static void start_v32(orig_t *o)
{
    static const int rate[V32_NRATES] = { 4800, 7200, 9600, 12000, 14400 };
    v32_cfg c;
    int i;
    memset(&c, 0, sizeof c);
    c.level = o->cfg.level;
    c.rates = V32_R4800;
    for (i = 1; i < V32_NRATES; i++) if (o->cfg.max_bps <= 0 || rate[i] <= o->cfg.max_bps) c.rates |= 1u << i;
    c.getbit = o->cfg.getbit; c.putbit = o->cfg.putbit; c.user = o->cfg.user;
    v32_init_call(&o->v32, &c);
}

/* V.34 (11.2.1.1.1): INFO0c now, the 75 ms of silence after CJ being over */
static void start_v34(orig_t *o)
{
    v34orig_cfg c;
    memset(&c, 0, sizeof c);
    c.level = o->cfg.level;
    c.law = o->cfg.law;
    c.io_delay = o->cfg.echo_delay;
    c.max_tx_rate = o->cfg.max_tx_bps; c.max_rx_rate = o->cfg.max_rx_bps;
    c.getbit = o->cfg.getbit; c.putbit = o->cfg.putbit; c.user = o->cfg.user;
    c.retrain_on_step = o->cfg.retrain_on_step;
    v34orig_init(&o->v34, &c);
    o->phase = ORIG_V34; o->t = 0;
}

/* V.90 (9.2.2.1.1): INFO0a now, the 75 ms of silence after CJ being over */
static void start_v90(orig_t *o)
{
    v90a_cfg c;
    memset(&c, 0, sizeof c);
    c.level = o->cfg.level;
    c.law = o->cfg.law;
    c.echo_delay = o->cfg.echo_delay > 0 ? o->cfg.echo_delay : 0;
    c.max_tx_bps = o->cfg.max_tx_bps; c.max_rx_bps = o->cfg.max_rx_bps;
    c.table1 = o->cfg.v90_table1;
    c.v92 = (o->cfg.modes & ORIG_MODE_V92) != 0;    /* PCM upstream if the answerer offers it */
    c.getbit = o->cfg.getbit; c.putbit = o->cfg.putbit; c.user = o->cfg.user;
    v90a_init(&o->v90, &c);
    o->phase = ORIG_V90; o->t = 0;
}

/* why a V.90 call ended */
static int v90_end(const orig_t *o)
{
    switch (o->v90.error) {
    case V90A_ERR_CLEARDOWN: return ORIG_END_CLEARDOWN;
    case V90A_ERR_HANGUP: return ORIG_END_HANGUP;
    case V90A_ERR_RETRAINS: return ORIG_END_RETRAIN;
    case V90A_ERR_RATE: return o->connected ? ORIG_END_LOST : ORIG_END_NO_COMMON;
    default: return o->connected ? ORIG_END_LOST : ORIG_END_NO_CARRIER;
    }
}

/* why a V.34 call ended */
static int v34_end(const orig_t *o)
{
    switch (o->v34.error) {
    case V34O_ERR_CLEARDOWN: return ORIG_END_CLEARDOWN;
    case V34O_ERR_HANGUP: return ORIG_END_HANGUP;
    case V34O_ERR_PHASE2:
        /* nothing to offer in INFO1c, or an INFO1a we cannot follow */
        if (!o->connected && o->v34.p2.error == P2L_ERR_NO_RATE) return ORIG_END_NO_COMMON;
        return o->connected ? ORIG_END_LOST : ORIG_END_NO_CARRIER;
    default: return o->connected ? ORIG_END_LOST : ORIG_END_NO_CARRIER;
    }
}

/* why a V.32bis call ended */
static int v32_end(const orig_t *o)
{
    if (o->v32.error == V32_ERR_NO_RATE)
        /* our R2 asked for the cleardown (nothing in common), or the answerer's R3 / E did */
        return o->v32.r2_word && !v32_word_rates(o->v32.r2_word) ? ORIG_END_NO_COMMON : ORIG_END_CLEARDOWN;
    return o->connected ? ORIG_END_LOST : ORIG_END_NO_CARRIER;
}

/* Annex A: ANS (2100 Hz, -43 dBm0 or more: as v32_aadet's 1800 Hz) and AA */
static void annex_a(orig_t *o, int16_t rx)
{
    int32_t c, s;
    int64_t pw;
    o->at_ph += (uint32_t)(((uint64_t)2100 << 32) / FS);
    s = sintab[(o->at_ph >> 22) & SIN_MASK];
    c = sintab[((o->at_ph >> 22) + SIN_N / 4) & SIN_MASK];
    /* to 0 Hz, two one-pole low-passes of about 20 Hz: our own AA, 300 Hz
     * away, is more than 40 dB down */
    o->at_i1 += (((rx * c) >> 7) - o->at_i1) >> 6;
    o->at_q1 += (((rx * s) >> 7) - o->at_q1) >> 6;
    o->at_i2 += (o->at_i1 - o->at_i2) >> 6;
    o->at_q2 += (o->at_q1 - o->at_q2) >> 6;
    pw = (int64_t)o->at_i2 * o->at_i2 + (int64_t)o->at_q2 * o->at_q2;
    if (pw > ((int64_t)157 * 157 / 4) << 16) { if (o->at_on < MS(60000)) o->at_on++; o->at_off = 0; }
    else if (o->at_off < MS(60000)) o->at_off++;
    if (o->t_aa >= 0 && o->t_aa < MS(60000)) o->t_aa++;
    if (o->t_quiet < 0) {
        if (o->at_off >= MS(ANS_OFF_MS)) {
            /* A.2.1.3: the timer of the rest of ANS stops at its end */
            o->t_quiet = o->at_off;
            if (o->t_aa >= 0) o->ans_rest = o->t_aa - o->at_off;
        }
    } else if (o->t_quiet < MS(60000)) o->t_quiet++;
    /* A.2.1.3: AA once ANS has been there for 1 s */
    if (o->t_aa < 0 && o->t_quiet < 0 && o->at_on >= MS(AA_AFTER_MS)) { o->aa = 1; o->t_aa = 0; }
    /* neither AC nor V.22bis after ANS: a V.21 answerer's mark is in
     * channel 2, where AA's 1800 Hz would drown it */
    if (o->aa && (o->cfg.modes & V8_MOD_V21) && o->tc < 0 && o->t_quiet >= MS(AA_GIVEUP_MS)) o->aa = 0;
}

/* V.21 receiver on channel 2: what the answerer sends */
static void v21_rx_init(orig_t *o)
{
    v21demod_init(&o->fdem, V21H_MARK, V21H_SPACE, FS, 300);
    arx_init(&o->uart, FS, 300);
    o->cd_thresh = v21demod_energy_of(&o->fdem, V21_CD_AMP);
    for (o->bpos = 0; o->bpos < 128; o->bpos++) o->bpf[o->bpos] = 0;
    o->bpos = 0; o->bpf_taps = v21h_bpf;
}

/* the Bell 103 receiver on the answer channel, 2225 / 2025 Hz */
static void bell_rx_init(orig_t *o)
{
    v21demod_init(&o->fdem, BELL103_A_MARK, BELL103_A_SPACE, FS, 300);
    arx_init(&o->uart, FS, 300);
    o->cd_thresh = v21demod_energy_of(&o->fdem, V21_CD_AMP);
    for (o->bpos = 0; o->bpos < 128; o->bpos++) o->bpf[o->bpos] = 0;
    o->bpos = 0; o->bpf_taps = bell103h_bpf;
}

static void start_v21(orig_t *o)
{
    /* peak amplitude of a sine with the configured RMS */
    v21mod_init(&o->fmod, V21L_MARK, V21L_SPACE, FS, 300, (int16_t)(o->cfg.level * 181 / 128));
    if (o->phase != ORIG_LEGACY) v21_rx_init(o);     /* in ORIG_LEGACY it has been listening already */
    o->fbit = 1; o->cd_run = 0; o->fsk_mod = ORIG_MOD_V21;
    o->phase = ORIG_V21; o->t = 0;
}

/* Bell 103 (orig.h): our mark, 1270 Hz, from now on */
static void start_b103(orig_t *o)
{
    v21mod_init(&o->fmod, BELL103_O_MARK, BELL103_O_SPACE, FS, 300, (int16_t)(o->cfg.level * 181 / 128));
    if (o->phase != ORIG_BELL) bell_rx_init(o);     /* in ORIG_BELL it has been listening already */
    o->fbit = 1; o->cd_run = 0; o->fsk_mod = ORIG_MOD_BELL103;
    o->connected = 0;
    o->phase = ORIG_V21; o->t = 0;
}

/* Bell 212A (orig.h): the V.22 caller at 1200 bit/s, its scrambled ones now */
static void start_b212(orig_t *o)
{
    v22m_init(&o->v22, V22_CALLER, 0, o->cfg.level, o->cfg.getbit, o->cfg.putbit, o->cfg.user);
    v22tx_set_mode(&o->v22.tx, V22_TX_SB1_1200);
    o->v22.state = V22S_TRAIN;
    o->via_bell = 1;
    o->cd_run = 0;
    o->phase = ORIG_V22; o->t = 0;
}

/* the Bell answer tone has been heard: 212A if offered (and not tried), else 103 */
static void bell_on_tone(orig_t *o)
{
    if ((o->cfg.modes & ORIG_MODE_BELL212A) && !o->b212_failed) start_b212(o);
    else start_b103(o);
}

#define BELL_MODES (ORIG_MODE_BELL103 | ORIG_MODE_BELL212A)
/* something V.8 can offer (CM): a V.8 modulation mode, or PCM */
static int v8_offer(const orig_t *o) { return (o->cfg.modes & (0xFFFFu | ORIG_MODE_V90)) != 0; }

/* only Bell offered, and a 2100 Hz answer tone heard: wait for a Bell answerer */
static void start_bell_wait(orig_t *o)
{
    bell_rx_init(o);
    tonedet_init(&o->t2100, BELL_2100);
    o->bell_run = 0;
    o->phase = ORIG_BELL; o->t = 0;
}

void orig_init(orig_t *o, const orig_cfg *cfg)
{
    v8orig_cfg vc;
    memset(o, 0, sizeof *o);
    o->cfg = *cfg;
    memset(&vc, 0, sizeof vc);
    vc.amp = (int16_t)(cfg->level * 181 / 128);
    vc.local.call_function = V8_CALLF_DATA;
    vc.local.modes = (uint16_t)cfg->modes;
    vc.local.n_mod_octets = 3;
    /* LAPM offered: the library has V.42 (V.8 6.4) */
    vc.local.has_prot = 1; vc.local.prot = V8_PROT_LAPM;
    if (cfg->modes & ORIG_MODE_V90) {
        /* V.90 9.1.1: the analogue modem, PCM in CM; V.34 is its upstream */
        vc.local.has_pcm = 1; vc.local.pcm = V8_PCM_ANALOG;
        vc.local.modes = (uint16_t)(vc.local.modes | V8_MOD_V34);
        /* V.8 6.3 and 7.3: with the PCM category the PSTN access category
         * "shall also be present": an analogue connection, not cellular
         * (0x0d, as the real V.90 / V.92 callers send it) */
        vc.local.has_access = 1; vc.local.access = 0;
    }
    /* CI says "a V.8 modem calls": not when only Bell is on offer */
    vc.send_ci = cfg->send_ci && (cfg->modes & (0xFFFFu | ORIG_MODE_V90));
    v8orig_init(&o->v8, &vc);
    if (cfg->modes & BELL_MODES) belltone_init(&o->btone, 0);
    o->phase = ORIG_V8;
}

/* Keep the echo of our own channel 1 (980 / 1180 Hz) out of the channel 2
 * receiver (modem/v21h_bpf.h: 1650 / 1850 Hz pass, 980 Hz 56 dB down); in
 * Bell 103 our 1070 / 1270 Hz out of the 2025 / 2225 Hz receiver
 * (modem/bell103h_bpf.h, 57 dB down). */
static int16_t v21_bandpass(orig_t *o, int16_t x)
{
    int32_t acc = 0;
    int k;
    o->bpf[o->bpos] = x;
    for (k = 0; k < V21H_BPF_N; k++) acc += (int32_t)o->bpf[(o->bpos - k) & 127] * o->bpf_taps[k] >> 4;
    o->bpos = (o->bpos + 1) & 127;
    acc >>= 11;
    return (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
}

/* one sample of the channel 2 receiver: the line level, 1 under the squelch */
static int v21_rx(orig_t *o, int16_t rx, int *carrier)
{
    int level = v21demod_tick(&o->fdem, v21_bandpass(o, rx));
    *carrier = o->fdem.energy >= o->cd_thresh;
    return *carrier ? level : 1;
}

static int16_t tick_v21(orig_t *o, int16_t rx)
{
    int carrier, level = v21_rx(o, rx, &carrier), c, done, i;
    int16_t out;
    if (!carrier) {
        if (o->cd_run > 0) o->cd_run = 0;
        if (o->connected && --o->cd_run < -MS(CD_LOSS_MS)) { fail(o, ORIG_END_LOST); return 0; }
    } else {
        if (o->cd_run < 0) o->cd_run = 0;
        if (o->cd_run < MS(V21_CD_ON_MS)) o->cd_run++;
        else if (!o->connected) { o->connected = 1; o->modulation = o->fsk_mod; o->bitrate = 300; }
    }
    c = arx_tick(&o->uart, level);
    if (c >= 0 && o->connected) {            /* hand the character on as start-stop bits */
        o->cfg.putbit(o->cfg.user, 0);
        for (i = 0; i < 8; i++) o->cfg.putbit(o->cfg.user, (c >> i) & 1);
        o->cfg.putbit(o->cfg.user, 1);
    }
    out = v21mod_tick(&o->fmod, o->fbit, &done);
    if (done) o->fbit = o->connected ? (o->cfg.getbit(o->cfg.user) & 1) : 1;
    if (!o->connected && o->t > MS(V21_WAIT_MS)) fail(o, ORIG_END_NO_CARRIER);
    return out;
}

int16_t orig_tick(orig_t *o, int16_t rx)
{
    int16_t tx = 0;
    o->t++;
    switch (o->phase) {
    case ORIG_V8:
        /* Bell offered: 2225 Hz before a 2100 Hz tone was told is a pre-V.8
         * Bell answerer */
        if ((o->cfg.modes & BELL_MODES) && o->v8.state == V8O_LISTEN && belltone_tick(&o->btone, rx)) {
            bell_on_tone(o);
            break;
        }
        if (v8orig_tick(&o->v8, rx, &tx) != V8O_DONE) {
            /* ANSam, but nothing to offer in CM: no V.8 (orig.h, 3.) */
            if (o->v8.state == V8O_TE && !v8_offer(o)) { start_bell_wait(o); tx = 0; }
            break;
        }
        switch (o->v8.result) {
        case V8O_RES_ANS:
            if (!v8_offer(o)) { start_bell_wait(o); break; }
            /* a pre-V.8 answerer: listen for which modulation it starts */
            if (o->cfg.modes & V8_MOD_V22) v22m_init(&o->v22, V22_CALLER, 1, o->cfg.level, o->cfg.getbit, o->cfg.putbit, o->cfg.user);
            v21_rx_init(o);
            o->mark_run = 0;
            if (o->cfg.modes & V8_MOD_V32) {
                /* Annex A: the V.32bis caller listens for AC from now on;
                 * what it sends goes out once AA is due */
                start_v32(o);
                o->aa = 0; o->t_aa = -1; o->ans_rest = -1; o->t_quiet = -1; o->tc = -1;
                o->at_on = o->at_off = 0;
            }
            o->phase = ORIG_LEGACY; o->t = 0;
            break;
        case V8O_RES_OK:
            o->used_v8 = 1;
            if (o->v8.selected == V8_SEL_V90 && (o->cfg.modes & ORIG_MODE_V90)) start_v90(o);
            else if (o->v8.selected == 1 && (o->cfg.modes & V8_MOD_V34)) start_v34(o);
            else if (o->v8.selected == 3 && (o->cfg.modes & V8_MOD_V32)) {
                start_v32(o);
                o->aa = 1;
                o->phase = ORIG_V32; o->t = 0;
            }
            else if (o->v8.selected == 4 && (o->cfg.modes & V8_MOD_V22)) start_v22(o);
            else if (o->v8.selected == 12 && (o->cfg.modes & V8_MOD_V21)) start_v21(o);
            else fail(o, ORIG_END_NO_COMMON);      /* a modulation not built yet */
            break;
        case V8O_RES_NO_TONE: fail(o, ORIG_END_NO_TONE); break;
        case V8O_RES_NO_JM: fail(o, ORIG_END_NO_JM); break;
        default: fail(o, ORIG_END_NO_COMMON); break;
        }
        break;

    case ORIG_LEGACY: {
        /* V.22bis: its caller is silent until the answerer's unscrambled
         * binary 1 (6.3.1.1), so it runs as it is; V.21: a steady mark on
         * channel 2 while there is no V.22bis carrier. With V.32bis: Annex A
         * (orig.h) */
        int carrier, level = v21_rx(o, rx, &carrier), v32 = (o->cfg.modes & V8_MOD_V32) != 0;
        if ((o->cfg.modes & BELL_MODES) && belltone_tick(&o->btone, rx)) { bell_on_tone(o); break; }
        if (v32) {
            int16_t t32 = v32_tick(&o->v32, rx);
            annex_a(o, rx);
            /* A.2.1.1, A.2.1.3: AC (64 T of it), or already its reversal */
            if (o->v32.state != V32S_FAILED && (o->v32.rx.ac_run >= 2 * 64 || o->v32.state != V32S_C_AA)) {
                o->aa = 1;
                o->phase = ORIG_V32; o->t = 0;
                tx = t32;
                break;
            }
            if (o->aa) tx = t32;
            if (o->tc >= 0 && o->tc < MS(60000)) o->tc++;
        }
        if (o->cfg.modes & V8_MOD_V22) {
            /* A.2.1.2: during Tc the V.22bis caller waits; then it hears
             * the unscrambled binary 1 for 155 ms once more */
            if (v32 && o->tc == MS(TC_MS - 155)) v22m_init(&o->v22, V22_CALLER, 1, o->cfg.level, o->cfg.getbit, o->cfg.putbit, o->cfg.user);
            if (!v32 || o->tc < 0 || o->tc >= MS(TC_MS - 155)) tx += v22m_tick(&o->v22, rx);
            /* it is V.22bis once its caller has heard 155 ms of the
             * unscrambled binary 1 and left V22S_WAIT (its carrier flag
             * alone also rises on a V.21 channel 2 mark) */
            if (o->v22.state != V22S_WAIT) {
                if (v32 && o->tc < 0 && !(o->t_aa >= 0 && (o->ans_rest < 0 || o->ans_rest > MS(ANS_LONG_MS)))) {
                    /* A.2.1.3 with ANS no longer than 800 ms from the
                     * start of AA, or no AA at all: A.2.1.2, Tc (the
                     * V.22bis caller waits again, held) */
                    o->tc = 0;
                    v22m_init(&o->v22, V22_CALLER, 1, o->cfg.level, o->cfg.getbit, o->cfg.putbit, o->cfg.user);
                } else {
                    /* AA stops (the V.32bis caller is left), 456 ms of
                     * silence, S1 (v22m) */
                    o->cd_run = 0;
                    o->phase = ORIG_V22;                              /* the start-up goes on */
                    break;
                }
            }
        }
        if (carrier && level == 1 && !(v32 && o->aa)) o->mark_run++; else o->mark_run = 0;
        if ((o->cfg.modes & V8_MOD_V21) && o->mark_run >= MS(V21_MARK_MS)) { start_v21(o); break; }
        if (o->t > MS(LEGACY_MS)) fail(o, ORIG_END_NO_CARRIER);
        break;
    }

    case ORIG_V22:
        tx = v22m_tick(&o->v22, rx);
        if (!o->connected) {
            if (o->v22.tx_ready && o->v22.rx_ready) {
                o->connected = 1; o->bitrate = o->v22.bitrate;
                o->modulation = o->bitrate == 2400 ? ORIG_MOD_V22BIS : o->via_bell ? ORIG_MOD_BELL212A : ORIG_MOD_V22;
                o->cd_run = 0;
            } else if (o->via_bell && (o->cfg.modes & ORIG_MODE_BELL103) && o->v22.state == V22S_TRAIN
                       && o->t > MS(B212_WAIT_MS)) {
                /* no 1200 bit/s answer to our scrambled ones: a Bell 103 answerer */
                o->b212_failed = 1;
                start_b103(o);
                tx = 0;
            } else if (o->t > MS(V22_TOTAL_MS) || o->v22.state == V22S_FAILED) {
                fail(o, ORIG_END_NO_CARRIER);
            }
        } else {
            if (o->v22.state == V22S_FAILED) fail(o, ORIG_END_LOST);     /* a retrain (6.4) that never completed */
            else if (o->v22.rx.carrier) o->cd_run = 0;
            else if (++o->cd_run > MS(CD_LOSS_MS)) fail(o, ORIG_END_LOST);
        }
        break;

    case ORIG_V21:
        tx = tick_v21(o, rx);
        break;

    case ORIG_V32:
        tx = v32_tick(&o->v32, rx);
        if (o->v32.state == V32S_FAILED) { fail(o, v32_end(o)); tx = 0; break; }   /* carrier loss included */
        if (o->v32.connected) {
            o->connected = 1; o->modulation = ORIG_MOD_V32BIS;
            o->bitrate = o->v32.bitrate;          /* may change at a retrain */
        } else if (o->t > MS(V32_TOTAL_MS)) fail(o, ORIG_END_NO_CARRIER);
        break;

    case ORIG_V34:
        tx = v34orig_tick(&o->v34, rx);
        if (o->v34.state == V34O_ENDED) { fail(o, v34_end(o)); tx = 0; break; }   /* carrier loss included */
        if (o->v34.state == V34O_DATA) {
            /* also after a rate renegotiation or a retrain */
            o->connected = 1; o->modulation = ORIG_MOD_V34;
            o->bitrate = v34orig_tx_rate(&o->v34); o->rx_bitrate = v34orig_rx_rate(&o->v34);
        } else if (!o->connected && o->t > MS(V34_TOTAL_MS)) fail(o, ORIG_END_NO_CARRIER);
        break;

    case ORIG_V90:
        tx = v90a_tick(&o->v90, rx);
        if (o->v90.state == V90A_ENDED) { fail(o, v90_end(o)); tx = 0; break; }
        if (o->v90.state == V90A_DATA) {
            /* bitrate: what we send (V.34 or PCM upstream), rx_bitrate: the PCM downstream */
            o->connected = 1; o->modulation = o->v90.pcm ? ORIG_MOD_V92 : ORIG_MOD_V90;
            o->bitrate = v90a_tx_rate(&o->v90); o->rx_bitrate = v90a_rx_rate(&o->v90);
        } else if (!o->connected && o->t > MS(V90_TOTAL_MS)) fail(o, ORIG_END_NO_CARRIER);
        break;

    case ORIG_BELL: {
        /* orig.h, 3.: silent; the Bell answer tone, or (Bell 103) a carrier
         * in its answer channel that is not the 2100 Hz tone */
        int carrier, tone = belltone_tick(&o->btone, rx), b212 = (o->cfg.modes & ORIG_MODE_BELL212A) && !o->b212_failed;
        (void)v21_rx(o, rx, &carrier);
        (void)tonedet_tick(&o->t2100, rx);
        if (carrier && o->t2100.blocks == 0) { if (o->bell_run < MS(60000)) o->bell_run++; }
        else o->bell_run = 0;
        if (tone && (b212 || (o->cfg.modes & ORIG_MODE_BELL103))) { bell_on_tone(o); break; }
        if ((o->cfg.modes & ORIG_MODE_BELL103) && o->bell_run >= MS(b212 ? BELL103_LATE_MS : BELL103_CD_MS)) {
            start_b103(o);
            break;
        }
        if (o->t > MS(BELL_WAIT_MS)) fail(o, ORIG_END_NO_CARRIER);
        break;
    }

    case ORIG_FAILED:
        break;
    }
    return tx;
}

int orig_moving(const orig_t *o)
{
    if (!o->connected) return 0;
    if (o->phase == ORIG_V22) return !o->v22.retraining && o->v22.tx_ready && o->v22.rx_ready;
    if (o->phase == ORIG_V32) return !o->v32.retraining && o->v32.tx_ready && o->v32.rx_ready;
    if (o->phase == ORIG_V34) return o->v34.state == V34O_DATA;
    if (o->phase == ORIG_V90) return o->v90.state == V90A_DATA;
    return 1;
}

int orig_retraining(const orig_t *o)
{
    if (o->phase == ORIG_V22) return o->v22.retraining != 0;
    if (o->phase == ORIG_V32) return o->v32.retraining != 0;
    if (o->phase == ORIG_V34) return o->connected && o->v34.state != V34O_DATA && o->v34.state != V34O_RENEG;
    /* V.90, V.92: a renegotiation is not one */
    if (o->phase == ORIG_V90) return o->connected && o->v90.state != V90A_DATA && o->v90.state != V90A_RENEG;
    return 0;
}

int orig_retrains(const orig_t *o)
{
    if (o->phase == ORIG_V22) return o->v22.retrains;
    if (o->phase == ORIG_V32) return o->v32.retrains;
    if (o->phase == ORIG_V34 || (o->phase == ORIG_FAILED && o->modulation == ORIG_MOD_V34)) return o->v34.retrains;
    if (o->phase == ORIG_V90 || (o->phase == ORIG_FAILED && (o->modulation == ORIG_MOD_V90 || o->modulation == ORIG_MOD_V92)))
        return o->v90.retrains;
    return 0;
}

int orig_snr_db10(const orig_t *o)
{
    /* V.32bis: mean power of the points of each rate, signal space units
     * squared (v32/test/test_code) */
    static const int v32_pw[V32_NRATES] = { 40, 40, 40, 42, 41 };
    int v;
    if (!o->connected) return -1;
    if (o->phase == ORIG_V90) return v90a_snr_db10(&o->v90);     /* of the PCM downstream */
    if (o->phase == ORIG_V34) {
        /* as answer_snr_db10: |e|^2 / |d|^2 in Q16; in data mode only */
        if (o->v34.state != V34O_DATA) return -1;
        v = probe_db10(65536, o->v34.dx.rx.mse > 0 ? o->v34.dx.rx.mse : 1);
        return v < 0 ? 0 : v > 600 ? 600 : v;
    }
    if (o->phase == ORIG_V32) {
        if (o->v32.rate < 0 || o->v32.retraining) return -1;
        /* 4096 = one unit squared, as v32rx_t.mse */
        v = probe_db10((int64_t)v32_pw[o->v32.rate] * 4096, o->v32.rx.mse > 0 ? o->v32.rx.mse : 1);
        return v < 0 ? 0 : v > 600 ? 600 : v;
    }
    if (o->phase != ORIG_V22) return -1;
    /* as answer_snr_db10: 4096 = one unit squared, the points have a mean
     * power of 10 */
    v = probe_db10(10 * 4096, o->v22.rx.mse > 0 ? o->v22.rx.mse : 1);
    return v < 0 ? 0 : v > 600 ? 600 : v;
}

int orig_retrain(orig_t *o)
{
    if (o->phase == ORIG_V34) return o->connected ? v34orig_retrain(&o->v34) : -1;
    if (o->phase == ORIG_V90) return o->connected ? v90a_retrain(&o->v90) : -1;
    if (o->phase == ORIG_V32) return o->connected ? v32_retrain(&o->v32) : -1;
    if (o->phase != ORIG_V22 || !o->connected || o->v22.bitrate != 2400) return -1;
    v22m_retrain(&o->v22);
    return 0;
}

int orig_rtd(const orig_t *o)
{
    if (o->phase == ORIG_V34) return o->connected ? o->v34.p2.rtd : -1;
    if (o->phase == ORIG_V90) return o->connected ? o->v90.p2.rtd : -1;     /* RTDEa */
    if (o->phase != ORIG_V32 || !o->connected || o->v32.nt <= 0) return -1;
    return o->v32.nt - 64 * 10 / 3;
}

int orig_renegotiate(orig_t *o, int max_rx_bps)
{
    if (o->phase == ORIG_V90) return o->connected ? v90a_renegotiate(&o->v90, max_rx_bps) : -1;
    return o->phase == ORIG_V34 && o->connected ? v34orig_renegotiate(&o->v34, max_rx_bps) : -1;
}

int orig_cleardown(orig_t *o)
{
    if (o->phase == ORIG_V90) return o->connected ? v90a_cleardown(&o->v90) : -1;
    return o->phase == ORIG_V34 && o->connected ? v34orig_cleardown(&o->v34) : -1;
}

