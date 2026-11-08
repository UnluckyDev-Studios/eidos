/*
 * sdk/session_trade_link.h -- the session trade on a private Link session.
 *
 * The trade uses the relay session a Link already opened (sdk/link.h).  It
 * does not speak the DS radio protocol and it does not run during co-op.
 * Public air rooms are refused: anyone can walk into one.
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

/* The engine, or NULL when this side cannot keep a journal. */
BwSessionTrade *bw_session_trade_link_engine(void);

/* 1 when a private Link session is open (not co-op, not the public air). */
int bw_session_trade_link_available(void);

#ifdef __cplusplus
}
#endif

#endif
