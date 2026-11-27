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

/* The explicit steps: ASCII 32..126 become tiles 0..94, the character map says so, and a string is printed through it. */
static void test_range_and_map(void)
{
    static tg_char_t st[300];
    tg_charmap_t cm;
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
                    tg_charmap_init(&cm, st, 300);
                    CHECK(tg_charmap_set_range(&cm, 32, 95, 0, 0));
                    const char *text = "Hello, World!~";                       /* includes '~', code 126, the last glyph */
                    tg_map_put_string(map, 4, 3, &cm, 2, 5, text, pal);
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
                tg_charmap_init(&cm, st, 300);
                CHECK(tg_layer_load_font_ptr(&l, &cm, &tg_font_unscii8_mcr, 0x20, 95, 7, 5, 2));
                CHECK(memcmp(a, b, 102 * tg_tile_bytes(tl, depth)) == 0);
                memset(a, 0, sizeof a);
                memset(b, 0, sizeof b);
                tg_font_load_range(&tg_font_unscii16, b, tl, depth, 7, 0x20, 95, 0, 5, 2);
                tg_font_load_range(&tg_font_unscii16, b, tl, depth, 7 + 95, 0x20, 95, 1, 5, 2);
                tg_charmap_init(&cm, st, 300);
                CHECK(tg_layer_load_font_ptr(&l, &cm, &tg_font_unscii16, 0x20, 95, 7, 5, 2));
                CHECK(memcmp(a, b, (7 + 190) * tg_tile_bytes(tl, depth)) == 0);
            }
    }

    /* A tall font: the top halves, then the bottom halves 95 tiles on; one put writes both rows. */
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
                tg_charmap_init(&cm, st, 300);
                CHECK(tg_charmap_set_range(&cm, 32, 95, 0, 95));
                const char *text = "Tall!";
                tg_map_put_string(map, 4, 3, &cm, 1, 2, text, 0);
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

/* ---- the character map ---- */

static int sorted(const tg_charmap_t *cm)
{
    for (unsigned i = 1; i < cm->count; i++)
        if (cm->chars[i - 1].codepoint >= cm->chars[i].codepoint)
            return 0;
    return 1;
}

static void test_charmap(void)
{
    static tg_char_t st[8];
    tg_charmap_t cm;
    tg_charmap_init(&cm, st, 8);
    CHECK(cm.count == 0 && cm.capacity == 8 && tg_charmap_find(&cm, 'A') == NULL);

    /* Set in any order, kept sorted. */
    CHECK(tg_charmap_set(&cm, 'C', 3, TG_NO_TILE));
    CHECK(tg_charmap_set(&cm, 'A', 1, TG_NO_TILE));
    CHECK(tg_charmap_set(&cm, 0x2500, 1023, 1022));
    CHECK(tg_charmap_set(&cm, 'B', 2, TG_NO_TILE));
    CHECK(cm.count == 4 && st[0].codepoint == 'A' && st[1].codepoint == 'B' && st[2].codepoint == 'C' && st[3].codepoint == 0x2500);
    const tg_char_t *c = tg_charmap_find(&cm, 0x2500);
    CHECK(c && c->tile == 1023 && c->lower == 1022);
    CHECK(tg_charmap_find(&cm, 'A')->tile == 1 && tg_charmap_find(&cm, 'A')->lower == TG_NO_TILE && !tg_charmap_find(&cm, 'D'));

    /* Setting one again replaces it. */
    CHECK(tg_charmap_set(&cm, 'B', 20, 21) && cm.count == 4 && tg_charmap_find(&cm, 'B')->tile == 20 && tg_charmap_find(&cm, 'B')->lower == 21);

    /* Tile numbers a map entry cannot name are refused, changing nothing. */
    tg_char_t before[8];
    memcpy(before, st, sizeof st);
    CHECK(!tg_charmap_set(&cm, 'D', 1024, TG_NO_TILE) && !tg_charmap_set(&cm, 'D', 5, 1024) && !tg_charmap_set(&cm, 'A', 5, 4000));
    CHECK(cm.count == 4 && memcmp(before, st, sizeof st) == 0);

    /* Full: a new character is refused, a replacement is not. */
    for (uint32_t k = 0; k < 4; k++)
        CHECK(tg_charmap_set(&cm, 'a' + k, 100 + k, TG_NO_TILE));
    CHECK(cm.count == 8 && !tg_charmap_set(&cm, 'z', 9, TG_NO_TILE) && !tg_charmap_find(&cm, 'z'));
    CHECK(tg_charmap_set(&cm, 'a', 9, TG_NO_TILE) && cm.count == 8 && tg_charmap_find(&cm, 'a')->tile == 9);

    /* Taking out: in the middle, at the ends, and one that is not there. */
    CHECK(tg_charmap_remove(&cm, 'C') && !tg_charmap_remove(&cm, 'C') && cm.count == 7 && !tg_charmap_find(&cm, 'C'));
    CHECK(tg_charmap_find(&cm, 'B') && tg_charmap_find(&cm, 0x2500) && sorted(&cm));
    CHECK(tg_charmap_remove(&cm, 'A') && tg_charmap_remove(&cm, 0x2500) && cm.count == 5 && sorted(&cm));
    CHECK(tg_charmap_find(&cm, 'B')->tile == 20 && tg_charmap_find(&cm, 'd')->tile == 103);
    tg_charmap_clear(&cm);
    CHECK(cm.count == 0 && !tg_charmap_find(&cm, 'B') && cm.capacity == 8);

    /* No map, or no storage. */
    CHECK(tg_charmap_find(NULL, 'A') == NULL && !tg_charmap_set(NULL, 'A', 1, TG_NO_TILE) && !tg_charmap_remove(NULL, 'A'));
    CHECK(!tg_charmap_set_range(NULL, 32, 5, 0, 0) && !tg_charmap_set_range(NULL, 32, 0, 0, 0));
    tg_charmap_t none;
    tg_charmap_init(&none, NULL, 10);
    CHECK(none.capacity == 0 && !tg_charmap_set(&none, 'A', 1, TG_NO_TILE) && !tg_charmap_find(&none, 'A'));
    tg_charmap_init(&none, st, 100000);
    CHECK(none.capacity == 0xFFFF);

    /* Ranges: all or nothing, and only the characters the map does not have need room. */
    tg_charmap_init(&cm, st, 8);
    CHECK(tg_charmap_set_range(&cm, 'A', 5, 10, 0) && cm.count == 5);
    CHECK(tg_charmap_find(&cm, 'A')->tile == 10 && tg_charmap_find(&cm, 'E')->tile == 14 && tg_charmap_find(&cm, 'E')->lower == TG_NO_TILE);
    memcpy(before, st, sizeof st);
    CHECK(!tg_charmap_set_range(&cm, 'C', 7, 50, 0));                    /* C..I: four are new, three places are left */
    CHECK(cm.count == 5 && memcmp(before, st, sizeof st) == 0);
    CHECK(tg_charmap_set_range(&cm, 'D', 5, 50, 100) && cm.count == 8);   /* D..H: three are new; D and E are replaced */
    CHECK(tg_charmap_find(&cm, 'D')->tile == 50 && tg_charmap_find(&cm, 'D')->lower == 150);
    CHECK(tg_charmap_find(&cm, 'H')->tile == 54 && tg_charmap_find(&cm, 'H')->lower == 154 && tg_charmap_find(&cm, 'C')->tile == 12);
    CHECK(tg_charmap_set_range(&cm, 'Z', 0, 0, 0) && cm.count == 8);    /* nothing to do */
    /* the last tile a map entry can name, with and without lower halves */
    tg_charmap_clear(&cm);
    CHECK(tg_charmap_set_range(&cm, 0, 2, 1022, 0) && !tg_charmap_set_range(&cm, 0, 2, 1023, 0));
    CHECK(tg_charmap_set_range(&cm, 0, 2, 1000, 22) && !tg_charmap_set_range(&cm, 0, 2, 1000, 23));
    CHECK(tg_charmap_find(&cm, 1)->tile == 1001 && tg_charmap_find(&cm, 1)->lower == 1023);
    /* the codepoints must not wrap round */
    tg_charmap_clear(&cm);
    CHECK(tg_charmap_set_range(&cm, 0xFFFFFFFFu, 1, 5, 0) && tg_charmap_find(&cm, 0xFFFFFFFFu)->tile == 5);
    CHECK(!tg_charmap_set_range(&cm, 0xFFFFFFFEu, 3, 5, 0) && cm.count == 1);

    /* Random sets, removals and ranges against a plain table. */
    static tg_char_t big[40];
    tg_charmap_init(&cm, big, 40);
    int ref_tile[64], ref_lower[64];
    for (int i = 0; i < 64; i++)
        ref_tile[i] = -1;
    for (int op = 0; op < 20000; op++) {
        const uint32_t cp = rnd() % 64;
        const unsigned tile = rnd() % 1100, lower = rnd() % 3 ? TG_NO_TILE : rnd() % 1100;
        int present = 0;
        for (int i = 0; i < 64; i++)
            present += ref_tile[i] >= 0;
        switch (rnd() % 8) {
        case 0: case 1: case 2: case 3: {
            const int want = tile <= 1023 && (lower == TG_NO_TILE || lower <= 1023) && (ref_tile[cp] >= 0 || present < 40);
            CHECK(tg_charmap_set(&cm, cp, tile, lower) == want);
            if (want) {
                ref_tile[cp] = (int)tile;
                ref_lower[cp] = (int)lower;
            }
            break;
        }
        case 4: case 5:
            CHECK(tg_charmap_remove(&cm, cp) == (ref_tile[cp] >= 0));
            ref_tile[cp] = -1;
            break;
        case 6: {
            const unsigned n = rnd() % 6, delta = rnd() % 2 ? 0 : 1 + rnd() % 10;
            const uint32_t first = rnd() % 60;
            int fresh = 0;
            for (unsigned i = 0; i < n; i++)
                fresh += ref_tile[first + i] < 0;
            const int want = tile + n + delta <= 1024 && present + fresh <= 40;
            CHECK(tg_charmap_set_range(&cm, first, n, tile, delta) == (want || n == 0));
            if (want)
                for (unsigned i = 0; i < n; i++) {
                    ref_tile[first + i] = (int)(tile + i);
                    ref_lower[first + i] = delta ? (int)(tile + i + delta) : TG_NO_TILE;
                }
            break;
        }
        default:
            if (rnd() % 50 == 0) {
                tg_charmap_clear(&cm);
                for (int i = 0; i < 64; i++)
                    ref_tile[i] = -1;
            }
            break;
        }
        int n = 0;
        for (uint32_t i = 0; i < 64; i++) {
            const tg_char_t *e = tg_charmap_find(&cm, i);
            CHECK((e != NULL) == (ref_tile[i] >= 0));
            if (e) {
                CHECK(e->tile == ref_tile[i] && e->lower == ref_lower[i]);
                n++;
            }
        }
        CHECK(cm.count == n && sorted(&cm));
    }
}

static void test_map_put(void)
{
    static uint16_t map[16 * 8];
    static tg_char_t st[300];
    tg_charmap_t cm;
    const uint16_t S = 0xFFFF;                             /* a sentinel for "never written" */
#define FILL() do { for (size_t i = 0; i < sizeof map / sizeof *map; i++) map[i] = S; } while (0)

    /* ASCII as tiles 0..94: codes below 32 have no tile and are skipped, but the position still moves on. */
    tg_charmap_init(&cm, st, 300);
    CHECK(tg_charmap_set_range(&cm, 32, 95, 0, 0));
    FILL();
    tg_map_put_string(map, 4, 3, &cm, 0, 0, "\x01\x1F A", 2);
    CHECK(map[0] == S && map[1] == S);
    CHECK(map[2] == TG_ENTRY(0, 2, 0) && map[3] == TG_ENTRY(33, 2, 0) && map[4] == S);

    /* The edges of the tile numbers; an entry poked in by hand past 1023 is skipped. */
    tg_charmap_clear(&cm);
    CHECK(tg_charmap_set(&cm, 0x7B, 1023, TG_NO_TILE) && !tg_charmap_set(&cm, 0x7C, 1024, TG_NO_TILE));
    FILL();
    tg_map_put_string(map, 4, 3, &cm, 0, 0, "\x7B\x7C\xFF", 0);
    CHECK(map[0] == TG_ENTRY(1023, 0, 0) && map[1] == S && map[2] == S);
    st[0].tile = 2000;
    FILL();
    tg_map_put_string(map, 4, 3, &cm, 0, 0, "\x7B", 0);
    CHECK(map[0] == S);
    st[0].tile = 1023;
    FILL();
    tg_map_put_string(map, 4, 3, &cm, 0, 0, "\x00\x01", 0);   /* a string cannot hold character 0 after the start... */
    CHECK(map[0] == S);                                       /* ...an empty string writes nothing (the NUL ends it) */

    /* Bytes above 0x7F are characters as they are, not UTF-8. */
    tg_charmap_clear(&cm);
    CHECK(tg_charmap_set_range(&cm, 32, 224, 0, 0));
    FILL();
    tg_map_put_string(map, 4, 3, &cm, 0, 0, "\xE9", 0);
    CHECK(map[0] == TG_ENTRY(233 - 32, 0, 0) && map[1] == S);

    /* Wrapping on both axes, with negative positions. */
    tg_charmap_clear(&cm);
    CHECK(tg_charmap_set_range(&cm, 'A', 26, 0, 0));
    FILL();
    tg_map_put_string(map, 4, 3, &cm, 14, 7, "ABCD", 0);        /* columns 14, 15, 0, 1 of row 7 */
    CHECK(map[7 * 16 + 14] == TG_ENTRY(0, 0, 0) && map[7 * 16 + 15] == TG_ENTRY(1, 0, 0));
    CHECK(map[7 * 16 + 0] == TG_ENTRY(2, 0, 0) && map[7 * 16 + 1] == TG_ENTRY(3, 0, 0));
    FILL();
    tg_map_put_string(map, 4, 3, &cm, -1, -1, "XY", 0);        /* column 15 and 0 of row 7 */
    CHECK(map[7 * 16 + 15] == TG_ENTRY('X' - 65, 0, 0) && map[7 * 16 + 0] == TG_ENTRY('Y' - 65, 0, 0));
    FILL();
    tg_map_put_string(map, 4, 3, &cm, 0, 9, "Z", 0);            /* row 9 is row 1 */
    CHECK(map[1 * 16] == TG_ENTRY('Z' - 65, 0, 0));

    /* Two tiles tall: the lower half one row down, wrapping at the bottom. */
    CHECK(tg_charmap_set(&cm, 'T', 7, 8));
    FILL();
    tg_map_put_string(map, 4, 3, &cm, 3, 7, "TA", 1);
    CHECK(map[7 * 16 + 3] == TG_ENTRY(7, 1, 0) && map[0 * 16 + 3] == TG_ENTRY(8, 1, 0));
    CHECK(map[7 * 16 + 4] == TG_ENTRY(0, 1, 0) && map[0 * 16 + 4] == S);   /* A is one tile tall */

    /* 16-bit codepoints, including 0, which a string cannot carry. */
    tg_charmap_clear(&cm);
    CHECK(tg_charmap_set(&cm, 300, 299, TG_NO_TILE) && tg_charmap_set(&cm, 5, 4, TG_NO_TILE) && tg_charmap_set(&cm, 1000, 999, TG_NO_TILE));
    FILL();
    const uint16_t g[] = {0, 300, 5, 0, 1000};
    tg_map_put_glyphs(map, 4, 3, &cm, 3, 1, g, 5, 4);
    CHECK(map[1 * 16 + 3] == S);                              /* 0 has no tile: skipped */
    CHECK(map[1 * 16 + 4] == TG_ENTRY(299, 4, 0));
    CHECK(map[1 * 16 + 5] == TG_ENTRY(4, 4, 0));
    CHECK(map[1 * 16 + 6] == S);                              /* the second 0 did not end the list */
    CHECK(map[1 * 16 + 7] == TG_ENTRY(999, 4, 0));
    CHECK(tg_charmap_set(&cm, 0, 50, TG_NO_TILE));
    tg_map_put_glyphs(map, 4, 3, &cm, 3, 1, g, 5, 4);
    CHECK(map[1 * 16 + 3] == TG_ENTRY(50, 4, 0) && map[1 * 16 + 6] == TG_ENTRY(50, 4, 0));

    /* Nothing to do, or nothing to do it with: nothing changes. */
    FILL();
    tg_map_put_glyphs(map, 4, 3, &cm, 0, 0, g, 0, 0);
    tg_map_put_string(map, 4, 3, &cm, 0, 0, "", 0);
    tg_map_put_string(map, 4, 3, NULL, 0, 0, "\x05", 0);
    tg_map_put_glyphs(map, 4, 3, NULL, 0, 0, g, 5, 0);
    tg_map_put_string(NULL, 4, 3, &cm, 0, 0, "\x05", 0);
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        CHECK(map[i] == S);
#undef FILL
}

/* ---- the layer API ---- */

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

/* The workflow in the header: glyphs 32..126 into tiles 100..194, entered in the map, and "Hello" just prints. */
static void test_layer_load_font(void)
{
    static tg_char_t st[300];
    tg_charmap_t cm;
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
                    tg_charmap_init(&cm, st, 300);
                    const uint8_t fg = (uint8_t)(3 + k), bg = (uint8_t)(k & 1 ? 0 : 6);
                    CHECK(tg_layer_load_font(&l, &cm, ALL_IDS[k], 32, 95, 100, fg, bg));
                    CHECK(cm.count == 95 && sorted(&cm));
                    for (uint32_t c = 32; c < 127; c++) {
                        const tg_char_t *e = tg_charmap_find(&cm, c);
                        CHECK(e && e->tile == c + 68 && e->lower == (per == 2 ? c + 68 + 95 : TG_NO_TILE));
                    }

                    const char *text = "Hello, World!~";
                    tg_layer_put_string(&l, &cm, 2, 1, text, pal);
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
                    /* Characters that were not loaded have no tile: skipped, cells untouched, positions still move on. */
                    for (size_t i = 0; i < 16 * 8; i++)
                        map[i] = SENTINEL;
                    tg_layer_put_string(&l, &cm, 0, 4, "\x01\x1F A\x7F\xE9~", 0);
                    CHECK(map[4 * 16 + 0] == SENTINEL && map[4 * 16 + 1] == SENTINEL);
                    CHECK(map[4 * 16 + 2] == TG_ENTRY(' ' + 68, 0, 0) && map[4 * 16 + 3] == TG_ENTRY('A' + 68, 0, 0));
                    CHECK(map[4 * 16 + 4] == SENTINEL && map[4 * 16 + 5] == SENTINEL);
                    CHECK(map[4 * 16 + 6] == TG_ENTRY('~' + 68, 0, 0));
                }

    /* Loads add up in one map: A..Z, then Greek in other tiles, and both print; loading characters again moves them. */
    static uint8_t tiles[1024 * 64];
    static uint16_t map[16 * 8];
    tg_layer_t l;
    reset_layer(&l, tiles, sizeof tiles, map, 3, 8);
    tg_charmap_init(&cm, st, 300);
    CHECK(tg_layer_load_font(&l, &cm, TG_FONT_UNSCII8, 0x41, 26, 5, 1, 0));
    CHECK(tg_layer_load_font(&l, &cm, TG_FONT_UNSCII8, 0x3B1, 10, 500, 1, 0));
    CHECK(cm.count == 36);
    tg_layer_put_string(&l, &cm, 0, 0, "AZaz", 0);                  /* a and z are not in the loaded 'A'..'Z' */
    CHECK(map[0] == TG_ENTRY(5, 0, 0) && map[1] == TG_ENTRY(30, 0, 0) && map[2] == SENTINEL && map[3] == SENTINEL);
    uint16_t greek[] = {0x3B1, 0x3BA, 0x3BB};
    tg_layer_put_glyphs(&l, &cm, 0, 1, greek, 3, 0);
    CHECK(map[16] == TG_ENTRY(500, 0, 0) && map[17] == TG_ENTRY(500 + 9, 0, 0));   /* the first and the tenth loaded */
    CHECK(map[18] == SENTINEL);                                                    /* 0x3BB would be the eleventh */
    CHECK(tg_layer_load_font(&l, &cm, TG_FONT_UNSCII8, 0x41, 3, 600, 1, 0) && cm.count == 36);
    CHECK(tg_charmap_find(&cm, 'A')->tile == 600 && tg_charmap_find(&cm, 'C')->tile == 602 && tg_charmap_find(&cm, 'D')->tile == 8);
}

static void test_layer_load_failures(void)
{
    static uint8_t tiles[1100 * 256];
    static uint16_t map[16 * 8];
    static tg_char_t st[200], saved[200];
    tg_charmap_t cm;
    tg_layer_t l;
    reset_layer(&l, tiles, sizeof tiles, map, 3, 8);
    tg_charmap_init(&cm, st, 200);
    CHECK(tg_charmap_set_range(&cm, 1, 2, 7, 3));      /* something there already, which must stay */
    memcpy(saved, st, sizeof st);
    memset(tiles, 0xCD, sizeof tiles);
#define UNCHANGED()                                                                      \
    do {                                                                                 \
        CHECK(cm.count == 2 && memcmp(st, saved, sizeof st) == 0);                       \
        for (size_t b = 0; b < sizeof tiles; b++)                                        \
            CHECK(tiles[b] == 0xCD);                                                     \
    } while (0)
    CHECK(!tg_layer_load_font(&l, &cm, TG_FONT_COUNT, 32, 95, 0, 1, 0));            UNCHANGED();
    CHECK(!tg_layer_load_font(&l, &cm, (tg_font_id_t)99, 32, 95, 0, 1, 0));         UNCHANGED();
    CHECK(!tg_layer_load_font(&l, &cm, TG_FONT_UNSCII8, 32, 0, 0, 1, 0));           UNCHANGED();   /* no glyphs */
    CHECK(!tg_layer_load_font_ptr(&l, &cm, NULL, 32, 95, 0, 1, 0));                 UNCHANGED();
    CHECK(!tg_layer_load_font(&l, NULL, TG_FONT_UNSCII8, 32, 95, 0, 1, 0));         UNCHANGED();   /* no map */
    CHECK(!tg_layer_load_font(&l, &cm, TG_FONT_UNSCII8, 32, 25, 1000, 1, 0));       UNCHANGED();   /* 1000 + 25 > 1024 */
    CHECK(!tg_layer_load_font(&l, &cm, TG_FONT_UNSCII8_TALL, 32, 13, 1000, 1, 0));  UNCHANGED();   /* two tiles each: 1026 */
    CHECK(!tg_layer_load_font(&l, &cm, TG_FONT_UNSCII8, 32, 1, 1024, 1, 0));        UNCHANGED();
    CHECK(!tg_layer_load_font(&l, &cm, TG_FONT_UNSCII8, 32, 199, 0, 1, 0));         UNCHANGED();   /* 199 + 2 > 200 */
    CHECK(!tg_layer_load_glyphs(&l, (const uint8_t *)"01234567", 8, 0, 25, 1000, 1, 0)); UNCHANGED();
    /* A layer that text cannot go on. */
    tg_layer_t bad = l;
    bad.kind = TG_LAYER_BITMAP;
    CHECK(!tg_layer_load_font(&bad, &cm, TG_FONT_UNSCII8, 32, 95, 0, 1, 0));
    bad = l; bad.kind = TG_LAYER_OFF;
    CHECK(!tg_layer_load_font(&bad, &cm, TG_FONT_UNSCII8, 32, 95, 0, 1, 0));
    bad = l; bad.tiles = NULL;
    CHECK(!tg_layer_load_font(&bad, &cm, TG_FONT_UNSCII8, 32, 95, 0, 1, 0));
    bad = l; bad.tile_log2 = 5;
    CHECK(!tg_layer_load_font(&bad, &cm, TG_FONT_UNSCII8, 32, 95, 0, 1, 0));
    bad = l; bad.depth = 2;
    CHECK(!tg_layer_load_font(&bad, &cm, TG_FONT_UNSCII8, 32, 95, 0, 1, 0));
    UNCHANGED();
    CHECK(!tg_layer_load_glyphs(&l, NULL, 8, 0, 4, 0, 1, 0));                  UNCHANGED();
    CHECK(!tg_layer_load_glyphs(&l, (const uint8_t *)"01234567", 12, 0, 1, 0, 1, 0)); UNCHANGED();
    CHECK(!tg_layer_load_glyphs(&l, (const uint8_t *)"01234567", 8, 0, 0, 0, 1, 0));  UNCHANGED();
#undef UNCHANGED

    /* Only the characters the map does not have yet need room. */
    static tg_char_t sst[95];
    tg_charmap_t small;
    tg_charmap_init(&small, sst, 94);
    CHECK(!tg_layer_load_font(&l, &small, TG_FONT_UNSCII8, 32, 95, 0, 1, 0) && small.count == 0);
    CHECK(tg_charmap_set_range(&small, 32, 10, 0, 0));
    CHECK(!tg_layer_load_font(&l, &small, TG_FONT_UNSCII8, 32, 95, 0, 1, 0) && small.count == 10);   /* 85 new: 95 > 94 */
    small.capacity = 95;
    CHECK(tg_layer_load_font(&l, &small, TG_FONT_UNSCII8, 32, 95, 0, 1, 0) && small.count == 95);

    /* Exactly filling the tile numbers is allowed. */
    memset(tiles, 0, sizeof tiles);
    tg_charmap_init(&cm, st, 200);
    CHECK(tg_layer_load_font(&l, &cm, TG_FONT_UNSCII8, 32, 24, 1000, 1, 0));
    CHECK(tg_layer_load_font(&l, &cm, TG_FONT_UNSCII8_TALL, 32, 12, 1000, 1, 0));
    CHECK(tg_charmap_find(&cm, ' ')->tile == 1000 && tg_charmap_find(&cm, ' ')->lower == 1012);
    CHECK(tg_charmap_find(&cm, 32 + 11)->lower == 1023 && tg_charmap_find(&cm, 32 + 12)->lower == TG_NO_TILE);   /* (from the first load) */
    CHECK(tg_layer_load_font(&l, &cm, TG_FONT_UNSCII8, 32, 1, 1023, 1, 0));

    /* The text functions do nothing, and do not crash, on a layer that cannot take text, or without a map. */
    tg_layer_t none = {.kind = TG_LAYER_OFF};
    tg_layer_put_string(&none, &cm, 0, 0, "x", 0);
    tg_layer_put_utf8(&none, &cm, 0, 0, "x", 0);
    tg_layer_put_glyphs(&none, &cm, 0, 0, (const uint16_t[]){1}, 1, 0);
    tg_layer_t nomap = l;
    nomap.map = NULL;
    tg_layer_put_string(&nomap, &cm, 0, 0, "x", 0);
    for (size_t i = 0; i < 16 * 8; i++)
        map[i] = SENTINEL;
    tg_layer_put_string(&l, NULL, 0, 0, " ", 0);
    tg_layer_put_utf8(&l, NULL, 0, 0, " ", 0);
    tg_layer_put_glyphs(&l, NULL, 0, 0, (const uint16_t[]){32}, 1, 0);
    CHECK(map[0] == SENTINEL);
}

/* Your own glyph array: load by pointer, then say which characters the tiles are. */
static void test_layer_low_level(void)
{
    static const uint8_t mine[4 * 8] = {
        0xFF, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xFF,   /* a box */
        0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01,   /* a diagonal */
        0x01, 0x03, 0x07, 0x0F, 0x1F, 0x3F, 0x7F, 0xFF,   /* a wedge */
        0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55,   /* a checkerboard */
    };
    static tg_char_t st[16];
    tg_charmap_t cm;
    for (unsigned tl = 3; tl <= 4; tl++)
        for (unsigned depth = 4; depth <= 8; depth += 4) {
            const unsigned size = 1u << tl;
            static uint8_t tiles[300 * 256];
            static uint16_t map[16 * 8];
            tg_layer_t l;
            reset_layer(&l, tiles, sizeof tiles, map, tl, depth);
            tg_charmap_init(&cm, st, 16);
            CHECK(tg_layer_load_glyphs(&l, mine, 8, 0, 4, 10, 5, 2));
            tg_layer_put_string(&l, &cm, 1, 1, "ABCD", 0);
            CHECK(map[1 * 16 + 1] == SENTINEL);                                          /* loading glyphs enters nothing */
            CHECK(tg_charmap_set_range(&cm, 'A', 4, 10, 0));
            tg_layer_put_string(&l, &cm, 1, 1, "ABCD", 0);
            for (unsigned i = 0; i < 4; i++) {
                CHECK(map[1 * 16 + 1 + i] == TG_ENTRY(10 + i, 0, 0));
                for (unsigned y = 0; y < size; y++) {
                    uint8_t idx[TG_WIDTH];
                    layer_cell(&l, 1 + i, 1, y, idx);
                    for (unsigned x = 0; x < size; x++)
                        CHECK(idx[x] == expect_pixel(mine + 8 * i, tl, depth, x, y, 5, 2));
                }
            }
            /* A letter past the four has no tile until the map is given one; clearing the map takes them all out. */
            tg_layer_put_string(&l, &cm, 0, 3, "E", 0);
            CHECK(map[3 * 16] == SENTINEL);
            CHECK(tg_charmap_set(&cm, 'E', 13, TG_NO_TILE));
            tg_layer_put_string(&l, &cm, 0, 3, "E", 0);
            CHECK(map[3 * 16] == TG_ENTRY(13, 0, 0));
            tg_charmap_clear(&cm);
            tg_layer_put_string(&l, &cm, 0, 4, "AE", 0);
            CHECK(map[4 * 16] == SENTINEL && map[4 * 16 + 1] == SENTINEL);
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
    /* and the map for them: two characters two tiles tall */
    tg_charmap_init(&cm, st, 16);
    CHECK(tg_charmap_set_range(&cm, 'a', 2, 0, 2));
    tg_layer_put_string(&l, &cm, 5, 2, "ba", 0);
    CHECK(map[2 * 16 + 5] == TG_ENTRY(1, 0, 0) && map[3 * 16 + 5] == TG_ENTRY(3, 0, 0));
    CHECK(map[2 * 16 + 6] == TG_ENTRY(0, 0, 0) && map[3 * 16 + 6] == TG_ENTRY(2, 0, 0));
}

static void test_layer_utf8_and_glyphs(void)
{
    static uint8_t tiles[600 * 64];
    static uint16_t map[16 * 8];
    static tg_char_t st[600];
    tg_charmap_t cm;
    tg_layer_t l;
    reset_layer(&l, tiles, sizeof tiles, map, 3, 8);
    tg_charmap_init(&cm, st, 600);

    /* The decoder on its own. */
    {
        const char *p = "A\xC3\xA9\xE2\x94\x80\xF0\x9F\x98\x80\xFF";
        CHECK(tg_utf8_next(&p) == 'A' && tg_utf8_next(&p) == 0xE9 && tg_utf8_next(&p) == 0x2500 && tg_utf8_next(&p) == 0x1F600);
        CHECK(tg_utf8_next(&p) == TG_BAD_UTF8 && *p == 0);
    }

    /* Box drawing from UTF-8 source text. */
    CHECK(tg_layer_load_font(&l, &cm, TG_FONT_UNSCII8, 0x2500, 128, 1, 1, 0));
#define BOX(cp) TG_ENTRY(1 + ((cp) - 0x2500), 0, 0)
    tg_layer_put_utf8(&l, &cm, 0, 0, "\xE2\x94\x80\xE2\x94\x82\xE2\x94\x8C", 0);               /* U+2500 U+2502 U+250C */
    CHECK(map[0] == BOX(0x2500) && map[1] == BOX(0x2502) && map[2] == BOX(0x250C) && map[3] == SENTINEL);
    /* Not in the map: skipped. A byte string is characters byte by byte, so it is not decoded. */
    for (size_t i = 0; i < 16 * 8; i++)
        map[i] = SENTINEL;
    tg_layer_put_utf8(&l, &cm, 0, 0, "a\xE2\x94\x80", 0);
    CHECK(map[0] == SENTINEL && map[1] == BOX(0x2500));
    tg_layer_put_string(&l, &cm, 0, 1, "\xE2\x94\x80", 0);
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
        tg_layer_put_utf8(&l, &cm, 0, 0, text, 0);
        for (int k = 0; k < bad[i].bad_bytes; k++)
            CHECK(map[k] == SENTINEL);
        CHECK(map[bad[i].bad_bytes] == BOX(0x2500));
    }

    /* A real U+FFFD is a character like any other, not a marker for bad bytes. */
    for (size_t k = 0; k < 16 * 8; k++)
        map[k] = SENTINEL;
    CHECK(tg_charmap_set(&cm, 0xFFFD, 5, TG_NO_TILE));
    tg_layer_put_utf8(&l, &cm, 0, 0, "\xEF\xBF\xBD\xEF\xBF\xBD\xFF", 0);
    CHECK(map[0] == TG_ENTRY(5, 0, 0) && map[1] == TG_ENTRY(5, 0, 0) && map[2] == SENTINEL);

    /* Mixed scripts: every set loaded is in the one map, so one put draws the whole string. */
    reset_layer(&l, tiles, sizeof tiles, map, 3, 8);
    tg_charmap_init(&cm, st, 600);
    const char *mixed = "Caf\xC3\xA9 \xC2\xA9";
    CHECK(tg_layer_load_font(&l, &cm, TG_FONT_UNSCII8, 0xA0, 96, 1, 3, 0));        /* Latin-1 */
    CHECK(tg_layer_load_font(&l, &cm, TG_FONT_UNSCII8, 0x20, 95, 200, 1, 0));      /* ASCII, in other tiles */
    tg_layer_put_utf8(&l, &cm, 0, 0, mixed, 0);
    CHECK(map[0] == TG_ENTRY(200 + 'C' - 0x20, 0, 0) && map[2] == TG_ENTRY(200 + 'f' - 0x20, 0, 0));
    CHECK(map[3] == TG_ENTRY(1 + 0xE9 - 0xA0, 0, 0) && map[4] == TG_ENTRY(200, 0, 0) && map[5] == TG_ENTRY(1 + 0xA9 - 0xA0, 0, 0));
    CHECK(map[6] == SENTINEL);

    /* 16-bit codepoint lists, including 0 and numbers over 255, with a font two tiles tall (and wrapping at the bottom). */
    static uint8_t ttiles[300 * 256];
    tg_layer_t t;
    reset_layer(&t, ttiles, sizeof ttiles, map, 4, 8);
    tg_charmap_init(&cm, st, 600);
    CHECK(tg_layer_load_font(&t, &cm, TG_FONT_UNSCII16, 0x2580, 32, 10, 1, 0));
    const uint16_t list[] = {0x2580, 0, 0x2588, 0xFFFF};
    tg_layer_put_glyphs(&t, &cm, 14, 7, list, 4, 0);                             /* x wraps; the lower halves wrap to row 0 */
    CHECK(map[7 * 16 + 14] == TG_ENTRY(10, 0, 0) && map[0 * 16 + 14] == TG_ENTRY(10 + 32, 0, 0));
    CHECK(map[7 * 16 + 15] == SENTINEL && map[0 * 16 + 15] == SENTINEL);         /* 0: no tile */
    CHECK(map[7 * 16 + 0] == TG_ENTRY(10 + 8, 0, 0) && map[0 * 16 + 0] == TG_ENTRY(10 + 8 + 32, 0, 0));
    CHECK(map[7 * 16 + 1] == SENTINEL);
    tg_layer_put_string(&t, &cm, -1, -1, "x", 0);                                /* 'x' has no tile: nothing, but no crash */
}

/* ---- tiles for only the characters a program shows ---- */

/* Tile t of the array is the glyph drawn in fg on bg, as the compositor shows it. */
static int tile_shows(const uint8_t *tiles, unsigned tl, unsigned depth, unsigned t, const uint8_t *glyph, uint8_t fg, uint8_t bg)
{
    static uint16_t one[1];
    one[0] = TG_ENTRY(t, 0, 0);
    for (unsigned y = 0; y < (1u << tl); y++) {
        uint8_t idx[TG_WIDTH];
        render_cell(one, 0, 0, (uint8_t *)tiles, tl, depth, 0, 0, y, idx);
        for (unsigned x = 0; x < (1u << tl); x++)
            if (idx[x] != expect_pixel(glyph, tl, depth, x, y, fg, bg))
                return 0;
    }
    return 1;
}

static int blank_tile(const uint8_t *tiles, size_t bytes, unsigned t)
{
    for (size_t i = 0; i < bytes; i++)
        if (tiles[t * bytes + i])
            return 0;
    return 1;
}

static unsigned tile_of(const tg_charmap_t *cm, uint32_t cp)
{
    const tg_char_t *c = tg_charmap_find(cm, cp);
    return c ? c->tile : 9999;
}

/* A codepoint the font has no glyph for, and its UTF-8. */
static uint32_t lacking(const tg_font_t *font, char *utf8)
{
    uint32_t cp = 0x300;
    while (tg_font_glyph(font, cp))
        cp++;
    utf8[0] = (char)(0xC0 | (cp >> 6));
    utf8[1] = (char)(0x80 | (cp & 0x3F));
    utf8[2] = 0;
    return cp;
}

static void test_add_chars(void)
{
    static uint8_t tiles[256 * 256], before_tiles[256 * 256];
    static tg_char_t st[300], before_st[300];
    static uint8_t keep[1024];
    tg_charmap_t cm;
    const tg_font_t *f = &tg_font_unscii8;
    memset(tiles, 0, sizeof tiles);
    tg_charmap_init(&cm, st, 300);

    /* Ten digits: the ten free tiles at the top, rising. */
    CHECK(tg_font_add_chars(&cm, f, "0123456789", tiles, 3, 8, 1, 255, NULL, 7, 0) == 10);
    for (int d = 0; d < 10; d++) {
        const tg_char_t *c = tg_charmap_find(&cm, (uint32_t)('0' + d));
        CHECK(c && c->tile == 246 + d && c->lower == TG_NO_TILE);
        CHECK(tile_shows(tiles, 3, 8, c->tile, tg_font_glyph(f, (uint32_t)('0' + d)), 7, 0));
    }
    /* Only what is new, in the order of the string. */
    CHECK(tg_font_add_chars(&cm, f, "SCORE 0", tiles, 3, 8, 1, 255, NULL, 7, 0) == 6);
    CHECK(tile_of(&cm, 'S') == 240 && tile_of(&cm, 'C') == 241 && tile_of(&cm, 'O') == 242 && tile_of(&cm, 'R') == 243);
    CHECK(tile_of(&cm, 'E') == 244 && tile_of(&cm, ' ') == 245 && tile_of(&cm, '0') == 246);
    /* The space is blank, but the map names it, so it is not free. */
    CHECK(blank_tile(tiles, 64, 245));
    CHECK(tg_font_add_chars(&cm, f, "X", tiles, 3, 8, 1, 255, NULL, 7, 0) == 1 && tile_of(&cm, 'X') == 239);
    /* A tile with something in it is not free, nor one the program keeps. */
    tiles[238 * 64 + 10] = 1;
    CHECK(tg_font_add_chars(&cm, f, "Y", tiles, 3, 8, 1, 255, NULL, 7, 0) == 1 && tile_of(&cm, 'Y') == 237);
    keep[236] = 1;
    CHECK(tg_font_add_chars(&cm, f, "Z", tiles, 3, 8, 1, 255, keep, 7, 0) == 1 && tile_of(&cm, 'Z') == 235);
    keep[236] = 0;
    /* Again, and twice in one string: nothing new, or once (in 236, which is free now that it is not kept). */
    CHECK(tg_font_add_chars(&cm, f, "ZZ00", tiles, 3, 8, 1, 255, NULL, 7, 0) == 0);
    CHECK(tg_font_add_chars(&cm, f, "QQQ", tiles, 3, 8, 1, 255, NULL, 7, 0) == 1 && tile_of(&cm, 'Q') == 236);
    /* Characters the font lacks are passed over; bad UTF-8 too. */
    char none[4];
    const uint32_t lack = lacking(f, none);
    char text[16];
    snprintf(text, sizeof text, "%s\xFFW", none);
    CHECK(tg_font_add_chars(&cm, f, text, tiles, 3, 8, 1, 255, NULL, 7, 0) == 1 && tile_of(&cm, 'W') == 234 && !tg_charmap_find(&cm, lack));
    /* UTF-8: é and ─, rising in the order of the string. */
    CHECK(tg_font_add_chars(&cm, f, "\xC3\xA9\xE2\x94\x80", tiles, 3, 8, 1, 255, NULL, 7, 0) == 2);
    CHECK(tile_of(&cm, 0xE9) == 232 && tile_of(&cm, 0x2500) == 233 && tile_shows(tiles, 3, 8, 233, tg_font_glyph(f, 0x2500), 7, 0));
    CHECK(tg_font_add_chars(&cm, f, "", tiles, 3, 8, 1, 255, NULL, 7, 0) == 0);

    /* Not enough free tiles, or no room in the map: nothing changes. */
    memcpy(before_tiles, tiles, sizeof tiles);
    memcpy(before_st, st, sizeof st);
    const unsigned count = cm.count;
#define SAME() CHECK(cm.count == count && memcmp(st, before_st, sizeof st) == 0 && memcmp(tiles, before_tiles, sizeof tiles) == 0)
    CHECK(tg_font_add_chars(&cm, f, "abcd", tiles, 3, 8, 1, 3, NULL, 7, 0) == -1);   SAME();
    cm.capacity = (uint16_t)(count + 1);
    CHECK(tg_font_add_chars(&cm, f, "de", tiles, 3, 8, 1, 255, NULL, 7, 0) == -1);  SAME();
    cm.capacity = 300;
    /* Bad arguments. */
    CHECK(tg_font_add_chars(NULL, f, "a", tiles, 3, 8, 1, 255, NULL, 7, 0) == -1);
    CHECK(tg_font_add_chars(&cm, NULL, "a", tiles, 3, 8, 1, 255, NULL, 7, 0) == -1);
    CHECK(tg_font_add_chars(&cm, f, NULL, tiles, 3, 8, 1, 255, NULL, 7, 0) == -1);
    CHECK(tg_font_add_chars(&cm, f, "a", NULL, 3, 8, 1, 255, NULL, 7, 0) == -1);
    CHECK(tg_font_add_chars(&cm, f, "a", tiles, 5, 8, 1, 255, NULL, 7, 0) == -1);
    CHECK(tg_font_add_chars(&cm, f, "a", tiles, 3, 2, 1, 255, NULL, 7, 0) == -1);
    CHECK(tg_font_add_chars(&cm, f, "a", tiles, 3, 8, 1, 1024, NULL, 7, 0) == -1);
    CHECK(tg_font_add_chars(&cm, f, "a", tiles, 3, 8, 9, 8, NULL, 7, 0) == -1);
    SAME();
#undef SAME
    /* Exactly enough, down to `lowest`. */
    CHECK(tg_font_add_chars(&cm, f, "abc", tiles, 3, 8, 1, 3, NULL, 7, 0) == 3);
    CHECK(tile_of(&cm, 'a') == 1 && tile_of(&cm, 'b') == 2 && tile_of(&cm, 'c') == 3);
    CHECK(tg_font_add_chars(&cm, f, "d", tiles, 3, 8, 0, 0, NULL, 7, 0) == 1 && tile_of(&cm, 'd') == 0);   /* tile 0 only if asked */

    /* A tall font: two tiles a character, the top half first; a put writes both rows. */
    memset(tiles, 0, sizeof tiles);
    tg_charmap_init(&cm, st, 300);
    CHECK(tg_font_add_chars(&cm, &tg_font_unscii16, "AB", tiles, 3, 8, 1, 255, NULL, 7, 0) == 2);
    CHECK(tile_of(&cm, 'A') == 252 && tg_charmap_find(&cm, 'A')->lower == 253);
    CHECK(tile_of(&cm, 'B') == 254 && tg_charmap_find(&cm, 'B')->lower == 255);
    CHECK(tile_shows(tiles, 3, 8, 252, tg_font_glyph(&tg_font_unscii16, 'A'), 7, 0));
    CHECK(tile_shows(tiles, 3, 8, 253, tg_font_glyph(&tg_font_unscii16, 'A') + 8, 7, 0));
    CHECK(tg_font_add_chars(&cm, &tg_font_unscii16, "CDE", tiles, 3, 8, 250, 255, NULL, 7, 0) == -1);   /* six tiles, two free */

    /* Each tile size and depth, and a layer. */
    for (unsigned tl = 3; tl <= 4; tl++)
        for (unsigned depth = 4; depth <= 8; depth += 4) {
            memset(tiles, 0, sizeof tiles);
            tg_charmap_init(&cm, st, 300);
            static uint16_t lmap[16 * 8];
            tg_layer_t l;
            reset_layer(&l, tiles, (size_t)256 * tg_tile_bytes(tl, depth), lmap, tl, depth);
            CHECK(tg_layer_add_chars(&l, &cm, f, "Hi!", 1, 200, NULL, 5, 2) == 3);
            CHECK(tile_of(&cm, 'H') == 198 && tile_of(&cm, 'i') == 199 && tile_of(&cm, '!') == 200);
            CHECK(tile_shows(tiles, tl, depth, 198, tg_font_glyph(f, 'H'), 5, 2) && tile_shows(tiles, tl, depth, 200, tg_font_glyph(f, '!'), 5, 2));
            tg_layer_put_string(&l, &cm, 0, 0, "iH", 3);
            CHECK(lmap[0] == TG_ENTRY(199, 3, 0) && lmap[1] == TG_ENTRY(198, 3, 0));
            tg_layer_t bad = l;
            bad.kind = TG_LAYER_OFF;
            CHECK(tg_layer_add_chars(&bad, &cm, f, "j", 1, 200, NULL, 5, 2) == -1 && !tg_charmap_find(&cm, 'j'));
        }

    /* Random tiles, maps, kept tiles and strings, against what the rules say should happen. */
    const char *const pool[] = {"A", "B", "C", "D", "E", "F", "0", "1", "2", " ", "\xC3\xA9", "\xE2\x94\x80", "\xFF", none};
    for (int round = 0; round < 3000; round++) {
        const tg_font_t *font = rnd() % 4 ? f : &tg_font_unscii16;
        const unsigned rows = font->height / 8, ntiles = 48;
        memset(tiles, 0, (size_t)ntiles * 64);
        for (unsigned t = 0; t < ntiles; t++)
            if (rnd() % 3 == 0)
                tiles[t * 64 + rnd() % 64] = (uint8_t)(1 + rnd() % 200);
        for (unsigned t = 0; t < ntiles; t++)
            keep[t] = rnd() % 8 == 0;
        tg_charmap_init(&cm, st, 1 + rnd() % 20);
        for (unsigned k = rnd() % 4; k > 0; k--)
            tg_charmap_set(&cm, 'A' + rnd() % 6, rnd() % ntiles, rnd() % 2 ? TG_NO_TILE : rnd() % ntiles);
        char s[64] = "";
        for (unsigned k = rnd() % 10; k > 0; k--)
            strcat(s, pool[rnd() % (sizeof pool / sizeof *pool)]);
        const unsigned hi = rnd() % ntiles, lo = rnd() % (hi + 1);
        memcpy(before_tiles, tiles, (size_t)ntiles * 64);
        memcpy(before_st, st, sizeof st);
        const unsigned old_count = cm.count;

        /* What should happen: the new characters, and the free tiles. */
        uint32_t want_cp[32];
        unsigned nwant = 0;
        for (const char *p = s; *p;) {
            const uint32_t cp = tg_utf8_next(&p);
            if (cp == TG_BAD_UTF8 || !tg_font_glyph(font, cp) || tg_charmap_find(&cm, cp))
                continue;
            int dup = 0;
            for (unsigned i = 0; i < nwant; i++)
                dup |= want_cp[i] == cp;
            if (!dup)
                want_cp[nwant++] = cp;
        }
        int is_free[48] = {0};
        unsigned nfree = 0;
        for (unsigned t = lo; t <= hi; t++) {
            int named = 0;
            for (unsigned i = 0; i < cm.count; i++)
                named |= st[i].tile == t || st[i].lower == t;
            is_free[t] = blank_tile(tiles, 64, t) && !keep[t] && !named;
            nfree += is_free[t];
        }
        const int expect = nwant == 0 ? 0 : (nfree < nwant * rows || old_count + nwant > cm.capacity) ? -1 : (int)nwant;

        const int r = tg_font_add_chars(&cm, font, s, tiles, 3, 8, lo, hi, keep, 9, 0);
        CHECK(r == expect);
        if (r <= 0) {
            CHECK(cm.count == old_count && memcmp(st, before_st, sizeof st) == 0 && memcmp(tiles, before_tiles, (size_t)ntiles * 64) == 0);
            continue;
        }
        CHECK(cm.count == old_count + nwant && sorted(&cm));
        for (unsigned i = 0; i < old_count; i++) {          /* what was there is still there */
            const tg_char_t *e = tg_charmap_find(&cm, before_st[i].codepoint);
            CHECK(e && e->tile == before_st[i].tile && e->lower == before_st[i].lower);
        }
        /* The new ones take the free tiles nearest hi, rising in the order of the string, top half first. */
        unsigned chosen[64], nch = 0;
        for (unsigned t = hi + 1; t-- > lo && nch < nwant * rows;)
            if (is_free[t])
                chosen[nch++] = t;
        for (unsigned i = 0; i < nch / 2; i++) {             /* (to rising order) */
            const unsigned x = chosen[i];
            chosen[i] = chosen[nch - 1 - i];
            chosen[nch - 1 - i] = x;
        }
        int used[48] = {0};
        for (unsigned i = 0; i < nwant; i++) {
            const tg_char_t *e = tg_charmap_find(&cm, want_cp[i]);
            const uint8_t *g = tg_font_glyph(font, want_cp[i]);
            CHECK(e && e->tile == chosen[i * rows] && e->lower == (rows == 2 ? chosen[i * rows + 1] : TG_NO_TILE));
            CHECK(tile_shows(tiles, 3, 8, e->tile, g, 9, 0));
            if (rows == 2)
                CHECK(tile_shows(tiles, 3, 8, e->lower, g + 8, 9, 0));
            used[e->tile] = 1;
            if (rows == 2)
                used[e->lower] = 1;
        }
        for (unsigned t = 0; t < ntiles; t++)                /* every other tile is as it was */
            if (!used[t])
                CHECK(memcmp(tiles + t * 64, before_tiles + t * 64, 64) == 0);
    }
}

static void test_layer_text(void)
{
    test_font_numbers();
    test_layer_load_font();
    test_layer_load_failures();
    test_layer_low_level();
    test_layer_utf8_and_glyphs();
    test_add_chars();
}

int main(void)
{
    test_structure();
    test_against_source();
    test_tile_from_glyph();
    test_range_and_map();
    test_charmap();
    test_map_put();
    test_layer_text();
    printf("all %d checks passed\n", checks);
    return 0;
}
