/* mkdir and fsync are POSIX, not C99. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * sdk/session_trade.c -- mutually confirmed session trade (session_trade.h).
 *
 * The bytes on the wire are the game's own 0xDC Pokemon record, encoded the
 * way poke_tool.c stores it.  The checksum is the game's halfword sum.  The
 * block order is the 32-row table selected by personality bits 13..17.
 * Nothing here reads a ROM.
 *
 * Commit, on each side:
 *   1. both confirms name the same offer revisions and the same two hashes;
 *   2. the journal is synced before the party is modified;
 *   3. the party slot is replaced in place (the count does not change);
 *   4. the save runs;
 *   5. SAVED is sent.  DONE waits for the partner's SAVED.
 * A disconnect before step 4 leaves the party alone.  A disconnect after our
 * own save does not roll that save back.
 */
#include "session_trade.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#else
#include <direct.h>
#include <io.h>
#include <windows.h>
#endif

#define MSG_OPEN     1u
#define MSG_OPEN_OK  2u
#define MSG_OPEN_NO  3u
#define MSG_OFFER    4u
#define MSG_CONFIRM  5u
#define MSG_CANCEL   6u
#define MSG_PREPARED 7u
#define MSG_SAVED    8u
#define MSG_ABORT    9u
#define MSG_RESUME   10u
#define MSG_VER      1u
#define HDR          (2u + BW_STRADE_ID_LEN)

#define JOUR_MAGIC   0x314A5445u /* 'ETJ1' little-endian */
#define JOUR_VERSION 1u
#define TOMBS        8u
#define EVO_MAX      8192u

#define HOLD_EVERSTONE     0x40u
#define SPECIES_KADABRA    0x40u
#define SPECIES_KARRABLAST 588u
#define SPECIES_SHELMET    616u
#define EVO_TRADE          5u
#define EVO_TRADE_ITEM     6u
#define EVO_TRADE_PAIR     7u

enum {
    WHY_NONE = 0,
    WHY_USER,
    WHY_STALE,
    WHY_MALFORMED,
    WHY_DISCONNECT,
    WHY_SAVE,
    WHY_EVOLVE,
    WHY_OFFER,
    WHY_BUSY
};

#define FLAG_PARTY 0x0001u
#define FLAG_BOX   0x0002u
#define FLAG_FAIL  0x0004u

#pragma pack(push, 1)
typedef struct Jour {
    u32 magic;
    u32 version;
    u32 phase;
    u32 slot;
    u32 peer;
    u32 rev_l;
    u32 rev_p;
    u16 evo_to;
    u16 evo_method;
    u8  tid[BW_STRADE_ID_LEN];
    u8  hash_out[BW_STRADE_HASH_LEN];
    u8  hash_in[BW_STRADE_HASH_LEN];
    u8  outgoing[BW_STRADE_MON_SIZE];
    u8  incoming[BW_STRADE_MON_SIZE];
    u8  applied[BW_STRADE_MON_SIZE];
    u32 tomb_n;
    u8  tombs[TOMBS][BW_STRADE_ID_LEN];
    u32 crc;
} Jour;
#pragma pack(pop)

struct BwSessionTrade {
    BwSessionTradeCfg cfg;
    BwTradeEvo *evos;
    u32 nevo;
    int phase;
    u32 partner;
    u32 stamp;
    u32 rng;
    char status[192];
    u8  tid[BW_STRADE_ID_LEN];
    u8  local_mon[BW_STRADE_MON_SIZE];
    u8  peer_mon[BW_STRADE_MON_SIZE];
    u32 rev_l, rev_p;
    u8  hash_l[BW_STRADE_HASH_LEN];
    u8  hash_p[BW_STRADE_HASH_LEN];
    int have_local, have_peer;
    int conf_l, conf_p;
    int peer_prepared, peer_saved;
    int save_started;
    u16 evo_to, evo_method;
    u8  applied[BW_STRADE_MON_SIZE];
    int have_applied;
    int resume_sent;
    int peer_left;
    u32 slot;
    Jour disk;
    int have_disk;
    int journal_damaged;
    int outcome;
    u32 confirm_wait;
    u32 confirm_left;
    int save_wait_logged;
};

/* ------------------------------------------------------------------ */
/* Bytes                                                               */
/* ------------------------------------------------------------------ */

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 rd16(const u8 *p)
{
    return (u16)((u16)p[0] | (u16)((u16)p[1] << 8));
}

static void wr32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static void wr16(u8 *p, u16 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void wr32be(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static u32 crc32_of(const void *data, u32 n)
{
    const u8 *p = (const u8 *)data;
    u32 c = 0xFFFFFFFFu;
    u32 i, b;

    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (b = 0; b < 8u; b++) {
            u32 mask = (u32)-(s32)(c & 1u);

            c = (c >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~c;
}

static u32 rotr(u32 x, u32 n)
{
    return (x >> n) | (x << (32u - n));
}

/* SHA-256.  Offers are 220 bytes, so the padded message fits in 256. */
static void sha256(const void *msg, u32 len, u8 out[32])
{
    static const u32 k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
        0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
        0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
        0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
        0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
        0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
        0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
        0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    u32 h[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    u8 buf[256];
    u32 blocks, bi, i;

    if (len > sizeof(buf) - 64u) {
        len = sizeof(buf) - 64u;
    }
    memset(buf, 0, sizeof buf);
    if (len > 0 && msg != NULL) {
        memcpy(buf, msg, len);
    }
    buf[len] = 0x80u;
    blocks = (len + 1u + 8u + 63u) / 64u;
    {
        u32 bits = len * 8u;

        buf[blocks * 64u - 4u] = (u8)(bits >> 24);
        buf[blocks * 64u - 3u] = (u8)(bits >> 16);
        buf[blocks * 64u - 2u] = (u8)(bits >> 8);
        buf[blocks * 64u - 1u] = (u8)bits;
    }
    for (bi = 0; bi < blocks; bi++) {
        const u8 *blk = buf + bi * 64u;
        u32 w[64];
        u32 a, b, c, d, e, f, g, hh;

        for (i = 0; i < 16u; i++) {
            w[i] = ((u32)blk[i * 4u] << 24) | ((u32)blk[i * 4u + 1u] << 16) |
                   ((u32)blk[i * 4u + 2u] << 8) | (u32)blk[i * 4u + 3u];
        }
        for (i = 16; i < 64u; i++) {
            u32 s0 = rotr(w[i - 15u], 7) ^ rotr(w[i - 15u], 18) ^ (w[i - 15u] >> 3);
            u32 s1 = rotr(w[i - 2u], 17) ^ rotr(w[i - 2u], 19) ^ (w[i - 2u] >> 10);

            w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
        }
        a = h[0]; b = h[1]; c = h[2]; d = h[3];
        e = h[4]; f = h[5]; g = h[6]; hh = h[7];
        for (i = 0; i < 64u; i++) {
            u32 s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            u32 ch = (e & f) ^ ((~e) & g);
            u32 t1 = hh + s1 + ch + k[i] + w[i];
            u32 s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            u32 maj = (a & b) ^ (a & c) ^ (b & c);
            u32 t2 = s0 + maj;

            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    for (i = 0; i < 8u; i++) {
        wr32be(out + i * 4u, h[i]);
    }
}

/* Empty-message vector.  A wrong hash would still match across two peers
 * that share this function, so the tests check the known answer as well. */
int bw_session_trade_sha256_selftest(void)
{
    static const u8 want[32] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8,
        0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
        0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
    };
    u8 got[32];

    sha256(NULL, 0, got);
    return memcmp(got, want, 32) == 0;
}

static void hash_mon(const u8 mon[BW_STRADE_MON_SIZE], u8 out[BW_STRADE_HASH_LEN])
{
    sha256(mon, BW_STRADE_MON_SIZE, out);
}

/* ------------------------------------------------------------------ */
/* The Pokemon record                                                  */
/* ------------------------------------------------------------------ */

/* Physical index of logical block 0..3 (A..D).  Rows 24..31 repeat 0..7,
 * which is the DS table (personality & 0x3E000) >> 13. */
static const u8 k_pos[32][4] = {
    { 0, 1, 2, 3 }, { 0, 1, 3, 2 }, { 0, 2, 1, 3 }, { 0, 3, 1, 2 },
    { 0, 2, 3, 1 }, { 0, 3, 2, 1 }, { 1, 0, 2, 3 }, { 1, 0, 3, 2 },
    { 2, 0, 1, 3 }, { 3, 0, 1, 2 }, { 2, 0, 3, 1 }, { 3, 0, 2, 1 },
    { 1, 2, 0, 3 }, { 1, 3, 0, 2 }, { 2, 1, 0, 3 }, { 3, 1, 0, 2 },
    { 2, 3, 0, 1 }, { 3, 2, 0, 1 }, { 1, 2, 3, 0 }, { 1, 3, 2, 0 },
    { 2, 1, 3, 0 }, { 3, 1, 2, 0 }, { 2, 3, 1, 0 }, { 3, 2, 1, 0 },
    { 0, 1, 2, 3 }, { 0, 1, 3, 2 }, { 0, 2, 1, 3 }, { 0, 3, 1, 2 },
    { 0, 2, 3, 1 }, { 0, 3, 2, 1 }, { 1, 0, 2, 3 }, { 1, 0, 3, 2 }
};

static u32 shuffle_of(u32 pid)
{
    return (pid & 0x3E000u) >> 13;
}

static void xor_area(u8 *data, u32 size, u32 seed)
{
    u32 i, n = size >> 1;
    u32 state = seed;

    for (i = 0; i < n; i++) {
        u16 v;

        state = state * 0x41C64E6Du + 0x6073u;
        v = (u16)(state >> 16);
        data[i * 2u] ^= (u8)v;
        data[i * 2u + 1u] ^= (u8)(v >> 8);
    }
}

static u16 sum_area(const u8 *data, u32 size)
{
    u16 sum = 0;
    u32 i, n = size >> 1;

    for (i = 0; i < n; i++) {
        sum = (u16)(sum + rd16(data + i * 2u));
    }
    return sum;
}

static void to_logical(u8 logical[0x80], u32 pid, const u8 physical[0x80])
{
    u32 s = shuffle_of(pid);
    int b;

    for (b = 0; b < 4; b++) {
        memcpy(logical + (u32)b * 0x20u, physical + (u32)k_pos[s][b] * 0x20u, 0x20u);
    }
}

static void to_physical(u8 physical[0x80], u32 pid, const u8 logical[0x80])
{
    u32 s = shuffle_of(pid);
    int b;

    for (b = 0; b < 4; b++) {
        memcpy(physical + (u32)k_pos[s][b] * 0x20u, logical + (u32)b * 0x20u, 0x20u);
    }
}

/* Decrypt a copy.  0 when the checksum matches and the flags are the
 * encoded form the game stores (both decoded bits clear, failure bit clear). */
static int open_record(const u8 in[BW_STRADE_MON_SIZE], u8 logical[0x80],
                       u8 party[0x54], u16 *flags_out)
{
    u8 tmp[BW_STRADE_MON_SIZE];
    u32 pid;
    u16 flags, sum, check;

    memcpy(tmp, in, sizeof tmp);
    pid = rd32(tmp);
    flags = rd16(tmp + 4);
    check = rd16(tmp + 6);
    if (flags_out != NULL) {
        *flags_out = flags;
    }
    if ((flags & (FLAG_PARTY | FLAG_BOX | FLAG_FAIL)) != 0u) {
        return -1;
    }
    xor_area(tmp + 0x08, 0x80, check);
    sum = sum_area(tmp + 0x08, 0x80);
    if (sum != check) {
        return -1;
    }
    xor_area(tmp + 0x88, 0x54, pid);
    to_logical(logical, pid, tmp + 0x08);
    memcpy(party, tmp + 0x88, 0x54);
    return 0;
}

int bw_session_trade_encode(u8 out[BW_STRADE_MON_SIZE], u32 personality,
                            const u8 logical[0x80], const u8 party_clear[0x54])
{
    u8 physical[0x80];
    u16 sum;

    if (out == NULL || logical == NULL || party_clear == NULL) {
        return -1;
    }
    memset(out, 0, BW_STRADE_MON_SIZE);
    wr32(out, personality);
    to_physical(physical, personality, logical);
    sum = sum_area(physical, 0x80);
    wr16(out + 6, sum);
    memcpy(out + 0x08, physical, 0x80);
    xor_area(out + 0x08, 0x80, sum);
    memcpy(out + 0x88, party_clear, 0x54);
    xor_area(out + 0x88, 0x54, personality);
    return 0;
}

int bw_session_trade_respecies(u8 mon[BW_STRADE_MON_SIZE], u16 species, int consume_item)
{
    u8 logical[0x80], party[0x54];
    u32 pid;

    if (mon == NULL || species == 0u) {
        return -1;
    }
    if (open_record(mon, logical, party, NULL) != 0) {
        return -1;
    }
    pid = rd32(mon);
    wr16(logical, species);
    if (consume_item) {
        wr16(logical + 2, 0);
    }
    return bw_session_trade_encode(mon, pid, logical, party);
}

static int shiny_of(u32 ot, u32 pid)
{
    u32 x = (ot >> 16) ^ (ot & 0xFFFFu) ^ (pid >> 16) ^ (pid & 0xFFFFu);

    return x < 8u;
}

static void fill_inspect(const u8 logical[0x80], const u8 party[0x54],
                         u32 pid, BwTradeInspect *out)
{
    u32 iv;
    int i;

    memset(out, 0, sizeof *out);
    out->personality = pid;
    out->species = rd16(logical);
    out->item = rd16(logical + 2);
    out->ot_id = rd32(logical + 4);
    for (i = 0; i < 4; i++) {
        out->moves[i] = rd16(logical + 0x20 + (u32)i * 2u);
    }
    iv = rd32(logical + 0x20 + 16);
    out->is_egg = (u8)((iv >> 30) & 1u);
    out->gender = (u8)((logical[0x20 + 24] >> 1) & 3u);
    out->form = (u8)((logical[0x20 + 24] >> 3) & 0x1Fu);
    out->nature = logical[0x20 + 25];
    out->level = party[4];
    out->has_mail = (u8)(party[0x1B] != 0xFFu);
    out->shiny = (u8)shiny_of(out->ot_id, pid);
    out->has_nick = (u8)((iv >> 31) & 1u);
    if (out->has_nick) {
        int k;

        for (k = 0; k < 11; k++) {
            u16 u = rd16(logical + 0x40u + (u32)k * 2u);

            if (u == 0u || u == 0xFFFFu) {
                break;
            }
            out->nick[k] = u;
            out->nick_n++;
        }
        if (out->nick_n == 0u) {
            out->has_nick = 0;
        }
    }
}

static int utf8_text_ok(const char *s, int max_bytes)
{
    int n = 0;

    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    while (s[n] != '\0') {
        unsigned char c = (unsigned char)s[n];
        int need, i;

        if (c < 0x20u || c == 0x7Fu) {
            return 0;
        }
        if (c < 0x80u) {
            need = 1;
        } else if ((c & 0xE0u) == 0xC0u) {
            if (c < 0xC2u) {
                return 0;
            }
            need = 2;
        } else if ((c & 0xF0u) == 0xE0u) {
            need = 3;
        } else {
            return 0;
        }
        if (n + need > max_bytes) {
            return 0;
        }
        for (i = 1; i < need; i++) {
            if (((unsigned char)s[n + i] & 0xC0u) != 0x80u) {
                return 0;
            }
        }
        if (need == 3 && c == 0xE0u && (unsigned char)s[n + 1] < 0xA0u) {
            return 0;
        }
        if (need == 3 && c == 0xEDu && (unsigned char)s[n + 1] >= 0xA0u) {
            return 0;
        }
        n += need;
    }
    return n > 0;
}

static int nick_utf8(const u16 *nick, int n, char *out, int cap)
{
    int i, at = 0;

    if (nick == NULL || n < 1 || out == NULL || cap < 2) {
        return 0;
    }
    if (n > 11) {
        n = 11;
    }
    for (i = 0; i < n; i++) {
        u32 u = nick[i];
        int need;

        if (u == 0u || u == 0xFFFFu) {
            break;
        }
        if (u < 0x20u || u == 0x7Fu || (u >= 0xD800u && u <= 0xDFFFu) || u > 0xFFFDu) {
            return 0;
        }
        need = u < 0x80u ? 1 : u < 0x800u ? 2 : 3;
        if (at + need >= cap) {
            return 0;
        }
        if (need == 1) {
            out[at++] = (char)u;
        } else if (need == 2) {
            out[at++] = (char)(0xC0u | (u >> 6));
            out[at++] = (char)(0x80u | (u & 0x3Fu));
        } else {
            out[at++] = (char)(0xE0u | (u >> 12));
            out[at++] = (char)(0x80u | ((u >> 6) & 0x3Fu));
            out[at++] = (char)(0x80u | (u & 0x3Fu));
        }
    }
    out[at] = '\0';
    return at > 0;
}

int bw_session_trade_format_label(u16 species, const char *species_name,
                                 int has_nick, const u16 *nick, int nick_n,
                                 char *out, int cap)
{
    char spec[48];
    char nk[48];
    int named, nicked;

    if (out == NULL || cap < 1) {
        return -1;
    }
    out[0] = '\0';
    named = species >= 1u && species <= (u16)BW_STRADE_VANILLA_SPECIES &&
            utf8_text_ok(species_name, 32);
    snprintf(spec, sizeof spec, "%s", named ? species_name : "Unbekannt");
    nicked = has_nick && nick_utf8(nick, nick_n, nk, (int)sizeof nk);
    if (nicked && strcmp(nk, spec) != 0) {
        snprintf(out, (size_t)cap, "%s (%s)", nk, spec);
    } else {
        snprintf(out, (size_t)cap, "%s", spec);
    }
    return 0;
}

static int limits_ok(const BwTradeInspect *in, const BwTradeLimits *lim)
{
    int i;

    if (in->species == 0u || in->species > lim->species_last) {
        return BW_STRADE_BAD_SPECIES;
    }
    if (in->item > lim->item_last) {
        return BW_STRADE_BAD_ITEM;
    }
    for (i = 0; i < 4; i++) {
        if (in->moves[i] > lim->move_last) {
            return BW_STRADE_BAD_MOVE;
        }
    }
    if (in->has_mail) {
        return BW_STRADE_HAS_MAIL;
    }
    if (in->level < 1u || in->level > 100u) {
        return BW_STRADE_BAD_LEVEL;
    }
    return BW_STRADE_OK;
}

static const BwTradeEvo *find_evo(const BwTradeEvo *evos, u32 nevo,
                                  const BwTradeInspect *in,
                                  const BwTradeInspect *partner,
                                  BwTradeHoldFn hold, void *hold_user)
{
    u32 i;

    if (in->is_egg || evos == NULL) {
        return NULL;
    }
    if (hold != NULL && hold(in->item, hold_user) == HOLD_EVERSTONE &&
        in->species != SPECIES_KADABRA) {
        return NULL;
    }
    for (i = 0; i < nevo; i++) {
        const BwTradeEvo *e = &evos[i];

        if (e->from_species != in->species || e->to_species == 0u) {
            continue;
        }
        if (e->method == EVO_TRADE) {
            return e;
        }
        if (e->method == EVO_TRADE_ITEM && in->item == e->param) {
            return e;
        }
        if (e->method == EVO_TRADE_PAIR && partner != NULL && !partner->is_egg) {
            int pair = (in->species == SPECIES_KARRABLAST &&
                        partner->species == SPECIES_SHELMET) ||
                       (in->species == SPECIES_SHELMET &&
                        partner->species == SPECIES_KARRABLAST);
            int blocked = hold != NULL &&
                          hold(partner->item, hold_user) == HOLD_EVERSTONE;

            if (pair && !blocked) {
                return e;
            }
        }
    }
    return NULL;
}

void bw_session_trade_predict(const u8 incoming[BW_STRADE_MON_SIZE],
                              const u8 *outgoing,
                              const BwTradeLimits *limits,
                              const BwTradeEvo *evos, u32 nevo,
                              BwTradeHoldFn hold, void *hold_user,
                              BwTradeInspect *out)
{
    u8 logical[0x80], party[0x54];
    u8 plog[0x80], pparty[0x54];
    BwTradeInspect partner;
    const BwTradeEvo *evo;
    BwTradeLimits lim;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    out->reason = BW_STRADE_BAD_ARG;
    if (incoming == NULL) {
        return;
    }
    if (limits == NULL) {
        bw_session_trade_limits_vanilla(&lim);
        limits = &lim;
    }
    if (open_record(incoming, logical, party, NULL) != 0) {
        out->reason = BW_STRADE_BAD_CHECKSUM;
        return;
    }
    fill_inspect(logical, party, rd32(incoming), out);
    out->reason = limits_ok(out, limits);
    if (out->reason != BW_STRADE_OK) {
        return;
    }
    memset(&partner, 0, sizeof partner);
    if (outgoing != NULL && open_record(outgoing, plog, pparty, NULL) == 0) {
        fill_inspect(plog, pparty, rd32(outgoing), &partner);
    }
    evo = find_evo(evos, nevo, out, outgoing != NULL ? &partner : NULL, hold, hold_user);
    if (evo != NULL && evo->to_species != 0u && evo->to_species <= limits->species_last) {
        out->evo_to = evo->to_species;
        out->evo_method = evo->method;
    }
}

/* ------------------------------------------------------------------ */
/* Journal                                                             */
/* ------------------------------------------------------------------ */

static void jour_seal(Jour *j)
{
    j->magic = JOUR_MAGIC;
    j->version = JOUR_VERSION;
    j->crc = crc32_of(j, (u32)offsetof(Jour, crc));
}

static int jour_ok(const Jour *j)
{
    if (j->magic != JOUR_MAGIC || j->version != JOUR_VERSION) {
        return 0;
    }
    return j->crc == crc32_of(j, (u32)offsetof(Jour, crc));
}

static int mkdir_parent(const char *path)
{
    char buf[540];
    size_t n, i;

    n = strlen(path);
    if (n == 0 || n >= sizeof buf) {
        return -1;
    }
    memcpy(buf, path, n + 1u);
    for (i = 1; i < n; i++) {
        if (buf[i] != '/' && buf[i] != '\\') {
            continue;
        }
        buf[i] = '\0';
#if defined(_WIN32)
        if (buf[0] != '\0') {
            (void)_mkdir(buf);
        }
#else
        if (buf[0] != '\0') {
            (void)mkdir(buf, 0775);
        }
#endif
        buf[i] = '/';
    }
    return 0;
}

static int sync_fp(FILE *f)
{
    if (fflush(f) != 0) {
        return -1;
    }
#if defined(_WIN32)
    return _commit(_fileno(f)) == 0 ? 0 : -1;
#else
    return fsync(fileno(f)) == 0 ? 0 : -1;
#endif
}

static int replace_file(const char *from, const char *to)
{
#if defined(_WIN32)
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

static int journal_write(BwSessionTrade *t)
{
    char tmp[540];
    FILE *f;
    int n;

    if (t->cfg.journal_path[0] == '\0') {
        return -1;
    }
    n = snprintf(tmp, sizeof tmp, "%s.tmp", t->cfg.journal_path);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        return -1;
    }
    jour_seal(&t->disk);
    if (mkdir_parent(t->cfg.journal_path) != 0) {
        return -1;
    }
    f = fopen(tmp, "wb");
    if (f == NULL) {
        return -1;
    }
    if (fwrite(&t->disk, 1, sizeof t->disk, f) != sizeof t->disk || sync_fp(f) != 0) {
        fclose(f);
        remove(tmp);
        return -1;
    }
    if (fclose(f) != 0) {
        remove(tmp);
        return -1;
    }
    if (replace_file(tmp, t->cfg.journal_path) != 0) {
        remove(tmp);
        return -1;
    }
    t->have_disk = 1;
    return 0;
}

static int journal_load(BwSessionTrade *t)
{
    FILE *f;
    Jour j;

    memset(&t->disk, 0, sizeof t->disk);
    t->have_disk = 0;
    t->journal_damaged = 0;
    if (t->cfg.journal_path[0] == '\0') {
        return 0;
    }
    f = fopen(t->cfg.journal_path, "rb");
    if (f == NULL) {
        return 0;
    }
    if (fread(&j, 1, sizeof j, f) != sizeof j || !jour_ok(&j)) {
        fclose(f);
        t->journal_damaged = 1;
        return -1;
    }
    fclose(f);
    t->disk = j;
    t->have_disk = 1;
    return 0;
}

static int tomb_has(const Jour *j, const u8 id[BW_STRADE_ID_LEN])
{
    u32 i, n = j->tomb_n < TOMBS ? j->tomb_n : TOMBS;

    for (i = 0; i < n; i++) {
        if (memcmp(j->tombs[i], id, BW_STRADE_ID_LEN) == 0) {
            return 1;
        }
    }
    return 0;
}

static void tomb_add(Jour *j, const u8 id[BW_STRADE_ID_LEN])
{
    u32 slot;

    if (tomb_has(j, id)) {
        return;
    }
    slot = j->tomb_n % TOMBS;
    memcpy(j->tombs[slot], id, BW_STRADE_ID_LEN);
    if (j->tomb_n < TOMBS) {
        j->tomb_n++;
    }
}

static int hash_eq(const u8 *a, const u8 *b)
{
    return memcmp(a, b, BW_STRADE_HASH_LEN) == 0;
}

static int slot_hash_is(BwSessionTrade *t, u32 slot, const u8 *want)
{
    u8 mon[BW_STRADE_MON_SIZE];
    u8 h[BW_STRADE_HASH_LEN];

    if (t->cfg.store.read == NULL ||
        t->cfg.store.read(t->cfg.store.user, slot, mon) != 0) {
        return 0;
    }
    hash_mon(mon, h);
    return hash_eq(h, want);
}

/* ------------------------------------------------------------------ */
/* Engine                                                              */
/* ------------------------------------------------------------------ */

static void touch(BwSessionTrade *t)
{
    t->stamp++;
}

static void set_status(BwSessionTrade *t, const char *s)
{
    snprintf(t->status, sizeof t->status, "%s", s != NULL ? s : "");
    touch(t);
}

static u32 rnd(BwSessionTrade *t)
{
    if (t->cfg.random != NULL) {
        return t->cfg.random(t->cfg.random_user);
    }
    t->rng = t->rng * 1664525u + 1013904223u;
    return t->rng;
}

static int send_raw(BwSessionTrade *t, const void *data, u32 n)
{
    if (t->cfg.send == NULL || t->partner == 0u) {
        return -1;
    }
    return t->cfg.send(t->cfg.send_user, t->partner, data, n);
}

static int send_hdr(BwSessionTrade *t, u8 type)
{
    u8 buf[HDR];

    buf[0] = type;
    buf[1] = MSG_VER;
    memcpy(buf + 2, t->tid, BW_STRADE_ID_LEN);
    return send_raw(t, buf, HDR);
}

static void clear_offers(BwSessionTrade *t)
{
    t->have_local = t->have_peer = 0;
    t->conf_l = t->conf_p = 0;
    t->peer_prepared = t->peer_saved = 0;
    t->save_started = 0;
    t->have_applied = 0;
    t->resume_sent = 0;
    t->peer_left = 0;
    t->rev_l = t->rev_p = 0;
    t->evo_to = t->evo_method = 0;
    t->slot = 0;
    memset(t->local_mon, 0, sizeof t->local_mon);
    memset(t->peer_mon, 0, sizeof t->peer_mon);
    memset(t->hash_l, 0, sizeof t->hash_l);
    memset(t->hash_p, 0, sizeof t->hash_p);
    memset(t->applied, 0, sizeof t->applied);
}

static void remember_tid(BwSessionTrade *t)
{
    if (!t->have_disk) {
        memset(&t->disk, 0, sizeof t->disk);
    }
    tomb_add(&t->disk, t->tid);
}

static int busy_phase(int phase)
{
    return phase == BW_STRADE_OPEN_SENT || phase == BW_STRADE_OPEN_IN ||
           phase == BW_STRADE_TALK || phase == BW_STRADE_PREPARED ||
           phase == BW_STRADE_APPLYING || phase == BW_STRADE_SAVED ||
           phase == BW_STRADE_AWAIT || phase == BW_STRADE_UNRESOLVED;
}

void bw_session_trade_limits_vanilla(BwTradeLimits *out)
{
    if (out == NULL) {
        return;
    }
    out->species_last = BW_STRADE_VANILLA_SPECIES;
    out->item_last = BW_STRADE_VANILLA_ITEM;
    out->move_last = BW_STRADE_VANILLA_MOVE;
}

BwSessionTrade *bw_session_trade_create(const BwSessionTradeCfg *cfg)
{
    BwSessionTrade *t;

    if (cfg == NULL || cfg->store.read == NULL || cfg->store.write == NULL ||
        cfg->store.count == NULL || cfg->send == NULL) {
        return NULL;
    }
    t = (BwSessionTrade *)calloc(1, sizeof *t);
    if (t == NULL) {
        return NULL;
    }
    t->cfg = *cfg;
    if (t->cfg.limits.species_last == 0u) {
        bw_session_trade_limits_vanilla(&t->cfg.limits);
    }
    t->rng = 0xA5A5A5A5u ^ (cfg->self_peer * 0x9E3779B9u);
    t->phase = BW_STRADE_IDLE;
    t->confirm_wait = 60u * 60u; /* one minute at the field rate */
    set_status(t, "No session trade.");
    return t;
}

static const char *phase_name(int phase)
{
    switch (phase) {
    case BW_STRADE_IDLE: return "idle";
    case BW_STRADE_OPEN_SENT: return "open-sent";
    case BW_STRADE_OPEN_IN: return "open-in";
    case BW_STRADE_TALK: return "talk";
    case BW_STRADE_PREPARED: return "prepared";
    case BW_STRADE_APPLYING: return "applying";
    case BW_STRADE_SAVED: return "saved";
    case BW_STRADE_AWAIT: return "await";
    case BW_STRADE_UNRESOLVED: return "unresolved";
    case BW_STRADE_DONE: return "done";
    case BW_STRADE_CANCELLED: return "cancelled";
    default: return "other";
    }
}

static void log_trade(const BwSessionTrade *t, const char *what)
{
    fprintf(stderr, "eidos: trade: %s (phase %s, peer %u)\n",
            what, phase_name(t->phase), (unsigned)t->partner);
}

void bw_session_trade_destroy(BwSessionTrade *t)
{
    if (t == NULL) {
        return;
    }
    free(t->evos);
    free(t);
}

int bw_session_trade_set_evolutions(BwSessionTrade *t, const BwTradeEvo *rows, u32 count)
{
    BwTradeEvo *copy = NULL;

    if (t == NULL || (count > 0u && rows == NULL) || count > EVO_MAX) {
        return -1;
    }
    if (count > 0u) {
        copy = (BwTradeEvo *)malloc(sizeof(BwTradeEvo) * count);
        if (copy == NULL) {
            return -1;
        }
        memcpy(copy, rows, sizeof(BwTradeEvo) * count);
    }
    free(t->evos);
    t->evos = copy;
    t->nevo = count;
    return 0;
}

int bw_session_trade_load_evo_narc(BwSessionTrade *t, const void *narc, u32 size)
{
    const u8 *p = (const u8 *)narc;
    const u8 *image = NULL;
    const u8 *fat = NULL;
    u32 left, image_n = 0, fat_n = 0;
    u32 count, i;
    BwTradeEvo *rows;
    u32 nrows = 0;

    if (t == NULL || p == NULL || size < 16u || rd32(p) != 0x4352414Eu) {
        return -1; /* 'NARC' little-endian */
    }
    left = size - 0x10u;
    p += 0x10u;
    while (left >= 8u) {
        u32 magic = rd32(p);
        u32 chunk = rd32(p + 4);

        if (chunk < 8u || chunk > left) {
            break;
        }
        if (magic == 0x46415442u) { /* 'BTAF' */
            fat = p;
            fat_n = chunk;
        } else if (magic == 0x46494D47u) { /* 'GMIF' */
            image = p + 8;
            image_n = chunk - 8u;
        }
        p += chunk;
        left -= chunk;
    }
    if (fat == NULL || image == NULL || fat_n < 12u) {
        return -1;
    }
    count = rd16(fat + 8);
    rows = (BwTradeEvo *)calloc(EVO_MAX, sizeof(BwTradeEvo));
    if (rows == NULL) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        u32 start, end, n, r;
        const u8 *mem;

        if (12u + (i + 1u) * 8u > fat_n) {
            break;
        }
        start = rd32(fat + 12u + i * 8u);
        end = rd32(fat + 12u + i * 8u + 4u);
        if (end < start || end > image_n) {
            continue;
        }
        mem = image + start;
        n = end - start;
        if (n < 42u) {
            continue;
        }
        for (r = 0; r < 7u; r++) {
            u16 method = rd16(mem + r * 6u);
            u16 param = rd16(mem + r * 6u + 2u);
            u16 to = rd16(mem + r * 6u + 4u);

            if (method == 0u || to == 0u) {
                continue;
            }
            if (nrows >= EVO_MAX) {
                free(rows);
                return -1;
            }
            rows[nrows].from_species = (u16)i;
            rows[nrows].method = method;
            rows[nrows].param = param;
            rows[nrows].to_species = to;
            nrows++;
        }
    }
    free(t->evos);
    t->evos = rows;
    t->nevo = nrows;
    return 0;
}

static void journal_capture(BwSessionTrade *t, u32 phase)
{
    Jour *j = &t->disk;

    j->phase = phase;
    j->slot = t->slot;
    j->peer = t->partner;
    j->rev_l = t->rev_l;
    j->rev_p = t->rev_p;
    j->evo_to = t->evo_to;
    j->evo_method = t->evo_method;
    memcpy(j->tid, t->tid, BW_STRADE_ID_LEN);
    memcpy(j->hash_out, t->hash_l, BW_STRADE_HASH_LEN);
    memcpy(j->hash_in, t->hash_p, BW_STRADE_HASH_LEN);
    memcpy(j->outgoing, t->local_mon, BW_STRADE_MON_SIZE);
    memcpy(j->incoming, t->peer_mon, BW_STRADE_MON_SIZE);
    if (t->have_applied) {
        memcpy(j->applied, t->applied, BW_STRADE_MON_SIZE);
    }
}

static int persist(BwSessionTrade *t, u32 phase)
{
    journal_capture(t, phase);
    return journal_write(t);
}

static void finish_idle(BwSessionTrade *t, const char *status)
{
    if (t->tid[0] != 0 || t->have_local) {
        remember_tid(t);
    }
    clear_offers(t);
    memset(t->tid, 0, sizeof t->tid);
    t->partner = 0;
    t->confirm_left = 0;
    t->save_wait_logged = 0;
    if (t->outcome != BW_STRADE_OUT_BOTH_SAVED) {
        t->outcome = BW_STRADE_OUT_CANCELLED;
    }
    t->phase = BW_STRADE_IDLE;
    if (t->have_disk) {
        t->disk.phase = BW_STRADE_IDLE;
        (void)journal_write(t);
    }
    set_status(t, status);
}

int bw_session_trade_blocked(const BwSessionTrade *t)
{
    if (t == NULL) {
        return 1;
    }
    if (t->journal_damaged) {
        return 1;
    }
    return busy_phase(t->phase);
}

static int mon_ok(BwSessionTrade *t, const u8 mon[BW_STRADE_MON_SIZE], BwTradeInspect *out)
{
    bw_session_trade_predict(mon, NULL, &t->cfg.limits, t->evos, t->nevo,
                             t->cfg.hold_effect, t->cfg.hold_user, out);
    return out->reason == BW_STRADE_OK;
}

void bw_session_trade_recover(BwSessionTrade *t)
{
    u8 hash_applied[BW_STRADE_HASH_LEN];
    int match_out, match_applied;

    if (t == NULL) {
        return;
    }
    if (journal_load(t) != 0) {
        t->phase = BW_STRADE_UNRESOLVED;
        set_status(t, "Trade journal is damaged. Party was not changed. Acknowledge after you have compared parties.");
        fprintf(stderr, "eidos: trade: journal damaged. Not acknowledged. Party was not changed.\n");
        return;
    }
    if (!t->have_disk || t->disk.phase == BW_STRADE_IDLE ||
        t->disk.phase == BW_STRADE_DONE || t->disk.phase == BW_STRADE_CANCELLED) {
        if (t->have_disk && t->disk.phase != BW_STRADE_IDLE) {
            tomb_add(&t->disk, t->disk.tid);
            t->disk.phase = BW_STRADE_IDLE;
            (void)journal_write(t);
        }
        t->phase = BW_STRADE_IDLE;
        set_status(t, "No open session trade.");
        return;
    }
    fprintf(stderr, "eidos: trade: journal on disk (phase %s). Not acknowledged. Compare parties before another trade.\n",
            phase_name((int)t->disk.phase));
    memcpy(t->tid, t->disk.tid, BW_STRADE_ID_LEN);
    t->partner = t->disk.peer;
    t->slot = t->disk.slot;
    t->rev_l = t->disk.rev_l;
    t->rev_p = t->disk.rev_p;
    t->evo_to = t->disk.evo_to;
    t->evo_method = t->disk.evo_method;
    memcpy(t->local_mon, t->disk.outgoing, BW_STRADE_MON_SIZE);
    memcpy(t->peer_mon, t->disk.incoming, BW_STRADE_MON_SIZE);
    memcpy(t->hash_l, t->disk.hash_out, BW_STRADE_HASH_LEN);
    memcpy(t->hash_p, t->disk.hash_in, BW_STRADE_HASH_LEN);
    memcpy(t->applied, t->disk.applied, BW_STRADE_MON_SIZE);
    t->have_local = t->have_peer = 1;
    hash_mon(t->disk.applied, hash_applied);
    {
        u32 i;

        t->have_applied = 0;
        for (i = 0; i < BW_STRADE_MON_SIZE; i++) {
            if (t->disk.applied[i] != 0u) {
                t->have_applied = 1;
                break;
            }
        }
    }
    match_out = slot_hash_is(t, t->slot, t->hash_l);
    match_applied = t->have_applied && slot_hash_is(t, t->slot, hash_applied);
    if (t->disk.phase == BW_STRADE_PREPARED || t->disk.phase == BW_STRADE_AWAIT) {
        if (match_out) {
            t->phase = BW_STRADE_AWAIT;
            t->disk.phase = BW_STRADE_AWAIT;
            (void)journal_write(t);
            set_status(t, "Trade paused before the save. Your party is unchanged. It resumes only if the partner still has the same trade.");
            return;
        }
        t->phase = BW_STRADE_UNRESOLVED;
        set_status(t, "Trade journal and party disagree. Nothing was written. Acknowledge only after comparing with your partner.");
        return;
    }
    if (t->disk.phase == BW_STRADE_APPLYING) {
        if (match_out && !match_applied) {
            t->phase = BW_STRADE_AWAIT;
            t->save_started = 0;
            t->disk.phase = BW_STRADE_AWAIT;
            (void)journal_write(t);
            set_status(t, "The save did not finish. Your party still has your Pokemon. The trade waits for the partner.");
            return;
        }
        t->phase = BW_STRADE_UNRESOLVED;
        set_status(t, "The save was interrupted after the party changed. Do not trade this Pokemon again until you and your partner compare.");
        return;
    }
    /* SAVED or UNRESOLVED */
    t->phase = BW_STRADE_UNRESOLVED;
    (void)match_applied;
    set_status(t, "This side saved a trade the partner has not confirmed. The party was left as saved. Compare before you acknowledge.");
}

int bw_session_trade_phase(const BwSessionTrade *t)
{
    return t != NULL ? t->phase : BW_STRADE_IDLE;
}

u32 bw_session_trade_partner(const BwSessionTrade *t)
{
    return t != NULL ? t->partner : 0u;
}

u32 bw_session_trade_stamp(const BwSessionTrade *t)
{
    return t != NULL ? t->stamp : 0u;
}

const char *bw_session_trade_status(const BwSessionTrade *t)
{
    return t != NULL ? t->status : "";
}

int bw_session_trade_hash_local(const BwSessionTrade *t, u8 out[BW_STRADE_HASH_LEN])
{
    if (t == NULL || out == NULL || !t->have_local) {
        return 0;
    }
    memcpy(out, t->hash_l, BW_STRADE_HASH_LEN);
    return 1;
}

int bw_session_trade_hash_peer(const BwSessionTrade *t, u8 out[BW_STRADE_HASH_LEN])
{
    if (t == NULL || out == NULL || !t->have_peer) {
        return 0;
    }
    memcpy(out, t->hash_p, BW_STRADE_HASH_LEN);
    return 1;
}

int bw_session_trade_peer_mon(const BwSessionTrade *t, u8 out[BW_STRADE_MON_SIZE])
{
    if (t == NULL || out == NULL || !t->have_peer) {
        return 0;
    }
    memcpy(out, t->peer_mon, BW_STRADE_MON_SIZE);
    return 1;
}

int bw_session_trade_local_mon(const BwSessionTrade *t, u8 out[BW_STRADE_MON_SIZE])
{
    if (t == NULL || out == NULL || !t->have_local) {
        return 0;
    }
    memcpy(out, t->local_mon, BW_STRADE_MON_SIZE);
    return 1;
}

static int new_tid(BwSessionTrade *t)
{
    int i;

    for (i = 0; i < 4; i++) {
        wr32(t->tid + (u32)i * 4u, rnd(t) ^ (t->cfg.self_peer << (i * 3)));
    }
    if (tomb_has(&t->disk, t->tid)) {
        t->tid[0] ^= 0x5Au;
    }
    return 0;
}

void bw_session_trade_note_peer(BwSessionTrade *t, u32 peer)
{
    if (t == NULL || t->phase != BW_STRADE_IDLE) {
        return;
    }
    t->partner = peer;
}

int bw_session_trade_start(BwSessionTrade *t, u32 peer)
{
    if (t == NULL || peer == 0u || peer == t->cfg.self_peer) {
        if (t != NULL) {
            set_status(t, "A private link with exactly one other player is required.");
        }
        return -1;
    }
    if (t->journal_damaged || busy_phase(t->phase)) {
        set_status(t, "A trade is already open.");
        return -1;
    }
    t->outcome = 0;
    clear_offers(t);
    t->partner = peer;
    new_tid(t);
    t->phase = BW_STRADE_OPEN_SENT;
    if (send_hdr(t, (u8)MSG_OPEN) != 0) {
        t->phase = BW_STRADE_IDLE;
        set_status(t, "The offer could not be sent.");
        return -1;
    }
    set_status(t, "Trade offered. Waiting for the other player to accept.");
    return 0;
}

int bw_session_trade_accept(BwSessionTrade *t)
{
    if (t == NULL || t->phase != BW_STRADE_OPEN_IN) {
        return -1;
    }
    if (send_hdr(t, (u8)MSG_OPEN_OK) != 0) {
        return -1;
    }
    t->phase = BW_STRADE_TALK;
    set_status(t, "Trade open. Choose a party Pokemon. This is not a DS Union Room trade.");
    return 0;
}

int bw_session_trade_decline(BwSessionTrade *t)
{
    if (t == NULL || t->phase != BW_STRADE_OPEN_IN) {
        return -1;
    }
    (void)send_hdr(t, (u8)MSG_OPEN_NO);
    finish_idle(t, "Trade declined.");
    return 0;
}

int bw_session_trade_offer(BwSessionTrade *t, u32 slot)
{
    u8 mon[BW_STRADE_MON_SIZE];
    u8 buf[HDR + 8u + BW_STRADE_MON_SIZE];
    BwTradeInspect info;
    int nmon;

    if (t == NULL || t->phase != BW_STRADE_TALK) {
        return -1;
    }
    if (t->peer_prepared || t->phase == BW_STRADE_PREPARED) {
        return -1;
    }
    nmon = t->cfg.store.count(t->cfg.store.user);
    if (nmon < 1 || nmon > 6 || slot >= (u32)nmon) {
        set_status(t, nmon < 1 ? "No Pokemon to offer." : "That party slot cannot be offered.");
        if (nmon < 1) {
            fprintf(stderr, "eidos: trade: no pokemon to offer\n");
        }
        return -1;
    }
    if (t->cfg.store.read(t->cfg.store.user, slot, mon) != 0 || !mon_ok(t, mon, &info)) {
        set_status(t, "That Pokemon cannot be traded.");
        return -1;
    }
    memcpy(t->local_mon, mon, sizeof mon);
    hash_mon(mon, t->hash_l);
    t->rev_l++;
    t->have_local = 1;
    t->slot = slot;
    t->conf_l = t->conf_p = 0;
    t->peer_prepared = 0;
    buf[0] = MSG_OFFER;
    buf[1] = MSG_VER;
    memcpy(buf + 2, t->tid, BW_STRADE_ID_LEN);
    wr32(buf + HDR, slot);
    wr32(buf + HDR + 4, t->rev_l);
    memcpy(buf + HDR + 8, mon, BW_STRADE_MON_SIZE);
    if (send_raw(t, buf, HDR + 8u + BW_STRADE_MON_SIZE) != 0) {
        set_status(t, "The offer could not be sent.");
        return -1;
    }
    set_status(t, "Your offer is sent. Both players must confirm these exact bytes.");
    return 0;
}

static int both_ready(const BwSessionTrade *t)
{
    return t->have_local && t->have_peer && t->rev_l > 0u && t->rev_p > 0u;
}

int bw_session_trade_confirm(BwSessionTrade *t)
{
    u8 buf[HDR + 8u + BW_STRADE_HASH_LEN * 2u];
    u8 now[BW_STRADE_MON_SIZE];
    u8 h[BW_STRADE_HASH_LEN];

    if (t == NULL || t->phase != BW_STRADE_TALK || !both_ready(t)) {
        if (t != NULL && t->phase == BW_STRADE_TALK) {
            set_status(t, "Both players have to offer a Pokemon before confirm.");
            fprintf(stderr, "eidos: trade: confirm refused, offers are not both ready\n");
        }
        return -1;
    }
    if (t->cfg.store.read(t->cfg.store.user, t->slot, now) != 0) {
        return -1;
    }
    hash_mon(now, h);
    if (!hash_eq(h, t->hash_l)) {
        set_status(t, "Your Pokemon changed. Offer it again.");
        t->conf_l = 0;
        return -1;
    }
    t->conf_l = 1;
    buf[0] = MSG_CONFIRM;
    buf[1] = MSG_VER;
    memcpy(buf + 2, t->tid, BW_STRADE_ID_LEN);
    wr32(buf + HDR, t->rev_l);
    wr32(buf + HDR + 4, t->rev_p);
    memcpy(buf + HDR + 8, t->hash_l, BW_STRADE_HASH_LEN);
    memcpy(buf + HDR + 8 + BW_STRADE_HASH_LEN, t->hash_p, BW_STRADE_HASH_LEN);
    if (send_raw(t, buf, sizeof buf) != 0) {
        t->conf_l = 0;
        return -1;
    }
    t->confirm_left = t->confirm_wait;
    set_status(t, t->conf_p
                   ? "Both confirmed. The trade saves when the field is free. Not saved yet."
                   : "You confirmed. Waiting for the other player. Not saved yet.");
    log_trade(t, t->conf_p ? "both sides confirmed" : "local confirm, waiting");
    return 0;
}

static void send_cancel(BwSessionTrade *t, u8 why)
{
    u8 buf[HDR + 1u];

    buf[0] = MSG_CANCEL;
    buf[1] = MSG_VER;
    memcpy(buf + 2, t->tid, BW_STRADE_ID_LEN);
    buf[HDR] = why;
    (void)send_raw(t, buf, sizeof buf);
}

int bw_session_trade_cancel(BwSessionTrade *t)
{
    if (t == NULL) {
        return -1;
    }
    if (t->phase == BW_STRADE_APPLYING || t->phase == BW_STRADE_SAVED ||
        t->phase == BW_STRADE_UNRESOLVED) {
        set_status(t, "The save has started. Cancel will not roll it back.");
        return -1;
    }
    if (t->phase == BW_STRADE_IDLE) {
        return -1;
    }
    send_cancel(t, WHY_USER);
    t->outcome = BW_STRADE_OUT_CANCELLED;
    if (t->phase == BW_STRADE_PREPARED || t->phase == BW_STRADE_AWAIT) {
        (void)persist(t, BW_STRADE_CANCELLED);
    }
    finish_idle(t, "Trade cancelled. No Pokemon moved.");
    return 0;
}

int bw_session_trade_acknowledge(BwSessionTrade *t)
{
    char prev[540];
    int n;

    if (t == NULL) {
        return -1;
    }
    if (t->phase != BW_STRADE_UNRESOLVED && t->phase != BW_STRADE_AWAIT &&
        !t->journal_damaged) {
        return -1;
    }
    n = snprintf(prev, sizeof prev, "%s.prev", t->cfg.journal_path);
    if (n > 0 && (size_t)n < sizeof prev && t->cfg.journal_path[0] != '\0') {
        (void)replace_file(t->cfg.journal_path, prev);
    }
    t->journal_damaged = 0;
    t->outcome = 0;
    if (t->phase == BW_STRADE_AWAIT) {
        finish_idle(t, "Paused trade dropped. Your party was not changed.");
        return 0;
    }
    /* UNRESOLVED: do not rewrite the party.  The copy above keeps the record. */
    remember_tid(t);
    clear_offers(t);
    memset(t->tid, 0, sizeof t->tid);
    t->partner = 0;
    t->phase = BW_STRADE_IDLE;
    t->disk.phase = BW_STRADE_IDLE;
    (void)journal_write(t);
    set_status(t, "Unresolved trade acknowledged. The party was not rewritten. The old journal is .prev.");
    return 0;
}

static void become_talk(BwSessionTrade *t)
{
    t->phase = BW_STRADE_TALK;
    set_status(t, "Trade open. Choose a party Pokemon. This is not a DS Union Room trade.");
}

static int tid_is(const BwSessionTrade *t, const u8 *msg)
{
    return memcmp(t->tid, msg + 2, BW_STRADE_ID_LEN) == 0;
}

static void on_offer(BwSessionTrade *t, const u8 *msg, u32 n)
{
    BwTradeInspect info;
    u32 rev;
    u8 h[BW_STRADE_HASH_LEN];
    const u8 *mon;

    if (n != HDR + 8u + BW_STRADE_MON_SIZE) {
        send_cancel(t, WHY_MALFORMED);
        finish_idle(t, "The partner's offer was malformed. Nothing moved.");
        return;
    }
    if (t->phase != BW_STRADE_TALK && t->phase != BW_STRADE_PREPARED &&
        t->phase != BW_STRADE_AWAIT) {
        return;
    }
    rev = rd32(msg + HDR + 4);
    mon = msg + HDR + 8;
    if (rev == 0u || (t->have_peer && rev < t->rev_p)) {
        set_status(t, "Ignored an older offer.");
        return;
    }
    if (!mon_ok(t, mon, &info)) {
        send_cancel(t, WHY_MALFORMED);
        finish_idle(t, "The partner's Pokemon failed the checksum or the party rules. Nothing moved.");
        return;
    }
    hash_mon(mon, h);
    if (t->have_peer && rev == t->rev_p && hash_eq(h, t->hash_p)) {
        return; /* replay of the current offer */
    }
    if (t->phase == BW_STRADE_PREPARED || t->peer_prepared) {
        send_cancel(t, WHY_OFFER);
        finish_idle(t, "The offer changed after it was locked. Nothing moved.");
        return;
    }
    memcpy(t->peer_mon, mon, BW_STRADE_MON_SIZE);
    memcpy(t->hash_p, h, BW_STRADE_HASH_LEN);
    t->rev_p = rev;
    t->have_peer = 1;
    t->conf_l = t->conf_p = 0;
    t->peer_prepared = 0;
    set_status(t, "Partner's Pokemon is here. Confirm only if it is the one you expect.");
}

static void on_confirm(BwSessionTrade *t, const u8 *msg, u32 n)
{
    u32 rev_own, rev_peer;
    const u8 *hash_own, *hash_peer;

    if (n != HDR + 8u + BW_STRADE_HASH_LEN * 2u || t->phase != BW_STRADE_TALK) {
        return;
    }
    rev_own = rd32(msg + HDR);
    rev_peer = rd32(msg + HDR + 4);
    hash_own = msg + HDR + 8;
    hash_peer = hash_own + BW_STRADE_HASH_LEN;
    /* Their "own" is our peer offer; their "peer" is our local offer. */
    if (!t->have_peer || !t->have_local || rev_own != t->rev_p || rev_peer != t->rev_l ||
        !hash_eq(hash_own, t->hash_p) || !hash_eq(hash_peer, t->hash_l)) {
        set_status(t, "Ignored a confirmation for a different offer.");
        return;
    }
    t->conf_p = 1;
    set_status(t, t->conf_l
                   ? "Both confirmed the same bytes. The trade saves when the field is free."
                   : "The other player confirmed. Confirm the same Pokemon to continue.");
}

static int send_prepared(BwSessionTrade *t)
{
    u8 buf[HDR + 8u + BW_STRADE_HASH_LEN * 2u];

    buf[0] = MSG_PREPARED;
    buf[1] = MSG_VER;
    memcpy(buf + 2, t->tid, BW_STRADE_ID_LEN);
    wr32(buf + HDR, t->rev_l);
    wr32(buf + HDR + 4, t->rev_p);
    memcpy(buf + HDR + 8, t->hash_l, BW_STRADE_HASH_LEN);
    memcpy(buf + HDR + 8 + BW_STRADE_HASH_LEN, t->hash_p, BW_STRADE_HASH_LEN);
    return send_raw(t, buf, sizeof buf);
}

static int send_saved(BwSessionTrade *t)
{
    u8 buf[HDR + BW_STRADE_HASH_LEN * 2u];

    buf[0] = MSG_SAVED;
    buf[1] = MSG_VER;
    memcpy(buf + 2, t->tid, BW_STRADE_ID_LEN);
    memcpy(buf + HDR, t->hash_l, BW_STRADE_HASH_LEN);
    memcpy(buf + HDR + BW_STRADE_HASH_LEN, t->hash_p, BW_STRADE_HASH_LEN);
    return send_raw(t, buf, sizeof buf);
}

static void lock_prepare(BwSessionTrade *t)
{
    BwTradeInspect in, out;
    u8 now[BW_STRADE_MON_SIZE];
    u8 h[BW_STRADE_HASH_LEN];

    if (t->cfg.store.read(t->cfg.store.user, t->slot, now) != 0) {
        send_cancel(t, WHY_OFFER);
        finish_idle(t, "Your party could not be read. Nothing moved.");
        return;
    }
    hash_mon(now, h);
    if (!hash_eq(h, t->hash_l)) {
        send_cancel(t, WHY_STALE);
        finish_idle(t, "Your Pokemon changed before the save. Nothing moved.");
        return;
    }
    bw_session_trade_predict(t->peer_mon, t->local_mon, &t->cfg.limits, t->evos, t->nevo,
                             t->cfg.hold_effect, t->cfg.hold_user, &in);
    if (in.reason != BW_STRADE_OK) {
        send_cancel(t, WHY_MALFORMED);
        finish_idle(t, "The offered Pokemon no longer passes the checks. Nothing moved.");
        return;
    }
    (void)out;
    if (in.evo_to != 0u && t->cfg.evolve == NULL) {
        send_cancel(t, WHY_EVOLVE);
        finish_idle(t, "This Pokemon would evolve and this build cannot evolve it safely. Nothing moved.");
        return;
    }
    t->evo_to = in.evo_to;
    t->evo_method = in.evo_method;
    if (persist(t, BW_STRADE_PREPARED) != 0) {
        send_cancel(t, WHY_SAVE);
        finish_idle(t, "The trade journal could not be saved. Nothing moved.");
        return;
    }
    t->phase = BW_STRADE_PREPARED;
    t->confirm_left = 0;
    log_trade(t, "journal locked, party not saved");
    if (send_prepared(t) != 0) {
        set_status(t, "Journal saved. The prepared notice could not be sent yet.");
        return;
    }
    set_status(t, "Both sides locked the same Pokemon. Waiting for the field to save.");
}

static int restore_outgoing(BwSessionTrade *t)
{
    if (t->cfg.store.write(t->cfg.store.user, t->slot, t->local_mon) != 0) {
        return -1;
    }
    return 0;
}

static void mark_done(BwSessionTrade *t)
{
    (void)persist(t, BW_STRADE_DONE);
    t->outcome = BW_STRADE_OUT_BOTH_SAVED;
    log_trade(t, "both sides reported a save; not one shared write");
    finish_idle(t, "Each side reported its own save. This is not one shared write.");
}

static void mark_unresolved(BwSessionTrade *t, const char *status)
{
    t->phase = BW_STRADE_UNRESOLVED;
    t->outcome = 0;
    (void)persist(t, BW_STRADE_UNRESOLVED);
    set_status(t, status);
    log_trade(t, "unresolved, journal kept, not acknowledged");
}

static void apply_and_save(BwSessionTrade *t)
{
    int ready;
    u8 now[BW_STRADE_MON_SIZE];
    u8 h[BW_STRADE_HASH_LEN];

    if (t->save_started) {
        return;
    }
    if (t->cfg.store.save_ready == NULL || t->cfg.store.save_begin == NULL ||
        t->cfg.store.save_poll == NULL) {
        mark_unresolved(t, "This build has no save hook. The party was not changed.");
        return;
    }
    ready = t->cfg.store.save_ready(t->cfg.store.user);
    if (ready == 0) {
        set_status(t, "Both confirmed. The save waits until the field is free. Not saved yet.");
        if (!t->save_wait_logged) {
            t->save_wait_logged = 1;
            log_trade(t, "save waiting, party not written");
        }
        return;
    }
    if (ready < 0) {
        send_cancel(t, WHY_SAVE);
        finish_idle(t, "This game cannot save the trade. Nothing moved.");
        return;
    }
    if (t->cfg.store.read(t->cfg.store.user, t->slot, now) != 0) {
        mark_unresolved(t, "The party could not be read at save time. It was not written.");
        return;
    }
    hash_mon(now, h);
    if (!hash_eq(h, t->hash_l)) {
        send_cancel(t, WHY_STALE);
        finish_idle(t, "Your Pokemon changed before the save. Nothing moved.");
        return;
    }
    if (t->cfg.store.write(t->cfg.store.user, t->slot, t->peer_mon) != 0) {
        mark_unresolved(t, "The party could not be updated. Compare it before another trade.");
        return;
    }
    if (t->evo_to != 0u) {
        if (t->cfg.evolve == NULL ||
            t->cfg.evolve(t->cfg.evolve_user, t->slot, t->evo_to, t->evo_method) != 0) {
            if (restore_outgoing(t) == 0) {
                send_cancel(t, WHY_EVOLVE);
                finish_idle(t, "Evolution could not be applied. Your Pokemon was put back.");
            } else {
                mark_unresolved(t, "Evolution failed and the previous Pokemon could not be restored.");
            }
            return;
        }
    }
    if (t->cfg.store.read(t->cfg.store.user, t->slot, t->applied) != 0) {
        if (restore_outgoing(t) == 0) {
            finish_idle(t, "The new Pokemon could not be re-read. Your Pokemon was put back.");
        } else {
            mark_unresolved(t, "The party could not be re-read after the trade.");
        }
        return;
    }
    t->have_applied = 1;
    if (persist(t, BW_STRADE_APPLYING) != 0) {
        if (restore_outgoing(t) == 0) {
            finish_idle(t, "The journal could not be updated. Your Pokemon was put back.");
        } else {
            mark_unresolved(t, "The journal failed after the party changed.");
        }
        return;
    }
    t->phase = BW_STRADE_APPLYING;
    t->save_started = 1;
    log_trade(t, "party updated, save not finished");
    if (t->cfg.store.save_begin(t->cfg.store.user) != 0) {
        t->save_started = 0;
        if (restore_outgoing(t) == 0) {
            (void)persist(t, BW_STRADE_CANCELLED);
            finish_idle(t, "The save did not start. Your Pokemon was put back.");
        } else {
            mark_unresolved(t, "The save did not start and the previous Pokemon could not be restored.");
        }
    }
}

void bw_session_trade_set_confirm_wait(BwSessionTrade *t, u32 frames)
{
    if (t == NULL) {
        return;
    }
    t->confirm_wait = frames;
    t->confirm_left = 0;
}

int bw_session_trade_may_mp_save(int trade_saving, int settle, int mp_saving,
                                int *drop_wanted)
{
    if (drop_wanted != NULL) {
        *drop_wanted = 0;
    }
    if (trade_saving || settle > 0 || mp_saving) {
        if (drop_wanted != NULL) {
            *drop_wanted = 1;
        }
        return 0;
    }
    return 1;
}

void bw_session_trade_poll(BwSessionTrade *t)
{
    int pr;

    if (t == NULL) {
        return;
    }
    /* One player confirmed and the other has not.  After the wait, close
     * the talk.  A locked journal is left alone: expiring it would look
     * like a decision about a save that may already be in progress. */
    if (t->phase == BW_STRADE_TALK && t->confirm_wait > 0u && !t->peer_prepared &&
        ((t->conf_l && !t->conf_p) || (!t->conf_l && t->conf_p))) {
        if (t->confirm_left == 0u) {
            t->confirm_left = t->confirm_wait;
        } else if (--t->confirm_left == 0u) {
            send_cancel(t, WHY_USER);
            log_trade(t, "confirm wait ended, nothing moved");
            finish_idle(t, "The confirm wait ended before both confirmed. Nothing moved.");
            return;
        }
    } else {
        t->confirm_left = 0;
    }
    if (t->phase == BW_STRADE_TALK && t->conf_l && t->conf_p && !t->peer_prepared) {
        /* Lock locally once.  The partner's PREPARED may already be in. */
    }
    if (t->phase == BW_STRADE_TALK && t->conf_l && t->conf_p &&
        t->disk.phase != BW_STRADE_PREPARED && t->disk.phase != BW_STRADE_APPLYING) {
        lock_prepare(t);
    }
    if (t->phase == BW_STRADE_PREPARED && t->peer_prepared) {
        apply_and_save(t);
    }
    if (t->phase != BW_STRADE_APPLYING) {
        if (t->phase == BW_STRADE_SAVED && t->peer_saved) {
            mark_done(t);
        }
        return;
    }
    pr = t->cfg.store.save_poll(t->cfg.store.user);
    if (pr == 0) {
        return;
    }
    if (pr < 0) {
        if (restore_outgoing(t) == 0 && t->cfg.store.save_begin(t->cfg.store.user) == 0) {
            int back = t->cfg.store.save_poll(t->cfg.store.user);

            if (back == 1) {
                (void)persist(t, BW_STRADE_CANCELLED);
                finish_idle(t, "The save failed. Your Pokemon was restored and saved.");
                return;
            }
        }
        mark_unresolved(t, "The save failed. Compare parties before another trade.");
        return;
    }
    t->phase = BW_STRADE_SAVED;
    log_trade(t, "local save finished, partner save not known");
    if (persist(t, BW_STRADE_SAVED) != 0) {
        mark_unresolved(t, "The save finished but the journal could not record it.");
        return;
    }
    if (t->peer_left) {
        mark_unresolved(t, "The save finished after the partner left. It was not rolled back.");
        return;
    }
    (void)send_saved(t);
    if (t->peer_saved) {
        mark_done(t);
    } else {
        set_status(t, "Your save has the traded Pokemon. Waiting for the partner's save.");
    }
}

static int same_trade(const BwSessionTrade *t, const u8 *msg, u32 n)
{
    if (n < HDR || msg[1] != MSG_VER) {
        return 0;
    }
    if (tomb_has(&t->disk, msg + 2)) {
        return -1; /* replay of a finished id */
    }
    return tid_is(t, msg);
}

void bw_session_trade_on_message(BwSessionTrade *t, u32 from, const void *data, u32 n)
{
    const u8 *msg = (const u8 *)data;
    int id;

    if (t == NULL || msg == NULL) {
        return;
    }
    if (n < HDR) {
        if ((t->phase == BW_STRADE_TALK || t->phase == BW_STRADE_OPEN_SENT ||
             t->phase == BW_STRADE_OPEN_IN || t->phase == BW_STRADE_PREPARED) &&
            (t->partner == 0u || from == t->partner)) {
            send_cancel(t, WHY_MALFORMED);
            if (t->phase == BW_STRADE_PREPARED) {
                (void)persist(t, BW_STRADE_CANCELLED);
            }
            finish_idle(t, "A bad message closed the trade. Nothing moved.");
        }
        return;
    }
    if (t->partner != 0u && from != t->partner && msg[0] != MSG_OPEN && msg[0] != MSG_RESUME) {
        return;
    }
    if (msg[1] != MSG_VER) {
        if (t->phase == BW_STRADE_TALK || t->phase == BW_STRADE_OPEN_SENT ||
            t->phase == BW_STRADE_OPEN_IN) {
            send_cancel(t, WHY_MALFORMED);
            finish_idle(t, "A bad message closed the trade. Nothing moved.");
        }
        return;
    }
    id = same_trade(t, msg, n);
    if (id < 0) {
        return; /* tombstoned replay */
    }
    if (msg[0] == MSG_OPEN) {
        if (t->phase == BW_STRADE_OPEN_SENT && from == t->partner) {
            int cmp = memcmp(t->tid, msg + 2, BW_STRADE_ID_LEN);

            if (cmp > 0 || (cmp == 0 && t->cfg.self_peer > from)) {
                memcpy(t->tid, msg + 2, BW_STRADE_ID_LEN);
                if (send_hdr(t, (u8)MSG_OPEN_OK) == 0) {
                    become_talk(t);
                }
            }
            return;
        }
        if (busy_phase(t->phase) || t->journal_damaged) {
            return;
        }
        t->partner = from;
        memcpy(t->tid, msg + 2, BW_STRADE_ID_LEN);
        clear_offers(t);
        t->phase = BW_STRADE_OPEN_IN;
        set_status(t, "The other player offers a trade. Accept to choose Pokemon.");
        return;
    }
    if (!id) {
        return;
    }
    switch (msg[0]) {
    case MSG_OPEN_OK:
        if (t->phase == BW_STRADE_OPEN_SENT) {
            become_talk(t);
        }
        break;
    case MSG_OPEN_NO:
        if (t->phase == BW_STRADE_OPEN_SENT) {
            finish_idle(t, "The other player declined.");
        }
        break;
    case MSG_OFFER:
        on_offer(t, msg, n);
        break;
    case MSG_CONFIRM:
        on_confirm(t, msg, n);
        break;
    case MSG_PREPARED:
        if (n != HDR + 8u + BW_STRADE_HASH_LEN * 2u) {
            send_cancel(t, WHY_MALFORMED);
            finish_idle(t, "A bad prepared notice closed the trade. Nothing moved.");
            break;
        }
        if (rd32(msg + HDR) != t->rev_p || rd32(msg + HDR + 4) != t->rev_l ||
            !hash_eq(msg + HDR + 8, t->hash_p) ||
            !hash_eq(msg + HDR + 8 + BW_STRADE_HASH_LEN, t->hash_l)) {
            set_status(t, "Ignored a prepared notice for a different offer.");
            break;
        }
            /* Same bytes.  Their lock can arrive before our confirm flag if
         * the notices were reordered.  That is not a reason to cancel.
         * The party is still untouched until this side locks and saves. */
        if (!t->have_local || !t->have_peer) {
            set_status(t, "Ignored a prepared notice before both offers.");
            break;
        }
        t->peer_prepared = 1;
        if (t->conf_l && t->conf_p) {
            set_status(t, "The partner locked the same Pokemon. Not saved yet.");
        } else {
            set_status(t, "The partner locked these bytes. Confirm them here. Not saved yet.");
        }
        log_trade(t, "partner locked the same bytes");
        break;
    case MSG_SAVED:
        if (n != HDR + BW_STRADE_HASH_LEN * 2u) {
            break;
        }
        if (!hash_eq(msg + HDR, t->hash_p) ||
            !hash_eq(msg + HDR + BW_STRADE_HASH_LEN, t->hash_l)) {
            set_status(t, "Ignored a save notice for a different offer.");
            break;
        }
        t->peer_saved = 1;
        t->peer_prepared = 1;
        if (t->phase == BW_STRADE_SAVED) {
            mark_done(t);
        } else {
            set_status(t, "The partner has saved. This side still has to save the same trade.");
        }
        break;
    case MSG_CANCEL:
    case MSG_ABORT:
        if (t->phase == BW_STRADE_APPLYING || t->phase == BW_STRADE_SAVED) {
            mark_unresolved(t, "The partner cancelled after this side started saving. Compare parties.");
            break;
        }
        if (t->phase == BW_STRADE_PREPARED || t->phase == BW_STRADE_AWAIT) {
            (void)persist(t, BW_STRADE_CANCELLED);
        }
        finish_idle(t, "The other player cancelled. No Pokemon moved.");
        break;
    case MSG_RESUME:
        if (n >= HDR + 4u + BW_STRADE_HASH_LEN * 2u && tid_is(t, msg)) {
            u32 phase = rd32(msg + HDR);

            /* Their outgoing hash is our incoming hash, and the reverse. */
            if (!hash_eq(msg + HDR + 4, t->hash_p) ||
                !hash_eq(msg + HDR + 4 + BW_STRADE_HASH_LEN, t->hash_l)) {
                mark_unresolved(t, "The partner resumed a different record. Nothing was written.");
                break;
            }
            t->peer_left = 0;
            if (phase == BW_STRADE_IDLE || phase == BW_STRADE_CANCELLED) {
                if (t->phase == BW_STRADE_AWAIT || t->phase == BW_STRADE_PREPARED) {
                    (void)persist(t, BW_STRADE_CANCELLED);
                    finish_idle(t, "The partner does not have this trade. Nothing moved.");
                }
                break;
            }
            if (phase == BW_STRADE_SAVED || phase == BW_STRADE_DONE ||
                phase == BW_STRADE_UNRESOLVED || phase == BW_STRADE_APPLYING) {
                t->peer_saved = 1;
                t->peer_prepared = 1;
            } else if (phase == BW_STRADE_PREPARED || phase == BW_STRADE_AWAIT) {
                t->peer_prepared = 1;
            }
            if (t->phase == BW_STRADE_AWAIT) {
                t->phase = BW_STRADE_PREPARED;
                t->conf_l = t->conf_p = 1;
                t->resume_sent = 0;
                set_status(t, "The partner is back. The locked trade continues.");
            }
            if (!t->resume_sent) {
                bw_session_trade_resume(t, from);
            }
        }
        break;
    default:
        if (t->phase == BW_STRADE_TALK || t->phase == BW_STRADE_OPEN_SENT) {
            send_cancel(t, WHY_MALFORMED);
            finish_idle(t, "A bad message closed the trade. Nothing moved.");
        }
        break;
    }
}

void bw_session_trade_on_disconnect(BwSessionTrade *t)
{
    if (t == NULL) {
        return;
    }
    /* A second notice (the frame, after the session is already gone) must
     * not restart the wait or clear a resume that was just sent. */
    if (t->peer_left && (t->phase == BW_STRADE_AWAIT ||
                         t->phase == BW_STRADE_UNRESOLVED ||
                         t->phase == BW_STRADE_APPLYING ||
                         t->phase == BW_STRADE_SAVED ||
                         t->phase == BW_STRADE_IDLE)) {
        return;
    }
    t->peer_left = 1;
    t->peer_prepared = 0;
    t->peer_saved = 0;
    t->resume_sent = 0;
    if (t->phase == BW_STRADE_OPEN_SENT || t->phase == BW_STRADE_OPEN_IN ||
        t->phase == BW_STRADE_TALK || t->phase == BW_STRADE_CANCELLED) {
        finish_idle(t, "The partner left before the save. Nothing moved.");
        return;
    }
    if (t->phase == BW_STRADE_PREPARED) {
        t->phase = BW_STRADE_AWAIT;
        (void)persist(t, BW_STRADE_AWAIT);
        set_status(t, "The partner left before the save. Your party is unchanged.");
        return;
    }
    if (t->phase == BW_STRADE_SAVED) {
        mark_unresolved(t, "The partner left after this side saved. This side will not roll that save back.");
        return;
    }
    if (t->phase == BW_STRADE_APPLYING) {
        set_status(t, "The partner left while this side was saving. The save will finish; it will not be rolled back.");
    }
}

void bw_session_trade_resume(BwSessionTrade *t, u32 peer)
{
    u8 buf[HDR + 4u + BW_STRADE_HASH_LEN * 2u];
    u32 phase;

    if (t == NULL || peer == 0u) {
        return;
    }
    if (t->phase != BW_STRADE_AWAIT && t->phase != BW_STRADE_UNRESOLVED &&
        t->phase != BW_STRADE_PREPARED && t->phase != BW_STRADE_SAVED &&
        t->phase != BW_STRADE_APPLYING) {
        return;
    }
    if (t->partner != 0u && peer != t->partner) {
        return;
    }
    if (t->resume_sent) {
        return;
    }
    t->resume_sent = 1;
    t->partner = peer;
    phase = (u32)t->phase;
    buf[0] = MSG_RESUME;
    buf[1] = MSG_VER;
    memcpy(buf + 2, t->tid, BW_STRADE_ID_LEN);
    wr32(buf + HDR, phase);
    memcpy(buf + HDR + 4, t->hash_l, BW_STRADE_HASH_LEN);
    memcpy(buf + HDR + 4 + BW_STRADE_HASH_LEN, t->hash_p, BW_STRADE_HASH_LEN);
    (void)send_raw(t, buf, sizeof buf);
}

int bw_session_trade_slot_info(const BwSessionTrade *t, u32 slot, BwTradeInspect *out)
{
    u8 mon[BW_STRADE_MON_SIZE];
    int n;

    if (out != NULL) {
        memset(out, 0, sizeof *out);
        out->reason = BW_STRADE_BAD_ARG;
    }
    if (t == NULL || out == NULL || t->cfg.store.count == NULL || t->cfg.store.read == NULL) {
        return -1;
    }
    n = t->cfg.store.count(t->cfg.store.user);
    if (n < 1 || slot >= (u32)n) {
        return -1;
    }
    if (t->cfg.store.read(t->cfg.store.user, slot, mon) != 0) {
        return -1;
    }
    bw_session_trade_predict(mon, NULL, &t->cfg.limits, t->evos, t->nevo,
                             t->cfg.hold_effect, t->cfg.hold_user, out);
    return 0;
}

int bw_session_trade_face(const BwSessionTrade *t, BwTradeFace *out)
{
    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof *out);
    out->slot = -1;
    out->party_n = -1;
    if (t == NULL) {
        return -1;
    }
    out->phase = t->phase;
    out->have_local = t->have_local;
    out->have_peer = t->have_peer;
    out->conf_local = t->conf_l;
    out->conf_peer = t->conf_p;
    out->peer_locked = t->peer_prepared;
    out->peer_saved = t->peer_saved;
    out->outcome = t->outcome;
    out->slot = t->have_local ? (int)t->slot : -1;
    if (t->cfg.store.count != NULL) {
        out->party_n = t->cfg.store.count(t->cfg.store.user);
    }
    if (t->have_local) {
        bw_session_trade_predict(t->local_mon, NULL, &t->cfg.limits, t->evos, t->nevo,
                                 t->cfg.hold_effect, t->cfg.hold_user, &out->local_info);
    }
    if (t->have_peer) {
        bw_session_trade_predict(t->peer_mon, t->have_local ? t->local_mon : NULL,
                                 &t->cfg.limits, t->evos, t->nevo,
                                 t->cfg.hold_effect, t->cfg.hold_user, &out->peer_info);
    }
    return 0;
}

int bw_session_trade_outcome(const BwSessionTrade *t)
{
    return t != NULL ? t->outcome : 0;
}

void bw_session_trade_clear_outcome(BwSessionTrade *t)
{
    if (t != NULL) {
        t->outcome = 0;
    }
}

int bw_session_trade_view_open(const BwSessionTrade *t)
{
    if (t == NULL) {
        return 0;
    }
    if (t->phase != BW_STRADE_IDLE) {
        return 1;
    }
    return t->outcome != BW_STRADE_OUT_NONE;
}

int bw_session_trade_success_ready(const BwSessionTrade *t)
{
    return t != NULL && t->phase == BW_STRADE_IDLE &&
           t->outcome == BW_STRADE_OUT_BOTH_SAVED;
}

int bw_session_trade_dismiss_success(BwSessionTrade *t)
{
    if (!bw_session_trade_success_ready(t)) {
        return -1;
    }
    t->outcome = BW_STRADE_OUT_NONE;
    fprintf(stderr, "eidos: trade: trade view closed after both saves\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* The screen's rows                                                   */
/* ------------------------------------------------------------------ */

static BwSessionTrade *g_ui;

void bw_session_trade_ui_bind(BwSessionTrade *t)
{
    g_ui = t;
}

static int ui_slots(void)
{
    int n;

    if (g_ui == NULL || g_ui->cfg.store.count == NULL) {
        return -1;
    }
    n = g_ui->cfg.store.count(g_ui->cfg.store.user);
    if (n < 0) {
        return -1;
    }
    if (n > 6) {
        n = 6;
    }
    return n;
}

static int ui_ack(void)
{
    if (g_ui == NULL) {
        return 0;
    }
    return g_ui->phase == BW_STRADE_AWAIT || g_ui->phase == BW_STRADE_UNRESOLVED ||
           g_ui->journal_damaged;
}

int bw_session_trade_ui_rows(void)
{
    int n = ui_slots();

    if (g_ui == NULL) {
        return 1;
    }
    if (n < 0) {
        n = 0;
    }
    return 3 + n + 2 + (ui_ack() ? 1 : 0);
}

static void put_txt(char *dst, int n, const char *s)
{
    if (dst == NULL || n <= 0) {
        return;
    }
    snprintf(dst, (size_t)n, "%s", s != NULL ? s : "");
}

static void format_mon(const BwSessionTrade *t, const u8 *mon, int have,
                       const u8 *given, char *dst, int n)
{
    BwTradeInspect info;

    if (!have || mon == NULL) {
        put_txt(dst, n, "none");
        return;
    }
    bw_session_trade_predict(mon, given, &t->cfg.limits, t->evos, t->nevo,
                             t->cfg.hold_effect, t->cfg.hold_user, &info);
    if (info.reason != BW_STRADE_OK) {
        snprintf(dst, (size_t)n, "invalid (%d)", info.reason);
        return;
    }
    if (given != NULL && info.evo_to != 0u) {
        snprintf(dst, (size_t)n, "#%03u Lv%u item %u -> #%03u",
                 info.species, info.level, info.item, info.evo_to);
    } else {
        snprintf(dst, (size_t)n, "#%03u Lv%u item %u",
                 info.species, info.level, info.item);
    }
}

void bw_session_trade_ui_row(int index, char *label, int nl, char *value, int nv)
{
    int slots = ui_slots();
    int ack = ui_ack();
    int confirm_at, cancel_at;

    put_txt(label, nl, "");
    put_txt(value, nv, "");
    if (g_ui == NULL) {
        put_txt(label, nl, "Session trade");
        put_txt(value, nv, "unavailable");
        return;
    }
    if (slots < 0) {
        slots = 0;
    }
    confirm_at = 3 + slots;
    cancel_at = confirm_at + 1;
    if (index == 0) {
        put_txt(label, nl, "Status");
        put_txt(value, nv, g_ui->status);
        return;
    }
    if (index == 1) {
        put_txt(label, nl, "You");
        format_mon(g_ui, g_ui->local_mon, g_ui->have_local, NULL, value, nv);
        return;
    }
    if (index == 2) {
        u8 h[BW_STRADE_HASH_LEN];
        char body[96];

        put_txt(label, nl, "Partner");
        format_mon(g_ui, g_ui->peer_mon, g_ui->have_peer,
                   g_ui->have_local ? g_ui->local_mon : NULL, body, (int)sizeof body);
        if (bw_session_trade_hash_peer(g_ui, h)) {
            snprintf(value, (size_t)nv, "%s  %02X%02X%02X%02X", body, h[0], h[1], h[2], h[3]);
        } else {
            put_txt(value, nv, body);
        }
        return;
    }
    if (index >= 3 && index < 3 + slots) {
        u8 mon[BW_STRADE_MON_SIZE];
        int slot = index - 3;

        snprintf(label, (size_t)nl, "Party %d", slot + 1);
        if (g_ui->cfg.store.read(g_ui->cfg.store.user, (u32)slot, mon) != 0) {
            put_txt(value, nv, "unreadable");
        } else {
            format_mon(g_ui, mon, 1, NULL, value, nv);
        }
        return;
    }
    if (index == confirm_at) {
        const char *name = "Confirm";

        if (g_ui->phase == BW_STRADE_OPEN_IN) {
            name = "Accept";
        } else if (g_ui->phase == BW_STRADE_IDLE) {
            name = "Offer trade";
        }
        put_txt(label, nl, name);
        put_txt(value, nv, "");
        return;
    }
    if (index == cancel_at) {
        put_txt(label, nl, "Cancel");
        return;
    }
    if (ack && index == cancel_at + 1) {
        put_txt(label, nl, "Acknowledge");
        put_txt(value, nv, "does not move Pokemon");
    }
}

int bw_session_trade_ui_activate(int index)
{
    int slots = ui_slots();
    int confirm_at, cancel_at;

    if (g_ui == NULL) {
        return -1;
    }
    if (slots < 0) {
        slots = 0;
    }
    confirm_at = 3 + slots;
    cancel_at = confirm_at + 1;
    if (index >= 3 && index < 3 + slots) {
        return bw_session_trade_offer(g_ui, (u32)(index - 3));
    }
    if (index == confirm_at) {
        if (g_ui->phase == BW_STRADE_OPEN_IN) {
            return bw_session_trade_accept(g_ui);
        }
        if (g_ui->phase == BW_STRADE_IDLE) {
            return bw_session_trade_start(g_ui, g_ui->partner);
        }
        return bw_session_trade_confirm(g_ui);
    }
    if (index == cancel_at) {
        if (g_ui->phase == BW_STRADE_OPEN_IN) {
            return bw_session_trade_decline(g_ui);
        }
        return bw_session_trade_cancel(g_ui);
    }
    if (ui_ack() && index == cancel_at + 1) {
        return bw_session_trade_acknowledge(g_ui);
    }
    return -1;
}

const char *bw_session_trade_ui_help(void)
{
    return "Session trade, not the DS wireless trade. The field picture is the trade screen. A confirms the exact offer. Nothing is saved until both have confirmed and this side's save has finished. A finished line is still not one shared write.";
}
