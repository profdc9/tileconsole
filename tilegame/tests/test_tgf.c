/* SPDX-License-Identifier: MIT */
/* Tests of the TGF files (src/tgf.c) in memory: what is written is read back the same, every way a file can be broken is caught (cut short at
 * every byte, each length one off, each TGAC spoiled, each range one past its end), what a later version may add is skipped, and random damage
 * never makes the reader go outside what it was given (AddressSanitizer). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tgf.h"

static unsigned long checks;

#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define EQ(a, b) do { const long long va_ = (long long)(a), vb_ = (long long)(b); checks++; \
    if (va_ != vb_) { fprintf(stderr, "FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); exit(1); } } while (0)

/* ---- a file in memory ---- */

typedef struct {
    uint8_t *buf;
    uint32_t len, cap, pos;
    int fail_at;   /* the call (from 1) that fails, or 0 */
    int calls;
} mem_t;

static int m_read(void *ctx, void *b, uint32_t n)
{
    mem_t *m = ctx;
    if (m->fail_at && ++m->calls == m->fail_at)
        return -5;
    const uint32_t k = m->pos >= m->len ? 0 : m->len - m->pos < n ? m->len - m->pos : n;
    memcpy(b, m->buf + m->pos, k);
    m->pos += k;
    return (int)k;
}

static int m_seek(void *ctx, uint32_t pos)
{
    mem_t *m = ctx;
    if (pos > m->len)
        return -6;
    m->pos = pos;
    return 0;
}

static int m_write(void *ctx, const void *b, uint32_t n)
{
    mem_t *m = ctx;
    if (m->fail_at && ++m->calls == m->fail_at)
        return -7;
    if (m->len + n > m->cap)
        return -8;
    memcpy(m->buf + m->len, b, n);
    m->len += n;
    return (int)n;
}

static uint8_t file[2300000], copy[2300000];
static mem_t mf;

static tgf_io_t io_of(mem_t *m, uint8_t *buf, uint32_t len)
{
    memset(m, 0, sizeof *m);
    m->buf = buf;
    m->len = len;
    m->cap = sizeof file;
    return (tgf_io_t){m_read, m_seek, m_write, m, len};
}

static char why[200];
static tgf_info_t info;

static int check_bytes(uint8_t *buf, uint32_t len)
{
    mem_t m;
    tgf_io_t io = io_of(&m, buf, len);
    why[0] = 0;
    return tgf_check(&io, &info, why, sizeof why);
}

/* ---- a whole file, and what it holds ---- */

static uint8_t pal[TGF_SET][3], tiles[TGF_SET * 256], sprites[TGF_SET * 256], flags[TGF_SET];
static tgf_frame_t frames1[3] = {{5, 10}, {6, 2}, {255, 0}}, frames32[1] = {{0, 7}};
static uint16_t map1[3 * 2] = {0, 1 | TGF_HFLIP, 2 | TGF_VFLIP, 255, 3 | TGF_HFLIP | TGF_VFLIP, 7}, map4[1] = {9};
static const tgf_char_t chars3[3] = {{' ', 10, TGF_NO_TILE}, {'A', 11, 12}, {0x2500, 255, TGF_NO_TILE}};
static uint32_t len_at[16], n_chunks;   /* where each chunk's length is, for the tests that spoil it */

static uint32_t write_full(int size)
{
    for (int i = 0; i < TGF_SET; i++) {
        pal[i][0] = (uint8_t)i;
        pal[i][1] = (uint8_t)(255 - i);
        pal[i][2] = (uint8_t)(i * 3);
        flags[i] = (uint8_t)(i % 7);
    }
    for (int i = 0; i < TGF_SET * 256; i++) {
        tiles[i] = (uint8_t)(i * 13 + 1);
        sprites[i] = (uint8_t)(i * 7 + 3);
    }
    const int px = size * size;
    tgf_io_t io = io_of(&mf, file, 0);
    n_chunks = 0;
#define MARK() (len_at[n_chunks++] = mf.len + 8)
    MARK();
    EQ(tgf_write_header(&io, size), 0);
    MARK();
    EQ(tgf_write_part(&io, TGF_PALT, 0, 256, size, pal), 0);
    MARK();
    EQ(tgf_write_part(&io, TGF_TILE, 3, 5, size, tiles + 3 * px), 0);
    MARK();
    EQ(tgf_write_part(&io, TGF_SPRT, 0, 3, size, sprites), 0);
    MARK();
    EQ(tgf_write_part(&io, TGF_FLAG, 10, 11, size, flags + 10), 0);
    MARK();
    EQ(tgf_write_table(&io, 1, 2, 3, frames1), 0);
    MARK();
    EQ(tgf_write_table(&io, 32, 1, 1, frames32), 0);
    MARK();
    EQ(tgf_write_chars(&io, 3, chars3), 0);
    MARK();
    EQ(tgf_write_map(&io, 1, 0, 3, 2, map1), 0);
    MARK();
    EQ(tgf_write_map(&io, 4, TGF_MAP_WRAPS, 1, 1, map4), 0);
    MARK();
    EQ(tgf_write_end(&io), 0);
#undef MARK
    return mf.len;
}

static void test_round_trip(int size)
{
    const uint32_t len = write_full(size);
    const int px = size * size;
    EQ(check_bytes(file, len), 0);
    EQ(info.tile_size, size);
    CHECK(info.part[TGF_PALT].first == 0 && info.part[TGF_PALT].count == 256);
    CHECK(info.part[TGF_TILE].first == 3 && info.part[TGF_TILE].count == 5);
    CHECK(info.part[TGF_SPRT].first == 0 && info.part[TGF_SPRT].count == 3);
    CHECK(info.part[TGF_FLAG].first == 10 && info.part[TGF_FLAG].count == 11);
    EQ(info.tables, 2);
    CHECK(info.table[0].present && info.table[0].mode == 2 && info.table[0].count == 3);
    CHECK(info.table[31].present && info.table[31].mode == 1 && info.table[31].count == 1 && !info.table[1].present);
    EQ(info.maps, 2);
    CHECK(info.map[0].present && info.map[0].w == 3 && info.map[0].h == 2 && info.map[0].flags == 0);
    CHECK(info.map[3].present && info.map[3].w == 1 && info.map[3].h == 1 && info.map[3].flags == TGF_MAP_WRAPS && !info.map[1].present);
    CHECK(info.chars.present && info.chars.count == 3);

    mem_t m;
    tgf_io_t io = io_of(&m, file, len);
    static uint8_t p2[TGF_SET][3], t2[TGF_SET * 256], s2[TGF_SET * 256], f2[TGF_SET];
    memset(p2, 0xEE, sizeof p2);
    memset(t2, 0xEE, sizeof t2);
    memset(s2, 0xEE, sizeof s2);
    memset(f2, 0xEE, sizeof f2);
    EQ(tgf_read_part(&io, &info, TGF_PALT, p2), 0);
    EQ(tgf_read_part(&io, &info, TGF_TILE, t2), 0);
    EQ(tgf_read_part(&io, &info, TGF_SPRT, s2), 0);
    EQ(tgf_read_part(&io, &info, TGF_FLAG, f2), 0);
    CHECK(!memcmp(p2, pal, sizeof pal));
    CHECK(!memcmp(t2 + 3 * px, tiles + 3 * px, 5u * px) && t2[3 * px - 1] == 0xEE && t2[8 * px] == 0xEE);   /* (just the range) */
    CHECK(!memcmp(s2, sprites, 3u * px) && s2[3 * px] == 0xEE);
    CHECK(!memcmp(f2 + 10, flags + 10, 11) && f2[9] == 0xEE && f2[21] == 0xEE);
    tgf_frame_t fr[TGF_FRAMES];
    EQ(tgf_read_table(&io, &info, 1, fr), 0);
    CHECK(fr[0].sprite == 5 && fr[0].ticks == 10 && fr[1].sprite == 6 && fr[1].ticks == 2 && fr[2].sprite == 255 && fr[2].ticks == 0);
    EQ(tgf_read_table(&io, &info, 32, fr), 0);
    CHECK(fr[0].sprite == 0 && fr[0].ticks == 7);
    EQ(tgf_read_table(&io, &info, 0, fr), TGF_ERANGE);
    uint16_t e1[6], e4[1];
    EQ(tgf_read_map(&io, &info, 1, e1), 0);
    CHECK(!memcmp(e1, map1, sizeof map1));
    EQ(tgf_read_map(&io, &info, 4, e4), 0);
    CHECK(e4[0] == 9);
    EQ(tgf_read_map(&io, &info, 5, e4), TGF_ERANGE);
    EQ(tgf_read_part(&io, &info, TGF_KINDS, f2), TGF_ERANGE);
    tgf_char_t ch[4];
    memset(ch, 0xEE, sizeof ch);
    EQ(tgf_read_chars(&io, &info, ch), 0);
    CHECK(!memcmp(ch, chars3, sizeof chars3) && ch[3].codepoint == 0xEEEEEEEE);

    /* the bytes, as the format says */
    static const uint8_t head[] = {'T', 'G', 'A', 'C', 'T', 'G', 'F', '1', 4, 0, 0, 0, 16, 0, 0, 0};
    if (size == 16)
        CHECK(!memcmp(file, head, sizeof head));
    static const uint8_t end[] = {'T', 'G', 'A', 'C', 'E', 'N', 'D', ' ', 0, 0, 0, 0};
    CHECK(!memcmp(file + len - 12, end, sizeof end));
}

/* ---- broken files ---- */

static void test_broken(void)
{
    const uint32_t len = write_full(8);
    memcpy(copy, file, len);

    /* cut short anywhere */
    for (uint32_t cut = 0; cut < len; cut++) {
        const int r = check_bytes(file, cut);
        CHECK(r == (cut < 12 ? TGF_ENOTTGF : TGF_EBROKEN));
    }
    /* one byte too many after the end */
    file[len] = 0;
    EQ(check_bytes(file, len + 1), TGF_EBROKEN);
    CHECK(strstr(why, "after the END") != NULL);

    /* each length one off, each way: the TGAC is not where it says (or the chunk is too short, or runs past the end) */
    for (uint32_t c = 0; c < n_chunks; c++) {
        for (int d = -1; d <= 1; d += 2) {
            memcpy(file, copy, len);
            const uint32_t v = (uint32_t)(file[len_at[c]] | file[len_at[c] + 1] << 8 | file[len_at[c] + 2] << 16) + (uint32_t)d;
            if (v > 0xFFFFFF)
                continue;   /* (END's length is 0: one less is not a length) */
            file[len_at[c]] = (uint8_t)v;
            file[len_at[c] + 1] = (uint8_t)(v >> 8);
            file[len_at[c] + 2] = (uint8_t)(v >> 16);
            const int r = check_bytes(file, len);
            if (r == 0)
                fprintf(stderr, "accepted: chunk %u, length %+d\n", c, d);
            CHECK(r == TGF_EBROKEN);
        }
        /* and each TGAC spoiled */
        memcpy(file, copy, len);
        file[len_at[c] - 8 + c % 4] ^= 0x20;
        EQ(check_bytes(file, len), c == 0 ? TGF_ENOTTGF : TGF_EBROKEN);
    }
    memcpy(file, copy, len);
    EQ(check_bytes(file, len), 0);

    /* not a file of this kind */
    memcpy(file, "TGACTGF2", 8);
    EQ(check_bytes(file, len), TGF_ENOTTGF);
    memcpy(file, "PK\3\4TGF1", 8);
    EQ(check_bytes(file, len), TGF_ENOTTGF);
    EQ(check_bytes(file, 0), TGF_ENOTTGF);
}

/* Chunks made by hand, for what the writer would never write. */
static uint32_t at;
static void raw(const void *p, uint32_t n) { memcpy(file + at, p, n); at += n; }
static void raw_chunk(const char *tag, const void *payload, uint32_t n)
{
    const uint8_t l[4] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
    raw("TGAC", 4);
    raw(tag, 4);
    raw(l, 4);
    raw(payload, n);
}
static void start(int size)
{
    at = 0;
    const uint8_t f[4] = {(uint8_t)size, 0, 0, 0};
    raw_chunk("TGF1", f, 4);
}
static int finish(void)
{
    raw_chunk("END ", "", 0);
    return check_bytes(file, at);
}

static void test_rules(void)
{
    static uint8_t big[2 * 1024 * 1024 + 64];
    /* the header */
    start(12);
    EQ(finish(), TGF_ERANGE);
    at = 0;
    raw_chunk("TGF1", "\x10\0\0\0more", 8);   /* (a later version's header: what it adds is skipped) */
    EQ(finish(), 0);
    EQ(info.tile_size, 16);
    at = 0;
    raw_chunk("TGF1", "\x10\0", 2);
    EQ(finish(), TGF_EBROKEN);
    start(8);
    raw_chunk("TGF1", "\x08\0\0\0", 4);
    EQ(finish(), TGF_EBROKEN);

    /* chunks that are not known are skipped, and so is what comes after the fields of one that is */
    start(8);
    raw_chunk("XTRA", "whatever this is", 16);
    raw_chunk("FLAG", "\x05\0\x02\0\x01\x04" "later", 11);
    EQ(finish(), 0);
    CHECK(info.part[TGF_FLAG].first == 5 && info.part[TGF_FLAG].count == 2);
    mem_t m;
    tgf_io_t io = io_of(&m, file, at);
    uint8_t f[TGF_SET] = {0};
    EQ(tgf_read_part(&io, &info, TGF_FLAG, f), 0);
    CHECK(f[5] == 1 && f[6] == 4 && f[7] == 0);

    /* ranges */
    start(8);
    raw_chunk("FLAG", "\xFF\0\x02\0\x01\x01", 6);   /* 255 and 256 */
    EQ(finish(), TGF_ERANGE);
    start(8);
    raw_chunk("FLAG", "\xFE\0\x02\0\x01\x01", 6);   /* 254 and 255: fits */
    EQ(finish(), 0);
    start(8);
    raw_chunk("FLAG", "\x00\0\x03\0\x01\x01", 6);   /* 3 said, 2 there */
    EQ(finish(), TGF_EBROKEN);
    start(8);
    raw_chunk("FLAG", "\x00\0", 2);
    EQ(finish(), TGF_EBROKEN);
    start(8);
    raw_chunk("PALT", "\x00\0\x01\0\x01\x02\x03", 7);
    raw_chunk("PALT", "\x01\0\x01\0\x01\x02\x03", 7);
    EQ(finish(), TGF_EBROKEN);
    CHECK(strstr(why, "two PALT") != NULL);
    uint8_t t[8 + 64];
    memset(t, 0, sizeof t);
    t[2] = 1;   /* one tile */
    t[4] = 4;   /* 4 bits: not yet */
    start(8);
    raw_chunk("TILE", t, sizeof t);
    EQ(finish(), TGF_ERANGE);
    t[4] = 8;
    start(8);
    raw_chunk("TILE", t, sizeof t);
    EQ(finish(), 0);
    start(16);   /* (64 bytes are not a 16 x 16 tile) */
    raw_chunk("TILE", t, sizeof t);
    EQ(finish(), TGF_EBROKEN);
    start(8);
    raw_chunk("SPRT", t, sizeof t);
    raw_chunk("SPRT", t, sizeof t);
    EQ(finish(), TGF_EBROKEN);

    /* animation tables */
    static const struct { uint8_t p[8]; uint32_t n; int want; } anims[] = {
        {{1, 0, 1, 0, 255, 0, 3, 0}, 8, 0},
        {{0, 0, 1, 0, 1, 0, 3, 0}, 8, TGF_ERANGE},    /* table 0 */
        {{33, 0, 1, 0, 1, 0, 3, 0}, 8, TGF_ERANGE},   /* table 33 */
        {{1, 3, 1, 0, 1, 0, 3, 0}, 8, TGF_ERANGE},    /* mode 3 */
        {{1, 0, 33, 0, 1, 0, 3, 0}, 8, TGF_ERANGE},   /* 33 frames */
        {{1, 0, 1, 0, 0, 1, 3, 0}, 8, TGF_ERANGE},    /* sprite 256 */
        {{1, 0, 2, 0, 1, 0, 3, 0}, 8, TGF_EBROKEN},   /* 2 frames said, 1 there */
        {{1, 0, 0, 0}, 4, 0},                         /* an empty table */
        {{1, 0}, 2, TGF_EBROKEN},
    };
    for (size_t i = 0; i < sizeof anims / sizeof *anims; i++) {
        start(8);
        raw_chunk("ANIM", anims[i].p, anims[i].n);
        const int r = finish();
        if (r != anims[i].want)
            fprintf(stderr, "anim case %zu: %d (%s)\n", i, r, why);
        EQ(r, anims[i].want);
    }
    start(8);
    raw_chunk("ANIM", "\x05\0\0\0", 4);
    raw_chunk("ANIM", "\x05\0\0\0", 4);
    EQ(finish(), TGF_EBROKEN);

    /* maps */
    static const struct { uint8_t p[16]; uint32_t n; int want; } maps[] = {
        {{2, 1, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 0xFF, 0x0C, 0x01, 0x00}, 16, 0},   /* 2 x 1: tile 255 flipped both ways, tile 1 */
        {{0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0}, 14, TGF_ERANGE},            /* layer 0 */
        {{5, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0}, 14, TGF_ERANGE},            /* layer 5 */
        {{1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0}, 12, TGF_ERANGE},                  /* 0 wide */
        {{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0x00, 0x01}, 14, TGF_ERANGE},      /* tile 256 */
        {{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0x01, 0x10}, 14, TGF_ERANGE},      /* bit 12 */
        {{1, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 0x01, 0x00}, 14, TGF_EBROKEN},     /* 2 tiles said, 1 there */
        {{1, 0, 0, 0, 1, 0, 0, 0}, 8, TGF_EBROKEN},
    };
    for (size_t i = 0; i < sizeof maps / sizeof *maps; i++) {
        start(8);
        raw_chunk("MAP ", maps[i].p, maps[i].n);
        const int r = finish();
        if (r != maps[i].want)
            fprintf(stderr, "map case %zu: %d (%s)\n", i, r, why);
        EQ(r, maps[i].want);
    }
    CHECK(strstr(why, "shorter") != NULL);
    start(8);
    raw_chunk("MAP ", maps[0].p, maps[0].n);
    raw_chunk("MAP ", maps[0].p, maps[0].n);
    EQ(finish(), TGF_EBROKEN);
    /* the biggest map, and one tile more */
    memset(big, 0, sizeof big);
    big[0] = 1;
    big[4] = 0x00, big[5] = 0x04;   /* 1024 x 1024 */
    big[8] = 0x00, big[9] = 0x04;
    start(8);
    raw_chunk("MAP ", big, 12 + 2u * 1024 * 1024);
    EQ(finish(), 0);
    CHECK(info.map[0].w == 1024 && info.map[0].h == 1024);
    big[8] = 0x01;   /* 1024 x 1025 */
    start(8);
    raw_chunk("MAP ", big, 12);   /* (too big: found before the length is looked at) */
    EQ(finish(), TGF_ERANGE);
    big[4] = 0xFF, big[5] = 0xFF, big[6] = 0xFF, big[7] = 0xFF;   /* 4G x 1025: the product overflows 32 bits */
    start(8);
    raw_chunk("MAP ", big, 12);
    EQ(finish(), TGF_ERANGE);

    /* the character table */
    {
        uint8_t c[4 + 8 * 4];
        const uint8_t ok[4 + 8 * 2] = {2, 0, 0, 0, 'A', 0, 0, 0, 5, 0, 0xFF, 0xFF, 0x00, 0x25, 0, 0, 255, 0, 254, 0};
        start(8);
        raw_chunk("CHAR", ok, sizeof ok);
        EQ(finish(), 0);
        CHECK(info.chars.present && info.chars.count == 2);
        memcpy(c, ok, sizeof ok);
        start(8);
        raw_chunk("CHAR", c, sizeof ok + 3);   /* (bytes after the entries: skipped) */
        EQ(finish(), 0);
        start(8);
        raw_chunk("CHAR", "\0\0\0\0", 4);     /* an empty table */
        EQ(finish(), 0);
        CHECK(info.chars.present && info.chars.count == 0);
        static const struct { uint32_t cp1; unsigned tile1, lower1; uint32_t cp2; int want; const char *what; } cases[] = {
            {'A', 5, TGF_NO_TILE, 'A', TGF_EBROKEN, "not in order"},          /* the same character twice */
            {'B', 5, TGF_NO_TILE, 'A', TGF_EBROKEN, "not in order"},
            {'A', 256, TGF_NO_TILE, 'B', TGF_ERANGE, "tiles are 0 to 255"},
            {'A', 5, 256, 'B', TGF_ERANGE, "tiles are 0 to 255"},
            {'A', 5, 0xFFFE, 'B', TGF_ERANGE, "tiles are 0 to 255"},
            {'A', 5, TGF_NO_TILE, 0x110000, TGF_ERANGE, "Unicode ends"},
            {'A', 0, 255, 0x10FFFF, 0, ""},                                     /* the edges */
        };
        for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
            uint8_t b[4 + 16] = {2, 0, 0, 0};
            b[4] = (uint8_t)cases[i].cp1, b[5] = (uint8_t)(cases[i].cp1 >> 8);
            b[8] = (uint8_t)cases[i].tile1, b[9] = (uint8_t)(cases[i].tile1 >> 8);
            b[10] = (uint8_t)cases[i].lower1, b[11] = (uint8_t)(cases[i].lower1 >> 8);
            b[12] = (uint8_t)cases[i].cp2, b[13] = (uint8_t)(cases[i].cp2 >> 8), b[14] = (uint8_t)(cases[i].cp2 >> 16);
            b[16] = 1, b[18] = 0xFF, b[19] = 0xFF;
            start(8);
            raw_chunk("CHAR", b, sizeof b);
            const int r = finish();
            if (r != cases[i].want)
                fprintf(stderr, "char case %zu: %d (%s)\n", i, r, why);
            EQ(r, cases[i].want);
            CHECK(strstr(why, cases[i].what) != NULL);
        }
        /* shorter than its fields, or than its entries; two tables; the most, and one more */
        start(8);
        raw_chunk("CHAR", "\1\0", 2);
        EQ(finish(), TGF_EBROKEN);
        start(8);
        raw_chunk("CHAR", ok, sizeof ok - 1);
        EQ(finish(), TGF_EBROKEN);
        start(8);
        raw_chunk("CHAR", ok, sizeof ok);
        raw_chunk("CHAR", ok, sizeof ok);
        EQ(finish(), TGF_EBROKEN);
        static uint8_t most[4 + 8 * (TGF_CHARS + 1)];
        for (uint32_t i = 0; i <= TGF_CHARS; i++) {
            most[4 + 8 * i] = (uint8_t)i, most[5 + 8 * i] = (uint8_t)(i >> 8);
            most[8 + 8 * i] = (uint8_t)(i % 256), most[10 + 8 * i] = 0xFF, most[11 + 8 * i] = 0xFF;
        }
        most[0] = (uint8_t)TGF_CHARS, most[1] = (uint8_t)(TGF_CHARS >> 8);
        start(8);
        raw_chunk("CHAR", most, 4 + 8 * TGF_CHARS);
        EQ(finish(), 0);
        CHECK(info.chars.count == TGF_CHARS);
        static tgf_char_t back[TGF_CHARS];
        mem_t m;
        tgf_io_t io = io_of(&m, file, at);
        EQ(tgf_read_chars(&io, &info, back), 0);
        CHECK(back[0].codepoint == 0 && back[TGF_CHARS - 1].codepoint == TGF_CHARS - 1 && back[300].tile == 300 % 256 && back[300].lower == TGF_NO_TILE);
        most[0] = (uint8_t)(TGF_CHARS + 1), most[1] = (uint8_t)((TGF_CHARS + 1) >> 8);
        start(8);
        raw_chunk("CHAR", most, sizeof most);
        EQ(finish(), TGF_ERANGE);
        /* the writer will not write what the reader would refuse */
        tgf_io_t w = io_of(&m, copy, 0);
        const tgf_char_t unsorted[2] = {{'B', 1, TGF_NO_TILE}, {'A', 2, TGF_NO_TILE}}, twice[2] = {{'A', 1, TGF_NO_TILE}, {'A', 2, TGF_NO_TILE}};
        const tgf_char_t far[1] = {{0x110000, 1, TGF_NO_TILE}}, high[1] = {{'A', 256, TGF_NO_TILE}}, low[1] = {{'A', 1, 300}};
        EQ(tgf_write_chars(&w, 2, unsorted), TGF_ERANGE);
        EQ(tgf_write_chars(&w, 2, twice), TGF_ERANGE);
        EQ(tgf_write_chars(&w, 1, far), TGF_ERANGE);
        EQ(tgf_write_chars(&w, 1, high), TGF_ERANGE);
        EQ(tgf_write_chars(&w, 1, low), TGF_ERANGE);
        EQ(tgf_write_chars(&w, TGF_CHARS + 1, back), TGF_ERANGE);
        EQ(tgf_write_chars(&w, -1, back), TGF_ERANGE);
        CHECK(m.len == 0);
        EQ(tgf_write_chars(&w, 0, back), 0);
        CHECK(m.len == 16);
    }
    (void)copy;
}

static void test_io_failures(void)
{
    /* every write failing in turn gives an error back, and so does every read */
    for (int k = 1; k < 40; k++) {
        mem_t m;
        tgf_io_t io = io_of(&m, file, 0);
        m.fail_at = k;
        int e = tgf_write_header(&io, 8);
        if (!e) e = tgf_write_part(&io, TGF_PALT, 0, 2, 8, pal);
        if (!e) e = tgf_write_part(&io, TGF_TILE, 0, 1, 8, tiles);
        if (!e) e = tgf_write_table(&io, 1, 0, 3, frames1);
        if (!e) e = tgf_write_map(&io, 1, 0, 3, 2, map1);
        if (!e) e = tgf_write_end(&io);
        CHECK(k <= 14 ? e < 0 : e == 0);   /* (14 writes in all: 2 for the header, 3 for each part and the map, 2 for the table, 1 for the end) */
    }
    const uint32_t len = write_full(8);
    for (int k = 1; k < 30; k++) {
        mem_t m;
        tgf_io_t io = io_of(&m, file, len);
        m.fail_at = k;
        const int e = tgf_check(&io, &info, why, sizeof why);
        CHECK(e == 0 || e == -5);
    }
    mem_t m;
    tgf_io_t io = io_of(&m, file, len);
    m.cap = 20;   /* a disk that fills up */
    m.len = 0;
    int e = tgf_write_header(&io, 8);
    if (!e)
        e = tgf_write_part(&io, TGF_PALT, 0, 256, 8, pal);
    CHECK(e < 0);
    EQ(tgf_write_part(&io, TGF_PALT, 200, 57, 8, pal), TGF_ERANGE);
    EQ(tgf_write_table(&io, 33, 0, 0, frames1), TGF_ERANGE);
    EQ(tgf_write_map(&io, 1, 0, 0, 5, map1), TGF_ERANGE);
}

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void test_random_damage(void)
{
    const uint32_t len = write_full(8);
    memcpy(copy, file, len);
    static uint8_t p[TGF_SET][3], t[TGF_SET * 256], f[TGF_SET];
    static uint16_t entries[TGF_MAP_MAX];
    int accepted = 0;
    for (int round = 0; round < 20000; round++) {
        memcpy(file, copy, len);
        const int hits = 1 + (int)(rnd() % 4);
        for (int h = 0; h < hits; h++)
            file[rnd() % len] = (uint8_t)rnd();
        const uint32_t cut = rnd() % 8 == 0 ? rnd() % len : len;
        if (check_bytes(file, cut) != 0)
            continue;
        accepted++;   /* (damage to the pixels or the colours is not damage to the file) */
        mem_t m;
        tgf_io_t io = io_of(&m, file, cut);
        EQ(tgf_read_part(&io, &info, TGF_PALT, p), 0);
        EQ(tgf_read_part(&io, &info, TGF_TILE, t), 0);
        EQ(tgf_read_part(&io, &info, TGF_SPRT, t), 0);
        EQ(tgf_read_part(&io, &info, TGF_FLAG, f), 0);
        tgf_frame_t fr[TGF_FRAMES];
        for (int n = 1; n <= TGF_TABLES; n++)
            EQ(tgf_read_table(&io, &info, n, fr), 0);
        static tgf_char_t ch[TGF_CHARS];
        EQ(tgf_read_chars(&io, &info, ch), 0);
        for (int i = 0; i < info.chars.count; i++)
            CHECK(ch[i].codepoint <= TGF_CODEPOINT_MAX && (i == 0 || ch[i].codepoint > ch[i - 1].codepoint) && ch[i].tile < TGF_SET &&
                  (ch[i].lower < TGF_SET || ch[i].lower == TGF_NO_TILE));
        for (int n = 1; n <= TGF_LAYERS; n++) {
            EQ(tgf_read_map(&io, &info, n, entries), 0);
            for (uint32_t i = 0; info.map[n - 1].present && i < info.map[n - 1].w * info.map[n - 1].h; i++)
                CHECK((entries[i] & 0x3FF) < TGF_SET && !(entries[i] & 0xF000));
        }
    }
    CHECK(accepted > 1000 && accepted < 20000);
}

int main(void)
{
    test_round_trip(16);
    test_round_trip(8);
    test_broken();
    test_rules();
    test_io_failures();
    test_random_damage();
    printf("test_tgf: %lu checks passed\n", checks);
    return 0;
}
