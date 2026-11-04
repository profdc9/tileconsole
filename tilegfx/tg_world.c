/* SPDX-License-Identifier: MIT */
#include "tg_world.h"

#include <string.h>

/* floor(v / 2^n), also for negative v */
static int32_t floor_shift(int32_t v, unsigned n)
{
    return v >= 0 ? v >> n : ~(~v >> n);
}

static unsigned ctz32(uint32_t v)
{
#if defined(__GNUC__)
    return (unsigned)__builtin_ctz(v);
#else
    unsigned n = 0;
    while (!(v & 1)) {
        v >>= 1;
        n++;
    }
    return n;
#endif
}

/* The coordinate in the big map that v stands for along an axis of `size` tiles, or -1 (outside, and no tile there). */
static int32_t resolve(tg_border_t border, int32_t v, int32_t size)
{
    if (v >= 0 && v < size)
        return v;
    switch (border) {
    case TG_BORDER_CLAMP:
        return v < 0 ? 0 : size - 1;
    case TG_BORDER_WRAP: {
        const int32_t m = v % size;
        return m < 0 ? m + size : m;
    }
    default:
        return -1;
    }
}

static uint16_t big_get(const tg_world_t *w, int32_t tx, int32_t ty)
{
    const int32_t rx = resolve(w->border, tx, w->map_w), ry = resolve(w->border, ty, w->map_h);
    return (rx < 0 || ry < 0) ? w->border_entry : w->map[(size_t)ry * (size_t)w->map_w + (size_t)rx];
}

static int32_t win_w(const tg_world_t *w) { return (int32_t)1 << w->w_log2; }
static int32_t win_h(const tg_world_t *w) { return (int32_t)1 << w->h_log2; }

static size_t slot_of(const tg_world_t *w, int32_t tx, int32_t ty)
{
    return ((size_t)((uint32_t)ty & (uint32_t)(win_h(w) - 1)) << w->w_log2) + ((uint32_t)tx & (uint32_t)(win_w(w) - 1));
}

static bool resident(const tg_world_t *w, int32_t tx, int32_t ty)
{
    return (uint64_t)((int64_t)tx - w->win_tx) < (uint64_t)win_w(w) && (uint64_t)((int64_t)ty - w->win_ty) < (uint64_t)win_h(w);
}

/* The hidden map is the one the display is not reading. */
static uint16_t *hidden(const tg_world_t *w) { return w->win[w->shown ^ 1]; }

/*
 * Write a slot of the hidden map and remember that it differs from the shown one. A slot that already holds the value needs nothing: it is
 * either the same in the shown map or already marked.
 */
static void put_slot(tg_world_t *w, int32_t tx, int32_t ty, uint16_t entry)
{
    const size_t i = slot_of(w, tx, ty);
    uint16_t *h = hidden(w);
    if (h[i] == entry)
        return;
    h[i] = entry;
    const uint32_t bit = (uint32_t)1 << (i & 31);
    if (!(w->dirty[i >> 5] & bit)) {
        w->dirty[i >> 5] |= bit;
        w->dirty_count++;
    }
}

static void put_from_map(tg_world_t *w, int32_t tx, int32_t ty)
{
    put_slot(w, tx, ty, big_get(w, tx, ty));
}

static void refill(tg_world_t *w)
{
    for (int32_t ty = w->win_ty; ty < w->win_ty + win_h(w); ty++)
        for (int32_t tx = w->win_tx; tx < w->win_tx + win_w(w); tx++)
            put_from_map(w, tx, ty);
}

/* Move the window to a new origin: load what comes in (a whole refill if nothing of the old window stays). */
static void move_window(tg_world_t *w, int32_t tx, int32_t ty)
{
    const int32_t W = win_w(w), H = win_h(w);
    const int64_t dx = (int64_t)tx - w->win_tx, dy = (int64_t)ty - w->win_ty;
    w->win_tx = tx;
    w->win_ty = ty;
    if (dx >= W || -dx >= W || dy >= H || -dy >= H) {
        refill(w);
        return;
    }
    if (dx != 0) {
        const int32_t first = dx > 0 ? tx + W - (int32_t)dx : tx, last = dx > 0 ? tx + W : tx - (int32_t)dx;
        for (int32_t gx = first; gx < last; gx++)
            for (int32_t gy = ty; gy < ty + H; gy++)
                put_from_map(w, gx, gy);
    }
    if (dy != 0) {
        const int32_t first = dy > 0 ? ty + H - (int32_t)dy : ty, last = dy > 0 ? ty + H : ty - (int32_t)dy;
        for (int32_t gy = first; gy < last; gy++)
            for (int32_t gx = tx; gx < tx + W; gx++)
                put_from_map(w, gx, gy);
    }
}

/* Where the window's origin is for a camera: centred, so that the visible tiles are inside it. */
static void window_origin(const tg_world_t *w, int32_t cam_x, int32_t cam_y, int32_t *tx, int32_t *ty)
{
    const int32_t vis_w = (TG_WIDTH >> w->tile_log2) + 1, vis_h = (TG_HEIGHT >> w->tile_log2) + 1;
    *tx = floor_shift(cam_x, w->tile_log2) - (win_w(w) - vis_w) / 2;
    *ty = floor_shift(cam_y, w->tile_log2) - (win_h(w) - vis_h) / 2;
}

bool tg_world_init(tg_world_t *w, tg_layer_t *layer, uint16_t *win_a, uint16_t *win_b, uint32_t *dirty,
                   uint16_t *map, int32_t map_w, int32_t map_h)
{
    if (!w || !layer || !win_a || !win_b || !dirty || !map || map_w <= 0 || map_h <= 0 || layer->kind != TG_LAYER_TILES)
        return false;
    const unsigned tl = layer->tile_log2, wl = layer->map_w_log2, hl = layer->map_h_log2;
    if (tl > 4 || wl > 15 || hl > 15 || wl + hl < 5)
        return false;
    if ((int32_t)1 << wl < (TG_WIDTH >> tl) + 1 || (int32_t)1 << hl < (TG_HEIGHT >> tl) + 1)
        return false;

    memset(w, 0, sizeof *w);
    w->layer = layer;
    w->win[0] = win_a;
    w->win[1] = win_b;
    w->dirty = dirty;
    w->dirty_words = (uint32_t)TG_WORLD_DIRTY_WORDS(wl, hl);
    w->w_log2 = (uint8_t)wl;
    w->h_log2 = (uint8_t)hl;
    w->tile_log2 = (uint8_t)tl;
    w->map = map;
    w->map_w = map_w;
    w->map_h = map_h;
    w->border = TG_BORDER_TILE;
    window_origin(w, 0, 0, &w->win_tx, &w->win_ty);

    memset(dirty, 0, w->dirty_words * sizeof *dirty);
    refill(w);                                 /* into win[1] ... */
    memcpy(win_a, win_b, TG_WORLD_WIN_ENTRIES(wl, hl) * sizeof *win_a);   /* ... and the shown one is the same */
    memset(dirty, 0, w->dirty_words * sizeof *dirty);
    w->dirty_count = 0;
    w->shown = 0;
    w->open = 1;
    layer->map = win_a;
    layer->scroll_x = layer->scroll_y = 0;
    return true;
}

static void ensure_open(tg_world_t *w)
{
    if (!w->open)
        tg_world_begin_frame(w);
}

void tg_world_begin_frame(tg_world_t *w)
{
    if (w->open)
        return;
    w->open = 1;
    if (w->dirty_count == 0)
        return;
    const uint16_t *src = w->win[w->shown];
    uint16_t *dst = hidden(w);
    for (uint32_t k = 0; k < w->dirty_words; k++) {
        uint32_t bits = w->dirty[k];
        if (!bits)
            continue;
        w->dirty[k] = 0;
        while (bits) {
            const size_t i = ((size_t)k << 5) + ctz32(bits);
            bits &= bits - 1;
            dst[i] = src[i];
        }
    }
    w->dirty_count = 0;
}

void tg_world_end_frame(tg_world_t *w)
{
    if (!w->open)
        return;
    w->shown ^= 1;
    w->layer->map = w->win[w->shown];
    w->layer->scroll_x = (uint32_t)w->cam_x;
    w->layer->scroll_y = (uint32_t)w->cam_y;
    w->open = 0;
}

void tg_world_set_camera(tg_world_t *w, int32_t x, int32_t y)
{
    ensure_open(w);
    w->cam_x = x;
    w->cam_y = y;
    int32_t tx, ty;
    window_origin(w, x, y, &tx, &ty);
    if (tx != w->win_tx || ty != w->win_ty)
        move_window(w, tx, ty);
}

void tg_world_set_border(tg_world_t *w, tg_border_t border, uint16_t entry)
{
    ensure_open(w);
    w->border = border;
    w->border_entry = entry;
    refill(w);
}

void tg_world_set_flags(tg_world_t *w, const uint8_t *flags)
{
    w->flags = flags;
}

void tg_world_reload(tg_world_t *w)
{
    ensure_open(w);
    refill(w);
}

uint16_t tg_world_get(const tg_world_t *w, int32_t tx, int32_t ty)
{
    return big_get(w, tx, ty);
}

void tg_world_set(tg_world_t *w, int32_t tx, int32_t ty, uint16_t entry)
{
    ensure_open(w);
    const bool inside = (uint32_t)tx < (uint32_t)w->map_w && (uint32_t)ty < (uint32_t)w->map_h;
    if (!inside && w->border != TG_BORDER_WRAP)
        return;
    const int32_t mx = resolve(w->border, tx, w->map_w), my = resolve(w->border, ty, w->map_h);
    w->map[(size_t)my * (size_t)w->map_w + (size_t)mx] = entry;

    if (w->border == TG_BORDER_TILE) {
        if (resident(w, tx, ty))
            put_slot(w, tx, ty, entry);
        return;
    }
    /* With clamp and wrap the tile can be on screen more than once: update every place in the window that stands for it. */
    for (int32_t gy = w->win_ty; gy < w->win_ty + win_h(w); gy++) {
        if (resolve(w->border, gy, w->map_h) != my)
            continue;
        for (int32_t gx = w->win_tx; gx < w->win_tx + win_w(w); gx++)
            if (resolve(w->border, gx, w->map_w) == mx)
                put_slot(w, gx, gy, entry);
    }
}

void tg_world_fill(tg_world_t *w, int32_t tx, int32_t ty, int32_t cols, int32_t rows, uint16_t entry)
{
    for (int32_t r = 0; r < rows; r++)
        for (int32_t c = 0; c < cols; c++)
            tg_world_set(w, tx + c, ty + r, entry);
}

void tg_world_blit(tg_world_t *w, int32_t tx, int32_t ty, const uint16_t *src, int32_t cols, int32_t rows, int32_t src_stride)
{
    for (int32_t r = 0; r < rows; r++)
        for (int32_t c = 0; c < cols; c++)
            tg_world_set(w, tx + c, ty + r, src[(size_t)r * (size_t)src_stride + (size_t)c]);
}

uint8_t tg_world_flags_tile(const tg_world_t *w, int32_t tx, int32_t ty)
{
    return w->flags ? w->flags[big_get(w, tx, ty) & TG_TILE_MASK] : 0;
}

uint8_t tg_world_flags_px(const tg_world_t *w, int32_t x, int32_t y)
{
    return tg_world_flags_tile(w, floor_shift(x, w->tile_log2), floor_shift(y, w->tile_log2));
}

uint8_t tg_world_flags_box(const tg_world_t *w, int32_t x, int32_t y, int32_t width, int32_t height)
{
    if (width <= 0 || height <= 0 || !w->flags)
        return 0;
    const int32_t tx0 = floor_shift(x, w->tile_log2), tx1 = floor_shift(x + width - 1, w->tile_log2);
    const int32_t ty0 = floor_shift(y, w->tile_log2), ty1 = floor_shift(y + height - 1, w->tile_log2);
    uint8_t f = 0;
    for (int32_t ty = ty0; ty <= ty1; ty++)
        for (int32_t tx = tx0; tx <= tx1; tx++)
            f |= tg_world_flags_tile(w, tx, ty);
    return f;
}
