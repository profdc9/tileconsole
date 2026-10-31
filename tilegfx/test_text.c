/* SPDX-License-Identifier: MIT */
/* Tests of text mode: the line renderer against a pixel-by-pixel reference, the helpers, the PC character set. Exits non-zero on the first failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tg_font.h"
#include "tg_fonts.h"
#include "tg_text.h"
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

static uint32_t rng = 7;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

/* The colour of one pixel of the 640 x 480 picture, worked out the plain way: which cell, which glyph row, which bit. */
static uint16_t ref_pixel(const tg_text_t *t, int x, int y)
{
    if (!t)
        return 0;
    const int ty = y - 40;
    if (ty < 0 || ty >= 400)
        return 0;
    const int col = x / 8, row = ty / 16;
    const uint16_t cell = t->cells[row * 80 + col];
    const int ch = cell & 255, fg = (cell >> 8) & 15, bg = (cell >> 12) & 15;
    const int bit = (t->font[ch * 16 + ty % 16] >> (7 - x % 8)) & 1;
    return t->palette[bit ? fg : bg];
}

static void check_line(tg_t *g, int line)
{
    uint32_t out[TG_WIDTH];
    memset(out, 0xA5, sizeof out);
    tg_render_display_line(g, line, out);
    for (int x = 0; x < 640; x++) {
        const uint16_t got = (uint16_t)(out[x / 2] >> (16 * (x & 1)));   /* the left pixel is the low half */
        CHECK(got == ref_pixel(g->text, x, line));
    }
}

static tg_text_t screen;

static void test_render_random(void)
{
    for (int round = 0; round < 60; round++) {
        for (int i = 0; i < TG_TEXT_CELLS; i++)
            screen.cells[i] = (uint16_t)rnd();
        for (size_t i = 0; i < sizeof screen.font; i++)
            screen.font[i] = (uint8_t)rnd();
        for (int i = 0; i < 16; i++)
            screen.palette[i] = (uint16_t)rnd();
        tg_t g;
        tg_init(&g);
        g.mode = TG_MODE_TEXT;
        g.text = &screen;
        for (int line = 0; line < 480; line++)
            check_line(&g, line);
    }
}

static void test_render_edges(void)
{
    tg_t g;
    tg_init(&g);
    g.mode = TG_MODE_TEXT;
    g.text = NULL;                                  /* no text: a black picture, not a crash */
    for (int line = 0; line < 480; line += 37)
        check_line(&g, line);
    for (int i = 0; i < TG_TEXT_CELLS; i++)
        screen.cells[i] = 0xFFFF;                   /* white on white everywhere, so that any pixel outside the text shows */
    for (int i = 0; i < 16; i++)
        screen.palette[i] = 0xFFFF;
    memset(screen.font, 0xFF, sizeof screen.font);
    g.text = &screen;
    for (int line = 0; line < 480; line++)
        check_line(&g, line);                       /* black above line 40 and from line 440, white between */
    uint32_t out[TG_WIDTH];
    tg_render_display_line(&g, 39, out);
    CHECK(out[0] == 0 && out[319] == 0);
    tg_render_display_line(&g, 40, out);
    CHECK(out[0] == 0xFFFFFFFFu && out[319] == 0xFFFFFFFFu);
    tg_render_display_line(&g, 439, out);
    CHECK(out[0] == 0xFFFFFFFFu && out[319] == 0xFFFFFFFFu);
    tg_render_display_line(&g, 440, out);
    CHECK(out[0] == 0 && out[319] == 0);
}

static void test_modes(void)
{
    tg_t g;
    tg_init(&g);
    CHECK(g.mode == TG_MODE_TILES && g.text == NULL);
    CHECK(tg_display_lines(&g) == 240 && tg_display_line_shift(&g) == 1);
    g.palette[0] = 0x1234;
    g.palette[7] = 0xBEEF;
    static uint8_t bitmap[TG_WIDTH * TG_HEIGHT];
    memset(bitmap, 7, sizeof bitmap);
    g.layers[0] = (tg_layer_t){.kind = TG_LAYER_BITMAP, .bitmap = bitmap};
    uint32_t a[TG_WIDTH], b[TG_WIDTH];
    tg_render_display_line(&g, 100, a);             /* tile mode: the same as tg_render_line */
    tg_render_line(&g, 100, b);
    CHECK(memcmp(a, b, sizeof a) == 0 && a[5] == 0xBEEFBEEFu);
    g.mode = TG_MODE_TEXT;
    CHECK(tg_display_lines(&g) == 480 && tg_display_line_shift(&g) == 0);
    CHECK(TG_TEXT_TOP == 40 && 480 >> 0 == 480 && 240 << 1 == 480);
}

static void test_helpers(void)
{
    tg_text_t t;
    memset(&t, 0, sizeof t);
    tg_text_init(&t);
    for (int i = 0; i < TG_TEXT_CELLS; i++)
        CHECK(t.cells[i] == 0x0720);                 /* a space, light grey on black */
    static const uint16_t pal[16] = {0x0000, 0x0015, 0x0540, 0x0555, 0xA800, 0xA815, 0xAAA0, 0xAD55,
                                     0x52AA, 0x52BF, 0x57EA, 0x57FF, 0xFAAA, 0xFABF, 0xFFEA, 0xFFFF};
    CHECK(memcmp(t.palette, pal, sizeof pal) == 0);

    tg_text_put_cell(&t, 3, 4, 'x', TG_TEXT_ATTR(TG_TEXT_YELLOW, TG_TEXT_BLUE));
    CHECK(t.cells[4 * 80 + 3] == ('x' | (0x1E << 8)));
    tg_text_put_cell(&t, -1, 0, 'a', 1);             /* outside the screen: nothing changes */
    tg_text_put_cell(&t, 80, 0, 'a', 1);
    tg_text_put_cell(&t, 0, -1, 'a', 1);
    tg_text_put_cell(&t, 0, 25, 'a', 1);
    for (int i = 0; i < TG_TEXT_CELLS; i++)
        CHECK(i == 4 * 80 + 3 || t.cells[i] == 0x0720);

    tg_text_clear(&t, 0x4F);
    for (int i = 0; i < TG_TEXT_CELLS; i++)
        CHECK(t.cells[i] == 0x4F20);

    tg_text_put_string(&t, 76, 24, "WRAPPED", 0x1F);   /* cut off at the right edge, and not on the row below (there is none here) */
    CHECK(t.cells[24 * 80 + 75] == 0x4F20 && t.cells[24 * 80 + 76] == 0x1F00 + 'W' && t.cells[24 * 80 + 79] == 0x1F00 + 'P');
    tg_text_put_string(&t, 78, 3, "abcd", 0x07);      /* not onto the next row either */
    CHECK(t.cells[3 * 80 + 79] == 0x0700 + 'b' && t.cells[4 * 80] == 0x4F20);
    tg_text_put_string(&t, -2, 5, "abcd", 0x07);      /* the part left of the screen is skipped, the rest is in place */
    CHECK(t.cells[5 * 80] == 0x0700 + 'c' && t.cells[5 * 80 + 1] == 0x0700 + 'd' && t.cells[5 * 80 + 2] == 0x4F20);
    tg_text_put_string(&t, 0, -1, "nothing", 0x07);
    tg_text_put_string(&t, 0, 25, "nothing", 0x07);
    CHECK(t.cells[0] == 0x4F20);
    tg_text_put_string(&t, 10, 6, "", 0x07);
    CHECK(t.cells[6 * 80 + 10] == 0x4F20);
    tg_text_put_string(&t, 0, 6, "\xDB\x01\xFF", 0x2E);   /* bytes are character codes, including the high ones */
    CHECK(t.cells[6 * 80] == 0x2E00 + 0xDB && t.cells[6 * 80 + 1] == 0x2E01 && t.cells[6 * 80 + 2] == 0x2EFF);

    /* Scrolling. */
    tg_text_clear(&t, 0x07);
    for (int r = 0; r < 25; r++)
        tg_text_put_cell(&t, 0, r, (uint8_t)('A' + r), 0x07);
    tg_text_scroll_up(&t, 0, 0x70);
    CHECK(t.cells[0] == 0x0700 + 'A' && t.cells[24 * 80] == 0x0700 + 'Y');
    tg_text_scroll_up(&t, -3, 0x70);
    CHECK(t.cells[0] == 0x0700 + 'A');
    tg_text_scroll_up(&t, 2, 0x70);
    CHECK(t.cells[0] == 0x0700 + 'C' && t.cells[22 * 80] == 0x0700 + 'Y');
    CHECK(t.cells[23 * 80] == 0x7020 && t.cells[24 * 80 + 79] == 0x7020 && t.cells[22 * 80 + 1] == 0x0720);
    tg_text_scroll_up(&t, 100, 0x30);                  /* more than the screen: all blank */
    for (int i = 0; i < TG_TEXT_CELLS; i++)
        CHECK(t.cells[i] == 0x3020);

    /* Redefining a glyph shows in the picture. */
    static const uint8_t mine[16] = {0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81, 1, 2, 4, 8, 16, 32, 64, 128};
    tg_text_set_glyph(&t, 200, mine);
    CHECK(memcmp(t.font + 200 * 16, mine, 16) == 0);
    CHECK(t.font[199 * 16 + 15] == 0 && t.font[201 * 16] == 0);   /* its neighbours are untouched */
    tg_text_clear(&t, TG_TEXT_ATTR(TG_TEXT_WHITE, TG_TEXT_RED));
    tg_text_put_cell(&t, 10, 10, 200, TG_TEXT_ATTR(TG_TEXT_BLACK, TG_TEXT_WHITE));
    tg_t g;
    tg_init(&g);
    g.mode = TG_MODE_TEXT;
    g.text = &t;
    for (int line = 0; line < 480; line++)
        check_line(&g, line);
    uint32_t out[TG_WIDTH];
    tg_render_display_line(&g, 40 + 160, out);          /* the top row of the glyph: 0x81, so the outer pixels are set */
    /* ... and the pixels: 0x81 is bit 7 and bit 0, with the foreground (black) where it is 1 and the background (white) between. */
    CHECK((uint16_t)out[40] == t.palette[TG_TEXT_BLACK] && (uint16_t)(out[40] >> 16) == t.palette[TG_TEXT_WHITE]);
    CHECK((uint16_t)out[43] == t.palette[TG_TEXT_WHITE] && (uint16_t)(out[43] >> 16) == t.palette[TG_TEXT_BLACK]);
}

static void test_cp437(void)
{
    static tg_text_t t;
    memset(&t, 0xEE, sizeof t.font);
    CHECK(tg_text_load_cp437(&t) == 0);
    /* Spot checks of the table. */
    CHECK(tg_cp437_to_unicode(0x41) == 'A' && tg_cp437_to_unicode(0x20) == ' ' && tg_cp437_to_unicode(0x00) == ' ');
    CHECK(tg_cp437_to_unicode(0x01) == 0x263A && tg_cp437_to_unicode(0x03) == 0x2665 && tg_cp437_to_unicode(0x1F) == 0x25BC);
    CHECK(tg_cp437_to_unicode(0x7F) == 0x2302 && tg_cp437_to_unicode(0x80) == 0x00C7 && tg_cp437_to_unicode(0x9B) == 0x00A2);
    CHECK(tg_cp437_to_unicode(0xB0) == 0x2591 && tg_cp437_to_unicode(0xC9) == 0x2554 && tg_cp437_to_unicode(0xDB) == 0x2588);
    CHECK(tg_cp437_to_unicode(0xE1) == 0x00DF && tg_cp437_to_unicode(0xE3) == 0x03C0 && tg_cp437_to_unicode(0xF8) == 0x00B0);
    CHECK(tg_cp437_to_unicode(0xFE) == 0x25A0 && tg_cp437_to_unicode(0xFF) == 0x00A0);
    for (int c = 0x20; c < 0x7F; c++)
        CHECK(tg_cp437_to_unicode((uint8_t)c) == (uint32_t)c);                      /* ASCII is ASCII */
    /* Every glyph is Unscii's glyph for that character, except the four drawn here. */
    int drawn = 0;
    for (int c = 0; c < 256; c++) {
        const uint8_t *g = tg_font_glyph(&tg_font_unscii16, tg_cp437_to_unicode((uint8_t)c));
        if (g)
            CHECK(memcmp(t.font + c * 16, g, 16) == 0);
        else
            drawn++;
    }
    CHECK(drawn == 4);
    static const uint8_t four[4] = {0x0F, 0x7F, 0xA9, 0xF9};
    for (int i = 0; i < 4; i++) {
        int set = 0;
        for (int r = 0; r < 16; r++)
            set += t.font[four[i] * 16 + r] != 0;
        CHECK(set >= 2);                                                            /* drawn, not blank */
    }
    /* Characters that must look like something, and the ones that must be blank. */
    for (int c = 0x21; c < 0x7F; c++) {
        int set = 0;
        for (int r = 0; r < 16; r++)
            set += t.font[c * 16 + r] != 0;
        CHECK(set > 0);
    }
    for (int r = 0; r < 16; r++)
        CHECK(t.font[0x20 * 16 + r] == 0 && t.font[r] == 0);                        /* space and NUL are blank */
    for (int r = 0; r < 16; r++)
        CHECK(t.font[0xDB * 16 + r] == 0xFF);                                       /* the full block */
    CHECK(t.font[0xDC * 16 + 7] == 0 && t.font[0xDC * 16 + 8] == 0xFF);             /* lower half block: the bottom 8 rows */
    CHECK(t.font[0xDF * 16 + 7] == 0xFF && t.font[0xDF * 16 + 8] == 0);             /* upper half block */
    CHECK(t.font[0xDD * 16] == 0xF0 && t.font[0xDE * 16] == 0x0F);                  /* left and right half blocks */
}

int main(void)
{
    test_render_random();
    test_render_edges();
    test_modes();
    test_helpers();
    test_cp437();
    printf("all %d checks passed\n", checks);
    return 0;
}
