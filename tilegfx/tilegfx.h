/*
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

    tg_sprite_t sprites[TG_MAX_SPRITES]; /* lower numbers are drawn in front of higher ones */
    uint8_t sprite_tile_log2, sprite_depth;
    const uint8_t *sprite_tiles;

    tg_line_cb_t line_cb;    /* called before each line is composed, for raster effects */
    void *line_cb_user;
};

void tg_init(tg_t *g);

/* Compose one line as palette indices. */
void tg_render_line_indices(tg_t *g, int line, uint8_t idx[TG_WIDTH]);

/* Compose one line as RGB565, two identical pixels per 32-bit word (one 640-pixel HDMI line). */
void tg_render_line(tg_t *g, int line, uint32_t out[TG_WIDTH]);

#endif
