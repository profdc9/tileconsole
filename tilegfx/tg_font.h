/* SPDX-License-Identifier: MIT */
/*
 * Bitmap fonts for tilegfx: the public-domain Unscii fonts (see fonts/FONTS.md) and the functions that turn glyphs into
 * tile graphics and text into tile map entries.
 *
 * A glyph is one byte per row, most significant bit leftmost, always 8 pixels wide: an 8x8 font has 8 bytes a glyph and
 * an 8x16 font 16. Any array in that format can be used, not only these fonts.
 *
 * Text goes on a tile layer, and the layer remembers how glyph numbers map to tiles (tg_layer_t.glyphs), so printing
 * needs only the string:
 *
 *   tg_layer_load_font(&layer, TG_FONT_UNSCII8, 32, 95, 100, fg, bg);   // glyphs 32..126 become tiles 100..194
 *   tg_layer_put_string(&layer, 2, 3, "Hello", 0);                      // the offset, 100 - 32 = 68, was stored
 *
 * Built-in fonts are loaded by number and the offset is worked out for you. For your own glyph array, load it by
 * pointer and set the offset yourself:
 *
 *   tg_layer_load_glyphs(&layer, my_glyphs, 8, 0, 64, 200, fg, bg);     // 64 glyphs into tiles 200..263
 *   tg_layer_set_glyph_offset(&layer, 200 - 32);                        // glyph number 32 is the first of them
 *   tg_layer_put_string(&layer, 2, 3, "Hello", 0);
 *
 * The layer's map and tile array are written to, so they must be in RAM.
 */
#ifndef TG_FONT_H
#define TG_FONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilegfx.h"

/* ---- the fonts ---- */

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

/* The built-in fonts, by number. Their glyph numbers are Unicode codepoints (ASCII is 32..126). */
typedef enum {
    TG_FONT_UNSCII8,           /* 8x8: the plain one */
    TG_FONT_UNSCII8_ALT,       /* 8x8: alternative letterforms */
    TG_FONT_UNSCII8_THIN,      /* 8x8: lighter strokes */
    TG_FONT_UNSCII8_FANTASY,   /* 8x8: a fantasy look */
    TG_FONT_UNSCII8_MCR,       /* 8x8: after the MCR arcade fonts */
    TG_FONT_UNSCII8_TALL,      /* 8x16: the 8x8 design stretched to double height */
    TG_FONT_UNSCII16,          /* 8x16: the full-height design (no double-width glyphs) */
    TG_FONT_COUNT
} tg_font_id_t;

/*
 * The font with this number, or NULL if the number is wrong or the font was left out of the build. Using font numbers
 * links tg_fontlist.c, which refers to every font; to keep unwanted fonts out of a program define TG_FONTS_ENABLED as a
 * mask of the ones to keep (bit n for font number n; the default is all of them). A font you name directly (see
 * tg_fonts.h) and use by pointer is linked on its own.
 */
const tg_font_t *tg_font_by_id(tg_font_id_t id);

/* The glyph for a Unicode codepoint (height bytes), or NULL if the font does not have one. */
const uint8_t *tg_font_glyph(const tg_font_t *font, uint32_t codepoint);

/* ---- one tile from one glyph ---- */

/* Bytes of tile graphics for one tile: tile_log2 3 or 4 (8x8, 16x16), depth 4 or 8 bits per pixel. */
size_t tg_tile_bytes(unsigned tile_log2, unsigned depth);

/*
 * Program one tile from an 8-row glyph: pixels that are set get colour index `fg`, the others `bg`. Index 0 is
 * transparent, so bg = 0 gives text with a see-through background. For 4 bpp tiles the indices are 0..15 (the layer
 * entry's sub-palette picks the actual colours); for 8 bpp they are palette indices. On a 16x16 tile every glyph pixel
 * becomes a 2x2 block. A glyph of an 8x16 font takes two tiles: pass glyph and then glyph + 8.
 */
void tg_tile_from_glyph(uint8_t *tile, unsigned tile_log2, unsigned depth, const uint8_t *glyph, uint8_t fg, uint8_t bg);

/* ---- text on a tile layer: the offset is stored in the layer ---- */

/*
 * Load a built-in font's glyphs first_glyph .. first_glyph + count - 1 into the layer's tiles first_tile onwards, using
 * the layer's tile size and depth, and store how glyph numbers map to tiles: the offset first_tile - first_glyph, and
 * the range of glyphs that now have tiles. A glyph the font lacks becomes a tile of all `bg`. Index 0 is transparent.
 *
 * An 8x16 font takes two tiles a glyph: the top halves go in first_tile .. first_tile + count - 1 and the bottom halves
 * in the count tiles after them, and tg_layer_put_* then fill two map rows. The tile array needs room for the tiles.
 *
 * Returns false, changing nothing, for a bad font number, a layer that is not a tile layer with 8x8 or 16x16 tiles of 4 or 8
 * bits, no tiles, no glyphs, or tile numbers that would go past 1023 (a map entry cannot name more).
 */
bool tg_layer_load_font(tg_layer_t *layer, tg_font_id_t font, uint32_t first_glyph, unsigned count, unsigned first_tile,
                        uint8_t fg, uint8_t bg);

/* The same, for a font you name by pointer (so that only the fonts you use are linked). */
bool tg_layer_load_font_ptr(tg_layer_t *layer, const tg_font_t *font, uint32_t first_glyph, unsigned count,
                            unsigned first_tile, uint8_t fg, uint8_t bg);

/*
 * The lower level: load `count` consecutive glyphs from an array of your own, rows_per_glyph bytes each (8 or 16), into
 * the layer's tiles first_tile onwards. `half` picks the top 8 rows (0) or the bottom 8 (1) of a 16-row glyph. Index 0 is
 * transparent. This does NOT say what glyph numbers the tiles stand for: follow it with tg_layer_set_glyph_offset.
 * Returns false, changing nothing, for the same reasons as tg_layer_load_font.
 */
bool tg_layer_load_glyphs(tg_layer_t *layer, const uint8_t *glyphs, unsigned rows_per_glyph, unsigned half, unsigned count,
                          unsigned first_tile, uint8_t fg, uint8_t bg);

/*
 * Say that tile number = glyph number + offset for tg_layer_put_*. This replaces the whole glyph mapping: every glyph
 * number is taken to have a tile, and glyphs are one tile tall. (tg_layer_load_font sets the offset for you.)
 */
void tg_layer_set_glyph_offset(tg_layer_t *layer, int offset);

/* Optionally limit tg_layer_put_* to the glyph numbers first .. first + count - 1; others are skipped. count 0 removes the limit. */
void tg_layer_set_glyph_range(tg_layer_t *layer, uint32_t first, uint32_t count);

/*
 * Put text on the layer's map, left to right from tile (x, y), wrapping round the map. The entry written for glyph g is
 * tile g + offset, with sub-palette `pal` for 4 bpp tiles; for a font two tiles tall the lower half goes on the row
 * below. A glyph without a tile (outside the stored range, or a tile number below 0 or above 1023) is skipped: its cell
 * is left as it was and the position still moves on, so call the function once for each glyph set to mix scripts.
 *
 *   tg_layer_put_string  takes each byte of the string as a glyph number
 *   tg_layer_put_utf8    decodes UTF-8 first (ASCII is unchanged), so "é" or "─" can be written as they are; bytes that
 *                        are not valid UTF-8 are skipped
 *   tg_layer_put_glyphs  takes a list of 16-bit glyph numbers (including 0, and above 255)
 */
void tg_layer_put_string(const tg_layer_t *layer, int x, int y, const char *text, unsigned pal);
void tg_layer_put_utf8(const tg_layer_t *layer, int x, int y, const char *text, unsigned pal);
void tg_layer_put_glyphs(const tg_layer_t *layer, int x, int y, const uint16_t *glyphs, size_t count, unsigned pal);

/* ---- the same without a layer: explicit, no stored state ---- */

/*
 * Program tiles first_tile .. first_tile + count - 1 of `tiles` from the glyphs first_cp .. first_cp + count - 1 of a font,
 * in order. `half` picks which 8 rows of the glyph are used: 0 is the top 8 (all there is for an 8-row font), 1 the
 * bottom 8 of a 16-row font. A codepoint the font lacks (and a half it lacks) becomes a tile of all `bg`.
 */
void tg_font_load_range(const tg_font_t *font, uint8_t *tiles, unsigned tile_log2, unsigned depth, unsigned first_tile,
                        uint32_t first_cp, unsigned count, unsigned half, uint8_t fg, uint8_t bg);

/*
 * Put glyph numbers into a map: the entry for glyph g is tile g + glyph_offset. A tile number below 0 or above 1023 is
 * skipped (the cell is left as it was). Glyph numbers above the range you loaded are not checked.
 */
void tg_map_put_glyphs(uint16_t *map, unsigned map_w_log2, unsigned map_h_log2, int x, int y, const uint16_t *glyphs,
                       size_t count, int glyph_offset, unsigned pal);
void tg_map_put_string(uint16_t *map, unsigned map_w_log2, unsigned map_h_log2, int x, int y, const char *text,
                       int glyph_offset, unsigned pal);

#endif
