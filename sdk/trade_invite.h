/*
 * sdk/trade_invite.h -- ask a visible co-op partner to trade.
 *
 * The field avatar has no script, so the game's talk check never starts a
 * conversation with it (sdk/mod_avatar.h).  This layer watches the same A
 * press after that check: if no event started, and the player faces the
 * adjacent cell the partner's avatar stands on, an invitation goes to that
 * peer.  Nothing is swapped until both have confirmed exact offers in the
 * session trade (sdk/session_trade.h).
 *
 * Facing is the game's own step (ov010 fldmmdl_move.c,
 * FLDMMDL_MOVE_AddDirVector): 0 is z-1, 1 is z+1, 2 is x-1, 3 is x+1.
 */
#ifndef BW_PORT_TRADE_INVITE_H
#define BW_PORT_TRADE_INVITE_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BW_TINV_IDLE = 0,
    BW_TINV_OUT,       /* we asked; waiting                              */
    BW_TINV_IN,        /* they asked; waiting for our answer             */
    BW_TINV_ENTERED    /* both agreed to open the trade; no swap yet     */
};

typedef struct BwTradeInvite BwTradeInvite;

typedef struct BwTradeInviteCfg {
    u32 self_peer;
    u32 timeout_ms;          /* 0: 15 seconds                                  */
    int (*send)(void *user, u32 peer, const void *data, u32 n);
    void *send_user;
    /* Both agreed.  we_invited is 1 on the side that pressed A.  The
     * callback opens the session trade.  It must not move a Pokemon. */
    void (*on_enter)(void *user, u32 peer, int we_invited);
    void *enter_user;
    u32 (*random)(void *user);
    void *random_user;
} BwTradeInviteCfg;

/* What the field knows this frame.  The invite does not read the game. */
typedef struct BwTradeLook {
    int field_quiet;     /* field is up and no event, menu, or battle runs */
    int have_self;
    s16 x, z;
    u8  dir;
    int a_edge;          /* A went down, and we are not already answering   */
    int accept_edge;     /* A while an invitation is waiting                */
    int decline_edge;    /* B while an invitation is waiting                */
    int trade_busy;      /* a session trade is already open                 */
    u32 partner;         /* peer on the faced cell, or 0                    */
} BwTradeLook;

BwTradeInvite *bw_trade_invite_create(const BwTradeInviteCfg *cfg);
void           bw_trade_invite_destroy(BwTradeInvite *t);

/* The cell one step in `dir`. */
void bw_trade_face_cell(s16 x, s16 z, u8 dir, s16 *ox, s16 *oz);
/* 1 when (ax,az) is the cell `dir` faces from (x,z). */
int  bw_trade_facing_cell(s16 x, s16 z, u8 dir, s16 ax, s16 az);

/* Apply a look, then timeouts.  0 if an invitation was sent, else -1. */
int  bw_trade_invite_consider(BwTradeInvite *t, const BwTradeLook *look, u64 now_ms);

void bw_trade_invite_accept(BwTradeInvite *t);
void bw_trade_invite_decline(BwTradeInvite *t);
void bw_trade_invite_cancel(BwTradeInvite *t);

/* A line from the co-op port ("ti1 i|a|d|c ..."). */
void bw_trade_invite_on_line(BwTradeInvite *t, u32 from, const char *line, u32 n, u64 now_ms);
void bw_trade_invite_on_leave(BwTradeInvite *t, u32 peer);
void bw_trade_invite_poll(BwTradeInvite *t, u64 now_ms);

int         bw_trade_invite_phase(const BwTradeInvite *t);
u32         bw_trade_invite_peer(const BwTradeInvite *t);
/* German prompt, or NULL when there is nothing to answer. */
const char *bw_trade_invite_prompt(const BwTradeInvite *t);
const char *bw_trade_invite_status(const BwTradeInvite *t);
/* 1 after both agreed, until the trade screen has been shown. */
int         bw_trade_invite_wants_trade_screen(const BwTradeInvite *t);

/* The co-op frame.  mp.c calls these.  They do nothing until a co-op game
 * is running. */
void bw_trade_invite_mp_on_line(u32 from, const char *line, u32 n);
void bw_trade_invite_mp_on_leave(u32 peer);
void bw_trade_invite_mp_frame(u64 now_ms);

/* The invitation the co-op frame holds, for the field picture. */
int         bw_trade_invite_live_phase(void);
const char *bw_trade_invite_live_prompt(void);
const char *bw_trade_invite_live_status(void);
int         bw_trade_invite_live_wants_screen(void);
void        bw_trade_invite_live_accept(void);
void        bw_trade_invite_live_decline(void);

#ifdef __cplusplus
}
#endif

#endif
