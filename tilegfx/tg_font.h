/* SPDX-License-Identifier: MIT */
/*
 * Bitmap fonts for tilegfx: the public-domain Unscii fonts (see fonts/FONTS.md) and the functions that turn glyphs into
 * tile graphics and text into tile map entries.
 *
 * A glyph is one byte per row, most significant bit leftmost, always 8 pixels wide: an 8x8 font has 8 bytes a glyph and
 * an 8x16 font 16. Any array in that format can be used, not only these fonts.
 *
 * Which tile is which character is kept in a character map (tg_charmap_t): pairs of a Unicode codepoint and its tile,
 * sorted, in storage the program gives, as it gives a layer's map and tiles. The loaders fill it in and the put functions
 * read it, so printing needs only the string:
 *
 *   static tg_char_t chars[128];
 *   static tg_charmap_t cm;
 *   tg_charmap_init(&cm, chars, 128);
 *   tg_layer_load_font(&layer, &cm, TG_FONT_UNSCII8, 32, 95, 100, fg, bg);   // glyphs 32..126 become tiles 100..194
 *   tg_layer_put_string(&layer, &cm, 2, 3, "Hello", 0);
 *
 * or, with tiles only for the characters a program shows, each put in a tile that is free (all transparent):
 *
 *   tg_layer_add_chars(&layer, &cm, &tg_font_unscii8, "SCORE 0123456789", 1, 255, NULL, fg, 0);
 *
 * The map belongs to the tile set, not to a layer: layers that share tiles share it, and one set of tiles can have several
 * maps (the same letters in two colours, for instance). The display never reads it: only loading and putting text do.
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

/* ---- the character map: which tile is which character ---- */

#define TG_NO_TILE 0xFFFF   /* tg_char_t.lower of a character one tile tall */

typedef struct {
    uint32_t codepoint;
    uint16_t tile;    /* the character's tile; for a font two tiles tall, the top half */
    uint16_t lower;   /* the bottom half's tile (put one map row below), or TG_NO_TILE */
} tg_char_t;

typedef struct {
    tg_char_t *chars;           /* sorted by codepoint, each at most once; the storage is the caller's */
    uint16_t count, capacity;
} tg_charmap_t;

/* An empty map in `storage`, which has room for `capacity` characters (at most 65535). */
void tg_charmap_init(tg_charmap_t *cm, tg_char_t *storage, unsigned capacity);
/* Every character taken out (the tiles are not touched). */
void tg_charmap_clear(tg_charmap_t *cm);
/* The character's entry, or NULL if it has no tile. */
const tg_char_t *tg_charmap_find(const tg_charmap_t *cm, uint32_t codepoint);
/* The character is tile `tile` (and `lower` below it, or TG_NO_TILE), replacing what it was. False, changing nothing, if a tile
 * number is above 1023 or the map is full. */
bool tg_charmap_set(tg_charmap_t *cm, uint32_t codepoint, unsigned tile, unsigned lower);
/* The character has no tile from now on. False if it had none. */
bool tg_charmap_remove(tg_charmap_t *cm, uint32_t codepoint);
/* Characters first .. first + count - 1 are tiles first_tile .. first_tile + count - 1 (with lower_delta not 0, each lower half
 * lower_delta tiles after its top half), replacing what they were: the old offset scheme, as entries. All or nothing: false,
 * changing nothing, if a tile number would be above 1023, the codepoints would go past 2^32 - 1, or the new characters do not fit. */
bool tg_charmap_set_range(tg_charmap_t *cm, uint32_t first, unsigned count, unsigned first_tile, unsigned lower_delta);

/* ---- loading glyphs into tiles ---- */

/*
 * Load a built-in font's glyphs first_glyph .. first_glyph + count - 1 into the layer's tiles first_tile onwards, using the
 * layer's tile size and depth, and enter them in the character map (tg_charmap_set_range). A glyph the font lacks becomes a
 * tile of all `bg`. Index 0 is transparent. An 8x16 font takes two tiles a glyph: the top halves go in first_tile ..
 * first_tile + count - 1 and the bottom halves in the count tiles after them. The tile array needs room for the tiles.
 *
 * Returns false, changing nothing, for a bad font number, no map, a layer that is not a tile layer with 8x8 or 16x16 tiles of 4
 * or 8 bits, no tiles, no glyphs, tile numbers that would go past 1023 (a map entry cannot name more), or no room in the map.
 */
bool tg_layer_load_font(tg_layer_t *layer, tg_charmap_t *cm, tg_font_id_t font, uint32_t first_glyph, unsigned count,
                        unsigned first_tile, uint8_t fg, uint8_t bg);

/* The same, for a font you name by pointer (so that only the fonts you use are linked). */
bool tg_layer_load_font_ptr(tg_layer_t *layer, tg_charmap_t *cm, const tg_font_t *font, uint32_t first_glyph, unsigned count,
                            unsigned first_tile, uint8_t fg, uint8_t bg);

/*
 * Tiles for the characters a program shows, and no more. Each character of the UTF-8 string `text` that the map does not have
 * yet and the font does have is drawn, in fg on bg, into a free tile among lowest .. highest and entered in the map; characters
 * the font lacks, and the same character again, are passed over. A tile is free if every pixel of it is 0 (transparent), no
 * entry of the map names it, and keep[tile] is 0 (keep, one byte per tile number, may be NULL: a program can mark tiles that
 * are in use although they are blank). The free tiles nearest `highest` are taken, in rising order for the characters in the
 * order of the string, so "0123456789" gets ten tiles in a row if there are ten free at the top. An 8x16 font takes two free
 * tiles a character, the top half first. The tile array needs room for tiles up to `highest`.
 *
 * All or nothing: returns how many characters were added (0 if the map had them all), or -1, changing nothing, if there are not
 * enough free tiles or not enough room in the map, or for a bad argument (no map, font, text or tiles; a tile size or depth that
 * is not 8 or 16 and 4 or 8; highest above 1023 or lowest above it).
 */
int tg_font_add_chars(tg_charmap_t *cm, const tg_font_t *font, const char *text, uint8_t *tiles, unsigned tile_log2,
                      unsigned depth, unsigned lowest, unsigned highest, const uint8_t *keep, uint8_t fg, uint8_t bg);

/* The same, into a layer's tiles (with its tile size and depth); -1 for a layer that cannot take text. */
int tg_layer_add_chars(tg_layer_t *layer, tg_charmap_t *cm, const tg_font_t *font, const char *text, unsigned lowest,
                       unsigned highest, const uint8_t *keep, uint8_t fg, uint8_t bg);

/*
 * The lower level: load `count` consecutive glyphs from an array of your own, rows_per_glyph bytes each (8 or 16), into
 * the layer's tiles first_tile onwards. `half` picks the top 8 rows (0) or the bottom 8 (1) of a 16-row glyph. Index 0 is
 * transparent. This does NOT say what characters the tiles stand for: follow it with tg_charmap_set_range or tg_charmap_set.
 * Returns false, changing nothing, for the same reasons as tg_layer_load_font.
 */
bool tg_layer_load_glyphs(tg_layer_t *layer, const uint8_t *glyphs, unsigned rows_per_glyph, unsigned half, unsigned count,
                          unsigned first_tile, uint8_t fg, uint8_t bg);

/*
 * Program tiles first_tile .. first_tile + count - 1 of `tiles` from the glyphs first_cp .. first_cp + count - 1 of a font,
 * in order, with no map. `half` picks which 8 rows of the glyph are used: 0 is the top 8 (all there is for an 8-row font), 1 the
 * bottom 8 of a 16-row font. A codepoint the font lacks (and a half it lacks) becomes a tile of all `bg`.
 */
void tg_font_load_range(const tg_font_t *font, uint8_t *tiles, unsigned tile_log2, unsigned depth, unsigned first_tile,
                        uint32_t first_cp, unsigned count, unsigned half, uint8_t fg, uint8_t bg);

/* ---- putting text on a map ---- */

/*
 * Put text on the layer's map, left to right from tile (x, y), wrapping round the map: each character's tile from the map, with
 * sub-palette `pal` for 4 bpp tiles; for a character two tiles tall the lower half goes on the row below. A character the map
 * has no tile for is skipped: its cell is left as it was and the position still moves on. Nothing happens without a map.
 *
 *   tg_layer_put_string  takes each byte of the string as a character (so "\xE9" is U+00E9)
 *   tg_layer_put_utf8    decodes UTF-8 first (ASCII is unchanged), so "é" or "─" can be written as they are; bytes that
 *                        are not valid UTF-8 are skipped
 *   tg_layer_put_glyphs  takes a list of 16-bit codepoints (including 0)
 */
void tg_layer_put_string(const tg_layer_t *layer, const tg_charmap_t *cm, int x, int y, const char *text, unsigned pal);
void tg_layer_put_utf8(const tg_layer_t *layer, const tg_charmap_t *cm, int x, int y, const char *text, unsigned pal);
void tg_layer_put_glyphs(const tg_layer_t *layer, const tg_charmap_t *cm, int x, int y, const uint16_t *glyphs, size_t count,
                         unsigned pal);

/* The same into a map of your own (map_w_log2 by map_h_log2 entries), with no layer. */
void tg_map_put_string(uint16_t *map, unsigned map_w_log2, unsigned map_h_log2, const tg_charmap_t *cm, int x, int y,
                       const char *text, unsigned pal);
void tg_map_put_glyphs(uint16_t *map, unsigned map_w_log2, unsigned map_h_log2, const tg_charmap_t *cm, int x, int y,
                       const uint16_t *glyphs, size_t count, unsigned pal);

/* The next codepoint of a UTF-8 string, moving *p past it; TG_BAD_UTF8 for a byte that does not begin a well formed one (and
 * *p moves one byte on). For a program's own text functions (the Lua layer's, which write through a world). */
#define TG_BAD_UTF8 0xFFFFFFFFu
uint32_t tg_utf8_next(const char **p);

#endif
