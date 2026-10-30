/*
 * SPDX-License-Identifier: MIT
 * tilegfx: a scanline compositor for tile layers, sprites and a bitmap layer.
 *
 * Every line is composed into 320 8-bit palette indices (index 0 is transparent),
 * then converted through a 256-entry RGB565 palette. Plain C with no dependencies,
 * so the same code runs in a desktop test program and on a microcontroller.
 *
 * Tile layers use a power-of-two tile array that wraps; the scroll position is in
 * pixels and the compositor derives the tile column and fine offset from it, so a
 * game scrolls by changing scroll_x/scroll_y and rewriting the column or row of
 * tiles that has just left the screen.
 */
#ifndef TILEGFX_H
#define TILEGFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TG_WIDTH 320
#define TG_HEIGHT 240
#define TG_MAX_LAYERS 4
#define TG_MAX_SPRITES 128
#define TG_MAX_SPRITES_PER_LINE 32

/* A tile array entry: bits 0-9 tile number, 10 h-flip, 11 v-flip, 12-15 sub-palette (4 bpp tiles). */
#define TG_TILE_MASK 0x03FF
#define TG_HFLIP 0x0400
#define TG_VFLIP 0x0800
#define TG_PAL_SHIFT 12
#define TG_ENTRY(tile, pal, flags) ((uint16_t)(((tile) & TG_TILE_MASK) | ((pal) << TG_PAL_SHIFT) | (flags)))

/*
 * Tile graphics, row by row. 8 bpp: one byte per pixel. 4 bpp: two pixels per byte, the
 * left pixel in the high nibble. A tile is (1 << tile_log2) pixels square: 8 or 16.
 */

typedef enum { TG_LAYER_OFF = 0, TG_LAYER_TILES, TG_LAYER_BITMAP } tg_layer_kind_t;

/*
 * How glyph numbers (character codes) map to tiles, for putting text on a layer (see tg_font.h). The compositor does not
 * use it. All zero means: tile number = glyph number, any glyph, one tile tall. It is filled in by the layer's glyph
 * loading functions and read by the ones that put text on the map.
 */
typedef struct {
    int32_t offset;       /* tile number = glyph number + offset */
    uint32_t first;       /* the glyph numbers that have tiles are first .. first + count - 1 ... */
    uint32_t count;       /* ... and 0 means that every glyph number is taken to have one */
    uint16_t row_delta;   /* for glyphs two tiles tall: the lower tile is the upper one + row_delta; 0 for one tile tall */
} tg_glyphmap_t;

typedef struct {
    tg_layer_kind_t kind;

    /* TG_LAYER_TILES */
    uint8_t tile_log2;               /* 3 (8x8) or 4 (16x16) */
    uint8_t depth;                   /* 4 or 8 bits per pixel */
    uint8_t map_w_log2, map_h_log2;  /* tile array size in tiles, as powers of two */
    const uint16_t *map;             /* (1 << map_w_log2) * (1 << map_h_log2) entries */
    const uint8_t *tiles;
    uint32_t scroll_x, scroll_y;     /* pixel position of the top left of the screen; wraps */
    const int16_t *line_scroll_x;    /* optional, TG_HEIGHT entries added to scroll_x per line */

    /* Text on a tile layer: written by tg_layer_load_* / tg_layer_set_glyph_*, used by tg_layer_put_*. The text functions
     * write to `map` and `tiles`, so for a layer used for text both must be in RAM, not const data. */
    tg_glyphmap_t glyphs;

    /* TG_LAYER_BITMAP: TG_WIDTH x TG_HEIGHT bytes, index 0 transparent */
    const uint8_t *bitmap;
} tg_layer_t;

#define TG_SPRITE_ON 1
#define TG_SPRITE_HFLIP 2
#define TG_SPRITE_VFLIP 4

typedef struct {
    int16_t x, y;   /* top left, in screen pixels; may be partly off screen */
    uint16_t tile;
    uint8_t flags;  /* TG_SPRITE_* */
    uint8_t pal;    /* sub-palette for 4 bpp sprite tiles */
    uint8_t above;  /* how many layers are drawn beneath this sprite: 0 = behind all, TG_MAX_LAYERS = in front of all */
} tg_sprite_t;

typedef struct tg tg_t;
typedef void (*tg_line_cb_t)(tg_t *g, int line, void *user);

struct tg {
    uint16_t palette[256];   /* RGB565; entry 0 is the backdrop colour */
    tg_layer_t layers[TG_MAX_LAYERS];   /* drawn in order, 0 at the back */

    tg_sprite_t sprites[TG_MAX_SPRITES]; /* get a slot from tg_sprite_alloc, then assign to it freely */
    uint8_t sprite_count;                /* sprites allocated: the length of sprite_order */
    uint8_t sprite_order[TG_MAX_SPRITES];/* the allocated slots, front-most first; the compositor walks this list */
    uint8_t sprite_prio[TG_MAX_SPRITES]; /* bookkeeping for the three functions below; do not write these directly */
    uint32_t sprite_used[TG_MAX_SPRITES / 32];
    uint8_t sprite_tile_log2, sprite_depth;
    const uint8_t *sprite_tiles;

    tg_line_cb_t line_cb;    /* called before each line is composed, for raster effects */
    void *line_cb_user;
};

void tg_init(tg_t *g);

/*
 * Sprites. tg_sprite_alloc returns the lowest free slot, cleared and set to TG_SPRITE_ON, with priority equal to the
 * slot number, or -1 if all TG_MAX_SPRITES are taken; fill in x, y, tile and the rest through g->sprites[slot], which
 * may be assigned as a whole. tg_sprite_free gives the slot back.
 *
 * Sprites are drawn in order of priority: a lower priority number is in front, and equal priorities go by slot. The
 * compositor looks only at allocated sprites, in that order, so unused slots cost nothing, and when more than
 * TG_MAX_SPRITES_PER_LINE sprites touch one line it is the front-most ones that are drawn. `above` still decides
 * which tile layers a sprite is in front of. Allocating, freeing and changing a priority keep the order list sorted;
 * none of them is meant to be called per line.
 */
int tg_sprite_alloc(tg_t *g);
void tg_sprite_free(tg_t *g, int slot);
void tg_sprite_set_priority(tg_t *g, int slot, uint8_t prio);

/* Compose one line as palette indices. */
void tg_render_line_indices(tg_t *g, int line, uint8_t idx[TG_WIDTH]);

/* Compose one line as RGB565, two identical pixels per 32-bit word (one 640-pixel HDMI line). */
void tg_render_line(tg_t *g, int line, uint32_t out[TG_WIDTH]);

#endif
