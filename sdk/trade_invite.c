/*
 * sdk/trade_invite.c -- the invitation in front of a session trade.
 *
 * One invitation at a time.  A second one while this side is waiting is
 * declined, unless both asked each other: the lower peer id keeps its
 * invitation and the other is declined.  An id that is not the current
 * one is ignored.  Accepting only opens the trade; the offers still have
 * to be confirmed byte for byte.
 */
#include "trade_invite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_TIMEOUT_MS 15000u

struct BwTradeInvite {
    BwTradeInviteCfg cfg;
    int phase;
    u32 peer;
    u32 id;
    u64 until;
    u32 rng;
    int entered;
    char status[96];
};

static void set_status(BwTradeInvite *t, const char *s)
{
    snprintf(t->status, sizeof t->status, "%s", s != NULL ? s : "");
}

static u32 rnd(BwTradeInvite *t)
{
    if (t->cfg.random != NULL) {
        return t->cfg.random(t->cfg.random_user);
    }
    t->rng = t->rng * 1664525u + 1013904223u;
    return t->rng;
}

static int send_line(BwTradeInvite *t, u32 peer, const char *line)
{
    if (t->cfg.send == NULL || peer == 0u) {
        return -1;
    }
    return t->cfg.send(t->cfg.send_user, peer, line, (u32)strlen(line));
}

static void go_idle(BwTradeInvite *t, const char *why)
{
    t->phase = BW_TINV_IDLE;
    t->peer = 0;
    t->id = 0;
    t->until = 0;
    t->entered = 0;
    set_status(t, why);
}

BwTradeInvite *bw_trade_invite_create(const BwTradeInviteCfg *cfg)
{
    BwTradeInvite *t;

    if (cfg == NULL || cfg->send == NULL || cfg->self_peer == 0u) {
        return NULL;
    }
    t = (BwTradeInvite *)calloc(1, sizeof *t);
    if (t == NULL) {
        return NULL;
    }
    t->cfg = *cfg;
    if (t->cfg.timeout_ms == 0u) {
        t->cfg.timeout_ms = DEFAULT_TIMEOUT_MS;
    }
    t->rng = 0xC0FFEEu ^ cfg->self_peer;
    set_status(t, "");
    return t;
}

void bw_trade_invite_destroy(BwTradeInvite *t)
{
    free(t);
}

void bw_trade_face_cell(s16 x, s16 z, u8 dir, s16 *ox, s16 *oz)
{
    s16 nx = x, nz = z;

    switch (dir) {
    case 0: nz = (s16)(z - 1); break; /* up:    z decreases */
    case 1: nz = (s16)(z + 1); break; /* down */
    case 2: nx = (s16)(x - 1); break; /* left */
    case 3: nx = (s16)(x + 1); break; /* right */
    default: break;
    }
    if (ox != NULL) {
        *ox = nx;
    }
    if (oz != NULL) {
        *oz = nz;
    }
}

int bw_trade_facing_cell(s16 x, s16 z, u8 dir, s16 ax, s16 az)
{
    s16 fx, fz;

    bw_trade_face_cell(x, z, dir, &fx, &fz);
    return fx == ax && fz == az;
}

static int begin_out(BwTradeInvite *t, u32 peer, u64 now)
{
    char line[64];
    u32 id = rnd(t) | 1u;

    snprintf(line, sizeof line, "ti1 i %u %u", id, t->cfg.self_peer);
    if (send_line(t, peer, line) != 0) {
        set_status(t, "Die Einladung konnte nicht gesendet werden.");
        return -1;
    }
    t->phase = BW_TINV_OUT;
    t->peer = peer;
    t->id = id;
    t->until = now + t->cfg.timeout_ms;
    t->entered = 0;
    set_status(t, "Tausch angeboten. Warte auf Antwort.");
    return 0;
}

static void enter(BwTradeInvite *t, int we_invited)
{
    u32 peer = t->peer;

    t->phase = BW_TINV_ENTERED;
    t->entered = 1;
    t->until = 0;
    set_status(t, "Tausch geoeffnet. Beide muessen das Angebot bestaetigen.");
    if (t->cfg.on_enter != NULL) {
        t->cfg.on_enter(t->cfg.enter_user, peer, we_invited);
    }
}

void bw_trade_invite_accept(BwTradeInvite *t)
{
    char line[64];

    if (t == NULL || t->phase != BW_TINV_IN) {
        return;
    }
    snprintf(line, sizeof line, "ti1 a %u", t->id);
    if (send_line(t, t->peer, line) != 0) {
        set_status(t, "Die Antwort konnte nicht gesendet werden.");
        return;
    }
    enter(t, 0);
}

void bw_trade_invite_decline(BwTradeInvite *t)
{
    char line[64];

    if (t == NULL || t->phase != BW_TINV_IN) {
        return;
    }
    snprintf(line, sizeof line, "ti1 d %u", t->id);
    (void)send_line(t, t->peer, line);
    go_idle(t, "Tausch abgelehnt.");
}

void bw_trade_invite_cancel(BwTradeInvite *t)
{
    char line[64];

    if (t == NULL || t->phase != BW_TINV_OUT) {
        return;
    }
    snprintf(line, sizeof line, "ti1 c %u", t->id);
    (void)send_line(t, t->peer, line);
    go_idle(t, "Einladung zurueckgezogen.");
}

static void decline_id(BwTradeInvite *t, u32 peer, u32 id)
{
    char line[64];

    snprintf(line, sizeof line, "ti1 d %u", id);
    (void)send_line(t, peer, line);
}

void bw_trade_invite_on_line(BwTradeInvite *t, u32 from, const char *line, u32 n, u64 now_ms)
{
    char buf[80];
    char kind;
    unsigned id = 0, claimed = 0;

    (void)now_ms;
    if (t == NULL || line == NULL || n < 5u || from == 0u || from == t->cfg.self_peer) {
        return;
    }
    if (n >= sizeof buf) {
        n = sizeof buf - 1u;
    }
    memcpy(buf, line, n);
    buf[n] = '\0';
    if (sscanf(buf, "ti1 %c %u %u", &kind, &id, &claimed) < 2 || id == 0u) {
        return;
    }
    if (kind == 'i') {
        if (t->phase == BW_TINV_OUT && from == t->peer) {
            /* Both asked.  The lower peer id keeps the invitation. */
            if (from < t->cfg.self_peer) {
                char cancel[64];

                snprintf(cancel, sizeof cancel, "ti1 c %u", t->id);
                (void)send_line(t, from, cancel);
                t->phase = BW_TINV_IN;
                t->id = id;
                t->until = now_ms == 0u ? 0u : now_ms + t->cfg.timeout_ms;
                set_status(t, "Tausch annehmen?");
            } else {
                decline_id(t, from, id);
            }
            return;
        }
        if (t->phase != BW_TINV_IDLE) {
            decline_id(t, from, id);
            return;
        }
        t->phase = BW_TINV_IN;
        t->peer = from;
        t->id = id;
        t->until = now_ms == 0u ? 0u : now_ms + t->cfg.timeout_ms;
        t->entered = 0;
        set_status(t, "Tausch annehmen?");
        return;
    }
    if (id != t->id || from != t->peer) {
        return; /* stale or for someone else */
    }
    if (kind == 'a' && t->phase == BW_TINV_OUT) {
        enter(t, 1);
    } else if (kind == 'd' && (t->phase == BW_TINV_OUT || t->phase == BW_TINV_IN)) {
        go_idle(t, "Tausch abgelehnt.");
    } else if (kind == 'c' && (t->phase == BW_TINV_IN || t->phase == BW_TINV_OUT)) {
        go_idle(t, "Die Einladung wurde zurueckgezogen.");
    }
}

void bw_trade_invite_on_leave(BwTradeInvite *t, u32 peer)
{
    if (t == NULL || peer == 0u || peer != t->peer) {
        return;
    }
    if (t->phase == BW_TINV_IDLE || t->phase == BW_TINV_ENTERED) {
        return;
    }
    go_idle(t, "Der andere Spieler ist weg.");
}

void bw_trade_invite_poll(BwTradeInvite *t, u64 now_ms)
{
    if (t == NULL) {
        return;
    }
    if (t->phase == BW_TINV_OUT || t->phase == BW_TINV_IN) {
        if (t->until == 0u) {
            t->until = now_ms + t->cfg.timeout_ms;
            return;
        }
    }
    if ((t->phase == BW_TINV_OUT || t->phase == BW_TINV_IN) &&
        t->until != 0u && now_ms >= t->until) {
        if (t->phase == BW_TINV_OUT) {
            char line[64];

            snprintf(line, sizeof line, "ti1 c %u", t->id);
            (void)send_line(t, t->peer, line);
        }
        go_idle(t, "Zeit abgelaufen.");
    }
}

int bw_trade_invite_consider(BwTradeInvite *t, const BwTradeLook *look, u64 now_ms)
{
    if (t == NULL || look == NULL) {
        return -1;
    }
    if (t->phase == BW_TINV_IN) {
        if (look->decline_edge) {
            bw_trade_invite_decline(t);
        } else if (look->accept_edge) {
            bw_trade_invite_accept(t);
        }
    }
    bw_trade_invite_poll(t, now_ms);
    if (t->phase != BW_TINV_IDLE) {
        return -1;
    }
    if (!look->field_quiet || !look->have_self || look->trade_busy || !look->a_edge) {
        return -1;
    }
    if (look->partner == 0u || look->partner == t->cfg.self_peer) {
        return -1;
    }
    return begin_out(t, look->partner, now_ms);
}

int bw_trade_invite_phase(const BwTradeInvite *t)
{
    return t != NULL ? t->phase : BW_TINV_IDLE;
}

u32 bw_trade_invite_peer(const BwTradeInvite *t)
{
    return t != NULL ? t->peer : 0u;
}

const char *bw_trade_invite_prompt(const BwTradeInvite *t)
{
    if (t == NULL || t->phase != BW_TINV_IN) {
        return NULL;
    }
    return "Tausch annehmen?";
}

const char *bw_trade_invite_status(const BwTradeInvite *t)
{
    return t != NULL ? t->status : "";
}

int bw_trade_invite_wants_trade_screen(const BwTradeInvite *t)
{
    return t != NULL && t->entered;
}
