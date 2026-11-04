/* SPDX-License-Identifier: MIT */
#include "tg_actor.h"

#include <string.h>

static int32_t floor_shift(int32_t v, unsigned n)
{
    return v >= 0 ? v >> n : ~(~v >> n);
}

static int32_t saturate(int64_t v)
{
    return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}

static int32_t px_of(int32_t v) { return floor_shift(v, 8); }

void tg_actors_init(tg_actors_t *p, tg_actor_t *storage, uint16_t n, uint16_t *order)
{
    memset(p, 0, sizeof *p);
    memset(storage, 0, n * sizeof *storage);
    p->a = storage;
    p->order = order;
    p->n = n;
    p->sprite_log2 = 4;
}

int tg_actor_alloc(tg_actors_t *p)
{
    for (int i = 0; i < p->n; i++)
        if (!(p->a[i].flags & TG_ACTOR_ACTIVE)) {
            p->a[i] = (tg_actor_t){.cols = 1, .rows = 1, .flags = TG_ACTOR_ACTIVE};
            return i;
        }
    return -1;
}

void tg_actor_free(tg_actors_t *p, int id)
{
    if (tg_actor(p, id))
        p->a[id].flags = 0;
}

tg_actor_t *tg_actor(tg_actors_t *p, int id)
{
    return (id >= 0 && id < p->n && (p->a[id].flags & TG_ACTOR_ACTIVE)) ? &p->a[id] : NULL;
}

void tg_actors_step(tg_actors_t *p, int frames)
{
    if (frames <= 0)
        return;
    for (int i = 0; i < p->n; i++) {
        tg_actor_t *a = &p->a[i];
        if (!(a->flags & TG_ACTOR_ACTIVE))
            continue;
        a->x = saturate((int64_t)a->x + (int64_t)a->vx * frames);
        a->y = saturate((int64_t)a->y + (int64_t)a->vy * frames);
    }
}

static int grid(uint8_t v) { return v < 1 ? 1 : v > 4 ? 4 : v; }

/* The collision box: from the actor's pixel position by (ox, oy), bw x bh. */
static void hitbox(const tg_actors_t *p, const tg_actor_t *a, int32_t *ox, int32_t *oy, int32_t *bw, int32_t *bh)
{
    if (a->hit_w && a->hit_h) {
        *ox = a->hit_x;
        *oy = a->hit_y;
        *bw = a->hit_w;
        *bh = a->hit_h;
    } else {
        *ox = *oy = 0;
        *bw = grid(a->cols) << p->sprite_log2;
        *bh = grid(a->rows) << p->sprite_log2;
    }
}

/* Move along one axis, a tile or less at a time so that no tile can be jumped over; returns the TG_HIT_* side that stopped it. */
static uint8_t move_axis(const tg_actors_t *p, tg_actor_t *a, const tg_world_t *w, int32_t d, bool horizontal)
{
    const unsigned tl = w->tile_log2;
    const int32_t ts = (int32_t)1 << tl;
    int32_t ox, oy, bw, bh;
    hitbox(p, a, &ox, &oy, &bw, &bh);
    int32_t *pos = horizontal ? &a->x : &a->y;

    while (d != 0) {
        const int32_t limit = ts * 256;
        const int32_t step = d > limit ? limit : d < -limit ? -limit : d;
        d -= step;
        const int32_t next = saturate((int64_t)*pos + step);
        const int32_t old_px = px_of(*pos), new_px = px_of(next);
        if (new_px == old_px) {
            *pos = next;
            continue;
        }
        const int32_t bx = (horizontal ? new_px : px_of(a->x)) + ox, by = (horizontal ? px_of(a->y) : new_px) + oy;
        bool blocked = (tg_world_flags_box(w, bx, by, bw, bh) & TG_TILE_SOLID) != 0;
        if (!horizontal && step > 0 && !blocked) {
            /* one-way: the bottom edge came into a row of tiles from above */
            const int32_t old_bottom = old_px + oy + bh, row_top = floor_shift(by + bh - 1, tl) * ts;
            blocked = old_bottom <= row_top && (tg_world_flags_box(w, bx, by + bh - 1, bw, 1) & TG_TILE_ONEWAY) != 0;
        }
        if (!blocked) {
            *pos = next;
            continue;
        }
        /* flush against the tile that stopped it; never backwards */
        int32_t flush;
        uint8_t hit;
        if (horizontal && step > 0) {
            flush = floor_shift(bx + bw - 1, tl) * ts - bw - ox;
            hit = TG_HIT_RIGHT;
        } else if (horizontal) {
            flush = (floor_shift(bx, tl) + 1) * ts - ox;
            hit = TG_HIT_LEFT;
        } else if (step > 0) {
            flush = floor_shift(by + bh - 1, tl) * ts - bh - oy;
            hit = TG_HIT_DOWN;
        } else {
            flush = (floor_shift(by, tl) + 1) * ts - oy;
            hit = TG_HIT_UP;
        }
        if (step > 0 ? flush > old_px : flush < old_px)
            *pos = flush * 256;
        return hit;
    }
    return 0;
}

uint8_t tg_actor_move(tg_actors_t *p, int id, const tg_world_t *w, int32_t dx, int32_t dy)
{
    tg_actor_t *a = tg_actor(p, id);
    if (!a)
        return 0;
    uint8_t hit = move_axis(p, a, w, dx, true);
    hit |= move_axis(p, a, w, dy, false);
    return hit;
}

bool tg_actors_overlap(const tg_actors_t *p, int ia, int ib)
{
    if (ia < 0 || ib < 0 || ia >= p->n || ib >= p->n)
        return false;
    const tg_actor_t *a = &p->a[ia], *b = &p->a[ib];
    if (!(a->flags & TG_ACTOR_ACTIVE) || !(b->flags & TG_ACTOR_ACTIVE))
        return false;
    int32_t ax, ay, aw, ah, bx, by, bw, bh;
    hitbox(p, a, &ax, &ay, &aw, &ah);
    hitbox(p, b, &bx, &by, &bw, &bh);
    ax += px_of(a->x);
    ay += px_of(a->y);
    bx += px_of(b->x);
    by += px_of(b->y);
    return (int64_t)ax < (int64_t)bx + bw && (int64_t)bx < (int64_t)ax + aw && (int64_t)ay < (int64_t)by + bh && (int64_t)by < (int64_t)ay + ah;
}

/* Is the actor drawn: on screen (screen position of its top left in *sx, *sy) and with the sprite priority in *key. */
static bool on_screen(const tg_actors_t *p, const tg_actor_t *a, int32_t cam_x, int32_t cam_y, int32_t *sx, int32_t *sy, uint8_t *key)
{
    if ((a->flags & (TG_ACTOR_ACTIVE | TG_ACTOR_HIDDEN)) != TG_ACTOR_ACTIVE)
        return false;
    const int64_t w = (int64_t)grid(a->cols) << p->sprite_log2, h = (int64_t)grid(a->rows) << p->sprite_log2;
    const int64_t x = (int64_t)px_of(a->x) - cam_x, y = (int64_t)px_of(a->y) - cam_y;
    if (x + w <= 0 || x >= TG_WIDTH || y + h <= 0 || y >= TG_HEIGHT)
        return false;
    *sx = (int32_t)x;
    *sy = (int32_t)y;
    if (p->flags & TG_ACTORS_YSORT) {
        const int64_t bottom = y + h;
        *key = (uint8_t)(255 - (bottom < 0 ? 0 : bottom > 255 ? 255 : bottom));
    } else {
        *key = a->prio;
    }
    return true;
}

/* Is the tile (row, col) of the grid, at (x, y) on the screen, itself on the screen? */
static bool part_visible(int32_t x, int32_t y, int32_t sz)
{
    return x + sz > 0 && x < TG_WIDTH && y + sz > 0 && y < TG_HEIGHT;
}

int tg_actors_draw(tg_actors_t *p, tg_t *g, int32_t cam_x, int32_t cam_y)
{
    p->sprite_log2 = g->sprite_tile_log2;
    const int32_t sz = (int32_t)1 << p->sprite_log2;

    for (int i = 0; i < p->used_count; i++)
        tg_sprite_free(g, p->used[i]);
    p->used_count = 0;

    /* order the visible actors by priority (a counting sort: stable, so ties go by number) */
    int32_t sx, sy;
    uint8_t key;
    memset(p->hist, 0, sizeof p->hist);
    int visible = 0;
    for (int i = 0; i < p->n; i++)
        if (on_screen(p, &p->a[i], cam_x, cam_y, &sx, &sy, &key)) {
            p->hist[key]++;
            visible++;
        }
    uint16_t at = 0;
    for (int k = 0; k < 256; k++) {
        const uint16_t c = p->hist[k];
        p->hist[k] = at;
        at = (uint16_t)(at + c);
    }
    for (int i = 0; i < p->n; i++)
        if (on_screen(p, &p->a[i], cam_x, cam_y, &sx, &sy, &key))
            p->order[p->hist[key]++] = (uint16_t)i;

    int drawn = 0;
    for (int k = 0; k < visible; k++) {
        const tg_actor_t *a = &p->a[p->order[k]];
        on_screen(p, a, cam_x, cam_y, &sx, &sy, &key);
        const int cols = grid(a->cols), rows = grid(a->rows);
        int parts = 0;
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < cols; c++)
                parts += part_visible(sx + c * sz, sy + r * sz, sz);
        if (parts > TG_MAX_SPRITES - g->sprite_count)
            continue;
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < cols; c++) {
                const int32_t x = sx + c * sz, y = sy + r * sz;
                if (!part_visible(x, y, sz))
                    continue;
                const int slot = tg_sprite_alloc(g);
                const int rr = (a->flags & TG_ACTOR_VFLIP) ? rows - 1 - r : r, cc = (a->flags & TG_ACTOR_HFLIP) ? cols - 1 - c : c;
                g->sprites[slot] = (tg_sprite_t){.x = (int16_t)x, .y = (int16_t)y, .tile = (uint16_t)(a->tile + rr * cols + cc),
                                                 .flags = (uint8_t)(TG_SPRITE_ON | ((a->flags & TG_ACTOR_HFLIP) ? TG_SPRITE_HFLIP : 0) |
                                                                    ((a->flags & TG_ACTOR_VFLIP) ? TG_SPRITE_VFLIP : 0)),
                                                 .pal = a->pal, .above = a->above};
                tg_sprite_set_priority(g, slot, key);
                p->used[p->used_count++] = (uint8_t)slot;
            }
        drawn++;
    }
    return drawn;
}
