/*
 * sdk/session_trade.h -- a mutually confirmed Pokemon trade over an eidos
 * session.
 *
 * This is a custom session trade.  It is not the DS Union Room, not the
 * C-Gear radio and not the uncoordinated export/import in sdk/mod_export.c.
 * The game's wireless stack (sdk/link.c, sdk/arm7/wm.c) is left alone.
 * Two players who already share a private Link session exchange offers on
 * their own channel, look at the partner's Pokemon, and each confirms the
 * exact bytes the other offered.  A later message, a changed party slot or
 * a replay of an old trade id does not commit.
 *
 * What this file will not claim
 * -----------------------------
 *
 * Two computers cannot commit one shared write.  Each side has its own
 * save and its own journal (docs/session-trade.md).  The journal is written
 * and synced before the party is touched.  If the save is interrupted, or
 * the partner disappears after only one side has saved, the trade stops in
 * a state that does not invent a second copy and does not throw the kept
 * record away.  That is fail-closed.  It is not a promise that both parties
 * always end with exactly one of each Pokemon when a machine dies in the
 * window after its save and before the partner's.
 *
 * Evolution uses the rows of archive a/0/1/3 (narc kind 0x13), which a
 * data mod replaces as a whole.  The German text banks a/0/0/2 and a/0/0/3
 * are not Pokemon records and are not read here.  Species above the port's
 * species limit are refused: that limit is still the DS table size
 * (sdk/bw_limits.c).  No vanilla evolution list is compiled into this file.
 */
#ifndef BW_PORT_SESSION_TRADE_H
#define BW_PORT_SESSION_TRADE_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BW_STRADE_MON_SIZE 0xDCu
#define BW_STRADE_HASH_LEN 32u
#define BW_STRADE_ID_LEN   16u

/* The DS content limits, until a caller raises them from bw_limit_get. */
#define BW_STRADE_VANILLA_SPECIES 649u
#define BW_STRADE_VANILLA_ITEM    626u
#define BW_STRADE_VANILLA_MOVE    559u

enum {
    BW_STRADE_IDLE = 0,
    BW_STRADE_OPEN_SENT,    /* we asked; waiting for accept                */
    BW_STRADE_OPEN_IN,      /* they asked; waiting for our accept          */
    BW_STRADE_TALK,         /* both in; offers and confirms                */
    BW_STRADE_PREPARED,     /* journal synced; party not saved yet         */
    BW_STRADE_APPLYING,     /* party updated; save in progress             */
    BW_STRADE_SAVED,        /* our save has the result; waiting for theirs */
    BW_STRADE_AWAIT,        /* partner left before either save             */
    BW_STRADE_UNRESOLVED,   /* one side may have saved and the other not   */
    BW_STRADE_DONE,
    BW_STRADE_CANCELLED
};

/* Why an offered record was refused.  0 is a record the trade can carry. */
enum {
    BW_STRADE_OK = 0,
    BW_STRADE_BAD_ARG,
    BW_STRADE_BAD_CHECKSUM,
    BW_STRADE_BAD_FLAGS,
    BW_STRADE_BAD_SPECIES,
    BW_STRADE_BAD_ITEM,
    BW_STRADE_BAD_MOVE,
    BW_STRADE_HAS_MAIL,
    BW_STRADE_BAD_LEVEL
};

typedef struct BwTradeLimits {
    u32 species_last;   /* highest species id the running game can store  */
    u32 item_last;      /* highest item id; 0 on a Pokemon is "no item"   */
    u32 move_last;      /* highest move id; 0 in a slot is an empty slot  */
} BwTradeLimits;

/* One evolution row, as a/0/1/3 stores it: three little-endian u16s. */
typedef struct BwTradeEvo {
    u16 from_species;   /* the narc member, which is the species id       */
    u16 method;         /* 5 trade, 6 trade holding `param`, 7 trade pair */
    u16 param;          /* held item for method 6; unused for 5 and 7     */
    u16 to_species;
} BwTradeEvo;

typedef struct BwTradeInspect {
    int  reason;            /* BW_STRADE_OK or a refusal                   */
    u16  species;
    u16  item;
    u16  moves[4];
    u8   level;
    u8   is_egg;
    u8   has_mail;
    u8   shiny;
    u8   form;
    u8   nature;
    u8   gender;            /* the saved gender: 0 male, 1 female, 2 none */
    u32  personality;
    u32  ot_id;
    u16  evo_to;            /* 0 when this side will not evolve it        */
    u16  evo_method;
} BwTradeInspect;

/*
 * The party's storage.  read and write move one encoded 0xDC record.
 * save_ready is 1 when a save of the bytes write is about to store can
 * start, 0 when the caller should wait, -1 when a save is impossible.
 * save_begin starts that save and does not start one on -2/0 from
 * save_ready: the engine calls save_begin only after save_ready is 1.
 * save_poll is 0 while the save runs, 1 when it is durable, -1 on failure.
 */
typedef struct BwTradeStore {
    void *user;
    int (*count)(void *user);
    int (*read)(void *user, u32 slot, u8 out[BW_STRADE_MON_SIZE]);
    int (*write)(void *user, u32 slot, const u8 mon[BW_STRADE_MON_SIZE]);
    int (*save_ready)(void *user);
    int (*save_begin)(void *user);
    int (*save_poll)(void *user);
} BwTradeStore;

/* 0x40 is the everstone's hold effect in the game's item param 1. */
typedef u8 (*BwTradeHoldFn)(u16 item, void *user);

/*
 * After the incoming record is in the slot and before the save.  `target`
 * is 0 when the species stays.  Method 6 must clear the held item.  0 on
 * success.  A failure aborts the commit and restores the previous record
 * when the store still accepts the write.
 */
typedef int (*BwTradeEvolveFn)(void *user, u32 slot, u16 target, u16 method);

typedef struct BwSessionTrade BwSessionTrade;

typedef struct BwSessionTradeCfg {
    u32            self_peer;
    BwTradeLimits  limits;
    BwTradeStore   store;
    int          (*send)(void *user, u32 peer, const void *data, u32 n);
    void          *send_user;
    char           journal_path[512];
    BwTradeHoldFn  hold_effect;     /* NULL: no item is treated as everstone */
    void          *hold_user;
    BwTradeEvolveFn evolve;         /* NULL: a species change cannot commit */
    void          *evolve_user;
    u32          (*random)(void *user);
    void          *random_user;
} BwSessionTradeCfg;

void bw_session_trade_limits_vanilla(BwTradeLimits *out);

BwSessionTrade *bw_session_trade_create(const BwSessionTradeCfg *cfg);
void            bw_session_trade_destroy(BwSessionTrade *t);

/* Copy `count` rows.  Replaces any rows already set.  0, or -1. */
int  bw_session_trade_set_evolutions(BwSessionTrade *t,
                                    const BwTradeEvo *rows, u32 count);

/*
 * Read evolution rows out of a NARC image (a/0/1/3).  Member i is species
 * i.  A member that is not 7 rows of 6 bytes is skipped, not guessed.
 * 0, or -1 when the image is not a NARC.  The running game's data mod is
 * whatever file the caller read; this function has no built-in table.
 */
int  bw_session_trade_load_evo_narc(BwSessionTrade *t, const void *narc,
                                   u32 size);

/* Journal on disk against the store.  Call once after create. */
void bw_session_trade_recover(BwSessionTrade *t);

/* Remember the only other player while no trade is open.  0 clears it. */
void bw_session_trade_note_peer(BwSessionTrade *t, u32 peer);
int  bw_session_trade_start(BwSessionTrade *t, u32 peer);
int  bw_session_trade_accept(BwSessionTrade *t);
int  bw_session_trade_decline(BwSessionTrade *t);
int  bw_session_trade_offer(BwSessionTrade *t, u32 slot);
int  bw_session_trade_confirm(BwSessionTrade *t);
int  bw_session_trade_cancel(BwSessionTrade *t);

/* Leave UNRESOLVED or AWAIT without moving a Pokemon.  The journal is
 * copied to `<path>.prev` first.  0, or -1 when there is nothing to ack. */
int  bw_session_trade_acknowledge(BwSessionTrade *t);

void bw_session_trade_on_message(BwSessionTrade *t, u32 from,
                                const void *data, u32 n);
void bw_session_trade_on_disconnect(BwSessionTrade *t);

/* Send a resume for a journal that is still open.  The link layer calls
 * this when the same partner is back.  It does not start a new trade. */
void bw_session_trade_resume(BwSessionTrade *t, u32 peer);

/* Drive prepare, the save and the partner's answers.  Once per frame. */
void bw_session_trade_poll(BwSessionTrade *t);

int         bw_session_trade_phase(const BwSessionTrade *t);
u32         bw_session_trade_partner(const BwSessionTrade *t);
u32         bw_session_trade_stamp(const BwSessionTrade *t);
const char *bw_session_trade_status(const BwSessionTrade *t);

/* 1 when a new trade must not start (a journal is still open). */
int  bw_session_trade_blocked(const BwSessionTrade *t);

/* The offer hashes (32 bytes).  0 when that side has no offer yet. */
int  bw_session_trade_hash_local(const BwSessionTrade *t, u8 out[BW_STRADE_HASH_LEN]);
int  bw_session_trade_hash_peer(const BwSessionTrade *t, u8 out[BW_STRADE_HASH_LEN]);

/* What we last read from the partner, or 0 when they have not offered. */
int  bw_session_trade_peer_mon(const BwSessionTrade *t, u8 out[BW_STRADE_MON_SIZE]);
int  bw_session_trade_local_mon(const BwSessionTrade *t, u8 out[BW_STRADE_MON_SIZE]);

/* Local evolution of `incoming`, with `outgoing` as the Pokemon we give
 * (method 7).  `evos` may be NULL.  Does not modify the records. */
void bw_session_trade_predict(const u8 incoming[BW_STRADE_MON_SIZE],
                              const u8 *outgoing,
                              const BwTradeLimits *limits,
                              const BwTradeEvo *evos, u32 nevo,
                              BwTradeHoldFn hold, void *hold_user,
                              BwTradeInspect *out);

/* Encode logical blocks A||B||C||D and a clear party area.  0, or -1. */
/* 1 when this file's SHA-256 matches the empty-message test vector. */
int  bw_session_trade_sha256_selftest(void);

int  bw_session_trade_encode(u8 out[BW_STRADE_MON_SIZE], u32 personality,
                             const u8 logical[0x80],
                             const u8 party_clear[0x54]);

/* Structural species change used by tests and by a host that has no
 * POKE_TOOL_ChangeSpecies.  Rewrites the checksum.  Does not refresh
 * ability, nickname or battle stats: those need the game's personal data.
 * `consume_item` clears the held item (evolution method 6).  0, or -1. */
int  bw_session_trade_respecies(u8 mon[BW_STRADE_MON_SIZE], u16 species,
                                int consume_item);

/* ------------------------------------------------------------------ */
/* The screen                                                          */
/* ------------------------------------------------------------------ */

/* Bind the engine the rows read.  NULL clears it. */
void bw_session_trade_ui_bind(BwSessionTrade *t);
int  bw_session_trade_ui_rows(void);
void bw_session_trade_ui_row(int index, char *label, int nl, char *value, int nv);
/* A on that row.  0 when the row did something, -1 when it refused. */
int  bw_session_trade_ui_activate(int index);
const char *bw_session_trade_ui_help(void);

#ifdef __cplusplus
}
#endif

#endif /* BW_PORT_SESSION_TRADE_H */
