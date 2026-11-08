/*
 * sdk/session_trade_link.c -- wire the session trade to a private Link.
 *
 * Messages travel under eidos:trade, which neither co-op nor the radio
 * reads.  The party is the game's own party.  The save is the game's own
 * GAME_DATA_SaveStart, and it runs only when the field is quiet, the same
 * moment co-op uses.  If those calls are not in this build, the trade
 * refuses to commit and does not write the party.
 *
 * Evolution rows are read from a/0/1/3 in the data root when that file is
 * there.  A data mod that replaces the narc is what this reads.  There is
 * no compiled species table.  German text banks are not consulted.
 */
#include "session_trade_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_limits.h"
#include "link.h"
#include "mod_net.h"
#include "mp.h"
#include "port.h"
#include "save.h"

#if defined(__GNUC__) || defined(__clang__)
#define ST_WEAK __attribute__((weak))
#else
#define ST_WEAK
#endif

extern int  GAME_DATA_SaveStart(void *gameData) ST_WEAK;
extern int  GAME_DATA_SaveMain(void *gameData) ST_WEAK;
extern void *GAMESYSTEM_GetGameData(const void *gs) ST_WEAK;
extern void *GAMESYSTEM_GetEvent(const void *gs) ST_WEAK;
extern void *GAMESYSTEM_GetFieldMap(const void *gs) ST_WEAK;
extern void *GAME_DATA_GetMyParty(const void *gd) ST_WEAK;
extern u32   POKEPARTY_GetCount(const void *party) ST_WEAK;
extern void *POKEPARTY_GetMember(void *party, u32 pos) ST_WEAK;
extern void  POKE_TOOL_ChangeSpecies(void *mon, u32 species) ST_WEAK;
extern void  POKE_TOOL_SetBoxValue(void *box, u32 param, u32 value) ST_WEAK;
extern void *POKE_TOOL_CastToBoxParam(void *mon) ST_WEAK;

/* Defined in sdk/mp.c so a build without the game still links. */
extern void *bw_dsref__GameSystemGlobal;

#define ST_PEERS 8

static BwSessionTrade *g_trade;
static void *g_save_gd;
static int g_saving;
static u32 g_peers[ST_PEERS];
static u32 g_npeers;
static int g_attached;

static void *game_data(void)
{
    void *gs;

    if (bw_dsref__GameSystemGlobal == NULL || GAMESYSTEM_GetGameData == NULL) {
        return NULL;
    }
    gs = *(void **)bw_dsref__GameSystemGlobal;
    if (gs == NULL) {
        return NULL;
    }
    return GAMESYSTEM_GetGameData(gs);
}

static void *party_of(void)
{
    void *gd = game_data();

    if (gd == NULL || GAME_DATA_GetMyParty == NULL) {
        return NULL;
    }
    return GAME_DATA_GetMyParty(gd);
}

static int field_quiet(void)
{
    void *gs;
    void *gd;

    if (bw_dsref__GameSystemGlobal == NULL || GAMESYSTEM_GetGameData == NULL ||
        GAMESYSTEM_GetEvent == NULL || GAMESYSTEM_GetFieldMap == NULL) {
        return 0;
    }
    gs = *(void **)bw_dsref__GameSystemGlobal;
    if (gs == NULL || GAMESYSTEM_GetFieldMap(gs) == NULL ||
        GAMESYSTEM_GetEvent(gs) != NULL) {
        return 0;
    }
    gd = GAMESYSTEM_GetGameData(gs);
    return gd != NULL;
}

static int st_count(void *user)
{
    void *party;
    u32 n;

    (void)user;
    party = party_of();
    if (party == NULL || POKEPARTY_GetCount == NULL) {
        return -1;
    }
    n = POKEPARTY_GetCount(party);
    if (n > 6u) {
        return -1;
    }
    return (int)n;
}

static int st_read(void *user, u32 slot, u8 out[BW_STRADE_MON_SIZE])
{
    void *party;
    void *mon;

    (void)user;
    party = party_of();
    if (party == NULL || POKEPARTY_GetMember == NULL || out == NULL) {
        return -1;
    }
    mon = POKEPARTY_GetMember(party, slot);
    if (mon == NULL) {
        return -1;
    }
    memcpy(out, mon, BW_STRADE_MON_SIZE);
    return 0;
}

static int st_write(void *user, u32 slot, const u8 mon[BW_STRADE_MON_SIZE])
{
    void *party;
    void *dst;

    (void)user;
    party = party_of();
    if (party == NULL || POKEPARTY_GetMember == NULL || mon == NULL) {
        return -1;
    }
    dst = POKEPARTY_GetMember(party, slot);
    if (dst == NULL) {
        return -1;
    }
    memcpy(dst, mon, BW_STRADE_MON_SIZE);
    return 0;
}

static int st_ready(void *user)
{
    (void)user;
    if (GAME_DATA_SaveStart == NULL || GAME_DATA_SaveMain == NULL) {
        return -1;
    }
    return field_quiet() ? 1 : 0;
}

static int st_begin(void *user)
{
    void *gd;

    (void)user;
    if (g_saving || GAME_DATA_SaveStart == NULL) {
        return -1;
    }
    if (!field_quiet()) {
        return -1;
    }
    gd = game_data();
    if (gd == NULL) {
        return -1;
    }
    g_save_gd = gd;
    g_saving = 1;
    (void)GAME_DATA_SaveStart(gd);
    return 0;
}

static int st_poll(void *user)
{
    int r;

    (void)user;
    if (!g_saving) {
        return 1;
    }
    if (GAME_DATA_SaveMain == NULL || g_save_gd == NULL) {
        g_saving = 0;
        return -1;
    }
    r = GAME_DATA_SaveMain(g_save_gd);
    if (r == 2) {
        g_saving = 0;
        return 1;
    }
    if (r == 3) {
        g_saving = 0;
        return -1;
    }
    return 0;
}

static int st_evolve(void *user, u32 slot, u16 target, u16 method)
{
    void *party;
    void *mon;
    void *box;

    (void)user;
    if (POKE_TOOL_ChangeSpecies == NULL) {
        return -1;
    }
    party = party_of();
    if (party == NULL || POKEPARTY_GetMember == NULL) {
        return -1;
    }
    mon = POKEPARTY_GetMember(party, slot);
    if (mon == NULL) {
        return -1;
    }
    if (target != 0u) {
        POKE_TOOL_ChangeSpecies(mon, target);
    }
    if (method == 6u) {
        if (POKE_TOOL_SetBoxValue == NULL) {
            return -1;
        }
        box = POKE_TOOL_CastToBoxParam != NULL ? POKE_TOOL_CastToBoxParam(mon) : mon;
        POKE_TOOL_SetBoxValue(box, 6u, 0u);
    }
    return 0;
}

static int st_send(void *user, u32 peer, const void *data, u32 n)
{
    (void)user;
    return bw_mod_net_trade_send(peer, data, n) == 0 ? 0 : -1;
}

static void load_narc(BwSessionTrade *t)
{
    const char *root = bw_data_root();
    char path[1100];
    FILE *f;
    long n;
    u8 *buf;

    if (root == NULL || root[0] == '\0') {
        return;
    }
    if (snprintf(path, sizeof path, "%s/a/0/1/3", root) >= (int)sizeof path) {
        return;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 16 || n > 8 * 1024 * 1024) {
        fclose(f);
        return;
    }
    rewind(f);
    buf = (u8 *)malloc((size_t)n);
    if (buf == NULL) {
        fclose(f);
        return;
    }
    if (fread(buf, 1, (size_t)n, f) == (size_t)n) {
        (void)bw_session_trade_load_evo_narc(t, buf, (u32)n);
    }
    fclose(f);
    free(buf);
}

static void forget_peer(u32 peer)
{
    u32 i;

    for (i = 0; i < g_npeers; i++) {
        if (g_peers[i] == peer) {
            g_peers[i] = g_peers[g_npeers - 1u];
            g_npeers--;
            return;
        }
    }
}

static void remember_peer(u32 peer)
{
    u32 i;

    if (peer == 0u || peer == bw_mod_net_self_peer()) {
        return;
    }
    for (i = 0; i < g_npeers; i++) {
        if (g_peers[i] == peer) {
            return;
        }
    }
    if (g_npeers < ST_PEERS) {
        g_peers[g_npeers++] = peer;
    }
}

static void on_net(u32 peer, int kind, const void *buf, u32 size)
{
    if (kind == (int)BW_SESSION_JOIN) {
        remember_peer(peer);
        return;
    }
    if (kind == (int)BW_SESSION_LEAVE) {
        forget_peer(peer);
        if (g_trade != NULL &&
            (peer == bw_session_trade_partner(g_trade) || g_npeers == 0u)) {
            bw_session_trade_on_disconnect(g_trade);
        }
        return;
    }
    if (kind == (int)BW_SESSION_MESSAGE && g_trade != NULL && buf != NULL) {
        bw_session_trade_on_message(g_trade, peer, buf, size);
    }
}

static int ensure(void)
{
    BwSessionTradeCfg cfg;
    char path[512];

    if (g_trade != NULL) {
        return 0;
    }
    if (bw_save_path(path, sizeof path, "session_trade/journal.bin") != 0) {
        return -1;
    }
    memset(&cfg, 0, sizeof cfg);
    cfg.self_peer = bw_mod_net_self_peer();
    cfg.limits.species_last = bw_limit_get(BW_PORT_LIMIT_SPECIES_LAST);
    cfg.limits.item_last = bw_limit_get(BW_PORT_LIMIT_ITEM_LAST);
    cfg.limits.move_last = bw_limit_get(BW_PORT_LIMIT_MOVE_LAST);
    cfg.store.user = NULL;
    cfg.store.count = st_count;
    cfg.store.read = st_read;
    cfg.store.write = st_write;
    cfg.store.save_ready = st_ready;
    cfg.store.save_begin = st_begin;
    cfg.store.save_poll = st_poll;
    cfg.send = st_send;
    snprintf(cfg.journal_path, sizeof cfg.journal_path, "%s", path);
    if (POKE_TOOL_ChangeSpecies != NULL) {
        cfg.evolve = st_evolve;
    }
    g_trade = bw_session_trade_create(&cfg);
    if (g_trade == NULL) {
        return -1;
    }
    load_narc(g_trade);
    bw_session_trade_recover(g_trade);
    return 0;
}

int bw_session_trade_link_available(void)
{
    BwMpState st = bw_mp_state();

    if (bw_link_state() != BW_LINK_OPEN || bw_link_on_air()) {
        return 0;
    }
    if (st != BW_MP_OFF && st != BW_MP_ENDED) {
        return 0;
    }
    return 1;
}

void bw_session_trade_link_attach(void)
{
    g_attached = 1;
    bw_mod_net_set_trade_handler(on_net);
    (void)ensure();
}

void bw_session_trade_link_detach(void)
{
    g_attached = 0;
    bw_mod_net_set_trade_handler(NULL);
    g_npeers = 0;
    if (g_trade != NULL) {
        bw_session_trade_on_disconnect(g_trade);
    }
    /* A save the game has already started still has to be polled.  The
     * frame keeps the engine until that poll finishes. */
    if (!g_saving && g_trade != NULL) {
        bw_session_trade_destroy(g_trade);
        g_trade = NULL;
    }
}

BwSessionTrade *bw_session_trade_link_engine(void)
{
    if (!bw_session_trade_link_available()) {
        return g_trade;
    }
    (void)ensure();
    return g_trade;
}

void bw_session_trade_link_frame(void)
{
    if (g_saving && g_trade != NULL) {
        bw_session_trade_poll(g_trade);
        if (!g_saving && !bw_session_trade_link_available()) {
            bw_session_trade_destroy(g_trade);
            g_trade = NULL;
        }
        return;
    }
    if (!g_attached && !bw_session_trade_link_available()) {
        return;
    }
    if (!bw_session_trade_link_available()) {
        if (g_trade != NULL && bw_session_trade_blocked(g_trade)) {
            bw_session_trade_on_disconnect(g_trade);
        }
        return;
    }
    if (ensure() != 0) {
        return;
    }
    if (g_npeers == 1u) {
        bw_session_trade_note_peer(g_trade, g_peers[0]);
        bw_session_trade_resume(g_trade, g_peers[0]);
    } else if (g_npeers != 1u && bw_session_trade_blocked(g_trade) &&
               bw_session_trade_phase(g_trade) != BW_STRADE_AWAIT &&
               bw_session_trade_phase(g_trade) != BW_STRADE_UNRESOLVED &&
               bw_session_trade_phase(g_trade) != BW_STRADE_APPLYING) {
        bw_session_trade_on_disconnect(g_trade);
    }
    bw_session_trade_poll(g_trade);
}
