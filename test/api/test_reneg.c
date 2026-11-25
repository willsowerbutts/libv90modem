/* A rate renegotiation counts in v90m_stats.renegotiations (V.34 11.6,
 * V.90 9.6.2.1, V.92), asked for by our caller through the engine
 * (orig_renegotiate; the public API has no call for it), on both ends and
 * not as a retrain: once data moves again; and when the line is cut during
 * it, as the call ends (a lost call), where a cleardown does not count
 * (test_api checks those). Linked with the development archive alone, for
 * the engine's handles (api/v90modem_priv.h). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "v90modem.h"
#include "test.h"
#include "../../api/v90modem_priv.h"

typedef struct { v90m_t *m; v90m_async_t *a; } end_t;
static int end_get(void *u) { return v90m_async_getbit(((end_t *)u)->a); }
static void end_put(void *u, int b) { v90m_async_putbit(((end_t *)u)->a, b); }

static void end_init(end_t *e, v90m_role role, unsigned mods)
{
    v90m_cfg c; int err;
    memset(e, 0, sizeof *e);
    e->a = v90m_async_create(&err);
    v90m_cfg_default(&c);
    c.role = role; c.modulations = mods;
    c.getbit = end_get; c.putbit = end_put; c.user = e;
    e->m = v90m_create(&c, &err);
    CHECK(e->m != NULL);
}

static void done(end_t *cl, end_t *an)
{
    v90m_destroy(cl->m); v90m_destroy(an->m); v90m_async_destroy(cl->a); v90m_async_destroy(an->a);
}

/* cut: 0, the renegotiation completes; 1, the line goes silent 100 ms into it */
static void reneg_call(const char *name, unsigned cmods, unsigned amods, int conn, int max_rx, int cut)
{
    static end_t cl, an;
    int16_t c_rx = 0, a_rx = 0;
    int k, k_rn = -1, k_cut = -1, stopped = 0, again = 0, k_end = -1, ended = 0;
    v90m_stats sc, sa;
    v90m_state s1, s2;
    end_init(&cl, V90M_ROLE_ORIGINATE, cmods);
    end_init(&an, V90M_ROLE_ANSWER, amods);
    if (cl.m && an.m) {
        for (k = 0; k < 8000 * 70; k++) {
            int16_t c_tx, a_tx;
            v90m_process_linear(cl.m, &c_rx, &c_tx, 1);
            v90m_process_linear(an.m, &a_rx, &a_tx, 1);
            a_rx = k_cut >= 0 && k >= k_cut ? 0 : c_tx;
            c_rx = k_cut >= 0 && k >= k_cut ? 0 : a_tx;
            v90m_get_state(cl.m, &s1); v90m_get_state(an.m, &s2);
            if (k_rn < 0 && s1.moving && s2.moving && s1.modulation == (v90m_conn)conn) k_rn = k + 8000;
            if (k == k_rn) CHECK_EQ_I(orig_renegotiate(v90m__orig(cl.m), max_rx), 0);
            if (k_rn >= 0 && k > k_rn && !s1.moving && !s1.retraining && !stopped) {
                stopped = 1;
                if (cut) k_cut = k + 800;
            }
            if (!cut && stopped && !again && s1.moving && s2.moving) {
                again = 1; k_end = k + 8000;
                v90m_get_stats(cl.m, &sc); v90m_get_stats(an.m, &sa);
                printf("  %s renegotiation: caller %d/%d bit/s, renegotiations %u / %u, retrains %u / %u\n", name,
                       s1.tx_bps, s1.rx_bps, sc.renegotiations, sa.renegotiations, sc.retrains, sa.retrains);
                CHECK_EQ_I(sc.renegotiations, 1); CHECK_EQ_I(sa.renegotiations, 1);
                CHECK_EQ_I(sc.retrains, 0); CHECK_EQ_I(sa.retrains, 0);
                /* the caller asked; only V.34 says so */
                CHECK_EQ_I(sc.stop_by, conn == V90M_CONN_V34 ? V90M_SIDE_NEAR : V90M_SIDE_NONE);
                CHECK_EQ_I(sa.stop_by, conn == V90M_CONN_V34 ? V90M_SIDE_FAR : V90M_SIDE_NONE);
                CHECK_EQ_I(sc.stop_why, conn == V90M_CONN_V34 ? V90M_STOP_ASKED : V90M_STOP_NONE);
                CHECK_EQ_I(sa.stop_why, conn == V90M_CONN_V34 ? V90M_STOP_ASKED : V90M_STOP_NONE);
            }
            if (again && k == k_end) break;
            if (cut && k_cut >= 0 && s1.end != V90M_END_NONE && s2.end != V90M_END_NONE) { ended = 1; break; }
        }
        CHECK(stopped);
        if (cut) {
            v90m_get_stats(cl.m, &sc); v90m_get_stats(an.m, &sa);
            printf("  %s renegotiation cut off: ends %d / %d, renegotiations %u / %u\n", name, s1.end, s2.end,
                   sc.renegotiations, sa.renegotiations);
            CHECK(ended);
            CHECK(s1.end != V90M_END_CLEARDOWN_OURS && s1.end != V90M_END_CLEARDOWN_FAR);
            CHECK_EQ_I(sc.renegotiations, 1); CHECK_EQ_I(sa.renegotiations, 1);
        } else CHECK(again);
    }
    done(&cl, &an);
}

int main(void)
{
    int cut;
    for (cut = 0; cut < 2; cut++) {
        reneg_call("V.34", V90M_MOD_V34 | V90M_MOD_V22, V90M_MOD_V34 | V90M_MOD_V22, V90M_CONN_V34, 16800, cut);
        reneg_call("V.90", V90M_MOD_V90 | V90M_MOD_V34, V90M_MOD_V90 | V90M_MOD_V34, V90M_CONN_V90, 36000, cut);
        reneg_call("V.92", V90M_MOD_V92 | V90M_MOD_V90 | V90M_MOD_V34, V90M_MOD_V92 | V90M_MOD_V90 | V90M_MOD_V34,
                   V90M_CONN_V92, 36000, cut);
    }
    T_REPORT("renegotiations");
}
