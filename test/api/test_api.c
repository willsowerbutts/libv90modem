/* The public API (include/v90modem.h), from the outside: the answering modem
 * comes from libv90modem.a as it is installed (only v90m_* visible); the
 * simulated calling modem and V.14 terminal come from the dev archive. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "v90modem.h"
#include "test.h"
/* the simulator's calling modem (dev archive) */
#include "callpeer.h"
#include "x2call.h"
#include "k56call.h"

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p; size_t i;
    for (i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

/* ---- configuration and memory ---- */
static void test_cfg(void)
{
    v90m_cfg c; v90m_t *m; int err = 99;
    static uint8_t buf[1 << 20] __attribute__((aligned(16)));

    /* the version: the library's string, the header's numbers, and the two agree */
    CHECK(strcmp(v90m_version(), "0.5.0") == 0);
    CHECK_EQ_I(V90M_VERSION_MAJOR * 10000 + V90M_VERSION_MINOR * 100 + V90M_VERSION_PATCH, 500);
    {
        char hv[32];
        snprintf(hv, sizeof hv, "%d.%d.%d", V90M_VERSION_MAJOR, V90M_VERSION_MINOR, V90M_VERSION_PATCH);
        CHECK(strcmp(v90m_version(), hv) == 0);
    }

    v90m_cfg_default(&c);
    CHECK_EQ_I(c.role, V90M_ROLE_ANSWER);
    CHECK_EQ_I(c.tx_level, 3000);
    CHECK_EQ_I(c.echo_delay, -1);
    CHECK(c.modulations & V90M_MOD_V90);
    CHECK(!(c.modulations & V90M_MOD_V92));

    c.role = V90M_ROLE_ORIGINATE;               /* the defaults call too (V.90 as the analogue modem) */
    m = v90m_create(&c, &err);
    CHECK(m != NULL); CHECK_EQ_I(err, V90M_OK);
    v90m_destroy(m);
    c.modulations |= V90M_MOD_V92;              /* V.92 calls too (C6), with V.90 */
    m = v90m_create(&c, &err);
    CHECK(m != NULL); CHECK_EQ_I(err, V90M_OK);
    v90m_destroy(m);
    c.modulations = V90M_MOD_V92 | V90M_MOD_V34;  /* ... but not without it: V.92 is V.90's start-up */
    m = v90m_create(&c, &err);
    CHECK(m == NULL); CHECK_EQ_I(err, V90M_ENOTSUP);
    /* the answering role refuses the same: V.92 without V.90, and nothing offered */
    c.role = V90M_ROLE_ANSWER;
    err = 99; m = v90m_create(&c, &err);
    CHECK(m == NULL); CHECK_EQ_I(err, V90M_ENOTSUP);
    c.modulations = 0;
    err = 99; m = v90m_create(&c, &err);
    CHECK(m == NULL); CHECK_EQ_I(err, V90M_EINVAL);
    /* Bell 103 and Bell 212A: both roles, and not in the defaults */
    v90m_cfg_default(&c);
    CHECK(!(c.modulations & (V90M_MOD_BELL103 | V90M_MOD_BELL212A)));
    CHECK_EQ_I(V90M_CONN_BELL103, 8); CHECK_EQ_I(V90M_CONN_BELL212A, 9);   /* appended: the others stay */
    c.modulations |= V90M_MOD_BELL103 | V90M_MOD_BELL212A;
    m = v90m_create(&c, &err);
    CHECK(m != NULL); CHECK_EQ_I(err, V90M_OK);
    v90m_destroy(m);
    c.role = V90M_ROLE_ORIGINATE; c.modulations = V90M_MOD_V21 | V90M_MOD_BELL103;
    m = v90m_create(&c, &err);
    CHECK(m != NULL); CHECK_EQ_I(err, V90M_OK);            /* Bell calls too */
    v90m_destroy(m);
    c.modulations = V90M_MOD_V22 | V90M_MOD_BELL212A;
    m = v90m_create(&c, &err);
    CHECK(m != NULL); CHECK_EQ_I(err, V90M_OK);
    v90m_destroy(m);
    c.modulations = V90M_MOD_BELL103;                       /* Bell alone: no V.8 */
    m = v90m_create(&c, &err);
    CHECK(m != NULL); CHECK_EQ_I(err, V90M_OK);
    v90m_destroy(m);
    c.modulations = V90M_MOD_BELL212A;
    m = v90m_create(&c, &err);
    CHECK(m != NULL); CHECK_EQ_I(err, V90M_OK);
    v90m_destroy(m);
    v90m_cfg_default(&c); c.role = V90M_ROLE_ORIGINATE;     /* not in the calling defaults either */
    CHECK(!(c.modulations & (V90M_MOD_BELL103 | V90M_MOD_BELL212A)));
    /* x2: answering only, with V.34, and not in the defaults; its numbers
     * appended to the enums */
    CHECK_EQ_I(V90M_MOD_X2, 0x0100);
    CHECK_EQ_I(V90M_CONN_X2, 10);
    CHECK_EQ_I(V90M_END_X2_FAILED, V90M_END_RETRAIN_FAILED + 1);
    v90m_cfg_default(&c);
    CHECK(!(c.modulations & V90M_MOD_X2));
    c.modulations = V90M_MOD_X2;                            /* alone: x2's upstream is V.34 */
    err = 99; m = v90m_create(&c, &err);
    CHECK(m == NULL); CHECK_EQ_I(err, V90M_ENOTSUP);
    c.modulations = V90M_MOD_X2 | V90M_MOD_V90 | V90M_MOD_V21;
    err = 99; m = v90m_create(&c, &err);
    CHECK(m == NULL); CHECK_EQ_I(err, V90M_ENOTSUP);
    c.modulations = V90M_MOD_X2 | V90M_MOD_V34;             /* the calling role has no x2 */
    c.role = V90M_ROLE_ORIGINATE;
    err = 99; m = v90m_create(&c, &err);
    CHECK(m == NULL); CHECK_EQ_I(err, V90M_ENOTSUP);
    c.role = V90M_ROLE_ANSWER;
    err = 99; m = v90m_create(&c, &err);
    CHECK(m != NULL); CHECK_EQ_I(err, V90M_OK);
    v90m_destroy(m);
    v90m_cfg_default(&c); c.modulations |= V90M_MOD_X2;
    err = 99; m = v90m_create(&c, &err);
    CHECK(m != NULL); CHECK_EQ_I(err, V90M_OK);
    v90m_destroy(m);
    /* K56flex: answering only, with V.34, not in the defaults; its numbers
     * appended */
    CHECK_EQ_I(V90M_MOD_K56, 0x0200);
    CHECK_EQ_I(V90M_CONN_K56FLEX, V90M_CONN_X2 + 1);
    CHECK_EQ_I(V90M_END_K56_FAILED, V90M_END_X2_FAILED + 1);
    v90m_cfg_default(&c);
    CHECK(!(c.modulations & V90M_MOD_K56));
    c.role = V90M_ROLE_ORIGINATE;
    CHECK(!(c.modulations & V90M_MOD_K56));
    c.role = V90M_ROLE_ANSWER; c.modulations = V90M_MOD_K56;            /* alone: its upstream is V.34 */
    err = 99; m = v90m_create(&c, &err);
    CHECK(m == NULL); CHECK_EQ_I(err, V90M_ENOTSUP);
    c.modulations = V90M_MOD_K56 | V90M_MOD_V90 | V90M_MOD_V21;
    err = 99; m = v90m_create(&c, &err);
    CHECK(m == NULL); CHECK_EQ_I(err, V90M_ENOTSUP);
    c.modulations = V90M_MOD_K56 | V90M_MOD_V34;                        /* the calling role has none */
    c.role = V90M_ROLE_ORIGINATE;
    err = 99; m = v90m_create(&c, &err);
    CHECK(m == NULL); CHECK_EQ_I(err, V90M_ENOTSUP);
    c.role = V90M_ROLE_ANSWER;
    err = 99; m = v90m_create(&c, &err);
    CHECK(m != NULL); CHECK_EQ_I(err, V90M_OK);
    v90m_destroy(m);
    v90m_cfg_default(&c); c.modulations |= V90M_MOD_K56 | V90M_MOD_X2;
    err = 99; m = v90m_create(&c, &err);
    CHECK(m != NULL); CHECK_EQ_I(err, V90M_OK);
    {
        /* in V.8bis (its CRe): nothing to retrain or clear down */
        uint8_t in[800], out[800];
        v90m_state st;
        memset(in, 0xff, sizeof in);
        CHECK_EQ_I(v90m_process_g711(m, in, out, sizeof in), V90M_OK);
        CHECK_EQ_I(v90m_retrain(m), V90M_ESTATE);
        CHECK_EQ_I(v90m_cleardown(m), V90M_ESTATE);
        v90m_get_state(m, &st);
        CHECK_EQ_I(st.connected, 0); CHECK_EQ_I(st.end, V90M_END_NONE);
    }
    v90m_destroy(m);
    c.role = V90M_ROLE_ANSWER; c.modulations = 0x0400;
    m = v90m_create(&c, &err);
    CHECK(m == NULL); CHECK_EQ_I(err, V90M_EINVAL);
    c.role = V90M_ROLE_ANSWER; c.modulations = 0x8000;
    m = v90m_create(&c, &err);
    CHECK(m == NULL); CHECK_EQ_I(err, V90M_EINVAL);
    v90m_cfg_default(&c); c.max_tx_code = 32;
    CHECK(v90m_create(&c, &err) == NULL); CHECK_EQ_I(err, V90M_EINVAL);

    v90m_cfg_default(&c);
    m = v90m_create(&c, &err);
    CHECK(m != NULL); CHECK_EQ_I(err, V90M_OK);
    c.role = V90M_ROLE_ORIGINATE;
    CHECK_EQ_I(v90m_reset(m, &c), V90M_OK);
    c.modulations |= V90M_MOD_V92;
    CHECK_EQ_I(v90m_reset(m, &c), V90M_OK);
    c.modulations = V90M_MOD_V92 | V90M_MOD_V22;
    CHECK_EQ_I(v90m_reset(m, &c), V90M_ENOTSUP);
    v90m_destroy(m);

    /* in memory of the caller's */
    v90m_cfg_default(&c);
    CHECK(v90m_mem_size() <= sizeof buf);
    CHECK(v90m_init(buf, v90m_mem_size() - 1, &c, &err) == NULL); CHECK_EQ_I(err, V90M_ENOMEM);
    CHECK(v90m_init(buf + 1, sizeof buf - 1, &c, &err) == NULL); CHECK_EQ_I(err, V90M_EINVAL);
    m = v90m_init(buf, sizeof buf, &c, &err);
    CHECK(m != NULL); CHECK_EQ_I(err, V90M_OK);
    v90m_destroy(m);                            /* not ours to free: does nothing */

    /* the bounds of the header, and memory full of rubbish */
    CHECK(v90m_mem_size() <= V90M_MEM_SIZE);
    {
        /* pinned: what v90d sizes its buffers by (K56flex's state lives in
         * the answerer's union) */
#if defined(__x86_64__)
        size_t expect = 118824;
#elif defined(__arm__)
        size_t expect = 118512;
#else
        size_t expect = v90m_mem_size();
#endif
        CHECK(v90m_mem_size() == expect);
    }
    CHECK(v90m_ec_mem_size() <= V90M_EC_MEM_SIZE);
    CHECK(v90m_async_mem_size() <= V90M_ASYNC_MEM_SIZE);
    {
        static union { v90m_align a; uint8_t b[V90M_MEM_SIZE]; } junk;
        v90m_state st;
        memset(junk.b, 0xA5, sizeof junk.b);
        m = v90m_init(junk.b, sizeof junk.b, &c, &err);
        CHECK(m != NULL);
        v90m_get_state(m, &st);
        CHECK_EQ_I(st.connected, 0); CHECK_EQ_I(st.modulation, V90M_CONN_NONE); CHECK_EQ_I(st.end, V90M_END_NONE);
    }
}

/* ---- G.711 at the Table 1 scale ---- */
/* ---- the error returns the header documents, each role, without a call ---- */
static void test_errors(void)
{
    static union { v90m_align a; uint8_t b[V90M_MEM_SIZE + 16]; } mem;
    static union { v90m_align a; uint8_t b[V90M_EC_MEM_SIZE + 16]; } ecmem;
    static union { v90m_align a; uint8_t b[V90M_ASYNC_MEM_SIZE + 16]; } amem;
    v90m_cfg c, bad; v90m_t *m; v90m_ec_cfg ec; v90m_ec_t *e; v90m_async_t *a;
    v90m_state st; v90m_stats sx; int err, role, opt, len;
    uint8_t cw[4] = { 0xFF, 0xFF, 0xFF, 0xFF }; int16_t lin[4] = { 0, 0, 0, 0 };

    err = 99; CHECK(v90m_create(NULL, &err) == NULL); CHECK_EQ_I(err, V90M_EINVAL);
    err = 99; v90m_cfg_default(&c); CHECK(v90m_init(NULL, sizeof mem, &c, &err) == NULL); CHECK_EQ_I(err, V90M_EINVAL);
    err = 99; CHECK(v90m_init(mem.b, sizeof mem.b, NULL, &err) == NULL); CHECK_EQ_I(err, V90M_EINVAL);
    for (role = 0; role < 2; role++) {
        v90m_cfg_default(&c); c.role = role ? V90M_ROLE_ORIGINATE : V90M_ROLE_ANSWER;
        /* each value out of range: V90M_EINVAL */
        bad = c; bad.role = (v90m_role)7;  CHECK(v90m_create(&bad, &err) == NULL); CHECK_EQ_I(err, V90M_EINVAL);
        bad = c; bad.tx_level = -1;        CHECK(v90m_create(&bad, &err) == NULL); CHECK_EQ_I(err, V90M_EINVAL);
        bad = c; bad.max_tx_code = -1;     CHECK(v90m_create(&bad, &err) == NULL); CHECK_EQ_I(err, V90M_EINVAL);
        bad = c; bad.max_down_bps = -1;    CHECK(v90m_create(&bad, &err) == NULL); CHECK_EQ_I(err, V90M_EINVAL);
        bad = c; bad.max_up_bps = -1;      CHECK(v90m_create(&bad, &err) == NULL); CHECK_EQ_I(err, V90M_EINVAL);
        bad = c; bad.echo_delay = -2;      CHECK(v90m_create(&bad, &err) == NULL); CHECK_EQ_I(err, V90M_EINVAL);
        bad = c; bad.modulations |= 0x400; CHECK(v90m_create(&bad, &err) == NULL); CHECK_EQ_I(err, V90M_EINVAL);   /* 0x100 is x2, 0x200 K56flex now */

        m = v90m_init(mem.b, sizeof mem.b, &c, &err);
        CHECK(m != NULL); CHECK_EQ_I(err, V90M_OK);
        if (!m) continue;
        /* v90m_reset: the same checks as create, and the modem kept as it was */
        bad = c; bad.tx_level = -1;
        CHECK_EQ_I(v90m_reset(m, &bad), V90M_EINVAL);
        CHECK_EQ_I(v90m_reset(m, NULL), V90M_EINVAL);
        /* the block calls */
        CHECK_EQ_I(v90m_process_g711(NULL, cw, cw, 4), V90M_EINVAL);
        CHECK_EQ_I(v90m_process_g711(m, NULL, cw, 4), V90M_EINVAL);
        CHECK_EQ_I(v90m_process_lin_rx(m, lin, NULL, 4), V90M_EINVAL);
        CHECK_EQ_I(v90m_process_linear(m, NULL, lin, 4), V90M_EINVAL);
        CHECK_EQ_I(v90m_process_linear(m, NULL, NULL, 0), V90M_OK);
        CHECK_EQ_I(v90m_process_g711(m, cw, cw, 4), V90M_OK);
        /* nothing to retrain or clear down before data mode */
        CHECK_EQ_I(v90m_retrain(m), V90M_ESTATE);
        CHECK_EQ_I(v90m_cleardown(m), V90M_ESTATE);
        /* before a connection: the stats say "none" */
        v90m_get_state(m, &st);
        CHECK_EQ_I(st.connected, 0); CHECK_EQ_I(st.odp_bypass, 0); CHECK_EQ_I(st.end, V90M_END_NONE);
        v90m_get_stats(m, &sx);
        CHECK_EQ_I(sx.snr_db10, -1); CHECK_EQ_I(sx.rtd, -1); CHECK_EQ_I(sx.echo_delay, -1);
        CHECK_EQ_I(sx.retrains, 0); CHECK_EQ_I(sx.renegotiations, 0);
        v90m_destroy(m);
    }
    v90m_destroy(NULL);

    /* V.42 */
    v90m_ec_cfg_default(&ec, 1);
    err = 99; CHECK(v90m_ec_create(NULL, &err) == NULL); CHECK_EQ_I(err, V90M_EINVAL);
    err = 99; CHECK(v90m_ec_init(ecmem.b + 1, sizeof ecmem.b - 1, &ec, &err) == NULL); CHECK_EQ_I(err, V90M_EINVAL);
    err = 99; CHECK(v90m_ec_init(ecmem.b, v90m_ec_mem_size() - 1, &ec, &err) == NULL); CHECK_EQ_I(err, V90M_ENOMEM);
    e = v90m_ec_init(ecmem.b, sizeof ecmem.b, &ec, &err);
    CHECK(e != NULL); CHECK_EQ_I(err, V90M_OK);
    if (e) {
        CHECK_EQ_I(v90m_ec_reset(e, NULL), V90M_EINVAL);
        CHECK_EQ_I(v90m_ec_reset(e, &ec), V90M_OK);
        /* no break before error control is up; none received */
        CHECK_EQ_I(v90m_ec_send_break(e, V90M_BRK_DESTRUCTIVE_EXPEDITED, 10), V90M_ESTATE);
        CHECK_EQ_I(v90m_ec_get_break(e, &opt, &len), 0);
        v90m_ec_destroy(e);
    }
    v90m_ec_destroy(NULL);

    /* start-stop */
    err = 99; CHECK(v90m_async_init(NULL, sizeof amem.b, &err) == NULL); CHECK_EQ_I(err, V90M_EINVAL);
    err = 99; CHECK(v90m_async_init(amem.b + 1, sizeof amem.b - 1, &err) == NULL); CHECK_EQ_I(err, V90M_EINVAL);
    err = 99; CHECK(v90m_async_init(amem.b, v90m_async_mem_size() - 1, &err) == NULL); CHECK_EQ_I(err, V90M_ENOMEM);
    a = v90m_async_init(amem.b, sizeof amem.b, &err);
    CHECK(a != NULL); CHECK_EQ_I(err, V90M_OK);
    v90m_async_destroy(a);
    v90m_async_destroy(NULL);
}

static void test_g711(void)
{
    int law, c, bad = 0;
    for (law = 0; law < 2; law++)
        for (c = 0; c < 256; c++) {
            int16_t v = v90m_g711_decode(law, (uint8_t)c);
            uint8_t back = v90m_g711_encode(law, v);
            /* both zero codewords decode to 0 */
            if (back != c && v90m_g711_decode(law, back) != v) bad++;
        }
    CHECK_EQ_I(bad, 0);
    CHECK_EQ_I(v90m_g711_decode(0, 0xFF), 0);
    CHECK(v90m_g711_decode(0, 0x80) > 30000);
    CHECK(v90m_g711_decode(0, 0x00) < -30000);
}

/* ---- a V.22bis call: the library answers, the simulator calls ---- */
typedef struct {
    v90m_t *m; v90m_async_t *a; callpeer_t c;
    int ev[8]; int n_ev;
    uint32_t hash;
} call_t;

static void on_event(void *user, v90m_event ev)
{
    call_t *k = user;
    if (k->n_ev < 8) k->ev[k->n_ev] = (int)ev;
    k->n_ev++;
}

static int async_get(void *u) { call_t *k = u; return v90m_async_getbit(k->a); }
static void async_put(void *u, int b) { call_t *k = u; v90m_async_putbit(k->a, b); }

static void call_init(call_t *k)
{
    v90m_cfg c; int err;
    memset(k, 0, sizeof *k);
    k->a = v90m_async_create(&err);
    v90m_cfg_default(&c);
    c.modulations = V90M_MOD_V22 | V90M_MOD_V21;
    c.getbit = async_get; c.putbit = async_put; c.on_event = on_event; c.user = k;
    k->m = v90m_create(&c, &err);
    callpeer_init(&k->c, 1, V8_MOD_V22 | V8_MOD_V21, 1, 3000);
    k->hash = 2166136261u;
}

/* n samples, the answerer's block size `blk` */
static void call_run(call_t *k, int16_t *line, int n, int blk)
{
    int16_t rx[160], tx[160]; int i, done = 0;
    while (done < n) {
        int b = n - done < blk ? n - done : blk;
        for (i = 0; i < b; i++) rx[i] = callpeer_tick(&k->c, line[done + i]);
        v90m_process_linear(k->m, rx, tx, (size_t)b);
        for (i = 0; i < b; i++) line[done + i] = tx[i];
        k->hash = fnv(k->hash, tx, sizeof tx[0] * (size_t)b);
        done += b;
    }
}

static void test_call(void)
{
    static call_t k1, k2;
    int16_t l1[80], l2[80];
    v90m_state st; v90m_stats sx;
    char got[64]; size_t ng = 0;
    int f, sent = 0;

    memset(l1, 0, sizeof l1); memset(l2, 0, sizeof l2);
    /* two modems in one process, side by side; the second one is run in
     * blocks of 1 sample, the first in blocks of 80: the same call must
     * come out of both */
    call_init(&k1); call_init(&k2);
    for (f = 0; f < 100 * 12; f++) {
        call_run(&k1, l1, 80, 80);
        call_run(&k2, l2, 80, 1);
        if (k1.c.connected && !sent && f > 600) {
            v14_write(&k1.c.dte, (const uint8_t *)"hello", 5);
            sent = 1;
        }
        {
            uint8_t b[16]; size_t r = v90m_async_read(k1.a, b, sizeof b);
            if (r && ng + r < sizeof got) { memcpy(got + ng, b, r); ng += r; }
        }
    }
    got[ng] = 0;
    v90m_get_state(k1.m, &st);
    CHECK_EQ_I(st.connected, 1);
    CHECK_EQ_I(st.modulation, V90M_CONN_V22BIS);
    CHECK_EQ_I(st.tx_bps, 2400); CHECK_EQ_I(st.rx_bps, 2400);
    CHECK_EQ_I(st.used_v8, 1);
    CHECK_EQ_I(st.end, V90M_END_NONE);
    CHECK_EQ_I(st.moving, 1);
    CHECK(strcmp(got, "hello") == 0);
    CHECK(k1.n_ev >= 2);
    CHECK_EQ_I(k1.ev[0], V90M_EV_CONNECTED);
    CHECK_EQ_I(k1.ev[1], V90M_EV_DATA_MOVING);
    v90m_get_stats(k1.m, &sx);
    CHECK_EQ_I(sx.modulation, V90M_CONN_V22BIS);
    CHECK_EQ_I(sx.retrains, 0);
    CHECK(sx.snr_db10 > 200);
    /* the second call: same events, and the same samples on the line up to
     * the moment the first one was handed data */
    v90m_get_state(k2.m, &st);
    CHECK_EQ_I(st.connected, 1);
    CHECK_EQ_I(st.modulation, V90M_CONN_V22BIS);
    CHECK_EQ_I(k2.ev[0], V90M_EV_CONNECTED);
    /* data was only handed to the caller's terminal: the answerers must
     * have sent the same samples */
    CHECK_EQ_I(k2.hash, k1.hash);
    printf("  V.22bis through the API: %d events; the answerers in blocks of 80 and of 1 sent %s samples\n",
           k1.n_ev, k1.hash == k2.hash ? "the same" : "different");

    /* a new call on the same memory */
    {
        v90m_cfg c; v90m_cfg_default(&c);
        c.modulations = V90M_MOD_V22 | V90M_MOD_V21;
        CHECK_EQ_I(v90m_reset(k2.m, &c), V90M_OK);
        v90m_get_state(k2.m, &st);
        CHECK_EQ_I(st.connected, 0); CHECK_EQ_I(st.modulation, V90M_CONN_NONE);
    }
    v90m_destroy(k1.m); v90m_destroy(k2.m);
    v90m_async_destroy(k1.a); v90m_async_destroy(k2.a);
}

/* block size alone: the same call cut into blocks of 1, 7 and 160 */
static void test_blocks(void)
{
    static call_t k[3];
    static const int blk[3] = { 1, 7, 160 };
    int16_t line[3][160];
    int j, f;
    for (j = 0; j < 3; j++) { call_init(&k[j]); memset(line[j], 0, sizeof line[j]); }
    for (f = 0; f < 50 * 8; f++)
        for (j = 0; j < 3; j++) call_run(&k[j], line[j], 160, blk[j]);
    CHECK(k[0].c.connected);
    CHECK_EQ_I(k[1].hash, k[0].hash);
    CHECK_EQ_I(k[2].hash, k[0].hash);
    for (j = 0; j < 3; j++) { v90m_destroy(k[j].m); v90m_async_destroy(k[j].a); }
}

/* ---- V.42 alone: two endpoints wired bit to bit ---- */
static void test_ec(void)
{
    v90m_ec_cfg co, ca; v90m_ec_t *o, *a; v90m_ec_status st;
    static uint8_t mem[1 << 18] __attribute__((aligned(16)));
    uint8_t buf[600], got[600]; size_t ng = 0;
    int i, ms, err;

    v90m_ec_cfg_default(&co, 1);
    v90m_ec_cfg_default(&ca, 0);
    CHECK_EQ_I(co.originator, 1); CHECK_EQ_I(ca.originator, 0);
    o = v90m_ec_create(&co, &err); CHECK(o != NULL);
    CHECK(v90m_ec_mem_size() <= sizeof mem);
    a = v90m_ec_init(mem, sizeof mem, &ca, &err); CHECK(a != NULL);
    for (i = 0; i < (int)sizeof buf; i++) buf[i] = (uint8_t)(i * 7);
    for (ms = 0; ms < 6000; ms++) {
        int b;
        for (b = 0; b < 24; b++) {              /* 24000 bit/s both ways */
            v90m_ec_rx_bit(a, v90m_ec_tx_bit(o));
            v90m_ec_rx_bit(o, v90m_ec_tx_bit(a));
        }
        v90m_ec_tick(o, 1); v90m_ec_tick(a, 1);
        if (ms == 2000) CHECK_EQ_I(v90m_ec_write(o, buf, sizeof buf), sizeof buf);
        ng += v90m_ec_read(a, got + ng, sizeof got - ng);
    }
    CHECK_EQ_I(v90m_ec_state(o), V90M_EC_CONNECTED);
    CHECK_EQ_I(v90m_ec_state(a), V90M_EC_CONNECTED);
    CHECK_EQ_I(ng, sizeof buf);
    CHECK(memcmp(got, buf, sizeof buf) == 0);
    CHECK(v90m_ec_tx_idle(o));
    v90m_ec_get_status(a, &st);
    CHECK_EQ_I(st.state, V90M_EC_CONNECTED);
    CHECK(st.rx_i >= 1);
    CHECK_EQ_I(st.comp_alg, V90M_EC_COMP_V44);   /* both ends offer V.44 by default */
    v90m_ec_release(o);
    for (ms = 0; ms < 3000; ms++) {
        int b;
        for (b = 0; b < 24; b++) { v90m_ec_rx_bit(a, v90m_ec_tx_bit(o)); v90m_ec_rx_bit(o, v90m_ec_tx_bit(a)); }
        v90m_ec_tick(o, 1); v90m_ec_tick(a, 1);
    }
    v90m_ec_get_status(a, &st);
    CHECK_EQ_I(st.state, V90M_EC_DISCONNECTED);
    CHECK_EQ_I(st.reason, V90M_EC_R_PEER_DISC);
    v90m_ec_destroy(o); v90m_ec_destroy(a);
}

/* ---- the calling role: an API caller and an API answerer back to back ---- */
typedef struct { v90m_t *m; v90m_async_t *a; int ev[8]; int n_ev; } end_t;
static void end_event(void *u, v90m_event ev) { end_t *e = u; if (e->n_ev < 8) e->ev[e->n_ev] = (int)ev; e->n_ev++; }
static int end_get(void *u) { return v90m_async_getbit(((end_t *)u)->a); }
static void end_put(void *u, int b) { v90m_async_putbit(((end_t *)u)->a, b); }

static void end_init(end_t *e, v90m_role role, unsigned mods)
{
    v90m_cfg c; int err;
    memset(e, 0, sizeof *e);
    e->a = v90m_async_create(&err);
    v90m_cfg_default(&c);
    c.role = role; c.modulations = mods;
    c.getbit = end_get; c.putbit = end_put; c.on_event = end_event; c.user = e;
    e->m = v90m_create(&c, &err);
    CHECK(e->m != NULL);
}

#define V90M_MOD_ALL_DEFAULT (V90M_MOD_V21 | V90M_MOD_V22 | V90M_MOD_V32BIS | V90M_MOD_V34 | V90M_MOD_V90)

static void test_originate(void)
{
    static end_t cl, an;
    v90m_cfg c; v90m_state sc, sa; int err, k;
    int16_t c_rx = 0, a_rx = 0;
    char got_c[64], got_a[64]; size_t nc = 0, na = 0;

    /* what the calling side can do: V.92 with V.90 */
    v90m_cfg_default(&c); c.role = V90M_ROLE_ORIGINATE;
    {
        v90m_t *m = v90m_create(&c, &err);                                      /* V.90 among the defaults calls now */
        CHECK(m != NULL); CHECK_EQ_I(err, V90M_OK);
        v90m_destroy(m);
    }
    c.modulations = V90M_MOD_V34 | V90M_MOD_V22;
    {
        v90m_t *m = v90m_create(&c, &err);                                      /* V.34 calls now */
        CHECK(m != NULL); CHECK_EQ_I(err, V90M_OK);
        v90m_destroy(m);
    }
    c.modulations = V90M_MOD_V34 | V90M_MOD_V90;
    {
        v90m_t *m = v90m_create(&c, &err);                                      /* V.90 calls now */
        CHECK(m != NULL); CHECK_EQ_I(err, V90M_OK);
        v90m_destroy(m);
    }
    c.modulations = V90M_MOD_V34 | V90M_MOD_V90 | V90M_MOD_V92;
    {
        v90m_t *m = v90m_create(&c, &err);                                      /* V.92 calls now */
        CHECK(m != NULL); CHECK_EQ_I(err, V90M_OK);
        v90m_destroy(m);
    }
    c.modulations = 0;
    CHECK(v90m_create(&c, &err) == NULL); CHECK_EQ_I(err, V90M_EINVAL);

    end_init(&cl, V90M_ROLE_ORIGINATE, V90M_MOD_V22 | V90M_MOD_V21);
    end_init(&an, V90M_ROLE_ANSWER, V90M_MOD_V22 | V90M_MOD_V21);
    for (k = 0; k < 8000 * 10; k++) {
        int16_t c_tx, a_tx;
        v90m_process_linear(cl.m, &c_rx, &c_tx, 1);
        v90m_process_linear(an.m, &a_rx, &a_tx, 1);
        a_rx = c_tx; c_rx = a_tx;                      /* a 4-wire line, one sample each way */
        if (k == 8000 * 6) {
            v90m_async_write(cl.a, (const uint8_t *)"from the caller", 15);
            v90m_async_write(an.a, (const uint8_t *)"from the answerer", 17);
        }
        nc += v90m_async_read(cl.a, (uint8_t *)got_c + nc, sizeof got_c - 1 - nc);
        na += v90m_async_read(an.a, (uint8_t *)got_a + na, sizeof got_a - 1 - na);
    }
    got_c[nc] = 0; got_a[na] = 0;
    v90m_get_state(cl.m, &sc); v90m_get_state(an.m, &sa);
    printf("  API caller to API answerer: caller %d %s %d bit/s (V.8 %d), answerer %d %s; caller got \"%s\", answerer got \"%s\"\n",
           sc.connected, sc.modulation == V90M_CONN_V22BIS ? "V.22bis" : "?", sc.tx_bps, sc.used_v8,
           sa.connected, sa.modulation == V90M_CONN_V22BIS ? "V.22bis" : "?", got_c, got_a);
    CHECK_EQ_I(sc.connected, 1); CHECK_EQ_I(sc.modulation, V90M_CONN_V22BIS); CHECK_EQ_I(sc.tx_bps, 2400);
    CHECK_EQ_I(sc.used_v8, 1); CHECK_EQ_I(sc.moving, 1);
    CHECK_EQ_I(sa.connected, 1); CHECK_EQ_I(sa.modulation, V90M_CONN_V22BIS);
    CHECK(strcmp(got_c, "from the answerer") == 0);
    CHECK(strcmp(got_a, "from the caller") == 0);
    CHECK_EQ_I(cl.ev[0], V90M_EV_CONNECTED);
    CHECK_EQ_I(v90m_retrain(cl.m), V90M_OK);           /* V.22bis at 2400: the caller may retrain */
    CHECK_EQ_I(v90m_cleardown(cl.m), V90M_ESTATE);     /* V.22bis has no cleardown */
    v90m_destroy(cl.m); v90m_destroy(an.m); v90m_async_destroy(cl.a); v90m_async_destroy(an.a);

    /* V.32bis: an API caller and an API answerer (its default modulations) back to back */
    v90m_cfg_default(&c); c.role = V90M_ROLE_ORIGINATE;
    c.modulations = V90M_MOD_V92 | V90M_MOD_V32BIS;
    CHECK(v90m_create(&c, &err) == NULL); CHECK_EQ_I(err, V90M_ENOTSUP);
    end_init(&cl, V90M_ROLE_ORIGINATE, V90M_MOD_V32BIS | V90M_MOD_V22);
    end_init(&an, V90M_ROLE_ANSWER, V90M_MOD_ALL_DEFAULT);
    nc = na = 0;
    if (cl.m && an.m) {
        int stopped = 0, moving_again = 0, k_rt = -1;
        v90m_stats st;
        for (k = 0; k < 8000 * 30; k++) {
            int16_t c_tx, a_tx;
            v90m_process_linear(cl.m, &c_rx, &c_tx, 1);
            v90m_process_linear(an.m, &a_rx, &a_tx, 1);
            a_rx = c_tx; c_rx = a_tx;
            v90m_get_state(cl.m, &sc);
            if (k_rt < 0 && sc.connected && sc.moving && k % 8000 == 0) {
                v90m_async_write(cl.a, (const uint8_t *)"from the caller", 15);
                v90m_async_write(an.a, (const uint8_t *)"from the answerer", 17);
                k_rt = k + 8000 * 2;
            }
            if (k == k_rt) {
                got_c[nc] = 0; got_a[na] = 0;
                v90m_get_state(an.m, &sa);
                v90m_get_stats(cl.m, &st);
                printf("  V.32bis: caller %d %s %d/%d bit/s (V.8 %d), answerer %d %s; SNR %.1f dB, round trip %d samples; caller got \"%s\", answerer got \"%s\"\n",
                       sc.connected, sc.modulation == V90M_CONN_V32BIS ? "V.32bis" : "?", sc.tx_bps, sc.rx_bps, sc.used_v8,
                       sa.connected, sa.modulation == V90M_CONN_V32BIS ? "V.32bis" : "?", st.snr_db10 / 10.0, st.rtd, got_c, got_a);
                CHECK_EQ_I(sc.modulation, V90M_CONN_V32BIS); CHECK_EQ_I(sc.tx_bps, 14400); CHECK_EQ_I(sc.rx_bps, 14400);
                CHECK_EQ_I(sc.used_v8, 1); CHECK_EQ_I(sc.moving, 1);
                CHECK_EQ_I(sa.connected, 1); CHECK_EQ_I(sa.modulation, V90M_CONN_V32BIS);
                CHECK(strcmp(got_c, "from the answerer") == 0);
                CHECK(strcmp(got_a, "from the caller") == 0);
                CHECK(st.snr_db10 > 300); CHECK(st.rtd >= 0 && st.rtd < 40); CHECK_EQ_I(st.retrains, 0);
                CHECK_EQ_I(cl.ev[0], V90M_EV_CONNECTED);
                CHECK_EQ_I(v90m_cleardown(cl.m), V90M_ESTATE);     /* V.32bis has no cleardown in data mode */
                CHECK_EQ_I(v90m_retrain(cl.m), V90M_OK);            /* 7.1 */
                nc = na = 0;
            }
            if (k > k_rt && k_rt >= 0) {
                if (!sc.moving && sc.retraining) stopped = 1;
                if (stopped && sc.moving && !moving_again) {
                    /* (before it knew our AA for a retrain, 128 T of it,
                     * the answerer's receiver took it for data: 7.2 has
                     * nothing quicker; what it made of it is dropped) */
                    moving_again = 1; na = 0;
                    while (v90m_async_read(an.a, (uint8_t *)got_a, sizeof got_a) > 0) ;
                    v90m_async_write(cl.a, (const uint8_t *)"again", 5);
                }
            }
            nc += v90m_async_read(cl.a, (uint8_t *)got_c + nc, sizeof got_c - 1 - nc);
            na += v90m_async_read(an.a, (uint8_t *)got_a + na, sizeof got_a - 1 - na);
        }
        got_a[na] = 0;
        v90m_get_stats(cl.m, &st);
        v90m_get_state(cl.m, &sc);
        printf("  V.32bis retrain asked for by the caller: stopped %d, moving again %d, retrains %u; answerer got \"%s\"\n",
               stopped, moving_again, st.retrains, got_a);
        CHECK(stopped); CHECK(moving_again); CHECK_EQ_I(st.retrains, 1); CHECK_EQ_I(sc.connected, 1);
        CHECK(strcmp(got_a, "again") == 0);
    }
    v90m_destroy(cl.m); v90m_destroy(an.m); v90m_async_destroy(cl.a); v90m_async_destroy(an.a);

    /* V.34: an API caller offering V.34, V.32bis and V.22 to an API answerer
     * (its default modulations: V.90 too, which a caller without PCM does
     * not take) back to back; a retrain and then a cleardown from the caller */
    end_init(&cl, V90M_ROLE_ORIGINATE, V90M_MOD_V34 | V90M_MOD_V32BIS | V90M_MOD_V22);
    end_init(&an, V90M_ROLE_ANSWER, V90M_MOD_ALL_DEFAULT);
    nc = na = 0; c_rx = a_rx = 0;
    if (cl.m && an.m) {
        int stopped = 0, moving_again = 0, k_rt = -1, k_cd = -1, k_wr = -1, early = -9;
        v90m_stats st;
        for (k = 0; k < 8000 * 40; k++) {
            int16_t c_tx, a_tx;
            v90m_process_linear(cl.m, &c_rx, &c_tx, 1);
            v90m_process_linear(an.m, &a_rx, &a_tx, 1);
            a_rx = c_tx; c_rx = a_tx;
            v90m_get_state(cl.m, &sc);
            if (k == 8000) early = v90m_cleardown(cl.m);          /* still in V.8 */
            if (k_rt < 0 && sc.connected && sc.moving && k % 8000 == 0) {
                v90m_async_write(cl.a, (const uint8_t *)"from the caller", 15);
                v90m_async_write(an.a, (const uint8_t *)"from the answerer", 17);
                k_rt = k + 8000 * 2;
            }
            if (k == k_rt) {
                got_c[nc] = 0; got_a[na] = 0;
                v90m_get_state(an.m, &sa);
                v90m_get_stats(cl.m, &st);
                printf("  V.34: caller %d %s %d/%d bit/s (V.8 %d), answerer %d %s %d/%d; SNR %.1f dB, round trip %d samples; caller got \"%s\", answerer got \"%s\"\n",
                       sc.connected, sc.modulation == V90M_CONN_V34 ? "V.34" : "?", sc.tx_bps, sc.rx_bps, sc.used_v8,
                       sa.connected, sa.modulation == V90M_CONN_V34 ? "V.34" : "?", sa.tx_bps, sa.rx_bps,
                       st.snr_db10 / 10.0, st.rtd, got_c, got_a);
                CHECK_EQ_I(early, V90M_ESTATE);
                CHECK_EQ_I(sc.modulation, V90M_CONN_V34); CHECK_EQ_I(sc.used_v8, 1); CHECK_EQ_I(sc.moving, 1);
                CHECK(sc.tx_bps >= 28800); CHECK(sc.rx_bps >= 28800);
                CHECK_EQ_I(sa.connected, 1); CHECK_EQ_I(sa.modulation, V90M_CONN_V34);
                CHECK_EQ_I(sa.tx_bps, sc.rx_bps); CHECK_EQ_I(sa.rx_bps, sc.tx_bps);
                CHECK_EQ_I(st.modulation, V90M_CONN_V34); CHECK_EQ_I(st.tx_bps, sc.tx_bps); CHECK_EQ_I(st.rx_bps, sc.rx_bps);
                CHECK(strcmp(got_c, "from the answerer") == 0);
                CHECK(strcmp(got_a, "from the caller") == 0);
                CHECK(st.snr_db10 > 300); CHECK(st.rtd >= 0 && st.rtd < 40); CHECK_EQ_I(st.retrains, 0);
                CHECK_EQ_I(cl.ev[0], V90M_EV_CONNECTED);
                CHECK_EQ_I(st.ec_in_db10, -1); CHECK_EQ_I(st.ec_far_peak, -1);   /* answering only */
                v90m_get_stats(an.m, &st);
                printf("  V.34 answerer's echo cancellers: %.1f dB in, %.1f dB left; near at %d+%d, far at %d+%d\n",
                       -st.ec_in_db10 / 10.0, -st.ec_res_db10 / 10.0, st.ec_near_delay, st.ec_near_peak,
                       st.ec_far_delay, st.ec_far_peak);
                CHECK(st.ec_in_db10 > 0); CHECK(st.ec_res_db10 >= st.ec_in_db10); CHECK(st.ec_near_delay >= 0);
                CHECK_EQ_I(v90m_retrain(cl.m), V90M_OK);            /* 11.5.1.1 */
                nc = na = 0;
            }
            if (k > k_rt && k_rt >= 0 && k_cd < 0) {
                if (!sc.moving && sc.retraining) stopped = 1;
                if (stopped && sc.moving && !moving_again) { moving_again = 1; k_wr = k + 800; }
                if (k == k_wr) {
                    /* (until it knew our silence and Tone B for a retrain,
                     * about 130 ms, the answerer's receiver took them for
                     * data, and the start-stop framer may have a character
                     * begun in it that the first data completes: what it
                     * made of them is dropped) */
                    na = 0;
                    while (v90m_async_read(an.a, (uint8_t *)got_a, sizeof got_a) > 0) ;
                    v90m_async_write(cl.a, (const uint8_t *)"again", 5);
                    k_cd = k + 8000 * 2;
                }
            }
            if (k == k_cd) {
                got_a[na] = 0;
                v90m_get_stats(cl.m, &st);
                printf("  V.34 retrain asked for by the caller: stopped %d, moving again %d, retrains %u; answerer got \"%s\"\n",
                       stopped, moving_again, st.retrains, got_a);
                CHECK(strcmp(got_a, "again") == 0); CHECK_EQ_I(st.retrains, 1);
                CHECK_EQ_I(sc.connected, 1); CHECK_EQ_I(sc.moving, 1);
                CHECK_EQ_I(v90m_cleardown(cl.m), V90M_OK);          /* 11.7 */
            }
            if (k_cd >= 0 && k > k_cd + 8000) break;
            nc += v90m_async_read(cl.a, (uint8_t *)got_c + nc, sizeof got_c - 1 - nc);
            na += v90m_async_read(an.a, (uint8_t *)got_a + na, sizeof got_a - 1 - na);
        }
        CHECK(stopped); CHECK(moving_again); CHECK(k_cd >= 0);
        v90m_get_state(cl.m, &sc); v90m_get_state(an.m, &sa);
        printf("  V.34 cleardown by the caller: caller end %d, answerer end %d; caller events %d (last %d)\n",
               sc.end, sa.end, cl.n_ev, cl.n_ev > 0 && cl.n_ev <= 8 ? cl.ev[cl.n_ev - 1] : -1);
        CHECK_EQ_I(sc.end, V90M_END_CLEARDOWN_OURS); CHECK_EQ_I(sc.connected, 0);
        CHECK_EQ_I(sa.end, V90M_END_CLEARDOWN_FAR);
        CHECK(cl.n_ev <= 8 && cl.ev[cl.n_ev - 1] == V90M_EV_ENDED);
        CHECK(an.n_ev <= 8 && an.ev[an.n_ev - 1] == V90M_EV_ENDED);
        CHECK_EQ_I(v90m_cleardown(cl.m), V90M_ESTATE);
    }
    v90m_destroy(cl.m); v90m_destroy(an.m); v90m_async_destroy(cl.a); v90m_async_destroy(an.a);

    /* V.90: an API caller offering V.90, V.34 and V.22 to an API answerer
     * (its default modulations) back to back. The 4-wire line passes the
     * answerer's codewords' levels to the caller as they are (a channel of
     * one tap), linear both ways; data both ways, a retrain asked for by the
     * caller (9.5.2.1), data again, a cleardown from the caller */
    end_init(&cl, V90M_ROLE_ORIGINATE, V90M_MOD_V90 | V90M_MOD_V34 | V90M_MOD_V22);
    end_init(&an, V90M_ROLE_ANSWER, V90M_MOD_ALL_DEFAULT);
    nc = na = 0; c_rx = a_rx = 0;
    if (cl.m && an.m) {
        int k_wr = -1, k_rt = -1, k_cd = -1, stopped = 0, n_ev0 = 0;
        v90m_stats st;
        for (k = 0; k < 8000 * 60; k++) {
            int16_t c_tx, a_tx;
            v90m_process_linear(cl.m, &c_rx, &c_tx, 1);
            v90m_process_linear(an.m, &a_rx, &a_tx, 1);
            a_rx = c_tx; c_rx = a_tx;
            v90m_get_state(cl.m, &sc); v90m_get_state(an.m, &sa);
            if (k_wr < 0 && sc.moving && sa.moving) {
                v90m_async_write(cl.a, (const uint8_t *)"from the caller", 15);
                v90m_async_write(an.a, (const uint8_t *)"from the answerer", 17);
                k_wr = k; k_rt = k + 8000 * 2;
            }
            if (k > k_rt && k_rt >= 0 && k_cd < 0) {
                /* the retrain: data stops, the rates stay, data moves again */
                if (!sc.moving && sc.retraining && sc.connected) stopped = 1;
                if (stopped && sc.moving && sa.moving) {
                    v90m_get_stats(cl.m, &st);
                    printf("  V.90 retrain asked for by the caller: moving again after %d ms, retrains %u (answerer: %d), %d/%d bit/s\n",
                           (k - k_rt) / 8, st.retrains, sa.retraining, sc.tx_bps, sc.rx_bps);
                    CHECK_EQ_I(st.retrains, 1); CHECK_EQ_I(sc.modulation, V90M_CONN_V90);
                    CHECK(sc.rx_bps >= 40000); CHECK_EQ_I(sa.tx_bps, sc.rx_bps); CHECK_EQ_I(sa.rx_bps, sc.tx_bps);
                    CHECK(cl.n_ev >= n_ev0 + 2 && cl.ev[n_ev0] == V90M_EV_DATA_STOPPED);
                    CHECK_EQ_I(st.renegotiations, 0);
                    k_cd = k + 8000;
                }
            }
            if (k == k_cd) CHECK_EQ_I(v90m_cleardown(cl.m), V90M_OK);          /* 9.7 */
            if (k == k_rt) {
                got_c[nc] = 0; got_a[na] = 0;
                v90m_get_stats(cl.m, &st);
                printf("  V.90: caller %d %s %d/%d bit/s (V.8 %d), answerer %d %s %d/%d; SNR %.1f dB, round trip %d samples; caller got \"%s\", answerer got \"%s\"\n",
                       sc.connected, sc.modulation == V90M_CONN_V90 ? "V.90" : "?", sc.tx_bps, sc.rx_bps, sc.used_v8,
                       sa.connected, sa.modulation == V90M_CONN_V90 ? "V.90" : "?", sa.tx_bps, sa.rx_bps,
                       st.snr_db10 / 10.0, st.rtd, got_c, got_a);
                CHECK_EQ_I(sc.modulation, V90M_CONN_V90); CHECK_EQ_I(sc.used_v8, 1); CHECK_EQ_I(sc.moving, 1);
                CHECK(sc.rx_bps >= 40000); CHECK(sc.tx_bps >= 24000);
                CHECK_EQ_I(sa.modulation, V90M_CONN_V90);
                CHECK_EQ_I(sa.tx_bps, sc.rx_bps); CHECK_EQ_I(sa.rx_bps, sc.tx_bps);
                CHECK_EQ_I(st.tx_bps, sc.tx_bps); CHECK_EQ_I(st.rx_bps, sc.rx_bps);
                CHECK(strcmp(got_c, "from the answerer") == 0);
                CHECK(strcmp(got_a, "from the caller") == 0);
                CHECK(st.snr_db10 > 300); CHECK(st.rtd >= 0 && st.rtd < 80);
                n_ev0 = cl.n_ev;
                CHECK_EQ_I(v90m_retrain(cl.m), V90M_OK);            /* 9.5.2.1 */
                CHECK_EQ_I(v90m_retrain(cl.m), V90M_ESTATE);        /* one at a time */
            }
            if (k_cd >= 0 && k > k_cd + 8000 * 2) break;
            nc += v90m_async_read(cl.a, (uint8_t *)got_c + nc, sizeof got_c - 1 - nc);
            na += v90m_async_read(an.a, (uint8_t *)got_a + na, sizeof got_a - 1 - na);
        }
        CHECK(k_cd >= 0);
        v90m_get_state(cl.m, &sc); v90m_get_state(an.m, &sa);
        printf("  V.90 cleardown by the caller: caller end %d, answerer end %d\n", sc.end, sa.end);
        CHECK_EQ_I(sc.end, V90M_END_CLEARDOWN_OURS); CHECK_EQ_I(sc.connected, 0);
        CHECK_EQ_I(sa.end, V90M_END_CLEARDOWN_FAR);
    }
    v90m_destroy(cl.m); v90m_destroy(an.m); v90m_async_destroy(cl.a); v90m_async_destroy(an.a);

    /* V.90: a retrain the answerer never answers (it is gone): the call ends
     * with V90M_END_RETRAIN_FAILED, reported as V90M_EV_ENDED */
    end_init(&cl, V90M_ROLE_ORIGINATE, V90M_MOD_V90 | V90M_MOD_V34 | V90M_MOD_V22);
    end_init(&an, V90M_ROLE_ANSWER, V90M_MOD_ALL_DEFAULT);
    c_rx = a_rx = 0;
    if (cl.m && an.m) {
        int k_rt = -1, gone = 0;
        for (k = 0; k < 8000 * 50; k++) {
            int16_t c_tx, a_tx;
            v90m_process_linear(cl.m, &c_rx, &c_tx, 1);
            v90m_process_linear(an.m, &a_rx, &a_tx, 1);
            a_rx = c_tx; c_rx = gone ? 0 : a_tx;
            v90m_get_state(cl.m, &sc); v90m_get_state(an.m, &sa);
            if (k_rt < 0 && sc.moving && sa.moving) k_rt = k + 8000;
            if (k == k_rt) { CHECK_EQ_I(v90m_retrain(cl.m), V90M_OK); gone = 1; }
            if (k_rt >= 0 && sc.end != V90M_END_NONE) break;
        }
        printf("  V.90 retrain never answered: caller end %d after %d ms, last event %d\n", sc.end, (k - k_rt) / 8,
               cl.n_ev > 0 && cl.n_ev <= 8 ? cl.ev[cl.n_ev - 1] : -1);
        CHECK_EQ_I(sc.end, V90M_END_RETRAIN_FAILED); CHECK_EQ_I(sc.connected, 0);
        CHECK(cl.n_ev <= 8 && cl.ev[cl.n_ev - 1] == V90M_EV_ENDED);
        CHECK_EQ_I(v90m_retrain(cl.m), V90M_ESTATE);
    }
    v90m_destroy(cl.m); v90m_destroy(an.m); v90m_async_destroy(cl.a); v90m_async_destroy(an.a);

    /* V.92: an API caller offering V.92, V.90, V.34 and V.22 to an API
     * answerer with V.92 on, back to back as above; data both ways, PCM
     * upstream, a retrain asked for by the caller (9.7.2.1), data again, a
     * cleardown from the caller */
    end_init(&cl, V90M_ROLE_ORIGINATE, V90M_MOD_V92 | V90M_MOD_V90 | V90M_MOD_V34 | V90M_MOD_V22);
    end_init(&an, V90M_ROLE_ANSWER, V90M_MOD_ALL_DEFAULT | V90M_MOD_V92);
    nc = na = 0; c_rx = a_rx = 0;
    if (cl.m && an.m) {
        int k_wr = -1, k_rt = -1, k_cd = -1, stopped = 0, n_ev0 = 0;
        v90m_stats st;
        for (k = 0; k < 8000 * 70; k++) {
            int16_t c_tx, a_tx;
            v90m_process_linear(cl.m, &c_rx, &c_tx, 1);
            v90m_process_linear(an.m, &a_rx, &a_tx, 1);
            a_rx = c_tx; c_rx = a_tx;
            v90m_get_state(cl.m, &sc); v90m_get_state(an.m, &sa);
            if (k_wr < 0 && sc.moving && sa.moving) {
                v90m_async_write(cl.a, (const uint8_t *)"from the caller", 15);
                v90m_async_write(an.a, (const uint8_t *)"from the answerer", 17);
                k_wr = k; k_rt = k + 8000 * 2;
            }
            if (k > k_rt && k_rt >= 0 && k_cd < 0) {
                if (!sc.moving && sc.retraining && sc.connected) stopped = 1;
                if (stopped && sc.moving && sa.moving) {
                    v90m_get_stats(cl.m, &st);
                    printf("  V.92 retrain asked for by the caller: moving again after %d ms, retrains %u (answerer: %d), %d/%d bit/s\n",
                           (k - k_rt) / 8, st.retrains, sa.retraining, sc.tx_bps, sc.rx_bps);
                    CHECK_EQ_I(st.retrains, 1); CHECK_EQ_I(sc.modulation, V90M_CONN_V92);
                    CHECK(sc.rx_bps >= 40000); CHECK(sc.tx_bps >= 24000);
                    CHECK_EQ_I(sa.tx_bps, sc.rx_bps); CHECK_EQ_I(sa.rx_bps, sc.tx_bps);
                    CHECK(cl.n_ev >= n_ev0 + 2 && cl.ev[n_ev0] == V90M_EV_DATA_STOPPED);
                    CHECK_EQ_I(st.renegotiations, 0);
                    k_cd = k + 8000;
                }
            }
            if (k == k_cd) CHECK_EQ_I(v90m_cleardown(cl.m), V90M_OK);          /* 9.11/V.92 */
            if (k == k_rt) {
                got_c[nc] = 0; got_a[na] = 0;
                v90m_get_stats(cl.m, &st);
                printf("  V.92: caller %d %s %d/%d bit/s (V.8 %d), answerer %d %s %d/%d; SNR %.1f dB, round trip %d samples; caller got \"%s\", answerer got \"%s\"\n",
                       sc.connected, sc.modulation == V90M_CONN_V92 ? "V.92" : "?", sc.tx_bps, sc.rx_bps, sc.used_v8,
                       sa.connected, sa.modulation == V90M_CONN_V92 ? "V.92" : "?", sa.tx_bps, sa.rx_bps,
                       st.snr_db10 / 10.0, st.rtd, got_c, got_a);
                CHECK_EQ_I(sc.modulation, V90M_CONN_V92); CHECK_EQ_I(sc.used_v8, 1); CHECK_EQ_I(sc.moving, 1);
                CHECK(sc.rx_bps >= 40000); CHECK(sc.tx_bps >= 24000);
                CHECK_EQ_I(sa.modulation, V90M_CONN_V92);
                CHECK_EQ_I(sa.tx_bps, sc.rx_bps); CHECK_EQ_I(sa.rx_bps, sc.tx_bps);
                CHECK_EQ_I(st.tx_bps, sc.tx_bps); CHECK_EQ_I(st.rx_bps, sc.rx_bps);
                CHECK(strcmp(got_c, "from the answerer") == 0);
                CHECK(strcmp(got_a, "from the caller") == 0);
                CHECK(st.snr_db10 > 300); CHECK(st.rtd >= 0 && st.rtd < 80);
                n_ev0 = cl.n_ev;
                CHECK_EQ_I(v90m_retrain(cl.m), V90M_OK);            /* 9.7.2.1 */
            }
            if (k_cd >= 0 && k > k_cd + 8000 * 3) break;
            nc += v90m_async_read(cl.a, (uint8_t *)got_c + nc, sizeof got_c - 1 - nc);
            na += v90m_async_read(an.a, (uint8_t *)got_a + na, sizeof got_a - 1 - na);
        }
        CHECK(k_cd >= 0);
        v90m_get_state(cl.m, &sc); v90m_get_state(an.m, &sa);
        printf("  V.92 cleardown by the caller: caller end %d, answerer end %d\n", sc.end, sa.end);
        CHECK_EQ_I(sc.end, V90M_END_CLEARDOWN_OURS); CHECK_EQ_I(sc.connected, 0);
        CHECK_EQ_I(sa.end, V90M_END_CLEARDOWN_FAR);
    }
    v90m_destroy(cl.m); v90m_destroy(an.m); v90m_async_destroy(cl.a); v90m_async_destroy(an.a);

    /* nobody answers with a tone */
    end_init(&cl, V90M_ROLE_ORIGINATE, V90M_MOD_V22 | V90M_MOD_V21);
    for (k = 0, c_rx = 0; k < 8000 * 11; k++) { int16_t t; v90m_process_linear(cl.m, &c_rx, &t, 1); }
    v90m_get_state(cl.m, &sc);
    CHECK_EQ_I(sc.end, V90M_END_NO_ANSWER_TONE);
    CHECK_EQ_I(cl.n_ev, 1); CHECK_EQ_I(cl.ev[0], V90M_EV_ENDED);
    v90m_destroy(cl.m); v90m_async_destroy(cl.a);
}

/* ---- Bell 103 and Bell 212A callers: the library answers ----
 * The callers are as plain as the real ones (the same as in test_answer),
 * made of the dev archive's V.21 and V.22 parts. Our echo comes back at a
 * fifth of what we send, 21 samples late. */
typedef struct { v21mod_t mod; v21demod_t dem; arx_t uart; v14_t dte; int fbit, cd, on; int64_t thr; } b103_t;

static void b103_init(b103_t *b)
{
    memset(b, 0, sizeof *b);
    v21mod_init(&b->mod, BELL103_O_MARK, BELL103_O_SPACE, 8000, 300, 4243);
    v21demod_init(&b->dem, BELL103_A_MARK, BELL103_A_SPACE, 8000, 300);
    arx_init(&b->uart, 8000, 300);
    v14_init(&b->dte);
    b->thr = v21demod_energy_of(&b->dem, 1200); b->fbit = 1;
}

/* once it has heard 150 ms in its band (2025 / 2225 Hz), its mark and then
 * start-stop characters */
static int16_t b103_tick(b103_t *b, int16_t rx)
{
    int level = v21demod_tick(&b->dem, rx), heard = b->dem.energy >= b->thr, c, i, done;
    int16_t out;
    b->cd = heard ? b->cd + 1 : 0;
    if (b->cd > 150 * 8) b->on = 1;
    if (!b->on) return 0;
    c = arx_tick(&b->uart, heard ? level : 1);
    if (c >= 0) {
        v14_putbit(&b->dte, 0);
        for (i = 0; i < 8; i++) v14_putbit(&b->dte, (c >> i) & 1);
        v14_putbit(&b->dte, 1);
    }
    out = v21mod_tick(&b->mod, b->fbit, &done);
    if (done) b->fbit = v14_getbit(&b->dte) & 1;
    return out;
}

/* nothing but 2225 Hz (three 100 ms blocks in a row) is an answering modem
 * to it; then V.22 at 1200 bit/s as a caller */
typedef struct { v22bis_t m; v14_t dte; int on, run, n; double re, im, pw; } b212_t;

static int16_t b212_tick(b212_t *b, int16_t rx)
{
    if (!b->on) {
        double w = 2.0 * 3.14159265358979323846 * 2225.0 * b->n / 8000.0;
        b->re += rx * cos(w); b->im += rx * sin(w); b->pw += (double)rx * rx;
        if (++b->n < 800) return 0;
        b->run = (b->pw > 800.0 * 400 * 400 && (b->re * b->re + b->im * b->im) * 4 > b->pw * 800) ? b->run + 1 : 0;
        b->n = 0; b->re = b->im = b->pw = 0;
        if (b->run < 3) return 0;
        v22m_init(&b->m, V22_CALLER, 0, 3000, v14_getbit, v14_putbit, &b->dte);
        v22tx_set_mode(&b->m.tx, V22_TX_SB1_1200);
        b->m.state = V22S_TRAIN;
        b->on = 1;
    }
    return v22m_tick(&b->m, rx);
}

typedef struct {
    v90m_t *m; v90m_async_t *a;
    int bell212;                    /* which caller */
    b103_t c103; b212_t c212;
    int ev[8]; int n_ev;
    int16_t echo[32]; int epos;
    char got_a[64], got_c[64]; size_t na, nc;
    double connected_s;
} bell_t;

static void bell_event(void *u, v90m_event ev) { bell_t *b = u; if (b->n_ev < 8) b->ev[b->n_ev] = (int)ev; b->n_ev++; }
static int bell_get(void *u) { return v90m_async_getbit(((bell_t *)u)->a); }
static void bell_put(void *u, int bit) { v90m_async_putbit(((bell_t *)u)->a, bit); }

/* a call of `seconds` against the Bell 103 (bell212 = 0) or 212A caller;
 * once connected each side sends a line */
static int bell_call(bell_t *b, int bell212, unsigned mods, int seconds)
{
    static const char *msg_c = "caller says hello, 0123456789", *msg_a = "answerer replies: ~{}[] OK";
    v90m_cfg c; int err, n, sent = 0, at = -1;
    int16_t atx = 0, ctx = 0;
    memset(b, 0, sizeof *b);
    b->bell212 = bell212;
    b->a = v90m_async_create(&err);
    v90m_cfg_default(&c);
    c.modulations = mods;
    c.getbit = bell_get; c.putbit = bell_put; c.on_event = bell_event; c.user = b;
    b->m = v90m_create(&c, &err);
    CHECK(b->m != NULL);
    if (!b->m) { v90m_async_destroy(b->a); return 0; }
    b103_init(&b->c103); v14_init(&b->c212.dte);
    for (n = 0; n < seconds * 8000; n++) {
        v90m_state st;
        int16_t rx = (int16_t)(ctx + b->echo[(b->epos + 32 - 21) & 31] / 5), heard = atx;
        v90m_process_linear(b->m, &rx, &atx, 1);
        b->echo[b->epos] = atx; b->epos = (b->epos + 1) & 31;
        ctx = bell212 ? b212_tick(&b->c212, heard) : b103_tick(&b->c103, heard);
        v90m_get_state(b->m, &st);
        if (at < 0 && st.connected) { at = n; b->connected_s = n / 8000.0; }
        if (at >= 0 && !sent && n > at + 4000) {
            v14_write(bell212 ? &b->c212.dte : &b->c103.dte, (const uint8_t *)msg_c, (int)strlen(msg_c));
            v90m_async_write(b->a, (const uint8_t *)msg_a, strlen(msg_a));
            sent = 1;
        }
        if (sent) {
            b->na += v90m_async_read(b->a, (uint8_t *)b->got_a + b->na, 63 - b->na);
            b->nc += (size_t)v14_read(bell212 ? &b->c212.dte : &b->c103.dte, (uint8_t *)b->got_c + b->nc, (int)(63 - b->nc));
        }
    }    return 1;
}

static void bell_done(bell_t *b) { v90m_destroy(b->m); v90m_async_destroy(b->a); }

static void test_bell(void)
{
    static bell_t b;
    static const char *msg_c = "caller says hello, 0123456789", *msg_a = "answerer replies: ~{}[] OK";
    const unsigned ours = V90M_MOD_V22 | V90M_MOD_V21 | V90M_MOD_BELL103 | V90M_MOD_BELL212A;
    v90m_state st; v90m_stats sx;

    /* Bell 103: we answer on 2225 / 2025 Hz, 300 bit/s, start-stop */
    if (bell_call(&b, 0, ours, 12)) {
        v90m_get_state(b.m, &st); v90m_get_stats(b.m, &sx);
        printf("  Bell 103 through the API: connected at %.2f s, \"%s\" / \"%s\"\n", b.connected_s, b.got_a, b.got_c);
        CHECK_EQ_I(st.connected, 1); CHECK_EQ_I(st.modulation, V90M_CONN_BELL103);
        CHECK_EQ_I(st.tx_bps, 300); CHECK_EQ_I(st.rx_bps, 300);
        CHECK_EQ_I(st.used_v8, 0); CHECK_EQ_I(st.moving, 1); CHECK_EQ_I(st.end, V90M_END_NONE);
        CHECK_EQ_I(sx.modulation, V90M_CONN_BELL103); CHECK_EQ_I(sx.snr_db10, -1);
        CHECK(b.n_ev >= 2); CHECK_EQ_I(b.ev[0], V90M_EV_CONNECTED); CHECK_EQ_I(b.ev[1], V90M_EV_DATA_MOVING);
        CHECK(strcmp(b.got_a, msg_c) == 0); CHECK(strcmp(b.got_c, msg_a) == 0);
        bell_done(&b);
    }

    /* the same without V.22: the wait is V.21's */
    if (bell_call(&b, 0, V90M_MOD_V21 | V90M_MOD_BELL103, 12)) {
        v90m_get_state(b.m, &st);
        printf("  Bell 103 without V.22: connected at %.2f s\n", b.connected_s);
        CHECK_EQ_I(st.connected, 1); CHECK_EQ_I(st.modulation, V90M_CONN_BELL103); CHECK_EQ_I(st.tx_bps, 300);
        CHECK(strcmp(b.got_a, msg_c) == 0); CHECK(strcmp(b.got_c, msg_a) == 0);
        bell_done(&b);
    }

    /* not offered: never Bell 103, and nothing from that caller */
    if (bell_call(&b, 0, V90M_MOD_V22 | V90M_MOD_V21, 30)) {
        v90m_get_state(b.m, &st);
        CHECK(st.modulation != V90M_CONN_BELL103); CHECK_EQ_I((int)b.na, 0);
        bell_done(&b);
    }

    /* Bell 212A: 2225 Hz after the V.22bis answer sequence went unanswered,
     * then 1200 bit/s as V.22 */
    if (bell_call(&b, 1, ours, 16)) {
        v90m_get_state(b.m, &st); v90m_get_stats(b.m, &sx);
        printf("  Bell 212A through the API: connected at %.2f s, \"%s\" / \"%s\"\n", b.connected_s, b.got_a, b.got_c);
        CHECK_EQ_I(st.connected, 1); CHECK_EQ_I(st.modulation, V90M_CONN_BELL212A);
        CHECK_EQ_I(st.tx_bps, 1200); CHECK_EQ_I(st.rx_bps, 1200);
        CHECK_EQ_I(st.used_v8, 0); CHECK_EQ_I(st.moving, 1); CHECK_EQ_I(st.end, V90M_END_NONE);
        CHECK_EQ_I(sx.modulation, V90M_CONN_BELL212A);
        CHECK(b.n_ev >= 2); CHECK_EQ_I(b.ev[0], V90M_EV_CONNECTED); CHECK_EQ_I(b.ev[1], V90M_EV_DATA_MOVING);
        CHECK(strcmp(b.got_a, msg_c) == 0); CHECK(strcmp(b.got_c, msg_a) == 0);
        bell_done(&b);
    }

    /* not offered: that caller hears no 2225 Hz from us and stays silent */
    if (bell_call(&b, 1, V90M_MOD_V22 | V90M_MOD_V21 | V90M_MOD_BELL103, 30)) {
        v90m_get_state(b.m, &st);
        CHECK_EQ_I(st.connected, 0); CHECK_EQ_I(st.modulation, V90M_CONN_NONE);
        bell_done(&b);
    }
}

/* ---- Bell calls from the API: an API caller and an API answerer back to back ---- */
static int bell_orig(unsigned ours, unsigned theirs, int seconds, v90m_state *sc, v90m_state *sa, char *got_c, char *got_a)
{
    static end_t cl, an;
    int16_t c_rx = 0, a_rx = 0;
    size_t nc = 0, na = 0;
    int k, k_wr = -1;
    end_init(&cl, V90M_ROLE_ORIGINATE, ours);
    end_init(&an, V90M_ROLE_ANSWER, theirs);
    if (!cl.m || !an.m) return 0;
    for (k = 0; k < 8000 * seconds; k++) {
        int16_t c_tx, a_tx;
        v90m_process_linear(cl.m, &c_rx, &c_tx, 1);
        v90m_process_linear(an.m, &a_rx, &a_tx, 1);
        a_rx = (int16_t)(c_tx + a_tx / 5); c_rx = (int16_t)(a_tx + c_tx / 5);     /* and a fifth of each one's own signal */
        v90m_get_state(cl.m, sc); v90m_get_state(an.m, sa);
        if (k_wr < 0 && sc->moving && sa->moving) {
            k_wr = k + 4000;
        }
        if (k == k_wr) {
            v90m_async_write(cl.a, (const uint8_t *)"from the caller", 15);
            v90m_async_write(an.a, (const uint8_t *)"from the answerer", 17);
        }
        nc += v90m_async_read(cl.a, (uint8_t *)got_c + nc, 63 - nc);
        na += v90m_async_read(an.a, (uint8_t *)got_a + na, 63 - na);
    }
    got_c[nc] = 0; got_a[na] = 0;
    v90m_get_state(cl.m, sc); v90m_get_state(an.m, sa);
    {
        v90m_stats st;
        v90m_get_stats(cl.m, &st);
        if (sc->connected) {
            CHECK_EQ_I(st.modulation, sc->modulation);
            if (sc->modulation == V90M_CONN_BELL103) CHECK_EQ_I(st.snr_db10, -1);
            if (sc->modulation == V90M_CONN_BELL103 || sc->modulation == V90M_CONN_BELL212A) {
                CHECK_EQ_I(v90m_retrain(cl.m), V90M_ESTATE);      /* neither has a retrain */
                CHECK_EQ_I(v90m_cleardown(cl.m), V90M_ESTATE);
            }
        }
    }
    v90m_destroy(cl.m); v90m_destroy(an.m); v90m_async_destroy(cl.a); v90m_async_destroy(an.a);
    return 1;
}

static void test_bell_originate(void)
{
    const unsigned theirs = V90M_MOD_V22 | V90M_MOD_V21 | V90M_MOD_BELL103 | V90M_MOD_BELL212A;
    v90m_state sc, sa; char got_c[64], got_a[64];

    if (bell_orig(V90M_MOD_BELL103, theirs, 14, &sc, &sa, got_c, got_a)) {
        printf("  API Bell 103 caller: %d mod %d %d/%d bit/s (V.8 %d), answerer mod %d; caller got \"%s\", answerer got \"%s\"\n",
               sc.connected, sc.modulation, sc.tx_bps, sc.rx_bps, sc.used_v8, sa.modulation, got_c, got_a);
        CHECK_EQ_I(sc.connected, 1); CHECK_EQ_I(sc.modulation, V90M_CONN_BELL103);
        CHECK_EQ_I(sc.tx_bps, 300); CHECK_EQ_I(sc.rx_bps, 300); CHECK_EQ_I(sc.used_v8, 0); CHECK_EQ_I(sc.moving, 1);
        CHECK_EQ_I(sa.modulation, V90M_CONN_BELL103);
        CHECK(strcmp(got_c, "from the answerer") == 0); CHECK(strcmp(got_a, "from the caller") == 0);
    }
    if (bell_orig(V90M_MOD_BELL212A | V90M_MOD_BELL103, theirs, 18, &sc, &sa, got_c, got_a)) {
        printf("  API Bell 212A caller: %d mod %d %d/%d bit/s (V.8 %d), answerer mod %d; caller got \"%s\", answerer got \"%s\"\n",
               sc.connected, sc.modulation, sc.tx_bps, sc.rx_bps, sc.used_v8, sa.modulation, got_c, got_a);
        CHECK_EQ_I(sc.connected, 1); CHECK_EQ_I(sc.modulation, V90M_CONN_BELL212A);
        CHECK_EQ_I(sc.tx_bps, 1200); CHECK_EQ_I(sc.rx_bps, 1200); CHECK_EQ_I(sc.used_v8, 0); CHECK_EQ_I(sc.moving, 1);
        CHECK_EQ_I(sa.modulation, V90M_CONN_BELL212A);
        CHECK(strcmp(got_c, "from the answerer") == 0); CHECK(strcmp(got_a, "from the caller") == 0);
    }
    /* Bell and V.22 offered, the answerer speaks V.8: V.8 decides */
    if (bell_orig(V90M_MOD_V22 | V90M_MOD_V21 | V90M_MOD_BELL212A | V90M_MOD_BELL103, theirs, 10, &sc, &sa, got_c, got_a)) {
        printf("  API caller with V.22 and Bell: mod %d %d bit/s (V.8 %d)\n", sc.modulation, sc.tx_bps, sc.used_v8);
        CHECK_EQ_I(sc.modulation, V90M_CONN_V22BIS); CHECK_EQ_I(sc.used_v8, 1);
        CHECK(strcmp(got_c, "from the answerer") == 0); CHECK(strcmp(got_a, "from the caller") == 0);
    }
}

/* ---- x2: the simulated client (sim/x2call) calls the library ---- */
#define NEV 16
static int x2_ev[NEV], x2_nev;
static void x2_on_event(void *user, v90m_event ev) { (void)user; if (x2_nev < NEV) x2_ev[x2_nev] = (int)ev; x2_nev++; }
static int ones_get(void *u) { (void)u; return 1; }
static void bits_put(void *u, int b) { (void)u; (void)b; }

static void test_x2(void)
{
    static x2call_t x;
    static uint8_t down[64];
    x2call_cfg xc; v90m_cfg c; v90m_state st; v90m_stats sx;
    v90m_t *m; int err;
    long n, data_at = -1;
    uint8_t rx = 0xff, tx;

    v90m_cfg_default(&c);
    c.modulations |= V90M_MOD_X2;
    c.getbit = ones_get; c.putbit = bits_put; c.on_event = x2_on_event;
    m = v90m_create(&c, &err);
    CHECK(m != NULL);
    if (!m) return;
    memset(&xc, 0, sizeof xc);
    xc.level = 3000; xc.max_index = 1; xc.up_mask = 0x03FE;
    xc.getbit = ones_get; xc.putbit = bits_put;
    x2call_init(&x, &xc);
    memset(down, 0x7f, sizeof down);
    for (n = 0; n < 8000L * 20; n++) {
        int16_t s;
        v90m_process_g711(m, &rx, &tx, 1);
        down[n & 63] = tx;
        s = x2call_tick(&x, down[(n - 30) & 63]);
        rx = v90m_g711_encode(0, (int16_t)(s / 2));
        v90m_get_state(m, &st);
        if (st.connected && data_at < 0) data_at = n;
        if (data_at >= 0 && n == data_at + 8000) {
            /* x2 has no cleardown signal: we go silent, the call ends as ours */
            v90m_get_stats(m, &sx);
            printf("  API x2 call: mod %d, %d / %d bit/s, SNR %.1f dB, rtd %d, echo delay %d\n", st.modulation,
                   st.tx_bps, st.rx_bps, sx.snr_db10 / 10.0, sx.rtd, sx.echo_delay);
            CHECK_EQ_I(st.modulation, V90M_CONN_X2); CHECK_EQ_I(sx.modulation, V90M_CONN_X2);
            CHECK_EQ_I(st.tx_bps, 32000); CHECK_EQ_I(st.rx_bps, 24000);
            CHECK_EQ_I(st.moving, 1); CHECK_EQ_I(st.used_v8, 1); CHECK_EQ_I(st.end, V90M_END_NONE);
            CHECK(sx.snr_db10 > 200); CHECK(sx.rtd > 0); CHECK(sx.echo_delay >= 0);
            CHECK_EQ_I(v90m_cleardown(m), V90M_OK);
        }
        if (st.end != V90M_END_NONE) break;
    }
    v90m_get_state(m, &st);
    CHECK(data_at > 0);
    CHECK_EQ_I(st.end, V90M_END_CLEARDOWN_OURS);
    CHECK(x2_nev >= 3);
    CHECK_EQ_I(x2_ev[0], V90M_EV_CONNECTED); CHECK_EQ_I(x2_ev[1], V90M_EV_DATA_MOVING);
    CHECK_EQ_I(x2_ev[x2_nev - 1], V90M_EV_ENDED);
    CHECK_EQ_I(v90m_cleardown(m), V90M_ESTATE);           /* over: nothing to send */
    v90m_destroy(m);
}

/* ---- K56flex: the simulated client (sim/k56call) calls the library. Its
 * INFO1c takes K56flex up at 3200 baud: our INFO1a goes out flagged, and
 * K56flex's start-up to data mode, 32000 down (it asks r = 1). There we
 * never retrain (V90M_ESTATE); our cleardown ends the call as ours. With
 * the client silent in data mode, the call ends as the line's loss. ---- */
static void k56_call(int silent, long *data_at, v90m_state *st, v90m_stats *sx, int *rt)
{
    static k56call_t k;
    static uint8_t down[64];
    k56call_cfg kc; v90m_cfg c;
    v90m_t *m; int err;
    long n;
    uint8_t rx = 0xff, tx;

    v90m_cfg_default(&c);
    c.modulations |= V90M_MOD_K56;
    c.getbit = ones_get; c.putbit = bits_put; c.on_event = x2_on_event;
    x2_nev = 0; *data_at = -1; *rt = 0;
    memset(sx, 0, sizeof *sx);
    m = v90m_create(&c, &err);
    CHECK(m != NULL);
    if (!m) return;
    k56call_default(&kc, 3000);
    kc.rate = 1; kc.silent = silent;
    kc.getbit = ones_get; kc.putbit = bits_put;
    k56call_init(&k, &kc);
    memset(down, 0xff, sizeof down);
    for (n = 0; n < 8000L * 40; n++) {
        int16_t s;
        v90m_process_g711(m, &rx, &tx, 1);
        down[n & 63] = tx;
        s = k56call_tick(&k, down[(n - 30) & 63]);
        rx = v90m_g711_encode(0, (int16_t)(s / 2));
        v90m_get_state(m, st);
        if (st->connected && *data_at < 0) *data_at = n;
        if (!silent && *data_at >= 0 && n == *data_at + 8000) {
            v90m_get_stats(m, sx);
            *rt = v90m_retrain(m);
            CHECK_EQ_I(v90m_cleardown(m), V90M_OK);
            /* silence at once */
            v90m_process_g711(m, &rx, &tx, 1);
            CHECK_EQ_I(tx, 0xff);
        }
        if (st->end != V90M_END_NONE) break;
    }
    v90m_get_state(m, st);
    if (!silent) CHECK_EQ_I(v90m_cleardown(m), V90M_ESTATE);    /* over: nothing to send */
    v90m_destroy(m);
}

static void test_k56(void)
{
    v90m_state st; v90m_stats sx;
    long data_at;
    int rt;

    k56_call(K56C_SIL_NONE, &data_at, &st, &sx, &rt);
    printf("  API K56flex call: mod %d, %d / %d bit/s, SNR %.1f dB, rtd %d, echo delay %d; retrain %d; end %d\n",
           sx.modulation, sx.tx_bps, sx.rx_bps, sx.snr_db10 / 10.0, sx.rtd, sx.echo_delay, rt, (int)st.end);
    CHECK(data_at > 0);
    CHECK_EQ_I(sx.modulation, V90M_CONN_K56FLEX);
    CHECK_EQ_I(sx.tx_bps, 32000); CHECK_EQ_I(sx.rx_bps, 31200);
    CHECK(sx.snr_db10 > 200); CHECK(sx.rtd > 0); CHECK(sx.echo_delay >= 0);
    CHECK_EQ_I(rt, V90M_ESTATE);
    CHECK_EQ_I(st.end, V90M_END_CLEARDOWN_OURS); CHECK_EQ_I(st.used_v8, 1);
    CHECK(x2_nev >= 3 && x2_nev <= NEV);
    if (x2_nev >= 3 && x2_nev <= NEV) {
        CHECK_EQ_I(x2_ev[0], V90M_EV_CONNECTED); CHECK_EQ_I(x2_ev[1], V90M_EV_DATA_MOVING);
        CHECK_EQ_I(x2_ev[x2_nev - 1], V90M_EV_ENDED);
    }

    k56_call(K56C_SIL_DATA, &data_at, &st, &sx, &rt);
    printf("  API K56flex call, the client silent in data mode: end %d\n", (int)st.end);
    CHECK(data_at > 0);
    CHECK_EQ_I(st.end, V90M_END_LOST);
}

/* K56flex: the client's renegotiations (sim/k56call), 1 s into data mode
 * from r = 13 to 10, 1 s into that data to 12: each gives DATA_STOPPED,
 * RATES (tx_bps 50000, then 54000), DATA_MOVING; then our cleardown. Its
 * renegotiation MP with rate 0 ends the call as the far end's cleardown;
 * the client silent after its renegotiation's S, as K56flex's failure.
 * TEST_SHORT (32-bit ARM under qemu, test/test.h): the one to 10 only, and
 * not the silent client (test_answer_k56 has it). */
#ifdef TEST_SHORT
#define K56_RENEGS 1
#else
#define K56_RENEGS 2
#endif
static void k56_reneg_call(int mode, v90m_state *st, int *bps, int *nbps)
{
    static k56call_t k;
    static uint8_t down[64];
    k56call_cfg kc; v90m_cfg c;
    v90m_t *m; int err, nmove = 0, last = 0;
    long n, cd_at = -1;
    uint8_t rx = 0xff, tx;

    v90m_cfg_default(&c);
    c.modulations |= V90M_MOD_K56;
    c.getbit = ones_get; c.putbit = bits_put; c.on_event = x2_on_event;
    x2_nev = 0; *nbps = 0;
    m = v90m_create(&c, &err);
    CHECK(m != NULL);
    if (!m) return;
    k56call_default(&kc, 3000);
    kc.rate = mode ? 1 : 13;
    kc.reneg_ms = 1000; kc.reneg_rate = mode ? 1 : 10;
    if (!mode && K56_RENEGS == 2) { kc.reneg_ms2 = 1000; kc.reneg_rate2 = 12; }
    kc.reneg_rate0 = mode == 1;
    if (mode == 2) kc.silent = K56C_SIL_RENEG;
    kc.getbit = ones_get; kc.putbit = bits_put;
    k56call_init(&k, &kc);
    memset(down, 0xff, sizeof down);
    for (n = 0; n < 8000L * 50; n++) {
        int16_t s;
        v90m_process_g711(m, &rx, &tx, 1);
        down[n & 63] = tx;
        s = k56call_tick(&k, down[(n - 30) & 63]);
        rx = v90m_g711_encode(0, (int16_t)(s / 2));
        v90m_get_state(m, st);
        for (; last < x2_nev && last < NEV; last++) {
            if (x2_ev[last] == V90M_EV_RATES && *nbps < 4) bps[(*nbps)++] = st->tx_bps;
            if (x2_ev[last] == V90M_EV_DATA_MOVING && ++nmove == K56_RENEGS + 1) cd_at = n + 8000;
        }
        if (n == cd_at) CHECK_EQ_I(v90m_cleardown(m), V90M_OK);
        if (st->end != V90M_END_NONE) break;
    }
    v90m_get_state(m, st);
    v90m_destroy(m);
}

static void test_k56_reneg(void)
{
    /* CONNECTED, DATA_MOVING; DATA_STOPPED, RATES, DATA_MOVING for each
     * renegotiation; DATA_STOPPED, ENDED at our cleardown */
    enum { NWANT = 2 + 3 * K56_RENEGS + 2 };
    static const int bps_want[2] = { 50000, 54000 };
    int want[NWANT], nw = 0;
    v90m_state st;
    int bps[4], nbps, k;

    want[nw++] = V90M_EV_CONNECTED; want[nw++] = V90M_EV_DATA_MOVING;
    for (k = 0; k < K56_RENEGS; k++) {
        want[nw++] = V90M_EV_DATA_STOPPED; want[nw++] = V90M_EV_RATES; want[nw++] = V90M_EV_DATA_MOVING;
    }
    want[nw++] = V90M_EV_DATA_STOPPED; want[nw++] = V90M_EV_ENDED;

    k56_reneg_call(0, &st, bps, &nbps);
    printf("  API K56flex call, the client's renegotiations to %s: %d events (",
           K56_RENEGS == 2 ? "10 and 12" : "10", x2_nev);
    for (k = 0; k < x2_nev && k < NEV; k++) printf("%s%d", k ? " " : "", x2_ev[k]);
    printf("), tx_bps at RATES");
    for (k = 0; k < nbps; k++) printf("%s %d", k ? "," : "", bps[k]);
    printf("; end %d\n", (int)st.end);
    CHECK_EQ_I(x2_nev, NWANT);
    for (k = 0; k < NWANT && k < x2_nev; k++) CHECK_EQ_I(x2_ev[k], want[k]);
    CHECK_EQ_I(nbps, K56_RENEGS);
    for (k = 0; k < K56_RENEGS && k < nbps; k++) CHECK_EQ_I(bps[k], bps_want[k]);
    CHECK_EQ_I(st.end, V90M_END_CLEARDOWN_OURS);

    k56_reneg_call(1, &st, bps, &nbps);
    printf("  API K56flex call, a renegotiation MP with rate 0: end %d\n", (int)st.end);
    CHECK_EQ_I(st.end, V90M_END_CLEARDOWN_FAR);
    CHECK(x2_nev >= 4 && x2_ev[2] == V90M_EV_DATA_STOPPED);

#ifndef TEST_SHORT
    k56_reneg_call(2, &st, bps, &nbps);
    printf("  API K56flex call, the client silent after its renegotiation's S: end %d\n", (int)st.end);
    CHECK_EQ_I(st.end, V90M_END_K56_FAILED);
    CHECK(x2_nev >= 4 && x2_ev[2] == V90M_EV_DATA_STOPPED);
#endif
}

/* K56flex: the client's retrain (sim/k56call) 1 s into data mode: DATA_STOPPED
 * (retraining 1, not a renegotiation), DATA_MOVING again at the same rates,
 * retrains 1; v90m_retrain in it V90M_ESTATE (we never retrain); then our
 * cleardown. Three in a row, 300 ms into data mode each: the third ends the
 * call as K56flex's (not under TEST_SHORT: test_answer_k56 has it on the
 * host). */
static void k56_retrain_call(int three, v90m_state *st, v90m_stats *sx, int *rt_ret, int *rt_seen)
{
    static k56call_t k;
    static uint8_t down[64];
    k56call_cfg kc; v90m_cfg c;
    v90m_t *m; int err, nmove = 0, last = 0;
    long n, cd_at = -1;
    uint8_t rx = 0xff, tx;

    v90m_cfg_default(&c);
    c.modulations |= V90M_MOD_K56;
    c.getbit = ones_get; c.putbit = bits_put; c.on_event = x2_on_event;
    x2_nev = 0; *rt_ret = 1; *rt_seen = 0;
    m = v90m_create(&c, &err);
    CHECK(m != NULL);
    if (!m) return;
    k56call_default(&kc, 3000);
    kc.rate = 1;
    kc.retrain_ms = three ? 300 : 1000; kc.retrains = three ? 3 : 1;
    kc.getbit = ones_get; kc.putbit = bits_put;
    k56call_init(&k, &kc);
    memset(down, 0xff, sizeof down);
    for (n = 0; n < 8000L * 60; n++) {
        int16_t s;
        v90m_process_g711(m, &rx, &tx, 1);
        down[n & 63] = tx;
        s = k56call_tick(&k, down[(n - 30) & 63]);
        rx = v90m_g711_encode(0, (int16_t)(s / 2));
        v90m_get_state(m, st);
        if (st->retraining && !*rt_seen) { *rt_seen = 1; *rt_ret = v90m_retrain(m); }
        for (; last < x2_nev && last < NEV; last++)
            if (x2_ev[last] == V90M_EV_DATA_MOVING && ++nmove == 2 && !three) cd_at = n + 8000;
        if (n == cd_at) CHECK_EQ_I(v90m_cleardown(m), V90M_OK);
        if (st->end != V90M_END_NONE) break;
    }
    v90m_get_state(m, st);
    v90m_get_stats(m, sx);
    v90m_destroy(m);
}

static void test_k56_retrain(void)
{
    static const int want[6] = { V90M_EV_CONNECTED, V90M_EV_DATA_MOVING, V90M_EV_DATA_STOPPED,
                                 V90M_EV_DATA_MOVING, V90M_EV_DATA_STOPPED, V90M_EV_ENDED };
    v90m_state st; v90m_stats sx;
    int rt_ret, rt_seen, k;

    k56_retrain_call(0, &st, &sx, &rt_ret, &rt_seen);
    printf("  API K56flex call, the client's retrain: %d events (", x2_nev);
    for (k = 0; k < x2_nev && k < NEV; k++) printf("%s%d", k ? " " : "", x2_ev[k]);
    printf("), retrains %u, renegotiations %u, v90m_retrain in it %d; end %d\n", sx.retrains,
           sx.renegotiations, rt_ret, (int)st.end);
    CHECK_EQ_I(x2_nev, 6);
    for (k = 0; k < 6 && k < x2_nev; k++) CHECK_EQ_I(x2_ev[k], want[k]);
    CHECK_EQ_I(rt_seen, 1); CHECK_EQ_I(rt_ret, V90M_ESTATE);
    CHECK_EQ_I(sx.retrains, 1); CHECK_EQ_I(sx.renegotiations, 0);
    CHECK_EQ_I(st.end, V90M_END_CLEARDOWN_OURS);

#ifndef TEST_SHORT
    k56_retrain_call(1, &st, &sx, &rt_ret, &rt_seen);
    printf("  API K56flex call, three retrains in a row: retrains %u, end %d\n", sx.retrains, (int)st.end);
    CHECK_EQ_I(sx.retrains, 3);
    CHECK_EQ_I(st.end, V90M_END_K56_FAILED);
#endif
}

int main(void)
{
    test_cfg();
    test_errors();
    test_g711();
    test_call();
    test_blocks();
    test_ec();
    test_originate();
    test_bell();
    test_bell_originate();
    test_x2();
    test_k56();
    test_k56_reneg();
    test_k56_retrain();
    T_REPORT("api");
}
