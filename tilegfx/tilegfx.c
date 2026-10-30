/* SPDX-License-Identifier: MIT */
#include "tilegfx.h"

#include <string.h>

/* On a microcontroller, define TG_HOT_SECTION as a section name (a string) to keep the compositor out of flash. */
#ifdef TG_HOT_SECTION
#define TG_HOT __attribute__((section(TG_HOT_SECTION)))
#else
#define TG_HOT
#endif

void tg_init(tg_t *g)
{
    memset(g, 0, sizeof *g);
    g->sprite_tile_log2 = 4;
    g->sprite_depth = 8;
}

typedef uint32_t __attribute__((may_alias, aligned(1))) tg_u32_unaligned;

/* Write one pixel of a tile or sprite row if it is opaque and on screen. */
TG_HOT static inline void put(uint8_t *dst, int x, uint8_t index)
{
    if (index && (unsigned)x < TG_WIDTH)
        dst[x] = index;
}

/*
 * Draw one row of one tile at screen x. `row` is the row of the tile already chosen with any
 * v-flip applied; `pal` only matters for 4 bpp.
 */
TG_HOT static inline void draw_tile_row(uint8_t *dst, int sx, const uint8_t *tiles, unsigned tile, unsigned row,
                                 unsigned tile_log2, unsigned depth, bool hflip, unsigned pal)
{
    const unsigned size = 1u << tile_log2;
    if (depth == 8) {
        const uint8_t *src = tiles + (((size_t)tile << (2 * tile_log2)) | (row << tile_log2));
        for (unsigned i = 0; i < size; i++)
            put(dst, sx + (int)i, src[hflip ? size - 1 - i : i]);
    } else {
        const uint8_t *src = tiles + (((size_t)tile << (2 * tile_log2 - 1)) | (row << (tile_log2 - 1)));
        const uint8_t base = (uint8_t)(pal << 4);
        for (unsigned i = 0; i < size; i++) {
            const unsigned p = hflip ? size - 1 - i : i;
            const uint8_t nib = (p & 1) ? (src[p >> 1] & 15) : (src[p >> 1] >> 4);
            if (nib)
                put(dst, sx + (int)i, (uint8_t)(base | nib));
        }
    }
}

TG_HOT static void draw_tile_layer(const tg_layer_t *L, int line, uint8_t *dst)
{
    const unsigned tl = L->tile_log2;
    const unsigned size = 1u << tl;
    const uint32_t map_w_mask = (1u << L->map_w_log2) - 1;
    const uint32_t y = ((uint32_t)line + L->scroll_y) & (((uint32_t)1 << (L->map_h_log2 + tl)) - 1);
    const unsigned row = y >> tl, fine_y = y & (size - 1);

    uint32_t x0 = L->scroll_x;
    if (L->line_scroll_x)
        x0 += (uint32_t)(int32_t)L->line_scroll_x[line];
    uint32_t col = (x0 >> tl) & map_w_mask;
    const unsigned fine_x = x0 & (size - 1);

    const uint16_t *map_row = L->map + ((size_t)row << L->map_w_log2);
    for (int sx = -(int)fine_x; sx < TG_WIDTH; sx += (int)size, col = (col + 1) & map_w_mask) {
        const uint16_t e = map_row[col];
        const unsigned r = (e & TG_VFLIP) ? size - 1 - fine_y : fine_y;
        draw_tile_row(dst, sx, L->tiles, e & TG_TILE_MASK, r, tl, L->depth, (e & TG_HFLIP) != 0,
                      e >> TG_PAL_SHIFT);
    }
}

TG_HOT static void draw_bitmap_layer(const tg_layer_t *L, int line, uint8_t *dst)
{
    const uint8_t *src = L->bitmap + (size_t)line * TG_WIDTH;
    for (int x = 0; x < TG_WIDTH; x++)
        if (src[x])
            dst[x] = src[x];
}

TG_HOT void tg_render_line_indices(tg_t *g, int line, uint8_t idx[TG_WIDTH])
{
    if (g->line_cb)
        g->line_cb(g, line, g->line_cb_user);

    /* Not memset: on a microcontroller that is a call into flash. The barrier stops GCC turning the loop back into one. */
    for (int i = 0; i < TG_WIDTH; i += 4) {
        *(tg_u32_unaligned *)(idx + i) = 0;
        __asm__ volatile("" ::: "memory");
    }

    /* Sprites that touch this line, lowest number first, up to the per-line limit. */
    const int sprite_size = 1 << g->sprite_tile_log2;
    const tg_sprite_t *on_line[TG_MAX_SPRITES_PER_LINE];
    int n = 0;
    for (int i = 0; i < TG_MAX_SPRITES && n < TG_MAX_SPRITES_PER_LINE; i++) {
        const tg_sprite_t *s = &g->sprites[i];
        if ((s->flags & TG_SPRITE_ON) && line >= s->y && line < s->y + sprite_size)
            on_line[n++] = s;
    }

    for (int slot = 0; slot <= TG_MAX_LAYERS; slot++) {
        /* Sprites sitting just above `slot` layers: draw the higher numbers first so lower ones end up in front. */
        for (int i = n - 1; i >= 0; i--) {
            const tg_sprite_t *s = on_line[i];
            const int above = s->above > TG_MAX_LAYERS ? TG_MAX_LAYERS : s->above;
            if (above != slot)
                continue;
            const unsigned fine_y = (unsigned)(line - s->y);
            const unsigned row = (s->flags & TG_SPRITE_VFLIP) ? (unsigned)sprite_size - 1 - fine_y : fine_y;
            draw_tile_row(idx, s->x, g->sprite_tiles, s->tile, row, g->sprite_tile_log2, g->sprite_depth,
                          (s->flags & TG_SPRITE_HFLIP) != 0, s->pal);
        }
        if (slot == TG_MAX_LAYERS)
            break;
        const tg_layer_t *L = &g->layers[slot];
        if (L->kind == TG_LAYER_TILES)
            draw_tile_layer(L, line, idx);
        else if (L->kind == TG_LAYER_BITMAP)
            draw_bitmap_layer(L, line, idx);
    }
}

TG_HOT void tg_render_line(tg_t *g, int line, uint32_t out[TG_WIDTH])
{
    uint8_t idx[TG_WIDTH];
    tg_render_line_indices(g, line, idx);
    for (int x = 0; x < TG_WIDTH; x++) {
        const uint32_t c = g->palette[idx[x]];
        out[x] = c | (c << 16);
    }
}
