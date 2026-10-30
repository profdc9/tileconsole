/* SPDX-License-Identifier: MIT */
/*
 * Bitmap fonts for tilegfx: the public-domain Unscii fonts (see fonts/FONTS.md) and the helpers that turn a glyph into
 * tile graphics, so that text can be put on a tile layer.
 *
 * A glyph is one byte per row, most significant bit leftmost, always 8 pixels wide: an 8x8 font has 8 bytes a glyph and a
 * 8x16 font 16. Any array in that format can be used with tg_tile_from_glyph, not only these fonts.
 */
#ifndef TG_FONT_H
#define TG_FONT_H

#include <stddef.h>
#include <stdint.h>

#include "tilegfx.h"

/* Codepoints are stored as runs of consecutive values; the glyph for first + k is glyph number index + k. */
typedef struct {
    uint32_t first;
    uint16_t count;
    uint16_t index;
} tg_glyph_run_t;

typedef struct {
    const tg_glyph_run_t *runs;   /* sorted by codepoint */
    uint16_t nruns, nglyphs;
    const uint8_t *glyphs;        /* nglyphs * height bytes */
    uint8_t height;               /* rows per glyph: 8 or 16 */
} tg_font_t;

/* The glyph for a Unicode codepoint (height bytes), or NULL if the font does not have one. */
const uint8_t *tg_font_glyph(const tg_font_t *font, uint32_t codepoint);

/* Bytes of tile graphics for one tile: tile_log2 3 or 4 (8x8, 16x16), depth 4 or 8 bits per pixel. */
size_t tg_tile_bytes(unsigned tile_log2, unsigned depth);

/*
 * Program one tile from an 8-row glyph: pixels that are set get colour index `fg`, the others `bg`. Index 0 is
 * transparent, so bg = 0 gives text with a see-through background. For 4 bpp tiles the indices are 0..15 (the layer
 * entry's sub-palette picks the actual colours); for 8 bpp they are palette indices. On a 16x16 tile every glyph pixel
 * becomes a 2x2 block. A glyph of an 8x16 font takes two tiles: pass glyph and then glyph + 8.
 */
void tg_tile_from_glyph(uint8_t *tile, unsigned tile_log2, unsigned depth, const uint8_t *glyph, uint8_t fg, uint8_t bg);

/* A run of characters whose tiles have been programmed, for tg_text_put. */
typedef struct {
    const tg_font_t *font;
    uint32_t first_cp;      /* the codepoint of the first character */
    uint16_t count;         /* how many characters */
    uint16_t first_tile;    /* the tile number of the first character (an 8x16 font uses two tiles a character) */
    uint8_t tiles_per_char; /* 1 for an 8-row font, 2 for a 16-row one */
} tg_charset_t;

/*
 * Program the tiles for the characters first_cp .. first_cp + count - 1 into `tiles` (the start of a tile array),
 * from tile number first_tile on. A character the font lacks becomes an empty tile. Typical use: first_cp = 0x20,
 * count = 96 for ASCII.
 */
tg_charset_t tg_font_load(const tg_font_t *font, uint8_t *tiles, unsigned tile_log2, unsigned depth, uint16_t first_tile,
                          uint32_t first_cp, uint16_t count, uint8_t fg, uint8_t bg);

/*
 * Write a string into a tile array (a layer's map, entries as in TG_ENTRY) with its top left at tile (x, y); positions
 * wrap round the map. `text` is UTF-8 (plain ASCII is unchanged), so "é" and "─" can be written as they are in source
 * code. A character outside the charset, and any byte that is not valid UTF-8, is skipped: its cell is left as it was
 * and the position still moves on. So to mix scripts in one string, call this once for each charset with the same
 * string and position; to clear text, write blank tiles over the area first. `pal` is the sub-palette for 4 bpp tiles.
 * An 8x16 font fills two map rows.
 */
void tg_text_put(uint16_t *map, unsigned map_w_log2, unsigned map_h_log2, int x, int y, const char *text,
                 const tg_charset_t *charset, unsigned pal);

#endif
