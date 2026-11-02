/* SPDX-License-Identifier: MIT */
/* Tests of the fonts and the glyph-to-tile helpers. Exits non-zero on the first failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tg_fonts.h"
#include "tilegfx.h"

static int checks;
#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static uint32_t rng = 99;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static const tg_font_t *const FONTS[] = {&tg_font_unscii8,         &tg_font_unscii8_alt,  &tg_font_unscii8_thin,
                                         &tg_font_unscii8_fantasy, &tg_font_unscii8_mcr, &tg_font_unscii8_tall,
                                         &tg_font_unscii16};
#define NFONTS ((int)(sizeof FONTS / sizeof *FONTS))

static int bytes_equal_hex(const uint8_t *g, int n, const char *hex)
{
    for (int i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1 || g[i] != v)
            return 0;
    }
    return hex[2 * n] == 0;
}

/* The tables are well formed: sorted, non-overlapping runs that account for every glyph exactly once. */
static void test_structure(void)
{
    for (int f = 0; f < NFONTS; f++) {
        const tg_font_t *font = FONTS[f];
        CHECK(font->height == 8 || font->height == 16);
        unsigned next_index = 0;
        for (unsigned r = 0; r < font->nruns; r++) {
            const tg_glyph_run_t *run = &font->runs[r];
            CHECK(run->count > 0);
            CHECK(run->index == next_index);
            next_index += run->count;
            if (r)   /* strictly after the previous run, and not touching it (it would have been one run) */
                CHECK(run->first > font->runs[r - 1].first + font->runs[r - 1].count);
            /* Both ends of the run resolve, to the right place; the codepoints around it do not belong to it. */
            CHECK(tg_font_glyph(font, run->first) == font->glyphs + (size_t)run->index * font->height);
            CHECK(tg_font_glyph(font, run->first + run->count - 1) ==
                  font->glyphs + (size_t)(run->index + run->count - 1) * font->height);
            CHECK(tg_font_glyph(font, run->first + run->count) == NULL);
            if (run->first > 0 && (r == 0 || font->runs[r - 1].first + font->runs[r - 1].count < run->first))
                CHECK(tg_font_glyph(font, run->first - 1) == NULL);
        }
        CHECK(next_index == font->nglyphs);

        /* Every codepoint up to the last one: found exactly when it lies in a run. */
        const tg_glyph_run_t *last = &font->runs[font->nruns - 1];
        const uint32_t top = last->first + last->count;
        unsigned found = 0;
        for (uint32_t cp = 0; cp < top + 50; cp++)
            found += tg_font_glyph(font, cp) != NULL;
        CHECK(found == font->nglyphs);
        CHECK(tg_font_glyph(font, 0x110000) == NULL && tg_font_glyph(font, 0xFFFFFFFFu) == NULL);

        /* All of printable ASCII is there, and the letters are not blank. */
        for (uint32_t c = 0x20; c < 0x7F; c++) {
            const uint8_t *g = tg_font_glyph(font, c);
            CHECK(g != NULL);
            int blank = 1;
            for (int i = 0; i < font->height; i++)
                blank &= g[i] == 0;
            CHECK(blank == (c == 0x20));
        }
    }
}

/* Glyphs copied from the original Unscii .hex files (viznut/unscii at 257b4df). */
static void test_against_source(void)
{
    static const struct { const tg_font_t *font; uint32_t cp; const char *hex; } golden[] = {
        {&tg_font_unscii8, 0x21, "1818181818001800"},
        {&tg_font_unscii8, 0x41, "183C66667E666600"},
        {&tg_font_unscii8, 0xE9, "0E003C667E603C00"},
        {&tg_font_unscii8, 0x2588, "FFFFFFFFFFFFFFFF"},
        {&tg_font_unscii8_alt, 0x21, "3078783030003000"},
        {&tg_font_unscii8_alt, 0x41, "3C667E6666666600"},
        {&tg_font_unscii8_thin, 0x21, "1010101000001000"},
        {&tg_font_unscii8_thin, 0x41, "1824427E42424200"},
        {&tg_font_unscii8_thin, 0xE9, "0E0038447C403800"},
        {&tg_font_unscii8_fantasy, 0x41, "1E6CCCFCCC6CEE00"},
        {&tg_font_unscii8_fantasy, 0xE9, "0E003E667E607C00"},
        {&tg_font_unscii8_mcr, 0x21, "1C1C1C1800181800"},
        {&tg_font_unscii8_mcr, 0x41, "7E6666FEE6E6E600"},
    };
    for (size_t i = 0; i < sizeof golden / sizeof *golden; i++) {
        const uint8_t *g = tg_font_glyph(golden[i].font, golden[i].cp);
        CHECK(g && bytes_equal_hex(g, 8, golden[i].hex));
    }
    static const struct { const tg_font_t *font; uint32_t cp; const char *hex; } tall[] = {
        {&tg_font_unscii8_tall, 0x21, "18181818181818181818000018180000"},
        {&tg_font_unscii8_tall, 0x41, "18183C3C666666667E7E666666660000"},
        {&tg_font_unscii8_tall, 0xE9, "0E0E00003C3C66667E7E60603C3C0000"},
        {&tg_font_unscii8_tall, 0x2588, "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"},
        {&tg_font_unscii16, 0x21, "00181818181818181800001818000000"},
        {&tg_font_unscii16, 0x41, "0000183C6666667E6666666666000000"},
        {&tg_font_unscii16, 0xE9, "00000C1800003C66667E60603C000000"},
        {&tg_font_unscii16, 0x2588, "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"},
    };
    for (size_t i = 0; i < sizeof tall / sizeof *tall; i++) {
        const uint8_t *g = tg_font_glyph(tall[i].font, tall[i].cp);
        CHECK(g && bytes_equal_hex(g, 16, tall[i].hex));
    }
    /* The variants are really different designs. */
    CHECK(memcmp(tg_font_glyph(&tg_font_unscii8, 0x41), tg_font_glyph(&tg_font_unscii8_thin, 0x41), 8) != 0);
    /* Codepoints that the fonts do not have. */
    for (int f = 0; f < NFONTS; f++) {
        CHECK(tg_font_glyph(FONTS[f], 0x378) == NULL);      /* unassigned */
        CHECK(tg_font_glyph(FONTS[f], 0xFFFF) == NULL);
        CHECK(tg_font_glyph(FONTS[f], 0x4E00) == NULL);     /* no CJK in the public-domain fonts */
    }
}

static uint8_t expect_pixel(const uint8_t *glyph, unsigned tile_log2, unsigned depth, unsigned x, unsigned y, uint8_t fg,
                            uint8_t bg)
{
    const unsigned scale = (1u << tile_log2) / 8;
    const int bit = (glyph[y / scale] >> (7 - x / scale)) & 1;
    uint8_t v = bit ? fg : bg;
    return depth == 4 ? v & 15 : v;
}

/* A tile programmed from a glyph comes out of the compositor as exactly that glyph. */
static void test_tile_from_glyph(void)
{
    CHECK(tg_tile_bytes(3, 4) == 32 && tg_tile_bytes(3, 8) == 64 && tg_tile_bytes(4, 4) == 128 && tg_tile_bytes(4, 8) == 256);
    for (unsigned tl = 3; tl <= 4; tl++)
        for (unsigned depth = 4; depth <= 8; depth += 4)
            for (int trial = 0; trial < 200; trial++) {
                uint8_t glyph[8];
                for (int i = 0; i < 8; i++)
                    glyph[i] = (uint8_t)rnd();
                const uint8_t fg = (uint8_t)(rnd() % 256), bg = (uint8_t)(rnd() % 3 ? rnd() % 256 : 0);
                static uint8_t tile[256];
                memset(tile, 0xA5, sizeof tile);   /* stale contents must be fully overwritten */
                tg_tile_from_glyph(tile, tl, depth, glyph, fg, bg);

                static uint16_t map[1];
                map[0] = TG_ENTRY(0, 0, 0);
                static tg_t g;
                tg_init(&g);
                g.layers[0] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = (uint8_t)tl, .depth = (uint8_t)depth,
                                           .map_w_log2 = 0, .map_h_log2 = 0, .map = map, .tiles = tile};
                for (unsigned y = 0; y < (1u << tl); y++) {
                    uint8_t idx[TG_WIDTH];
                    tg_render_line_indices(&g, (int)y, idx);
                    for (unsigned x = 0; x < (1u << tl); x++)
                        CHECK(idx[x] == expect_pixel(glyph, tl, depth, x, y, fg, bg));
                }
            }

    /* Bad arguments change nothing. */
    uint8_t tile[256], glyph[8] = {0xFF};
    memset(tile, 0x5A, sizeof tile);
    tg_tile_from_glyph(tile, 5, 8, glyph, 1, 0);
    tg_tile_from_glyph(tile, 3, 3, glyph, 1, 0);
    for (size_t i = 0; i < sizeof tile; i++)
        CHECK(tile[i] == 0x5A);
}

/* A map and a tile array wrapped in a tg_t, to look at through the compositor. */
static void render_cell(uint16_t *map, unsigned map_w_log2, unsigned map_h_log2, uint8_t *tiles, unsigned tl, unsigned depth,
                        unsigned cx, unsigned cy, unsigned row, uint8_t *idx)
{
    static tg_t g;
    tg_init(&g);
    g.layers[0] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = (uint8_t)tl, .depth = (uint8_t)depth,
                               .map_w_log2 = (uint8_t)map_w_log2, .map_h_log2 = (uint8_t)map_h_log2, .map = map, .tiles = tiles};
    g.layers[0].scroll_x = cx << tl;
    g.layers[0].scroll_y = cy << tl;
    tg_render_line_indices(&g, (int)row, idx);
}

/* The workflow in the header: ASCII 32..126 become tiles 0..94, and a string is printed with offset -32. */
static void test_range_and_offset(void)
{
    static const tg_font_t *const small[] = {&tg_font_unscii8, &tg_font_unscii8_alt, &tg_font_unscii8_thin,
                                             &tg_font_unscii8_fantasy, &tg_font_unscii8_mcr};
    for (size_t f = 0; f < sizeof small / sizeof *small; f++)
        for (unsigned tl = 3; tl <= 4; tl++)
            for (unsigned depth = 4; depth <= 8; depth += 4)
                for (unsigned pal = 0; pal < 3; pal++) {
                    const tg_font_t *font = small[f];
                    const unsigned size = 1u << tl;
                    static uint8_t tiles[100 * 256];
                    static uint16_t map[16 * 8];
                    memset(tiles, 0xEE, sizeof tiles);
                    memset(map, 0, sizeof map);
                    const uint8_t fg = (uint8_t)(3 + f), bg = (uint8_t)(f & 1 ? 0 : 6);
                    tg_font_load_range(font, tiles, tl, depth, 0, 32, 95, 0, fg, bg);
                    const char *text = "Hello, World!~";                       /* includes '~', code 126, the last glyph */
                    tg_map_put_string(map, 4, 3, 2, 5, text, -32, pal);
                    for (unsigned i = 0; text[i]; i++) {
                        CHECK(map[5 * 16 + 2 + i] == TG_ENTRY((uint8_t)text[i] - 32, pal, 0));
                        const uint8_t *glyph = tg_font_glyph(font, (uint8_t)text[i]);
                        for (unsigned y = 0; y < size; y++) {
                            uint8_t idx[TG_WIDTH];
                            render_cell(map, 4, 3, tiles, tl, depth, 2 + i, 5, y, idx);
                            for (unsigned x = 0; x < size; x++) {
                                uint8_t want = expect_pixel(glyph, tl, depth, x, y, fg, bg);
                                if (depth == 4 && want)
                                    want |= (uint8_t)(pal << 4);
                                CHECK(idx[x] == want);
                            }
                        }
                    }
                    /* The tiles past the range were not touched. */
                    const size_t bytes = tg_tile_bytes(tl, depth);
                    for (size_t b = 95 * bytes; b < 100 * bytes; b++)
                        CHECK(tiles[b] == 0xEE);
                }

    /* The layer loader makes exactly the tiles of the explicit range loader, for one and for two tiles a glyph. */
    {
        static uint8_t a[300 * 256], b[300 * 256];
        static uint16_t m[16 * 8];
        for (unsigned tl = 3; tl <= 4; tl++)
            for (unsigned depth = 4; depth <= 8; depth += 4) {
                memset(a, 0, sizeof a);
                memset(b, 0, sizeof b);
                tg_layer_t l = {.kind = TG_LAYER_TILES, .tile_log2 = (uint8_t)tl, .depth = (uint8_t)depth, .map_w_log2 = 4,
                                .map_h_log2 = 3, .map = m, .tiles = a};
                tg_font_load_range(&tg_font_unscii8_mcr, b, tl, depth, 7, 0x20, 95, 0, 5, 2);
                CHECK(tg_layer_load_font_ptr(&l, &tg_font_unscii8_mcr, 0x20, 95, 7, 5, 2));
                CHECK(memcmp(a, b, 102 * tg_tile_bytes(tl, depth)) == 0);
                memset(a, 0, sizeof a);
                memset(b, 0, sizeof b);
                tg_font_load_range(&tg_font_unscii16, b, tl, depth, 7, 0x20, 95, 0, 5, 2);
                tg_font_load_range(&tg_font_unscii16, b, tl, depth, 7 + 95, 0x20, 95, 1, 5, 2);
                CHECK(tg_layer_load_font_ptr(&l, &tg_font_unscii16, 0x20, 95, 7, 5, 2));
                CHECK(memcmp(a, b, (7 + 190) * tg_tile_bytes(tl, depth)) == 0);
            }
    }

    /* A tall font in two ranges: the top halves, then the bottom halves; printed one map row apart. */
    for (unsigned tl = 3; tl <= 4; tl++)
        for (unsigned depth = 4; depth <= 8; depth += 4)
            for (int which = 0; which < 2; which++) {
                const tg_font_t *font = which ? &tg_font_unscii16 : &tg_font_unscii8_tall;
                const unsigned size = 1u << tl;
                static uint8_t tiles[200 * 256];
                static uint16_t map[16 * 8];
                memset(tiles, 0, sizeof tiles);
                memset(map, 0, sizeof map);
                tg_font_load_range(font, tiles, tl, depth, 0, 32, 95, 0, 7, 0);      /* tiles 0..94: tops */
                tg_font_load_range(font, tiles, tl, depth, 95, 32, 95, 1, 7, 0);     /* tiles 95..189: bottoms */
                const char *text = "Tall!";
                tg_map_put_string(map, 4, 3, 1, 2, text, -32, 0);
                tg_map_put_string(map, 4, 3, 1, 3, text, -32 + 95, 0);
                for (unsigned i = 0; text[i]; i++) {
                    const uint8_t *glyph = tg_font_glyph(font, (uint8_t)text[i]);
                    for (unsigned half = 0; half < 2; half++)
                        for (unsigned y = 0; y < size; y++) {
                            uint8_t idx[TG_WIDTH];
                            render_cell(map, 4, 3, tiles, tl, depth, 1 + i, 2 + half, y, idx);
                            for (unsigned x = 0; x < size; x++)
                                CHECK(idx[x] == expect_pixel(glyph + 8 * half, tl, depth, x, y, 7, 0));
                        }
                }
            }
    /* Asking for the second half of an 8-row font gives blanks. */
    {
        static uint8_t tiles[4 * 64];
        memset(tiles, 0x77, sizeof tiles);
        tg_font_load_range(&tg_font_unscii8, tiles, 3, 8, 0, 0x41, 4, 1, 9, 3);
        for (size_t b = 0; b < sizeof tiles; b++)
            CHECK(tiles[b] == 3);
    }

    /* Codepoints the font lacks become tiles of background; the ones it has are the glyphs. */
    {
        static uint8_t tiles[16 * 64];
        memset(tiles, 0, sizeof tiles);
        int have = 0, lack = 0;
        tg_font_load_range(&tg_font_unscii8, tiles, 3, 8, 0, 0x300, 16, 0, 9, 4);
        for (unsigned i = 0; i < 16; i++) {
            const uint8_t *g = tg_font_glyph(&tg_font_unscii8, 0x300 + i);
            have += g != NULL;
            lack += g == NULL;
            for (unsigned y = 0; y < 8; y++)
                for (unsigned x = 0; x < 8; x++)
                    CHECK(tiles[i * 64 + y * 8 + x] == (g && (g[y] >> (7 - x) & 1) ? 9 : 4));
        }
        CHECK(have > 0 && lack > 0);                       /* the range really mixes the two cases */
    }
}

static void test_map_put(void)
{
    static uint16_t map[16 * 8];
    const uint16_t S = 0xFFFF;                             /* a sentinel for "never written" */
#define FILL() do { for (size_t i = 0; i < sizeof map / sizeof *map; i++) map[i] = S; } while (0)

    /* Offset -32: codes below 32 have no tile and are skipped, but the position still moves on. */
    FILL();
    tg_map_put_string(map, 4, 3, 0, 0, "\x01\x1F A", -32, 2);
    CHECK(map[0] == S && map[1] == S);
    CHECK(map[2] == TG_ENTRY(0, 2, 0) && map[3] == TG_ENTRY(33, 2, 0) && map[4] == S);

    /* Positive offsets, and the edges of the tile number range. */
    FILL();
    tg_map_put_string(map, 4, 3, 0, 0, "\x7B\x7C\xFF", 900, 0);   /* 123 + 900 = 1023 fits; 124 + 900 = 1024 and 255 + 900 do not */
    CHECK(map[0] == TG_ENTRY(1023, 0, 0) && map[1] == S && map[2] == S);
    FILL();
    tg_map_put_string(map, 4, 3, 0, 0, "\x00\x01", 0, 0);        /* a string cannot hold glyph 0 after the start... */
    CHECK(map[0] == S);                                       /* ...an empty string writes nothing (the NUL ends it) */

    /* Bytes above 0x7F are glyph numbers as they are, not UTF-8. */
    FILL();
    tg_map_put_string(map, 4, 3, 0, 0, "\xE9", -32, 0);
    CHECK(map[0] == TG_ENTRY(233 - 32, 0, 0));

    /* Wrapping on both axes, with negative positions. */
    FILL();
    tg_map_put_string(map, 4, 3, 14, 7, "ABCD", -65, 0);        /* columns 14, 15, 0, 1 of row 7 */
    CHECK(map[7 * 16 + 14] == TG_ENTRY(0, 0, 0) && map[7 * 16 + 15] == TG_ENTRY(1, 0, 0));
    CHECK(map[7 * 16 + 0] == TG_ENTRY(2, 0, 0) && map[7 * 16 + 1] == TG_ENTRY(3, 0, 0));
    FILL();
    tg_map_put_string(map, 4, 3, -1, -1, "XY", -65, 0);        /* column 15 and 0 of row 7 */
    CHECK(map[7 * 16 + 15] == TG_ENTRY('X' - 65, 0, 0) && map[7 * 16 + 0] == TG_ENTRY('Y' - 65, 0, 0));
    FILL();
    tg_map_put_string(map, 4, 3, 0, 9, "Z", -65, 0);            /* row 9 is row 1 */
    CHECK(map[1 * 16] == TG_ENTRY('Z' - 65, 0, 0));

    /* Glyph numbers as 16 bit values: above 255, and including 0, which a string cannot carry. */
    FILL();
    const uint16_t g[] = {0, 300, 5, 0, 1000};
    tg_map_put_glyphs(map, 4, 3, 3, 1, g, 5, -1, 4);
    CHECK(map[1 * 16 + 3] == S);                              /* 0 - 1 = -1: skipped */
    CHECK(map[1 * 16 + 4] == TG_ENTRY(299, 4, 0));
    CHECK(map[1 * 16 + 5] == TG_ENTRY(4, 4, 0));
    CHECK(map[1 * 16 + 6] == S);                              /* the second 0 did not end the list */
    CHECK(map[1 * 16 + 7] == TG_ENTRY(999, 4, 0));
    FILL();
    tg_map_put_glyphs(map, 4, 3, 0, 0, g, 0, 0, 0);            /* nothing to do */
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        CHECK(map[i] == S);
    FILL();
    tg_map_put_string(map, 4, 3, 0, 0, "", 0, 0);
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        CHECK(map[i] == S);
}

/* ---- the layer API: the offset is stored in the layer ---- */

#define SENTINEL 0xFFFF

static void reset_layer(tg_layer_t *l, uint8_t *tiles, size_t tiles_bytes, uint16_t *map, unsigned tl, unsigned depth)
{
    memset(tiles, 0, tiles_bytes);
    for (size_t i = 0; i < 16 * 8; i++)
        map[i] = SENTINEL;
    *l = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = (uint8_t)tl, .depth = (uint8_t)depth, .map_w_log2 = 4,
                      .map_h_log2 = 3, .map = map, .tiles = tiles};
}

/* One line of one map cell, as the compositor draws it. */
static void layer_cell(const tg_layer_t *l, unsigned cx, unsigned cy, unsigned row, uint8_t *idx)
{
    static tg_t g;
    tg_init(&g);
    g.layers[0] = *l;
    g.layers[0].scroll_x = cx << l->tile_log2;
    g.layers[0].scroll_y = cy << l->tile_log2;
    tg_render_line_indices(&g, (int)row, idx);
}

static const tg_font_id_t ALL_IDS[] = {TG_FONT_UNSCII8,         TG_FONT_UNSCII8_ALT,  TG_FONT_UNSCII8_THIN,
                                       TG_FONT_UNSCII8_FANTASY, TG_FONT_UNSCII8_MCR,  TG_FONT_UNSCII8_TALL,
                                       TG_FONT_UNSCII16};

static void test_font_numbers(void)
{
    CHECK(TG_FONT_COUNT == NFONTS);
    CHECK(tg_font_by_id(TG_FONT_UNSCII8) == &tg_font_unscii8);
    CHECK(tg_font_by_id(TG_FONT_UNSCII8_ALT) == &tg_font_unscii8_alt);
    CHECK(tg_font_by_id(TG_FONT_UNSCII8_THIN) == &tg_font_unscii8_thin);
    CHECK(tg_font_by_id(TG_FONT_UNSCII8_FANTASY) == &tg_font_unscii8_fantasy);
    CHECK(tg_font_by_id(TG_FONT_UNSCII8_MCR) == &tg_font_unscii8_mcr);
    CHECK(tg_font_by_id(TG_FONT_UNSCII8_TALL) == &tg_font_unscii8_tall);
    CHECK(tg_font_by_id(TG_FONT_UNSCII16) == &tg_font_unscii16);
    CHECK(tg_font_by_id(TG_FONT_COUNT) == NULL && tg_font_by_id((tg_font_id_t)99) == NULL && tg_font_by_id((tg_font_id_t)-1) == NULL);
}

/* The workflow in the header: glyphs 32..126 into tiles 100..194, the offset 68 is stored, and "Hello" just prints. */
static void test_layer_load_font(void)
{
    for (size_t k = 0; k < sizeof ALL_IDS / sizeof *ALL_IDS; k++)
        for (unsigned tl = 3; tl <= 4; tl++)
            for (unsigned depth = 4; depth <= 8; depth += 4)
                for (unsigned pal = 0; pal < 3; pal++) {
                    const tg_font_t *font = tg_font_by_id(ALL_IDS[k]);
                    const unsigned per = font->height / 8, size = 1u << tl;
                    static uint8_t tiles[300 * 256];
                    static uint16_t map[16 * 8];
                    tg_layer_t l;
                    reset_layer(&l, tiles, sizeof tiles, map, tl, depth);
                    const uint8_t fg = (uint8_t)(3 + k), bg = (uint8_t)(k & 1 ? 0 : 6);
                    CHECK(tg_layer_load_font(&l, ALL_IDS[k], 32, 95, 100, fg, bg));
                    CHECK(l.glyphs.offset == 100 - 32);                       /* 68, worked out by the library */
                    CHECK(l.glyphs.first == 32 && l.glyphs.count == 95);
                    CHECK(l.glyphs.row_delta == (per == 2 ? 95 : 0));

                    const char *text = "Hello, World!~";
                    tg_layer_put_string(&l, 2, 1, text, pal);
                    for (unsigned i = 0; text[i]; i++) {
                        const unsigned c = (uint8_t)text[i];
                        CHECK(map[1 * 16 + 2 + i] == TG_ENTRY(c + 68, pal, 0));
                        if (per == 2)
                            CHECK(map[2 * 16 + 2 + i] == TG_ENTRY(c + 68 + 95, pal, 0));
                        else
                            CHECK(map[2 * 16 + 2 + i] == SENTINEL);
                        const uint8_t *glyph = tg_font_glyph(font, c);
                        for (unsigned half = 0; half < per; half++)
                            for (unsigned y = 0; y < size; y++) {
                                uint8_t idx[TG_WIDTH];
                                layer_cell(&l, 2 + i, 1 + half, y, idx);
                                for (unsigned x = 0; x < size; x++) {
                                    uint8_t want = expect_pixel(glyph + 8 * half, tl, depth, x, y, fg, bg);
                                    if (depth == 4 && want)
                                        want |= (uint8_t)(pal << 4);
                                    CHECK(idx[x] == want);
                                }
                            }
                    }
                    /* Glyphs outside what was loaded have no tile: skipped, cells untouched, positions still move on. */
                    for (size_t i = 0; i < 16 * 8; i++)
                        map[i] = SENTINEL;
                    tg_layer_put_string(&l, 0, 4, "\x01\x1F A\x7F\xE9~", 0);
                    CHECK(map[4 * 16 + 0] == SENTINEL && map[4 * 16 + 1] == SENTINEL);
                    CHECK(map[4 * 16 + 2] == TG_ENTRY(' ' + 68, 0, 0) && map[4 * 16 + 3] == TG_ENTRY('A' + 68, 0, 0));
                    CHECK(map[4 * 16 + 4] == SENTINEL && map[4 * 16 + 5] == SENTINEL);
                    CHECK(map[4 * 16 + 6] == TG_ENTRY('~' + 68, 0, 0));
                                }

    /* Other ranges and offsets, including a negative one. */
    static uint8_t tiles[300 * 256];
    static uint16_t map[16 * 8];
    tg_layer_t l;
    reset_layer(&l, tiles, sizeof tiles, map, 3, 8);
    CHECK(tg_layer_load_font(&l, TG_FONT_UNSCII8, 0x41, 26, 5, 1, 0));
    CHECK(l.glyphs.offset == 5 - 0x41);
    tg_layer_put_string(&l, 0, 0, "AZaz", 0);                       /* a and z are not in the loaded 'A'..'Z' */
    CHECK(map[0] == TG_ENTRY(5, 0, 0) && map[1] == TG_ENTRY(30, 0, 0) && map[2] == SENTINEL && map[3] == SENTINEL);
    CHECK(tg_layer_load_font(&l, TG_FONT_UNSCII8, 0x3B1, 10, 500, 1, 0));   /* Greek: a large glyph number, a positive offset... */
    CHECK(l.glyphs.offset == 500 - 0x3B1);
    uint16_t greek[] = {0x3B1, 0x3BA, 0x3BB};
    tg_layer_put_glyphs(&l, 0, 1, greek, 3, 0);
    CHECK(map[16] == TG_ENTRY(500, 0, 0) && map[17] == TG_ENTRY(500 + 9, 0, 0));   /* the first and the tenth loaded */
    CHECK(map[18] == SENTINEL);                                                    /* 0x3BB would be the eleventh */
}

static void test_layer_load_failures(void)
{
    static uint8_t tiles[1100 * 256];
    static uint16_t map[16 * 8];
    tg_layer_t l;
    reset_layer(&l, tiles, sizeof tiles, map, 3, 8);
    const tg_glyphmap_t untouched = {.offset = 7, .first = 1, .count = 2, .row_delta = 3};
    l.glyphs = untouched;
    memset(tiles, 0xCD, sizeof tiles);
#define UNCHANGED()                                                                      \
    do {                                                                                 \
        CHECK(memcmp(&l.glyphs, &untouched, sizeof untouched) == 0);                     \
        for (size_t b = 0; b < sizeof tiles; b++)                                        \
            CHECK(tiles[b] == 0xCD);                                                     \
    } while (0)
    CHECK(!tg_layer_load_font(&l, TG_FONT_COUNT, 32, 95, 0, 1, 0));            UNCHANGED();
    CHECK(!tg_layer_load_font(&l, (tg_font_id_t)99, 32, 95, 0, 1, 0));         UNCHANGED();
    CHECK(!tg_layer_load_font(&l, TG_FONT_UNSCII8, 32, 0, 0, 1, 0));           UNCHANGED();   /* no glyphs */
    CHECK(!tg_layer_load_font_ptr(&l, NULL, 32, 95, 0, 1, 0));                 UNCHANGED();
    CHECK(!tg_layer_load_font(&l, TG_FONT_UNSCII8, 32, 25, 1000, 1, 0));       UNCHANGED();   /* 1000 + 25 > 1024 */
    CHECK(!tg_layer_load_font(&l, TG_FONT_UNSCII8_TALL, 32, 13, 1000, 1, 0));  UNCHANGED();   /* two tiles each: 1026 */
    CHECK(!tg_layer_load_font(&l, TG_FONT_UNSCII8, 32, 1, 1024, 1, 0));        UNCHANGED();
    CHECK(!tg_layer_load_glyphs(&l, (const uint8_t *)"01234567", 8, 0, 25, 1000, 1, 0)); UNCHANGED();
    /* A layer that text cannot go on. */
    tg_layer_t bad = l;
    bad.kind = TG_LAYER_BITMAP;
    CHECK(!tg_layer_load_font(&bad, TG_FONT_UNSCII8, 32, 95, 0, 1, 0));
    bad = l; bad.kind = TG_LAYER_OFF;
    CHECK(!tg_layer_load_font(&bad, TG_FONT_UNSCII8, 32, 95, 0, 1, 0));
    bad = l; bad.tiles = NULL;
    CHECK(!tg_layer_load_font(&bad, TG_FONT_UNSCII8, 32, 95, 0, 1, 0));
    bad = l; bad.tile_log2 = 5;
    CHECK(!tg_layer_load_font(&bad, TG_FONT_UNSCII8, 32, 95, 0, 1, 0));
    bad = l; bad.depth = 2;
    CHECK(!tg_layer_load_font(&bad, TG_FONT_UNSCII8, 32, 95, 0, 1, 0));
    UNCHANGED();
    CHECK(!tg_layer_load_glyphs(&l, NULL, 8, 0, 4, 0, 1, 0));                  UNCHANGED();
    CHECK(!tg_layer_load_glyphs(&l, (const uint8_t *)"01234567", 12, 0, 1, 0, 1, 0)); UNCHANGED();
    CHECK(!tg_layer_load_glyphs(&l, (const uint8_t *)"01234567", 8, 0, 0, 0, 1, 0));  UNCHANGED();

    /* Exactly filling the tile numbers is allowed. */
    memset(tiles, 0, sizeof tiles);
    CHECK(tg_layer_load_font(&l, TG_FONT_UNSCII8, 32, 24, 1000, 1, 0));
    CHECK(tg_layer_load_font(&l, TG_FONT_UNSCII8_TALL, 32, 12, 1000, 1, 0));
    CHECK(l.glyphs.row_delta == 12 && l.glyphs.offset == 1000 - 32);
    CHECK(tg_layer_load_font(&l, TG_FONT_UNSCII8, 32, 1, 1023, 1, 0));

    /* The text functions do nothing, and do not crash, on a layer that cannot take text. */
    tg_layer_t none = {.kind = TG_LAYER_OFF};
    tg_layer_put_string(&none, 0, 0, "x", 0);
    tg_layer_put_utf8(&none, 0, 0, "x", 0);
    tg_layer_put_glyphs(&none, 0, 0, (const uint16_t[]){1}, 1, 0);
    tg_layer_t nomap = l;
    nomap.map = NULL;
    tg_layer_put_string(&nomap, 0, 0, "x", 0);
}

/* Your own glyph array: load by pointer, then set the offset with its own call. */
static void test_layer_low_level(void)
{
    static const uint8_t mine[4 * 8] = {
        0xFF, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xFF,   /* a box */
        0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01,   /* a diagonal */
        0x01, 0x03, 0x07, 0x0F, 0x1F, 0x3F, 0x7F, 0xFF,   /* a wedge */
        0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55,   /* a checkerboard */
    };
    for (unsigned tl = 3; tl <= 4; tl++)
        for (unsigned depth = 4; depth <= 8; depth += 4) {
            const unsigned size = 1u << tl;
            static uint8_t tiles[300 * 256];
            static uint16_t map[16 * 8];
            tg_layer_t l;
            reset_layer(&l, tiles, sizeof tiles, map, tl, depth);
            CHECK(tg_layer_load_glyphs(&l, mine, 8, 0, 4, 10, 5, 2));
            CHECK(l.glyphs.offset == 0 && l.glyphs.count == 0 && l.glyphs.row_delta == 0);   /* loading by pointer sets no offset */
            tg_layer_set_glyph_offset(&l, 10 - 'A');                                      /* glyph number 65 is the first */
            CHECK(l.glyphs.offset == 10 - 65 && l.glyphs.count == 0);
            tg_layer_put_string(&l, 1, 1, "ABCD", 0);
            for (unsigned i = 0; i < 4; i++) {
                CHECK(map[1 * 16 + 1 + i] == TG_ENTRY(10 + i, 0, 0));
                for (unsigned y = 0; y < size; y++) {
                    uint8_t idx[TG_WIDTH];
                    layer_cell(&l, 1 + i, 1, y, idx);
                    for (unsigned x = 0; x < size; x++)
                        CHECK(idx[x] == expect_pixel(mine + 8 * i, tl, depth, x, y, 5, 2));
                }
            }
            /* Unchecked until a range is given: letters past the four still get tile numbers (what is there is up to you). */
            tg_layer_put_string(&l, 0, 3, "E", 0);
            CHECK(map[3 * 16] == TG_ENTRY(14, 0, 0));
            tg_layer_set_glyph_range(&l, 'A', 4);
            tg_layer_put_string(&l, 0, 4, "ABCDE", 0);
            CHECK(map[4 * 16 + 3] == TG_ENTRY(13, 0, 0) && map[4 * 16 + 4] == SENTINEL);
            tg_layer_set_glyph_range(&l, 0, 0);
            tg_layer_put_string(&l, 0, 5, "E", 0);
            CHECK(map[5 * 16] == TG_ENTRY(14, 0, 0));
            /* Setting an offset by hand starts again: no range, one tile tall, even after a built-in tall font. */
            CHECK(tg_layer_load_font(&l, TG_FONT_UNSCII8_TALL, 32, 20, 150, 1, 0));
            CHECK(l.glyphs.row_delta == 20 && l.glyphs.count == 20);
            tg_layer_set_glyph_offset(&l, 3);
            CHECK(l.glyphs.row_delta == 0 && l.glyphs.count == 0 && l.glyphs.first == 0 && l.glyphs.offset == 3);
        }

    /* Rows of a 16-row glyph array, top and bottom halves; a half that is not there is blank. */
    static const uint8_t tall[2 * 16] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
                                         0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F};
    static uint8_t tiles[8 * 64];
    static uint16_t map[16 * 8];
    tg_layer_t l;
    reset_layer(&l, tiles, sizeof tiles, map, 3, 8);
    CHECK(tg_layer_load_glyphs(&l, tall, 16, 0, 2, 0, 9, 1));       /* tops of the two glyphs: tiles 0, 1 */
    CHECK(tg_layer_load_glyphs(&l, tall, 16, 1, 2, 2, 9, 1));       /* bottoms: tiles 2, 3 */
    CHECK(tg_layer_load_glyphs(&l, tall, 16, 2, 1, 4, 9, 1));       /* a third "half" does not exist: blank */
    for (unsigned y = 0; y < 8; y++)
        for (unsigned x = 0; x < 8; x++) {
            CHECK(tiles[0 * 64 + y * 8 + x] == (tall[y] >> (7 - x) & 1 ? 9 : 1));
            CHECK(tiles[1 * 64 + y * 8 + x] == (tall[16 + y] >> (7 - x) & 1 ? 9 : 1));
            CHECK(tiles[2 * 64 + y * 8 + x] == (tall[8 + y] >> (7 - x) & 1 ? 9 : 1));
            CHECK(tiles[3 * 64 + y * 8 + x] == (tall[24 + y] >> (7 - x) & 1 ? 9 : 1));
            CHECK(tiles[4 * 64 + y * 8 + x] == 1);
        }
}

static void test_layer_utf8_and_glyphs(void)
{
    static uint8_t tiles[400 * 64];
    static uint16_t map[16 * 8];
    tg_layer_t l;
    reset_layer(&l, tiles, sizeof tiles, map, 3, 8);

    /* Box drawing from UTF-8 source text. */
    CHECK(tg_layer_load_font(&l, TG_FONT_UNSCII8, 0x2500, 128, 1, 1, 0));
#define BOX(cp) TG_ENTRY(1 + ((cp) - 0x2500), 0, 0)
    tg_layer_put_utf8(&l, 0, 0, "\xE2\x94\x80\xE2\x94\x82\xE2\x94\x8C", 0);               /* U+2500 U+2502 U+250C */
    CHECK(map[0] == BOX(0x2500) && map[1] == BOX(0x2502) && map[2] == BOX(0x250C) && map[3] == SENTINEL);
    /* Not in the loaded range: skipped. A byte string is raw glyph numbers, so it is not decoded. */
    for (size_t i = 0; i < 16 * 8; i++)
        map[i] = SENTINEL;
    tg_layer_put_utf8(&l, 0, 0, "a\xE2\x94\x80", 0);
    CHECK(map[0] == SENTINEL && map[1] == BOX(0x2500));
    tg_layer_put_string(&l, 0, 1, "\xE2\x94\x80", 0);
    CHECK(map[16] == SENTINEL && map[17] == SENTINEL && map[18] == SENTINEL);   /* bytes 0xE2 0x94 0x80: no tiles */

    /* Ill-formed UTF-8 is skipped a byte at a time, so a valid character after it lands in the right place. */
    static const struct { const char *text; int bad_bytes; } bad[] = {
        {"\xC0\x80", 2}, {"\xE0\x80\x80", 3}, {"\xED\xA0\x80", 3}, {"\xF4\x90\x80\x80", 4},
        {"\xE2\x94", 2}, {"\x80", 1},         {"\xFF", 1},         {"\xE2\x94\x41", 3},
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        char text[16];
        snprintf(text, sizeof text, "%s\xE2\x94\x80", bad[i].text);
        for (size_t k = 0; k < 16 * 8; k++)
            map[k] = SENTINEL;
        tg_layer_put_utf8(&l, 0, 0, text, 0);
        for (int k = 0; k < bad[i].bad_bytes; k++)
            CHECK(map[k] == SENTINEL);
        CHECK(map[bad[i].bad_bytes] == BOX(0x2500));
    }

    /* A real U+FFFD is a character like any other, not a marker for bad bytes. */
    for (size_t k = 0; k < 16 * 8; k++)
        map[k] = SENTINEL;
    tg_layer_set_glyph_offset(&l, 5 - 0xFFFD);
    tg_layer_put_utf8(&l, 0, 0, "\xEF\xBF\xBD\xEF\xBF\xBD", 0);
    CHECK(map[0] == TG_ENTRY(5, 0, 0) && map[1] == TG_ENTRY(5, 0, 0) && map[2] == SENTINEL);

    /* Mixed scripts: each load makes its glyphs the current ones; the earlier tiles stay, and so do the cells written. */
    reset_layer(&l, tiles, sizeof tiles, map, 3, 8);
    const char *mixed = "Caf\xC3\xA9 \xC2\xA9";
    CHECK(tg_layer_load_font(&l, TG_FONT_UNSCII8, 0xA0, 96, 1, 3, 0));        /* Latin-1 */
    tg_layer_put_utf8(&l, 0, 0, mixed, 0);
    CHECK(map[0] == SENTINEL && map[3] == TG_ENTRY(1 + 0xE9 - 0xA0, 0, 0) && map[5] == TG_ENTRY(1 + 0xA9 - 0xA0, 0, 0));
    CHECK(tg_layer_load_font(&l, TG_FONT_UNSCII8, 0x20, 95, 200, 1, 0));      /* ASCII, in other tiles */
    tg_layer_put_utf8(&l, 0, 0, mixed, 0);
    CHECK(map[0] == TG_ENTRY(200 + 'C' - 0x20, 0, 0) && map[4] == TG_ENTRY(200, 0, 0));     /* the space */
    CHECK(map[3] == TG_ENTRY(1 + 0xE9 - 0xA0, 0, 0));                         /* still the é from the first call */

    /* 16-bit glyph lists, including 0 and numbers over 255, with a font two tiles tall (and wrapping at the bottom). */
    static uint8_t ttiles[300 * 256];
    tg_layer_t t;
    reset_layer(&t, ttiles, sizeof ttiles, map, 4, 8);
    CHECK(tg_layer_load_font(&t, TG_FONT_UNSCII16, 0x2580, 32, 10, 1, 0));
    const uint16_t list[] = {0x2580, 0, 0x2588, 0xFFFF};
    tg_layer_put_glyphs(&t, 14, 7, list, 4, 0);                                  /* x wraps; the lower halves wrap to row 0 */
    CHECK(map[7 * 16 + 14] == TG_ENTRY(10, 0, 0) && map[0 * 16 + 14] == TG_ENTRY(10 + 32, 0, 0));
    CHECK(map[7 * 16 + 15] == SENTINEL && map[0 * 16 + 15] == SENTINEL);         /* glyph 0: no tile */
    CHECK(map[7 * 16 + 0] == TG_ENTRY(10 + 8, 0, 0) && map[0 * 16 + 0] == TG_ENTRY(10 + 8 + 32, 0, 0));
    CHECK(map[7 * 16 + 1] == SENTINEL);
    tg_layer_put_string(&t, -1, -1, "x", 0);                                     /* 'x' is outside the range: nothing, but no crash */
}

static void test_layer_text(void)
{
    test_font_numbers();
    test_layer_load_font();
    test_layer_load_failures();
    test_layer_low_level();
    test_layer_utf8_and_glyphs();
}

int main(void)
{
    test_structure();
    test_against_source();
    test_tile_from_glyph();
    test_range_and_offset();
    test_map_put();
    test_layer_text();
    printf("all %d checks passed\n", checks);
    return 0;
}
