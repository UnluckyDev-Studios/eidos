/*
 * sdk/session_trade_link.h -- the session trade on a private Link session.
 *
 * The trade uses the relay session a Link or a co-op game already opened.
 * It does not speak the DS radio protocol.  Public air rooms are refused.
 * Co-op does not build the trade until an invitation is accepted or the
 * player opens the trade screen.  Each side still saves its own file.
 */
#ifndef BW_PORT_SESSION_TRADE_LINK_H
#define BW_PORT_SESSION_TRADE_LINK_H

#include "session_trade.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install the trade channel.  Called when a private Link session opens. */
void bw_session_trade_link_attach(void);
/* The session closed.  An unfinished trade is journaled, not rolled back
 * if a save already started. */
void bw_session_trade_link_detach(void);
/* Once a frame, on the game's thread. */
void bw_session_trade_link_frame(void);

/* The engine, creating it when a trade screen or an accepted invitation
 * needs it.  NULL when this side cannot keep a journal. */
BwSessionTrade *bw_session_trade_link_engine(void);

/* The engine if one already exists.  Does not create one and does not
 * read the evolution table.  The idle co-op field uses this. */
BwSessionTrade *bw_session_trade_link_current(void);

/* 1 when a private Link session is open (not co-op, not the public air). */
int bw_session_trade_link_available(void);

/* 1 when the trade screen may open: a private Link, or a running co-op
 * game.  The menu entry uses this.  The in-world invitation uses co-op. */
int bw_session_trade_screen_ok(void);

/* The next incoming trade open is the one both players just agreed to.
 * It is accepted once.  Offers are still chosen by hand. */
void bw_session_trade_link_arm_accept(void);

#ifdef __cplusplus
}
#endif

#endif
