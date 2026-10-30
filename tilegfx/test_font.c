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

/* Text put into a map by tg_text_put shows up on screen as the glyphs of its characters. */
static void check_text(const tg_font_t *font, unsigned tl, unsigned depth, uint8_t fg, uint8_t bg, unsigned pal)
{
    const unsigned per = font->height / 8, size = 1u << tl;
    static uint8_t tiles[(96 * 2 + 4) * 256];
    static uint16_t map[16 * 8];
    memset(map, 0, sizeof map);
    memset(tiles, 0, sizeof tiles);   /* tile 0 must be blank, whatever layout the array held before */
    tg_charset_t cs = tg_font_load(font, tiles, tl, depth, 1, 0x20, 96, fg, bg);   /* tile 0 stays blank and unused */
    CHECK(cs.tiles_per_char == per && cs.first_tile == 1 && cs.count == 96);

    const char *text = "Hi! 0x7F\xE9\xC3";                  /* the last two are not valid UTF-8: skipped, the cell stays as it was */
    tg_text_put(map, 4, 3, 14, 2, text, &cs, pal);          /* starts near the right edge: wraps to the left */

    static tg_t g;
    tg_init(&g);
    g.layers[0] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = (uint8_t)tl, .depth = (uint8_t)depth,
                               .map_w_log2 = 4, .map_h_log2 = 3, .map = map, .tiles = tiles};
    const unsigned n = (unsigned)strlen(text);
    for (unsigned ch = 0; ch < n; ch++) {
        const unsigned code = (uint8_t)text[ch];
        const int skipped = code < 0x20 || code >= 0x20 + 96;   /* outside the charset: the cell was never written */
        const uint8_t *glyph = tg_font_glyph(font, skipped ? 0x20 : code);
        const unsigned mx = (14 + ch) & 15;                   /* map column, wrapped */
        for (unsigned half = 0; half < per; half++)
            for (unsigned y = 0; y < size; y++) {
                const unsigned map_row = (2 + half) & 7;
                uint8_t idx[TG_WIDTH];
                /* Scroll so that the map cell (mx, map_row) is at the top left of the screen. */
                g.layers[0].scroll_x = mx * size;
                g.layers[0].scroll_y = map_row * size;
                tg_render_line_indices(&g, (int)y, idx);
                for (unsigned x = 0; x < size; x++) {
                    uint8_t want = skipped ? 0 : expect_pixel(glyph + 8 * half, tl, depth, x, y, fg, bg);
                    if (depth == 4 && want)
                        want |= (uint8_t)(pal << 4);          /* the entry's sub-palette */
                    CHECK(idx[x] == want);
                }
            }
    }
    /* Cells that were not written are still empty (tile 0, blank). */
    uint8_t idx[TG_WIDTH];
    g.layers[0].scroll_x = 5 * size;
    g.layers[0].scroll_y = 6 * size;
    tg_render_line_indices(&g, 0, idx);
    for (unsigned x = 0; x < size; x++)
        CHECK(idx[x] == 0);
}

static void test_text(void)
{
    for (int f = 0; f < NFONTS; f++)
        for (unsigned tl = 3; tl <= 4; tl++)
            for (unsigned depth = 4; depth <= 8; depth += 4) {
                check_text(FONTS[f], tl, depth, 7, 0, 3);       /* transparent background */
                check_text(FONTS[f], tl, depth, 9, 2, 1);       /* opaque background */
            }

    /* Wrapping vertically, for an 8x16 font whose characters take two rows. */
    static uint8_t tiles[(96 * 2 + 1) * 64];
    static uint16_t map[4 * 4];
    memset(map, 0, sizeof map);
    tg_charset_t cs = tg_font_load(&tg_font_unscii16, tiles, 3, 8, 1, 0x20, 96, 1, 0);
    tg_text_put(map, 2, 2, 0, 3, "A", &cs, 0);                  /* top half in row 3, bottom half wraps to row 0 */
    CHECK(map[3 * 4] == TG_ENTRY(1 + ('A' - 0x20) * 2, 0, 0));
    CHECK(map[0] == TG_ENTRY(1 + ('A' - 0x20) * 2 + 1, 0, 0));
    /* Negative positions wrap too. */
    memset(map, 0, sizeof map);
    tg_text_put(map, 2, 2, -1, 0, "BC", &cs, 0);
    CHECK(map[3] == TG_ENTRY(1 + ('B' - 0x20) * 2, 0, 0) && map[0] == TG_ENTRY(1 + ('C' - 0x20) * 2, 0, 0));

    /* UTF-8: 2, 3 and 4 byte sequences are decoded; ill-formed ones are one first-character each. */
    static uint8_t tiles2[(128 + 1) * 64];
    static uint16_t map2[8 * 2];
    tg_charset_t box = tg_font_load(&tg_font_unscii8, tiles2, 3, 8, 1, 0x2500, 128, 1, 0);   /* box drawing */
#define BOXTILE(cp) TG_ENTRY(1 + ((cp) - 0x2500), 0, 0)
    memset(map2, 0, sizeof map2);
    tg_text_put(map2, 3, 1, 0, 0, "\xE2\x94\x80\xE2\x94\x82\xE2\x94\x8C", &box, 0);   /* U+2500 U+2502 U+250C */
    CHECK(map2[0] == BOXTILE(0x2500) && map2[1] == BOXTILE(0x2502) && map2[2] == BOXTILE(0x250C));
    CHECK(map2[3] == 0);                                                                      /* nothing after the string */
    memset(map2, 0, sizeof map2);
    tg_text_put(map2, 3, 1, 0, 0, "a\xE2\x94\x80", &box, 0);                  /* "a" is not in this charset: skipped, the next one lands after it */
    CHECK(map2[0] == 0 && map2[1] == BOXTILE(0x2500));
    memset(map2, 0xFF, sizeof map2);                                          /* skipped cells keep whatever they held */
    tg_text_put(map2, 3, 1, 0, 0, "a\xE2\x94\x80", &box, 0);
    CHECK(map2[0] == 0xFFFF && map2[1] == BOXTILE(0x2500) && map2[2] == 0xFFFF);
    /* Ill-formed UTF-8 is skipped one byte at a time, so the box character after it lands at the right place. */
    static const struct { const char *text; int skipped_bytes; } bad[] = {
        {"\xC0\x80", 2},          /* overlong NUL: both bytes are bad */
        {"\xE0\x80\x80", 3},      /* overlong */
        {"\xED\xA0\x80", 3},      /* a surrogate */
        {"\xF4\x90\x80\x80", 4},  /* above U+10FFFF */
        {"\xE2\x94", 2},          /* cut short by the end of the string (nothing follows to land) */
        {"\xE2\x94\x41", 3},      /* cut short by an ASCII byte: 2 bad bytes, then "A", which this charset lacks */
        {"\x80", 1},              /* a stray continuation byte */
        {"\xFF", 1},
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        char text[16];
        snprintf(text, sizeof text, "%s\xE2\x94\x80", bad[i].text);
        memset(map2, 0, sizeof map2);
        tg_text_put(map2, 3, 1, 0, 0, text, &box, 0);
        for (int k = 0; k < bad[i].skipped_bytes; k++)
            CHECK(map2[k] == 0);
        if (i != 4)   /* the case cut short by the end of the string is followed by a valid character, which is fine too */
            CHECK(map2[bad[i].skipped_bytes] == BOXTILE(0x2500));
    }
    /* A 4-byte sequence (U+1FB00, a legacy-computing symbol) decodes, and a font that has it finds it. */
    tg_charset_t lc = tg_font_load(&tg_font_unscii8, tiles2, 3, 8, 1, 0x1FB00, 16, 1, 0);
    memset(map2, 0, sizeof map2);
    tg_text_put(map2, 3, 1, 0, 0, "\xF0\x9F\xAC\x80\xF0\x9F\xAC\x83", &lc, 0);           /* U+1FB00 U+1FB03 */
    CHECK(map2[0] == TG_ENTRY(1, 0, 0) && map2[1] == TG_ENTRY(1 + 3, 0, 0) && map2[2] == 0);
    CHECK(tg_font_glyph(&tg_font_unscii8, 0x1FB00) != NULL);

    /* A character the font lacks becomes a blank tile. */
    tg_charset_t odd = tg_font_load(&tg_font_unscii8, tiles, 3, 8, 0, 0x378, 4, 5, 6);
    for (size_t i = 0; i < 4 * 64; i++)
        CHECK(tiles[i] == 6);                                     /* all background */
    (void)odd;
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

    /* The same tiles as tg_font_load makes for a one-tile-per-character font. */
    {
        static uint8_t a[110 * 256], b[110 * 256];
        memset(a, 0, sizeof a);
        memset(b, 0, sizeof b);
        for (unsigned tl = 3; tl <= 4; tl++)
            for (unsigned depth = 4; depth <= 8; depth += 4) {
                tg_font_load_range(&tg_font_unscii8_mcr, a, tl, depth, 7, 0x20, 95, 0, 5, 2);
                tg_font_load(&tg_font_unscii8_mcr, b, tl, depth, 7, 0x20, 95, 5, 2);
                CHECK(memcmp(a, b, 102 * tg_tile_bytes(tl, depth)) == 0);
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

int main(void)
{
    test_structure();
    test_against_source();
    test_tile_from_glyph();
    test_text();
    test_range_and_offset();
    test_map_put();
    printf("all %d checks passed\n", checks);
    return 0;
}
