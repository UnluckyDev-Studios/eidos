/*
 * sdk/trade_invite_mp.c -- the co-op field's side of a trade invitation.
 *
 * Avatars are drawn by sdk/mp.c and have no talk script, so this file
 * reads the player's cell after the game's frame.  A starts an invitation
 * only when the field is quiet (no event, so an NPC or a sign already
 * took the button) and a visible avatar stands on the faced cell.
 * Accepting opens the session trade.  It does not choose a Pokemon.
 */
#include "trade_invite.h"

#include <string.h>

#include "input.h"
#include "mod_net.h"
#include "mp.h"
#include "session_trade.h"
#include "session_trade_link.h"

static BwTradeInvite *g_inv;
static u32 g_prev_buttons;

static int send_port(void *user, u32 peer, const void *data, u32 n)
{
    (void)user;
    return bw_mod_net_port_send(peer, data, n) == 0 ? 0 : -1;
}

static void on_enter(void *user, u32 peer, int we_invited)
{
    BwSessionTrade *trade;

    (void)user;
    trade = bw_session_trade_link_engine();
    if (trade == NULL) {
        return;
    }
    if (we_invited) {
        (void)bw_session_trade_start(trade, peer);
    } else {
        bw_session_trade_note_peer(trade, peer);
        bw_session_trade_link_arm_accept();
    }
}

static BwTradeInvite *engine(void)
{
    BwTradeInviteCfg cfg;
    u32 self;

    if (g_inv != NULL) {
        return g_inv;
    }
    self = bw_mod_net_self_peer();
    if (self == 0u) {
        return NULL;
    }
    memset(&cfg, 0, sizeof cfg);
    cfg.self_peer = self;
    cfg.send = send_port;
    cfg.on_enter = on_enter;
    g_inv = bw_trade_invite_create(&cfg);
    return g_inv;
}

void bw_trade_invite_mp_on_line(u32 from, const char *line, u32 n)
{
    BwTradeInvite *t;

    if (!bw_mp_can_trade()) {
        return;
    }
    t = engine();
    if (t == NULL) {
        return;
    }
    bw_trade_invite_on_line(t, from, line, n, 0);
}

void bw_trade_invite_mp_on_leave(u32 peer)
{
    if (g_inv != NULL) {
        bw_trade_invite_on_leave(g_inv, peer);
    }
}

void bw_trade_invite_mp_frame(u64 now_ms)
{
    BwTradeInvite *t;
    BwTradeLook look;
    u32 buttons, edge;
    u16 zone;
    s16 x, z, fx, fz;
    u8 dir;
    BwSessionTrade *trade;

    if (!bw_mp_can_trade()) {
        g_prev_buttons = 0;
        return;
    }
    t = engine();
    if (t == NULL) {
        return;
    }
    memset(&look, 0, sizeof look);
    buttons = bw_input_player_buttons();
    edge = buttons & ~g_prev_buttons;
    g_prev_buttons = buttons;
    look.field_quiet = bw_mp_field_quiet();
    look.have_self = bw_mp_self_cell(&zone, &x, &z, &dir) == 0;
    look.a_edge = (edge & BW_KEY_A) != 0;
    look.accept_edge = look.a_edge;
    look.decline_edge = (edge & BW_KEY_B) != 0;
    if (look.have_self) {
        bw_trade_face_cell(x, z, dir, &fx, &fz);
        look.partner = bw_mp_partner_at(fx, fz);
    }
    trade = bw_session_trade_link_engine();
    look.trade_busy = trade != NULL && bw_session_trade_blocked(trade) &&
                      bw_session_trade_phase(trade) != BW_STRADE_IDLE;
    (void)bw_trade_invite_consider(t, &look, now_ms);
}

int bw_trade_invite_live_phase(void)
{
    return g_inv != NULL ? bw_trade_invite_phase(g_inv) : BW_TINV_IDLE;
}

const char *bw_trade_invite_live_prompt(void)
{
    return g_inv != NULL ? bw_trade_invite_prompt(g_inv) : NULL;
}

const char *bw_trade_invite_live_status(void)
{
    return g_inv != NULL ? bw_trade_invite_status(g_inv) : "";
}

int bw_trade_invite_live_wants_screen(void)
{
    return g_inv != NULL && bw_trade_invite_wants_trade_screen(g_inv);
}

void bw_trade_invite_live_accept(void)
{
    if (g_inv != NULL) {
        bw_trade_invite_accept(g_inv);
    }
}

void bw_trade_invite_live_decline(void)
{
    if (g_inv != NULL) {
        bw_trade_invite_decline(g_inv);
    }
}
