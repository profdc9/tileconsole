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

static bool sprite_allocated(const tg_t *g, int slot)
{
    return slot >= 0 && slot < TG_MAX_SPRITES && (g->sprite_used[slot >> 5] >> (slot & 31) & 1);
}

/* Put an allocated slot into sprite_order: after every entry that is in front of it (lower priority, or equal and lower slot). */
static void order_insert(tg_t *g, int slot)
{
    int k = g->sprite_count;
    while (k > 0) {
        const int o = g->sprite_order[k - 1];
        if (g->sprite_prio[o] < g->sprite_prio[slot] || (g->sprite_prio[o] == g->sprite_prio[slot] && o < slot))
            break;
        g->sprite_order[k] = g->sprite_order[k - 1];
        k--;
    }
    g->sprite_order[k] = (uint8_t)slot;
    g->sprite_count++;
}

static void order_remove(tg_t *g, int slot)
{
    int k = 0;
    while (k < g->sprite_count && g->sprite_order[k] != slot)
        k++;
    for (; k + 1 < g->sprite_count; k++)
        g->sprite_order[k] = g->sprite_order[k + 1];
    g->sprite_count--;
}

int tg_sprite_alloc(tg_t *g)
{
    for (int i = 0; i < TG_MAX_SPRITES; i++) {
        if (sprite_allocated(g, i))
            continue;
        g->sprites[i] = (tg_sprite_t){.flags = TG_SPRITE_ON};
        g->sprite_used[i >> 5] |= 1u << (i & 31);
        g->sprite_prio[i] = (uint8_t)i;
        order_insert(g, i);
        return i;
    }
    return -1;
}

void tg_sprite_free(tg_t *g, int slot)
{
    if (!sprite_allocated(g, slot))
        return;
    order_remove(g, slot);
    g->sprite_used[slot >> 5] &= ~(1u << (slot & 31));
    g->sprites[slot].flags = 0;
}

void tg_sprite_set_priority(tg_t *g, int slot, uint8_t prio)
{
    if (!sprite_allocated(g, slot))
        return;
    order_remove(g, slot);
    g->sprite_prio[slot] = prio;
    order_insert(g, slot);
}

/* Write one pixel of a tile or sprite row if it is opaque and on screen. */
TG_HOT static inline void put(uint8_t *dst, int x, uint8_t index)
{
    if (index && (unsigned)x < TG_WIDTH)
        dst[x] = index;
}

/*
 * Merge four pixels (a little-endian word of palette indices) into dst; index 0 is transparent. dst and the source
 * may be unaligned. A word with no opaque pixel costs a compare and a branch.
 */
TG_HOT static inline uint32_t opaque_mask(uint32_t v)
{
    /* 0xFF in every byte of v that is not zero (no carries cross a byte, so this is exact for any v). */
    return (((v & 0x7F7F7F7Fu) + 0x7F7F7F7Fu) | v) >> 7 & 0x01010101u;
}

TG_HOT static inline void merge4(uint8_t *dst, uint32_t v)
{
    if (v == 0)
        return;
    const uint32_t m = opaque_mask(v) * 0xFFu;
    tg_u32_unaligned *d = (tg_u32_unaligned *)dst;
    *d = (*d & ~m) | (v & m);
}

/* The same, for 4 bpp pixels already spread one per byte: the sub-palette `base` (pal * 16 in every byte) is added
 * to the opaque ones. */
TG_HOT static inline void merge4_pal(uint8_t *dst, uint32_t v, uint32_t base)
{
    if (v == 0)
        return;
    const uint32_t m = opaque_mask(v) * 0xFFu;
    tg_u32_unaligned *d = (tg_u32_unaligned *)dst;
    *d = (*d & ~m) | ((v | base) & m);
}

/* Spread two bytes of 4 bpp pixels (x = b0 | b1 << 8) into four bytes, one pixel each, left pixel in the low byte. */
TG_HOT static inline uint32_t spread4(uint32_t x)
{
    return ((x >> 4) & 0x0000000Fu) | ((x & 0x0000000Fu) << 8) | ((x << 4) & 0x000F0000u) | ((x << 16) & 0x0F000000u);
}

/* Draw one tile row that lies entirely on screen. */
TG_HOT static inline __attribute__((always_inline)) void draw_tile_row_fast(uint8_t *dst, const uint8_t *tiles, unsigned tile, unsigned row,
                                             unsigned tile_log2, unsigned depth, bool hflip, unsigned pal)
{
    const unsigned size = 1u << tile_log2;
    if (depth == 8) {
        const tg_u32_unaligned *src = (const tg_u32_unaligned *)(tiles + (((size_t)tile << (2 * tile_log2)) | (row << tile_log2)));
        const unsigned words = size >> 2;
        if (hflip) {
            for (unsigned i = 0; i < words; i++)
                merge4(dst + 4 * i, __builtin_bswap32(src[words - 1 - i]));
        } else {
            for (unsigned i = 0; i < words; i++)
                merge4(dst + 4 * i, src[i]);
        }
    } else {
        const tg_u32_unaligned *src = (const tg_u32_unaligned *)(tiles + (((size_t)tile << (2 * tile_log2 - 1)) | (row << (tile_log2 - 1))));
        const unsigned words = size >> 3;   /* each word holds 8 pixels */
        const uint32_t base = (pal << 4) * 0x01010101u;
        for (unsigned i = 0; i < words; i++) {
            uint32_t w;
            if (hflip) {
                w = __builtin_bswap32(src[words - 1 - i]);
                w = ((w & 0x0F0F0F0Fu) << 4) | ((w >> 4) & 0x0F0F0F0Fu);   /* swap the nibbles of each byte */
            } else {
                w = src[i];
            }
            if (w == 0)
                continue;
            merge4_pal(dst + 8 * i, spread4(w & 0xFFFFu), base);
            merge4_pal(dst + 8 * i + 4, spread4(w >> 16), base);
        }
    }
}

/*
 * Draw one row of one tile at screen x. `row` is the row of the tile already chosen with any v-flip applied; `pal`
 * only matters for 4 bpp. Tiles that are partly off screen take the slow, clipped path.
 */
TG_HOT static inline __attribute__((always_inline)) void draw_tile_row(uint8_t *dst, int sx, const uint8_t *tiles, unsigned tile, unsigned row,
                                        unsigned tile_log2, unsigned depth, bool hflip, unsigned pal)
{
    const unsigned size = 1u << tile_log2;
    if (sx >= 0 && sx + (int)size <= TG_WIDTH) {
        draw_tile_row_fast(dst + sx, tiles, tile, row, tile_log2, depth, hflip, pal);
        return;
    }
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

TG_HOT /* One copy of the tile loop per tile size and depth, so that sizes and depths are constants and everything unrolls. */
TG_HOT static inline __attribute__((always_inline)) void draw_tile_layer_impl(const tg_layer_t *L, int line, uint8_t *dst,
                                                                              const unsigned tl, const unsigned depth)
{
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
        draw_tile_row(dst, sx, L->tiles, e & TG_TILE_MASK, r, tl, depth, (e & TG_HFLIP) != 0, e >> TG_PAL_SHIFT);
    }
}

TG_HOT static void draw_layer_8px_4bpp(const tg_layer_t *L, int line, uint8_t *dst) { draw_tile_layer_impl(L, line, dst, 3, 4); }
TG_HOT static void draw_layer_8px_8bpp(const tg_layer_t *L, int line, uint8_t *dst) { draw_tile_layer_impl(L, line, dst, 3, 8); }
TG_HOT static void draw_layer_16px_4bpp(const tg_layer_t *L, int line, uint8_t *dst) { draw_tile_layer_impl(L, line, dst, 4, 4); }
TG_HOT static void draw_layer_16px_8bpp(const tg_layer_t *L, int line, uint8_t *dst) { draw_tile_layer_impl(L, line, dst, 4, 8); }

TG_HOT static void draw_tile_layer(const tg_layer_t *L, int line, uint8_t *dst)
{
    if (L->tile_log2 == 3)
        (L->depth == 8 ? draw_layer_8px_8bpp : draw_layer_8px_4bpp)(L, line, dst);
    else
        (L->depth == 8 ? draw_layer_16px_8bpp : draw_layer_16px_4bpp)(L, line, dst);
}

TG_HOT static void draw_bitmap_layer(const tg_layer_t *L, int line, uint8_t *dst)
{
    const uint8_t *src = L->bitmap + (size_t)line * TG_WIDTH;
    for (int x = 0; x < TG_WIDTH; x += 4)
        merge4(dst + x, *(const tg_u32_unaligned *)(src + x));
}

TG_HOT static void draw_sprite_row(uint8_t *dst, int sx, const uint8_t *tiles, unsigned tile, unsigned row,
                                   unsigned tile_log2, unsigned depth, bool hflip, unsigned pal)
{
    draw_tile_row(dst, sx, tiles, tile, row, tile_log2, depth, hflip, pal);
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

    /* Sprites that touch this line, front-most first, up to the per-line limit. */
    const int sprite_size = 1 << g->sprite_tile_log2;
    const tg_sprite_t *on_line[TG_MAX_SPRITES_PER_LINE];
    int n = 0;
    for (int i = 0; i < g->sprite_count && n < TG_MAX_SPRITES_PER_LINE; i++) {
        const tg_sprite_t *s = &g->sprites[g->sprite_order[i]];
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
            draw_sprite_row(idx, s->x, g->sprite_tiles, s->tile, row, g->sprite_tile_log2, g->sprite_depth,
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
    uint8_t idx[TG_WIDTH] __attribute__((aligned(4)));
    tg_render_line_indices(g, line, idx);
    const uint16_t *pal = g->palette;
    for (int x = 0; x < TG_WIDTH; x += 4) {
        const uint32_t v = *(const tg_u32_unaligned *)(idx + x);
        const uint32_t c0 = pal[v & 255], c1 = pal[(v >> 8) & 255], c2 = pal[(v >> 16) & 255], c3 = pal[v >> 24];
        out[x] = c0 * 0x10001u;
        out[x + 1] = c1 * 0x10001u;
        out[x + 2] = c2 * 0x10001u;
        out[x + 3] = c3 * 0x10001u;
    }
}

/* ---- text mode ---- */

/* One display line of text mode: 80 cells, each 8 pixels, which is 4 words. A word is made from two bits of a glyph row, so each cell
 * needs four words (background pair, foreground pair, and the two mixed pairs) built from its two colours. */
TG_HOT static void render_text_line(const tg_t *g, int line, uint32_t out[TG_WIDTH])
{
    const tg_text_t *t = g->text;
    const int y = line - TG_TEXT_TOP;
    if (!t || y < 0 || y >= TG_TEXT_ROWS * TG_GLYPH_HEIGHT) {   /* above, below or no text: black */
        for (int x = 0; x < TG_WIDTH; x++) {
            out[x] = 0;
            __asm__ volatile("" ::: "memory");   /* not memset: on a microcontroller that is a call into flash */
        }
        return;
    }
    const uint16_t *cell = t->cells + (y / TG_GLYPH_HEIGHT) * TG_TEXT_COLS;
    int cursor = -1;   /* the column whose cell is drawn solid on this line, if the cursor is on it and showing */
    if ((t->cursor_flags & TG_CURSOR_VISIBLE) && t->cursor_row == y / TG_GLYPH_HEIGHT && t->cursor_col >= 0 && t->cursor_col < TG_TEXT_COLS) {
        const int r = y % TG_GLYPH_HEIGHT;
        if (r >= t->cursor_start && r <= t->cursor_end && (!(t->cursor_flags & TG_CURSOR_BLINK) || (g->frame / TG_CURSOR_BLINK_FRAMES) % 2 == 0))
            cursor = t->cursor_col;
    }
    const uint8_t *font_row = t->font + (y % TG_GLYPH_HEIGHT);
    const uint16_t *pal = t->palette;
    for (int col = 0; col < TG_TEXT_COLS; col++) {
        const uint32_t c = cell[col];
        const uint32_t fg = pal[(c >> 8) & 15], bg = pal[(c >> 12) & 15];
        const uint32_t bits = font_row[(c & 255) * TG_GLYPH_HEIGHT];
        const uint32_t pair[4] = {bg * 0x10001u, bg | fg << 16, fg | bg << 16, fg * 0x10001u};   /* left pixel in the low half */
        out[4 * col] = pair[bits >> 6];
        out[4 * col + 1] = pair[(bits >> 4) & 3];
        out[4 * col + 2] = pair[(bits >> 2) & 3];
        out[4 * col + 3] = pair[bits & 3];
    }
    if (cursor >= 0) {
        const uint32_t solid = pal[(cell[cursor] >> 8) & 15] * 0x10001u;
        out[4 * cursor] = out[4 * cursor + 1] = out[4 * cursor + 2] = out[4 * cursor + 3] = solid;
    }
}

TG_HOT int tg_display_lines(const tg_t *g) { return g->mode == TG_MODE_TEXT ? TG_DISPLAY_HEIGHT : TG_HEIGHT; }

TG_HOT int tg_display_line_shift(const tg_t *g) { return g->mode == TG_MODE_TEXT ? 0 : 1; }

TG_HOT void tg_render_display_line(tg_t *g, int line, uint32_t out[TG_WIDTH])
{
    if (g->mode == TG_MODE_TEXT)
        render_text_line(g, line, out);
    else
        tg_render_line(g, line, out);
}
