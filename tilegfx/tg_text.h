/* SPDX-License-Identifier: MIT */
/*
 * Text mode helpers (see tilegfx.h for the mode and the cell format). A tg_text_t is the whole text screen: 80 x 25 cells, the 256
 * glyphs (8 x 16, in RAM so that they can be redefined) and the 16 colours.
 *
 *   static tg_text_t screen;
 *   tg_text_init(&screen);                   // standard palette, a blank screen
 *   tg_text_load_cp437(&screen);             // the IBM PC character set from Unscii 8x16 (needs tg_font_unscii16)
 *   tg_text_put_string(&screen, 0, 0, "Hello", TG_TEXT_ATTR(TG_TEXT_YELLOW, TG_TEXT_BLUE));
 *   tg_t g; tg_init(&g); g.mode = TG_MODE_TEXT; g.text = &screen;
 *   plat_present(&g);                        // the platform layer takes a copy, so the screen can be changed at once
 *
 * Characters are bytes, the codes of the IBM PC character set (code page 437).
 */
#ifndef TG_TEXT_H
#define TG_TEXT_H

#include "tilegfx.h"

#define TG_RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

/* The standard colours of a PC: the numbers to use in TG_TEXT_ATTR. */
enum {
    TG_TEXT_BLACK, TG_TEXT_BLUE, TG_TEXT_GREEN, TG_TEXT_CYAN, TG_TEXT_RED, TG_TEXT_MAGENTA, TG_TEXT_BROWN, TG_TEXT_LIGHT_GREY,
    TG_TEXT_DARK_GREY, TG_TEXT_LIGHT_BLUE, TG_TEXT_LIGHT_GREEN, TG_TEXT_LIGHT_CYAN, TG_TEXT_LIGHT_RED, TG_TEXT_LIGHT_MAGENTA,
    TG_TEXT_YELLOW, TG_TEXT_WHITE
};

/* The standard 16 colours (the VGA ones, with brown for number 6). */
void tg_text_default_palette(uint16_t palette[16]);

/* The standard palette, and a screen of spaces in light grey on black. The glyphs are not touched (see tg_text_load_cp437). */
void tg_text_init(tg_text_t *t);

/* Fill the 256 glyphs with the IBM PC character set (code page 437, including the face, card suit, arrow and box drawing
 * characters) from the Unscii 8x16 font; four characters it has no glyph for are drawn by this library. Returns how many characters
 * ended up without a glyph (none; they would be blank). */
unsigned tg_text_load_cp437(tg_text_t *t);

/* The Unicode codepoint that the PC shows for a character code (U+0020 for code 0). */
uint32_t tg_cp437_to_unicode(uint8_t ch);

/* Redefine a glyph: 16 bytes, top row first, the leftmost pixel in bit 7 of each. */
void tg_text_set_glyph(tg_text_t *t, uint8_t ch, const uint8_t rows[TG_GLYPH_HEIGHT]);

void tg_text_clear(tg_text_t *t, uint8_t attr);   /* spaces in this attribute */
void tg_text_put_cell(tg_text_t *t, int col, int row, uint8_t ch, uint8_t attr);   /* outside the screen: nothing */
/* The string goes along the row and is cut off at the right edge (it does not wrap); characters left of the screen are skipped. */
void tg_text_put_string(tg_text_t *t, int col, int row, const char *text, uint8_t attr);
/* Move the screen up by this many rows; the rows that come in at the bottom are spaces in `attr`. */
void tg_text_scroll_up(tg_text_t *t, int rows, uint8_t attr);

#endif
