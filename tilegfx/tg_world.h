/*
 * SPDX-License-Identifier: MIT
 * tg_world: a large tile map, and the small map the compositor reads kept in step with a camera. See WORLD.md.
 *
 * The layer's own tile map (a power of two, wrapping) is the window. Global tile (tx, ty) lives in slot (tx & (W-1), ty & (H-1)), so the
 * layer's scroll is simply the camera in global pixels. There are two window maps: the display reads one while everything is written to the
 * other, and a dirty bitmap makes the second catch up with the first by copying only the slots that changed. The program calls, once a frame:
 *
 *     plat_wait_frame();
 *     tg_world_begin_frame(&w);
 *     ... tg_world_set(), tg_world_set_camera() ...
 *     tg_world_end_frame(&w);
 *     plat_present(&g);        (g holds the layer that was given to tg_world_init)
 */
#ifndef TG_WORLD_H
#define TG_WORLD_H

#include "tilegfx.h"

#define TG_TILES_MAX 1024                      /* tile numbers are 10 bits */

/* The flag byte of a tile number (not of a map entry: the flips and the palette do not matter). */
#define TG_TILE_SOLID 0x01
#define TG_TILE_HAZARD 0x02
#define TG_TILE_ONEWAY 0x04                    /* solid from above only */
#define TG_TILE_LIQUID 0x08
/* 0x10 .. 0x80: the program's */

typedef enum {
    TG_BORDER_TILE,                            /* outside the big map: border_entry */
    TG_BORDER_CLAMP,                           /* the nearest tile of the map */
    TG_BORDER_WRAP                             /* the map repeats */
} tg_border_t;

/* The caller's storage: both window maps in SRAM, and the dirty bitmap (one bit a slot). */
#define TG_WORLD_WIN_ENTRIES(w_log2, h_log2) ((size_t)1 << ((w_log2) + (h_log2)))
#define TG_WORLD_DIRTY_WORDS(w_log2, h_log2) ((((size_t)1 << ((w_log2) + (h_log2))) + 31) / 32)

typedef struct {
    tg_layer_t *layer;                         /* the layer it drives: part of the tg_t that is given to plat_present */
    uint16_t *win[2];                          /* the window maps: layer->map is one of them (win[shown]), writes go to the other */
    uint32_t *dirty;                           /* slots where the hidden map differs from the shown one */
    uint32_t dirty_words, dirty_count;         /* count: bits set (0: begin_frame has nothing to copy) */
    uint8_t shown, open, w_log2, h_log2, tile_log2;

    uint16_t *map;                             /* the big map: row-major, map_w * map_h entries, anywhere (PSRAM); the world edits it */
    int32_t map_w, map_h;                      /* in tiles; any size */
    tg_border_t border;
    uint16_t border_entry;
    const uint8_t *flags;                      /* TG_TILES_MAX bytes, or NULL (all zero) */

    int32_t cam_x, cam_y;                      /* global pixels */
    int32_t win_tx, win_ty;                    /* the global tile in the window's top left */
} tg_world_t;

/*
 * `layer` must be set up already (kind TG_LAYER_TILES, tile_log2, depth, tiles, and map_w_log2 and map_h_log2, which are the window's size: at
 * least the visible tiles plus one each way, and at least 32 slots). Returns false if not. The two window maps and the bitmap are the caller's
 * (TG_WORLD_WIN_ENTRIES, TG_WORLD_DIRTY_WORDS) and `map` is the big map. The camera starts at 0, 0 and the frame is open: edits may follow at once.
 */
bool tg_world_init(tg_world_t *w, tg_layer_t *layer, uint16_t *win_a, uint16_t *win_b, uint32_t *dirty,
                   uint16_t *map, int32_t map_w, int32_t map_h);
void tg_world_set_border(tg_world_t *w, tg_border_t border, uint16_t entry);
void tg_world_set_flags(tg_world_t *w, const uint8_t *flags);   /* one byte per tile number */
void tg_world_reload(tg_world_t *w);                            /* after the big map was changed behind the world's back */

/*
 * begin_frame comes after plat_wait_frame (the map the display reads is then not the one written to). Anything that writes calls it by itself if
 * the program has not, which is right only after the wait. end_frame swaps the maps and sets layer->map and the scroll; do it once for each
 * plat_present. Both do nothing if the frame is already begun, or ended.
 */
void tg_world_begin_frame(tg_world_t *w);
void tg_world_set_camera(tg_world_t *w, int32_t x, int32_t y);  /* global pixels, signed; streams the tiles that come into the window.
                                                                   * With TG_BORDER_WRAP and a map size that is a multiple of the window's (the
                                                                   * same size, say) the camera is kept within one map: cam_x, cam_y are then
                                                                   * the position modulo the map, and the scrolling can go on for ever. */
void tg_world_end_frame(tg_world_t *w);

/* With TG_BORDER_WRAP: the size of the map in pixels (the distance after which the world repeats), for tg_actors_set_wrap. Otherwise false. */
bool tg_world_wrap_px(const tg_world_t *w, int32_t *width, int32_t *height);

/* The big map: the newest value, whatever is on screen. Outside it: the border rule. */
uint16_t tg_world_get(const tg_world_t *w, int32_t tx, int32_t ty);
/* Outside the big map a write is ignored (with TG_BORDER_WRAP it wraps). It is seen at the next end_frame, all of an edit together. */
void tg_world_set(tg_world_t *w, int32_t tx, int32_t ty, uint16_t entry);
void tg_world_fill(tg_world_t *w, int32_t tx, int32_t ty, int32_t cols, int32_t rows, uint16_t entry);
void tg_world_blit(tg_world_t *w, int32_t tx, int32_t ty, const uint16_t *src, int32_t cols, int32_t rows, int32_t src_stride);

/* Flags of tiles. A box is in global pixels; the result is the OR of the flags of every tile it touches (0 if it is empty). */
uint8_t tg_world_flags_tile(const tg_world_t *w, int32_t tx, int32_t ty);
uint8_t tg_world_flags_px(const tg_world_t *w, int32_t x, int32_t y);
uint8_t tg_world_flags_box(const tg_world_t *w, int32_t x, int32_t y, int32_t width, int32_t height);

#endif
