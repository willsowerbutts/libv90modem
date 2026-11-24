/* v90m-replay: play a recorded call into the answering modem and report what
 * it made of it, phase by phase. The tool to run on a recording that comes
 * with a bug report (README: "Reporting a bug").
 *
 *   v90m-replay [-a] [-R] [-9] [-x] [-k] [-B] [-M] [-F] [-u BPS] [-d BPS] [-l DBM0 | -r RMS] [-e SAMPLES] [-P CODE] [-s SAMPLE] [-b FILE] RECORDING
 *
 *   -a          A-law line (mu-law otherwise)
 *   -R          retrain_on_step
 *   -9          V.92 offered (PCM upstream up to 48000 bit/s unless -u)
 *   -x          x2 offered
 *   -k          K56flex offered: V.8bis comes first, and what the recording
 *               holds is lined up with our answer tone after it (V.8bis
 *               then hears silence and gives up)
 *   -B          Bell 103 and Bell 212A offered too (V90M_MOD_BELL103,
 *               V90M_MOD_BELL212A), as a host may offer them (nsdiald's
 *               sidecar lists them as bell103, bell212): without them a
 *               caller without V.8 hears no 2225 Hz from us after 4 s
 *   -M          in data mode transmit mark only (binary 1, the idle line)
 *               in place of what the V.42 endpoint sends: "our transmit
 *               differs in 0" then proves the recording modem sent nothing
 *               but mark
 *   -F          print the caller's V.42 frames: the HDLC frames in the bits
 *               the V.42 endpoint receives, one line each with the time,
 *               length, address, control and frame type (SABME, UA, I with
 *               N(S) / N(R), RR, XID, ...), and whether its FCS (16 or
 *               32 bits) held; a count of them at the end
 *   -u, -d BPS  upstream / downstream rate ceilings (33600 or 48000; 56000)
 *   -l DBM0     transmit level (default -12); a decimal such as -14.4 is
 *               accepted, to 0.01 dB (the library's level is an rms value,
 *               so any step is possible; whole numbers give the same level
 *               as ever)
 *   -r RMS      transmit level as the rms itself, 1..16384: the level a
 *               recording's sidecar gives (nsdiald's tx_level_rms), which no
 *               -l may reach exactly. Of -l and -r the last one given holds
 *   -e SAMPLES  the echo delay, as the recording modem was told (default: measured)
 *   -P CODE     INFO0d maximum power code, 1..31 (default 23)
 *   -s SAMPLE   start the modem at this sample of the recording (0-based),
 *               with no search for the answer tone and no silence assumed
 *               before it, for a recording that begins at the answer tone.
 *               Times in the report are the recording's, as without -s.
 *               With -k, the alignment
 *               after V.8bis is not made: the modem starts cold at SAMPLE.
 * The modem must be set up as the recording modem was: what it hears holds
 * the echo of what that modem sent. "our transmit differs from the
 * recording in 0" at the end says it was.
 *
 * RECORDING: per 10 ms block 80 codewords heard then 80 sent; or the octets
 * "V90DLIN1" and then per block 80 little-endian 16-bit linear samples heard
 * and 80 codewords sent (a codec with linear receive).
 *
 * Built from the sources with the modem's trace hooks in (-DP34D_TRACE);
 * the lines it prints come from tools/trace.c, shared with v90d's server
 * replay. V92_TRACE=FILE in the environment: a line per sample of the V.92
 * engine. bits.txt: the bits the V.90 / V.34 receiver decided. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "v90modem_priv.h"
#include "v90_pcm.h"
#include "trace.h"

typedef struct { const uint8_t *d; long n, bs, txo; int lin; } rec_t;
static uint8_t rec_sent(const rec_t *r, long i) { return r->d[(i / 80) * r->bs + r->txo + i % 80]; }
static uint8_t rec_heard(const rec_t *r, long i) { return r->d[(i / 80) * r->bs + i % 80]; }
static int16_t rec_heard_lin(const rec_t *r, long i)
{
    const uint8_t *q = r->d + (i / 80) * r->bs + 2 * (i % 80);
    return (int16_t)(uint16_t)(q[0] | (q[1] << 8));
}
static int silent(int alaw, uint8_t c) { return alaw ? (c & 0x7F) == 0x55 : (c & 0x7F) == 0x7F; }

/* the answer tone: the first sound sent after 200 ms of silence, more than
 * a second into the recording */
static long recorded_answer(const rec_t *r, int alaw)
{
    long i, run = 0;
    for (i = 0; i < r->n; i++) {
        if (silent(alaw, rec_sent(r, i))) run++;
        else { if (run >= 1590 && run <= 1612 && i > 8000) return i; run = 0; }
    }
    return -1;
}

/* the transmit level in hundredths of dBm0 (at most 0) as the library's rms
 * value: whole dB by the steps this tool always used, the rest by steps of
 * 0.01 dB (0.99884937 in Q30, from 10^(-0.01/20)), the value held to 16
 * fractional bits meanwhile */
static int level_rms(int hund)
{
    int32_t v = 15770;
    int dbm0, f;
    if (hund > 0) hund = 0;
    dbm0 = hund / 100;                          /* 0 or negative */
    f = hund % 100;
    for (; dbm0 < 0; dbm0++) v = (v * 29205 + 16384) >> 15;
    if (f < 0) {
        int64_t w = (int64_t)v * 65536;
        for (; f < 0; f++) w = (w * 1072506344 + ((int64_t)1 << 29)) >> 30;
        v = (int32_t)((w + 32768) >> 16);
    }
    return (int)v;
}

/* [+-]digits[.d[d]] -> hundredths, without strtod or atof (locale); the
 * integer part up to 3 digits */
static int parse_level(const char *s, int *hund)
{
    int neg = 0, ip = 0, n = 0, fp = 0, fd = 0;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    for (; *s >= '0' && *s <= '9'; s++, n++) ip = ip * 10 + (*s - '0');
    if (n == 0 || n > 3) return -1;
    if (*s == '.') {
        for (s++; *s >= '0' && *s <= '9'; s++, fd++) fp = fp * 10 + (*s - '0');
        if (fd == 0 || fd > 2) return -1;
        if (fd == 1) fp *= 10;
    }
    if (*s) return -1;
    *hund = (ip * 100 + fp) * (neg ? -1 : 1);
    return 0;
}

/* a plain count: digits only, at most 9 */
static int parse_count(const char *s, long *v)
{
    int n = 0;
    long x = 0;
    for (; *s >= '0' && *s <= '9'; s++, n++) x = x * 10 + (*s - '0');
    if (n == 0 || n > 9 || *s) return -1;
    *v = x;
    return 0;
}

/* the data side: V.42, answering, from the moment the modem connects */
static v90m_ec_t *ec;
static int ec_on;
static long bits_rx, bits_tx;
static int opt_mark;     /* -M: mark only in data mode */
static int get(void *u) { (void)u; bits_tx++; return ec_on && !opt_mark ? v90m_ec_tx_bit(ec) : 1; }
static FILE *rxbits;     /* RXBITS=FILE in the environment: every bit received in data mode */

/* -F: the caller's V.42 frames, from the bits put() is given */
static int opt_frames;
static long now;         /* the sample being replayed, for the frame lines */
static v42hdlc_rx_t fr_rx;
static unsigned long fr_good, fr_bad;

/* the frame type from its control field (V.42 8.2, modulo 128) */
static void frame_type(const uint8_t *f, int len, char *out, size_t n)
{
    static const struct { uint8_t c; const char *name; } un[] = {
        { 0x6F, "SABME" }, { 0x63, "UA" }, { 0x0F, "DM" }, { 0x43, "DISC" },
        { 0x87, "FRMR" }, { 0xAF, "XID" }, { 0xE3, "TEST" }, { 0x03, "UI" } };
    static const char *sn[4] = { "RR", "RNR", "REJ", "SREJ" };
    uint8_t c = f[1];
    size_t k;
    if (!(c & 1)) {
        if (len >= 3) snprintf(out, n, "I N(S) %d N(R) %d%s", c >> 1, f[2] >> 1, f[2] & 1 ? " P" : "");
        else snprintf(out, n, "I (short)");
    } else if ((c & 3) == 1) {
        if (len >= 3) snprintf(out, n, "%s N(R) %d%s", sn[(c >> 2) & 3], f[2] >> 1, f[2] & 1 ? " P/F" : "");
        else snprintf(out, n, "%s (short)", sn[(c >> 2) & 3]);
    } else {
        for (k = 0; k < sizeof un / sizeof un[0]; k++)
            if ((c & ~0x10) == un[k].c) { snprintf(out, n, "%s%s", un[k].name, c & 0x10 ? " P/F" : ""); return; }
        snprintf(out, n, "U 0x%02X", c);
    }
}

static void frame_bit(int b)
{
    char type[48];
    int len = v42hdlc_rx_bit(&fr_rx, b);
    if (len > 0) {
        fr_good++;
        frame_type(fr_rx.buf, len, type, sizeof type);
        printf("%7.3f s    V.42 frame in: %d octets, address 0x%02X (C/R %d), control 0x%02X, %s, FCS-%d ok\n", now / 8000.0, len,
               fr_rx.buf[0], (fr_rx.buf[0] >> 1) & 1, fr_rx.buf[1], type, fr_rx.last_fcs32 ? 32 : 16);
    } else if (len == V42HDLC_RX_BAD_FCS) {
        /* the receiver keeps such a frame's octets in buf, not its length */
        fr_bad++;
        printf("%7.3f s    V.42 frame in: address 0x%02X, control 0x%02X, FCS bad\n", now / 8000.0, fr_rx.buf[0], fr_rx.buf[1]);
    }
}

static void put(void *u, int b)
{
    (void)u; bits_rx++;
    if (rxbits) fputc('0' + b, rxbits);
    if (opt_frames) frame_bit(b);
    if (ec_on) v90m_ec_rx_bit(ec, b);
}
static int rp_ones(void *u) { (void)u; return 1; }
static void rp_drop(void *u, int bit) { (void)u; (void)bit; }

/* TXBITS=FILE in the environment, with -x: the box's own downstream data
 * bits, decoded from the codewords it really sent (the replay's x2tx keeps
 * the frame grid; its map, before each frame, the levels) */
static FILE *txbits; static int txown;
static x2map_t tx_dec; static int tx_have, tx_k, tx_lev, tx_seg = -1; static uint8_t tx_oct[X2MAP_N]; static v90_scr_t tx_dscr;
static long tx_frames, tx_bad, tx_nbits;
static void x2_down(const x2tx_t *t, uint8_t sent)
{
    if (t->seg >= X2TX_SYNC && t->fpos == 1) {
        /* a frame begins: the decoder's own differential sign, from the
         * map's (re)starts: the first sync frame, and after a marker */
        if ((t->seg == X2TX_SYNC && tx_seg != X2TX_SYNC) || (tx_seg == X2TX_MARK && t->seg != X2TX_MARK))
            x2map_init(&tx_dec, 1, 0, 1);
        tx_seg = t->seg;
        tx_k = 0; tx_have = t->seg != X2TX_MARK;
        tx_dec.index = t->map.index; tx_dec.pad_mode = t->map.pad_mode; tx_dec.levels_a = t->map.levels_a;
        tx_lev = t->seg >= X2TX_START && t->seg <= X2TX_DATA && !t->map.levels_a;
    }
    if (tx_have && tx_k < X2MAP_N) {
        tx_oct[tx_k++] = sent;
        if (tx_k == X2MAP_N) {
            uint8_t in[X2MAP_N], bits[64]; int k, j, n;
            for (k = 0; k < X2MAP_N; k++) {
                const uint8_t *rv, *sn; int c = x2map_list(&tx_dec, k, &rv, &sn);
                in[k] = tx_oct[k];
                for (j = 0; j < c; j++) if ((sn[j] | 0x80) == (tx_oct[k] | 0x80)) { in[k] = (uint8_t)((rv[j] & 0x7f) | (tx_oct[k] & 0x80)); break; }
            }
            n = x2map_unframe(&tx_dec, in, bits);
            if (tx_lev) {
                tx_frames++;
                if (n < 0) { tx_bad++; n = x2map_bits(&tx_dec); for (k = 0; k < n; k++) bits[k] = 1; }
                for (k = 0; k < n; k++) fputc('0' + v90_descramble_gpc(&tx_dscr, bits[k]), txbits);
                tx_nbits += n;
            }
            tx_have = 0;
        }
    }
}

static int opt_alaw, opt_step, opt_v92, opt_x2, opt_k56, opt_bell, opt_hund = -1200, opt_up, opt_down = 56000, opt_echo = -1, opt_code;
static long opt_rms;    /* -r, 0 = from opt_hund */

static void configure(v90m_cfg *c, int lin)
{
    v90m_cfg_default(c);
    c->modulations = V90M_MOD_V21 | V90M_MOD_V22 | V90M_MOD_V32BIS | V90M_MOD_V34 | V90M_MOD_V90 | (opt_v92 ? V90M_MOD_V92 : 0)
                     | (opt_x2 ? V90M_MOD_X2 : 0) | (opt_k56 ? V90M_MOD_K56 : 0)
                     | (opt_bell ? V90M_MOD_BELL103 | V90M_MOD_BELL212A : 0);
    c->tx_level = opt_rms ? (int)opt_rms : level_rms(opt_hund);
    c->max_down_bps = opt_down;
    c->max_up_bps = opt_up ? opt_up : opt_v92 ? 48000 : 33600;
    c->alaw = opt_alaw;
    c->codec_linear_rx = lin;
    c->echo_delay = opt_echo;
    c->max_tx_code = opt_code;
    c->retrain_on_step = opt_step;
    c->getbit = get;
    c->putbit = put;
}

int main(int argc, char **argv)
{
    static uint8_t file[(24 << 20) + 8];
    static long dsec[120];
    rec_t r; v90m_cfg c; v90m_ec_cfg ecc; v90m_t *m; answer_t *a;
    long len, ans, k0, i, diff = 0, first = -1;
    int arg, err, lv = -1, lr = -1;
    long start = -1;
    int bad = 0;
    const char *bits_path = "bits.txt";
    FILE *f;

    for (arg = 1; arg < argc && argv[arg][0] == '-' && argv[arg][1]; arg++) {
        const char *o = argv[arg];
        if (!strcmp(o, "-a")) opt_alaw = 1;
        else if (!strcmp(o, "-R")) opt_step = 1;
        else if (!strcmp(o, "-9")) opt_v92 = 1;
        else if (!strcmp(o, "-x")) opt_x2 = 1;
        else if (!strcmp(o, "-k")) opt_k56 = 1;
        else if (!strcmp(o, "-B")) opt_bell = 1;
        else if (!strcmp(o, "-M")) opt_mark = 1;
        else if (!strcmp(o, "-F")) opt_frames = 1;
        else if (arg + 1 < argc && !strcmp(o, "-b")) bits_path = argv[++arg];
        else if (arg + 1 < argc && !strcmp(o, "-u")) opt_up = atoi(argv[++arg]);
        else if (arg + 1 < argc && !strcmp(o, "-d")) opt_down = atoi(argv[++arg]);
        else if (arg + 1 < argc && !strcmp(o, "-l")) { if (parse_level(argv[++arg], &opt_hund)) bad = 1; opt_rms = 0; }
        else if (arg + 1 < argc && !strcmp(o, "-r")) {
            if (parse_count(argv[++arg], &opt_rms) || opt_rms < 1 || opt_rms > V90M_TX_LEVEL_MAX) bad = 1;
        }
        else if (arg + 1 < argc && !strcmp(o, "-s")) { if (parse_count(argv[++arg], &start)) bad = 1; }
        else if (arg + 1 < argc && !strcmp(o, "-e")) opt_echo = atoi(argv[++arg]);
        else if (arg + 1 < argc && !strcmp(o, "-P")) opt_code = atoi(argv[++arg]);
        else break;
    }
    if (bad || arg >= argc || !(f = fopen(argv[arg], "rb"))) {
        fprintf(stderr, "usage: v90m-replay [-a] [-R] [-9] [-x] [-k] [-B] [-M] [-F] [-u BPS] [-d BPS] [-l DBM0 | -r RMS] [-e SAMPLES] [-P CODE] [-s SAMPLE] [-b FILE] RECORDING\n"
                        "  -l: a number with up to two decimals, e.g. -14.4; -r: the transmit rms, 1..16384;\n"
                        "  -s: a sample index, 0 or more; -B: Bell 103 and 212A offered too;\n"
                        "  -M: mark only in data mode; -F: print the caller's V.42 frames\n");
        return 2;
    }
    len = (long)fread(file, 1, sizeof file, f);
    fclose(f);
    memset(&r, 0, sizeof r);
    r.d = file; r.bs = 160; r.txo = 80;
    if (len >= 8 && !memcmp(file, "V90DLIN1", 8)) { r.lin = 1; r.bs = 240; r.txo = 160; r.d = file + 8; len -= 8; }
    r.n = len / r.bs * 80;
    printf("recording: %s receive, %ld samples\n", r.lin ? "16-bit linear" : "companded", r.n);
    trace_bits_path = bits_path;
    if (getenv("RXBITS")) rxbits = fopen(getenv("RXBITS"), "w");
    if (getenv("TXBITS")) txbits = fopen(getenv("TXBITS"), "w");
    txown = getenv("TXOWN") != NULL;            /* TXBITS from our own transmit (checks the decoder) */
    v90_scr_init(&tx_dscr);
    v42hdlc_rx_init(&fr_rx);
    v42hdlc_rx_set_fcs(&fr_rx, V42HDLC_FCS_EITHER);

    configure(&c, r.lin);
    if (!(m = v90m_create(&c, &err))) { fprintf(stderr, "v90m_create: %d\n", err); return 1; }
    a = v90m__answer(m);
    if (start >= 0) {
        if (start >= r.n) { fprintf(stderr, "-s %ld: the recording has %ld samples\n", start, r.n); return 2; }
        k0 = 0; ans = start;
        printf("the modem starts at sample %ld of the recording (%.3f s); times are the recording's\n", start, start / 8000.0);
    } else {
    /* where this modem starts its answer tone: run it on silence until it
     * sends its first sound, which is what recorded_answer() finds (with
     * K56flex offered: the first after V.8bis, which hears nothing) */
    for (k0 = 0; k0 < 8000 * 5; k0++) {
        uint8_t s = opt_alaw ? 0xD5 : 0xFF, t;
        v90m_process_g711(m, &s, &t, 1);
        if (!silent(opt_alaw, t) && a->phase != ANS_V8BIS) break;
    }
    ans = recorded_answer(&r, opt_alaw);
    if (ans < 0) { fprintf(stderr, "no answer tone in the recording\n"); return 1; }
    printf("answer tone at %.3f s in the recording; the modem starts %.3f s before it\n", ans / 8000.0, k0 / 8000.0);
    }

    v90m_ec_cfg_default(&ecc, 0);
    ec = v90m_ec_create(&ecc, &err);
    v90m_reset(m, &c);
    trace_reset();
    tr_on = 1;
    for (i = start >= 0 ? start : ans - k0; i < r.n; i++) {
        uint8_t want = 0xFF, tx; v90m_state st;
        now = i;
        if (i < 0) { uint8_t s = opt_alaw ? 0xD5 : 0xFF; v90m_process_g711(m, &s, &tx, 1); continue; }
        want = rec_sent(&r, i);
        /* the echo cancellers take what was really sent as their reference
         * once data moves (V.34, V.92): this modem's data are not the
         * recording's */
        rec_tx = (int16_t)v90_octet_linear(opt_alaw, want);
        rec_tx_on = (a->phase == ANS_V34 && a->connected)
                    || (a->phase == ANS_X2 && (a->u.x2.state == X2D_START || a->u.x2.state == X2D_DATA || a->u.x2.state == X2D_RENEG));
        if (rec_tx92 && (a->phase == ANS_V92 || a->phase == ANS_P3X)) { a->u.v92.cfg.getbit = rp_ones; a->u.v92.cfg.putbit = rp_drop; }
        if (r.lin) { int16_t x = rec_heard_lin(&r, i); v90m_process_lin_rx(m, &x, &tx, 1); }
        else { uint8_t x = rec_heard(&r, i); v90m_process_g711(m, &x, &tx, 1); }
        if (tx != want) { if (first < 0) first = i; diff++; dsec[(i / 8000) % 120]++; }
        if (txbits && a->phase == ANS_X2) x2_down(&a->u.x2.tx, txown ? tx : want);

        trace_a(a, i);
        v90m_get_state(m, &st);
        if (st.connected && !ec_on) {
            v90m_ec_reset(ec, &ecc);
            ec_on = 1;
        }
        if (ec_on && i % 8 == 0) v90m_ec_tick(ec, 1);
        {
            int s = ec_on ? v90m_ec_state(ec) : -1;
            if (s != lv || a->retrains != lr) {
                lv = s; lr = a->retrains;
                printf("%7.3f s    V.42 state %d, retrains %d, p34 state %d\n", i / 8000.0, lv, lr, (int)a->p34.state);
            }
            if (ec_on && a->phase == ANS_X2 && i % 8000 == 0) {
                v90m_ec_status es; v90m_ec_get_status(ec, &es);
                printf("%7.3f s      x2 data: state %d, index %d pad %d, %d down %d up, receiver mse %.1f dB, LAPM rx %lu bad %lu, rx I %lu, bits %ld down %ld, timing %+.0f ppm\n", i / 8000.0,
                       (int)a->u.x2.state, a->u.x2.index, a->u.x2.pad_mode, a->u.x2.down_rate, a->u.x2.up_rate,
                       10 * log10(a->u.x2.up.rx.mse / 65536.0 + 1e-9), es.rx_frames, es.rx_frames_bad, es.rx_i, bits_rx, tx_nbits,
                       (double)a->u.x2.up.rx.t_integ / (1 << 24) / (8000.0 / a->u.x2.sym_rate) * 1e6);
            }
            if (ec_on && a->phase == ANS_V90 && i % 16000 == 0) {
                v90m_ec_status es; v90m_ec_get_status(ec, &es);
                printf("%7.3f s      data: receiver mse %.1f dB, LAPM rx bad %lu, rx I %lu, tx I %lu, p34 state %d\n", i / 8000.0,
                       10 * log10(a->p34.up.rx.mse / 65536.0 + 1e-9), es.rx_frames_bad, es.rx_i, es.tx_i, (int)a->p34.state);
            }
        }
        {
            static int lm = -1, lrt = -1, lc = -1;
            if (st.moving != lm || st.retraining != lrt || st.connected != lc) {
                printf("%7.3f s    modem connected %d moving %d retraining %d\n", i / 8000.0, st.connected, st.moving, st.retraining);
                lm = st.moving; lrt = st.retraining; lc = st.connected;
            }
        }
        trace_b(a, i);
        if (st.end != V90M_END_NONE) { printf("%7.3f s  the modem stopped (%d)\n", i / 8000.0, (int)st.end); break; }
    }
    {
        v90m_stats sx; v90m_get_stats(m, &sx);
        printf("modulation %d, %d bit/s down, %d up, SNR %.1f dB, round trip %d, echo delay %d, %u retrains, %u renegotiations; %ld bits received\n",
               (int)sx.modulation, sx.tx_bps, sx.rx_bps, sx.snr_db10 / 10.0, sx.rtd, sx.echo_delay, sx.retrains, sx.renegotiations, bits_rx);
    }
    for (i = 0; i < 120; i++) if (dsec[i]) printf("  second %ld: %ld samples differ\n", i, dsec[i]);
    printf("%ld samples; after the answer our transmit differs from the recording in %ld (first at %.3f s)\n", r.n, diff, first / 8000.0);
    if (txbits) printf("downstream frames decoded %ld, not levels %ld\n", tx_frames, tx_bad);
    if (opt_frames) printf("V.42 frames in: %lu with a good FCS, %lu bad\n", fr_good, fr_bad);
    if (rxbits) fclose(rxbits);
    if (txbits) fclose(txbits);
    v90m_ec_destroy(ec);
    v90m_destroy(m);
    return 0;
}
