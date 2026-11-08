/*
 * tests/test_trade_invite.c -- the in-world trade invitation, with no game
 * and no ROM.  Positions and the A press are supplied by the test.
 */
#include "trade_invite.h"
#include "port_test.h"

#include <stdio.h>
#include <string.h>

typedef struct Pipe {
    char line[8][80];
    u32  peer[8];
    int  n;
    struct Pipe *other;
    BwTradeInvite *dest;
    u32 from;
} Pipe;

static int pipe_send(void *user, u32 peer, const void *data, u32 n)
{
    Pipe *p = (Pipe *)user;

    (void)peer;
    if (p->n >= 8 || n >= 80u) {
        return -1;
    }
    memcpy(p->line[p->n], data, n);
    p->line[p->n][n] = '\0';
    p->peer[p->n] = peer;
    p->n++;
    return 0;
}

static void flush(Pipe *p)
{
    int i, n = p->n;

    p->n = 0;
    for (i = 0; i < n; i++) {
        bw_trade_invite_on_line(p->dest, p->from, p->line[i],
                               (u32)strlen(p->line[i]), 1000);
    }
}

static u32 seq(void *user)
{
    u32 *n = (u32 *)user;

    *n += 3u;
    return *n;
}

static int g_entered;
static u32 g_enter_peer;
static int g_we;

static void on_enter(void *user, u32 peer, int we)
{
    (void)user;
    g_entered++;
    g_enter_peer = peer;
    g_we = we;
}

static BwTradeInvite *open_one(Pipe *out, u32 self, u32 seed, u32 timeout)
{
    BwTradeInviteCfg cfg;
    static u32 seeds[4];

    memset(&cfg, 0, sizeof cfg);
    seeds[self] = seed;
    cfg.self_peer = self;
    cfg.timeout_ms = timeout;
    cfg.send = pipe_send;
    cfg.send_user = out;
    cfg.on_enter = on_enter;
    cfg.random = seq;
    cfg.random_user = &seeds[self];
    return bw_trade_invite_create(&cfg);
}

static BwTradeLook look_face(int quiet, int a, u32 partner)
{
    BwTradeLook L;

    memset(&L, 0, sizeof L);
    L.field_quiet = quiet;
    L.have_self = 1;
    L.x = 10;
    L.z = 10;
    L.dir = 3; /* right: the cell is 11,10 */
    L.a_edge = a;
    L.partner = partner;
    return L;
}

static void test_geometry(void)
{
    s16 x, z;

    printf("facing\n");
    bw_trade_face_cell(5, 5, 0, &x, &z);
    BW_CHECK_EQ("up z", z, 4);
    BW_CHECK_EQ("up x", x, 5);
    bw_trade_face_cell(5, 5, 1, &x, &z);
    BW_CHECK_EQ("down z", z, 6);
    bw_trade_face_cell(5, 5, 2, &x, &z);
    BW_CHECK_EQ("left x", x, 4);
    bw_trade_face_cell(5, 5, 3, &x, &z);
    BW_CHECK_EQ("right x", x, 6);
    BW_CHECK_EQ("faces that cell", bw_trade_facing_cell(5, 5, 3, 6, 5), 1);
    BW_CHECK_EQ("not the cell beside", bw_trade_facing_cell(5, 5, 3, 5, 6), 0);
    BW_CHECK_EQ("not two away", bw_trade_facing_cell(5, 5, 3, 7, 5), 0);
}

static int test_invite(void)
{
    Pipe ab, ba;
    BwTradeInvite *a, *b;
    BwTradeLook L;

    printf("invite\n");
    g_entered = 0;
    memset(&ab, 0, sizeof ab);
    memset(&ba, 0, sizeof ba);
    a = open_one(&ab, 1, 10, 1000);
    b = open_one(&ba, 2, 20, 1000);
    ab.dest = b;
    ab.from = 1;
    ba.dest = a;
    ba.from = 2;
    BW_REQUIRE("engines", a != NULL && b != NULL);

    L = look_face(0, 1, 2);
    BW_CHECK_EQ("npc or event blocks", bw_trade_invite_consider(a, &L, 100), -1);
    BW_CHECK_EQ("no send while busy field", ab.n, 0);

    L = look_face(1, 1, 0);
    BW_CHECK_EQ("empty cell", bw_trade_invite_consider(a, &L, 100), -1);

    L = look_face(1, 0, 2);
    BW_CHECK_EQ("no button", bw_trade_invite_consider(a, &L, 100), -1);

    L = look_face(1, 1, 2);
    L.trade_busy = 1;
    BW_CHECK_EQ("trade already open", bw_trade_invite_consider(a, &L, 100), -1);
    L.trade_busy = 0;

    BW_CHECK_EQ("ask", bw_trade_invite_consider(a, &L, 100), 0);
    BW_CHECK_EQ("waiting", bw_trade_invite_phase(a), BW_TINV_OUT);
    flush(&ab);
    BW_CHECK_EQ("prompt", bw_trade_invite_phase(b), BW_TINV_IN);
    BW_CHECK_EQ("german prompt", bw_trade_invite_prompt(b) != NULL, 1);
    /* a repeated copy of the same invitation does not stack */
    flush(&ab);
    BW_CHECK_EQ("still one", bw_trade_invite_phase(b), BW_TINV_IN);

    L = look_face(1, 1, 1);
    L.a_edge = 0;
    L.accept_edge = 1;
    bw_trade_invite_consider(b, &L, 200);
    BW_CHECK_EQ("accepted", bw_trade_invite_phase(b), BW_TINV_ENTERED);
    BW_CHECK_EQ("screen", bw_trade_invite_wants_trade_screen(b), 1);
    flush(&ba);
    BW_CHECK_EQ("inviter entered", bw_trade_invite_phase(a), BW_TINV_ENTERED);
    BW_CHECK_EQ("both entered", g_entered, 2);
    BW_CHECK_EQ("peer", g_enter_peer, 2);
    /* no second swap-like enter */
    bw_trade_invite_accept(b);
    BW_CHECK_EQ("accept twice ignored", g_entered, 2);

    bw_trade_invite_destroy(a);
    bw_trade_invite_destroy(b);
    return 0;
}

static int test_decline_timeout_stale(void)
{
    Pipe ab, ba;
    BwTradeInvite *a, *b;
    BwTradeLook L;
    char stale[64];

    printf("decline, timeout, stale\n");
    g_entered = 0;
    memset(&ab, 0, sizeof ab);
    memset(&ba, 0, sizeof ba);
    a = open_one(&ab, 1, 1, 500);
    b = open_one(&ba, 2, 2, 500);
    ab.dest = b;
    ab.from = 1;
    ba.dest = a;
    ba.from = 2;
    L = look_face(1, 1, 2);
    BW_CHECK_EQ("ask", bw_trade_invite_consider(a, &L, 1000), 0);
    flush(&ab);
    L.accept_edge = 0;
    L.a_edge = 0;
    L.decline_edge = 1;
    L.partner = 1;
    bw_trade_invite_consider(b, &L, 1100);
    BW_CHECK_EQ("declined", bw_trade_invite_phase(b), BW_TINV_IDLE);
    flush(&ba);
    BW_CHECK_EQ("inviter hears decline", bw_trade_invite_phase(a), BW_TINV_IDLE);
    BW_CHECK_EQ("no enter", g_entered, 0);

    L = look_face(1, 1, 2);
    BW_CHECK_EQ("ask again", bw_trade_invite_consider(a, &L, 2000), 0);
    BW_CHECK_EQ("out", bw_trade_invite_phase(a), BW_TINV_OUT);
    flush(&ab);
    snprintf(stale, sizeof stale, "ti1 a 1");
    bw_trade_invite_on_line(b, 1, stale, (u32)strlen(stale), 2100);
    BW_CHECK_EQ("stale ignored", bw_trade_invite_phase(b), BW_TINV_IN);
    bw_trade_invite_poll(a, 2000 + 500);
    BW_CHECK_EQ("timeout", bw_trade_invite_phase(a), BW_TINV_IDLE);
    bw_trade_invite_on_leave(b, 1);
    BW_CHECK_EQ("leave clears", bw_trade_invite_phase(b), BW_TINV_IDLE);
    BW_CHECK_EQ("still no enter", g_entered, 0);

    bw_trade_invite_destroy(a);
    bw_trade_invite_destroy(b);
    return 0;
}

static int test_tie_and_repeat(void)
{
    Pipe ab, ba;
    BwTradeInvite *a, *b;
    BwTradeLook La, Lb;

    printf("simultaneous\n");
    g_entered = 0;
    memset(&ab, 0, sizeof ab);
    memset(&ba, 0, sizeof ba);
    a = open_one(&ab, 1, 4, 5000);
    b = open_one(&ba, 2, 8, 5000);
    ab.dest = b;
    ab.from = 1;
    ba.dest = a;
    ba.from = 2;
    La = look_face(1, 1, 2);
    Lb = look_face(1, 1, 1);
    BW_CHECK_EQ("a asks", bw_trade_invite_consider(a, &La, 100), 0);
    BW_CHECK_EQ("b asks", bw_trade_invite_consider(b, &Lb, 100), 0);
    flush(&ab);
    flush(&ba);
    /* peer 1 is lower, so peer 1's invitation stands and peer 2 answers it */
    BW_CHECK_EQ("lower stays out", bw_trade_invite_phase(a), BW_TINV_OUT);
    BW_CHECK_EQ("higher answers", bw_trade_invite_phase(b), BW_TINV_IN);
    bw_trade_invite_accept(b);
    flush(&ba);
    BW_CHECK_EQ("tie entered", g_entered >= 2, 1);
    BW_CHECK_EQ("a entered", bw_trade_invite_phase(a), BW_TINV_ENTERED);

    bw_trade_invite_destroy(a);
    bw_trade_invite_destroy(b);
    return 0;
}

int main(void)
{
    test_geometry();
    test_invite();
    test_decline_timeout_stale();
    test_tie_and_repeat();
    BW_REPORT("test_trade_invite");
    return bw_failures == 0 ? 0 : 1;
}
