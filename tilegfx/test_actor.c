/* SPDX-License-Identifier: MIT
 * Headless checks for tg_actor: the pool, the velocity step, drawing (against a reference that builds the sprites by hand, compared through
 * the compositor), and collisions (against a pixel-by-pixel reference on a model of the map). Exits non-zero on the first failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tg_actor.h"

static int checks;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        checks++;                                                                      \
        if (!(cond)) {                                                                 \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                     \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

static uint32_t rng = 246813579u;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
static int32_t rr(int32_t lo, int32_t hi) { return lo + (int32_t)(rnd() % (uint32_t)(hi - lo + 1)); }   /* lo .. hi */
static int32_t floordiv(int32_t v, int32_t s) { const int32_t q = v / s; return (v % s != 0 && v < 0) ? q - 1 : q; }
static int32_t posmod(int32_t v, int32_t s) { return ((v % s) + s) % s; }

#define NT 64

static uint8_t *make_tiles(unsigned log2, unsigned depth)
{
    const size_t px = (size_t)1 << (2 * log2);
    uint8_t *packed = calloc(NT, px);
    for (size_t t = 0; t < NT; t++)
        for (size_t p = 0; p < px; p++) {
            const uint8_t v = rnd() & 3 ? (uint8_t)(1 + rnd() % (depth == 8 ? 255 : 15)) : 0;
            if (depth == 8)
                packed[t * px + p] = v;
            else
                packed[(t * px + p) / 2] |= (p & 1) ? v : (uint8_t)(v << 4);
        }
    return packed;
}

/* ---- the pool ---- */

static void test_pool(void)
{
    tg_actor_t store[8];
    uint16_t order[32];
    tg_actors_t p;
    tg_actors_init(&p, store, 8, order);
    int ids[8];
    for (int i = 0; i < 8; i++) {
        ids[i] = tg_actor_alloc(&p);
        CHECK(ids[i] == i);
        CHECK(tg_actor(&p, i) && tg_actor(&p, i)->cols == 1 && tg_actor(&p, i)->rows == 1 && tg_actor(&p, i)->x == 0);
    }
    CHECK(tg_actor_alloc(&p) == -1);
    tg_actor(&p, 3)->x = 12345;
    tg_actor_free(&p, 3);
    CHECK(tg_actor(&p, 3) == NULL);
    tg_actor_free(&p, 3);                      /* twice, and out of range: harmless */
    tg_actor_free(&p, -1);
    tg_actor_free(&p, 8);
    CHECK(tg_actor(&p, -1) == NULL && tg_actor(&p, 8) == NULL);
    CHECK(tg_actor_alloc(&p) == 3);            /* the lowest free number again, and cleared */
    CHECK(tg_actor(&p, 3)->x == 0);
    CHECK(tg_actor_alloc(&p) == -1);

    /* step: the velocity for each frame that went by, inactive actors stay, and the sum does not wrap */
    tg_actor_free(&p, 5);
    tg_actor(&p, 0)->vx = 300; tg_actor(&p, 0)->vy = -70;
    store[5].vx = 1000;                        /* inactive */
    tg_actor(&p, 1)->x = INT32_MAX - 100; tg_actor(&p, 1)->vx = 1000;
    tg_actor(&p, 2)->y = INT32_MIN + 5; tg_actor(&p, 2)->vy = -1000;
    tg_actors_step(&p, 3);
    CHECK(tg_actor(&p, 0)->x == 900 && tg_actor(&p, 0)->y == -210);
    CHECK(store[5].x == 0);
    CHECK(tg_actor(&p, 1)->x == INT32_MAX && tg_actor(&p, 2)->y == INT32_MIN);
    tg_actors_step(&p, 0);
    tg_actors_step(&p, -2);
    CHECK(tg_actor(&p, 0)->x == 900);

    /* a world that wraps: positions stay within it */
    CHECK(!tg_actors_set_wrap(&p, 100, 0) && p.wrap_w == 0);               /* too narrow: nothing changes */
    CHECK(!tg_actors_set_wrap(&p, 300, 159) && p.wrap_w == 0 && p.wrap_h == 0);
    CHECK(!tg_actors_set_wrap(&p, 8388608, 0));
    CHECK(tg_actors_set_wrap(&p, 192, 160) && p.wrap_w == 192 && p.wrap_h == 160);
    CHECK(tg_actors_set_wrap(&p, 300, 200));
    tg_actor(&p, 0)->x = 299 * 256; tg_actor(&p, 0)->vx = 512;
    tg_actor(&p, 0)->y = 5 * 256;   tg_actor(&p, 0)->vy = -10 * 256;
    tg_actors_step(&p, 1);
    CHECK(tg_actor(&p, 0)->x == 256 && tg_actor(&p, 0)->y == 195 * 256);
    tg_actor(&p, 0)->vx = -1000000; tg_actor(&p, 0)->vy = 1000000;
    for (int k = 0; k < 50; k++) {
        tg_actors_step(&p, 3);
        CHECK(tg_actor(&p, 0)->x >= 0 && tg_actor(&p, 0)->x < 300 * 256 && tg_actor(&p, 0)->y >= 0 && tg_actor(&p, 0)->y < 200 * 256);
    }
    CHECK(tg_actors_set_wrap(&p, 0, 0));
}

/* ---- drawing ---- */

typedef struct { int32_t sx, sy; uint8_t key; int id; } vis_t;

/* The sprites an actor pool should produce, built by hand in a fresh tg_t with the same foreign sprite; returns the number of actors drawn. */
static int reference_draw(tg_t *rg, const tg_actor_t *acts, int n, int32_t cam_x, int32_t cam_y, bool ysort, int32_t sz, int32_t wrap_w, int32_t wrap_h)
{
    static vis_t v[1024];
    int count = 0;
    for (int i = 0; i < n; i++) {
        const tg_actor_t *a = &acts[i];
        if (!(a->flags & TG_ACTOR_ACTIVE) || (a->flags & TG_ACTOR_HIDDEN))
            continue;
        const int32_t w = a->cols * sz, h = a->rows * sz;
        int32_t rx = floordiv(a->x, 256) - cam_x, ry = floordiv(a->y, 256) - cam_y;
        if (wrap_w) rx = posmod(rx + w, wrap_w) - w;                 /* seen from -w to the world's width - w */
        if (wrap_h) ry = posmod(ry + h, wrap_h) - h;
        for (int copy = 0; copy < 4; copy++) {                       /* and, in a world that wraps, one more a world on */
            if (((copy & 1) && !wrap_w) || ((copy & 2) && !wrap_h))
                continue;
            const int32_t sx = rx + ((copy & 1) ? wrap_w : 0), sy = ry + ((copy & 2) ? wrap_h : 0);
            if (sx + w <= 0 || sx >= TG_WIDTH || sy + h <= 0 || sy >= TG_HEIGHT)
                continue;
            int32_t key = a->prio;
            if (ysort) {
                int32_t bottom = sy + h;
                bottom = bottom < 0 ? 0 : bottom > 255 ? 255 : bottom;
                key = 255 - bottom;
            }
            /* insertion into the list, ordered by key and then by number and copy */
            int k = count++;
            while (k > 0 && v[k - 1].key > key) {
                v[k] = v[k - 1];
                k--;
            }
            v[k] = (vis_t){sx, sy, (uint8_t)key, i};
        }
    }
    int drawn = 0;
    for (int k = 0; k < count; k++) {
        const tg_actor_t *a = &acts[v[k].id];
        int parts = 0;
        for (int r = 0; r < a->rows; r++)
            for (int c = 0; c < a->cols; c++) {
                const int32_t x = v[k].sx + c * sz, y = v[k].sy + r * sz;
                parts += x + sz > 0 && x < TG_WIDTH && y + sz > 0 && y < TG_HEIGHT;
            }
        if (parts > TG_MAX_SPRITES - rg->sprite_count)
            continue;
        for (int r = 0; r < a->rows; r++)
            for (int c = 0; c < a->cols; c++) {
                const int32_t x = v[k].sx + c * sz, y = v[k].sy + r * sz;
                if (!(x + sz > 0 && x < TG_WIDTH && y + sz > 0 && y < TG_HEIGHT))
                    continue;
                const int s = tg_sprite_alloc(rg);
                const bool hf = a->flags & TG_ACTOR_HFLIP, vf = a->flags & TG_ACTOR_VFLIP;
                tg_sprite_t *sp = &rg->sprites[s];
                sp->x = (int16_t)x;
                sp->y = (int16_t)y;
                sp->tile = (uint16_t)(a->tile + (vf ? a->rows - 1 - r : r) * a->cols + (hf ? a->cols - 1 - c : c));
                sp->flags = (uint8_t)(TG_SPRITE_ON | (hf ? TG_SPRITE_HFLIP : 0) | (vf ? TG_SPRITE_VFLIP : 0));
                sp->pal = a->pal;
                sp->above = a->above;
                tg_sprite_set_priority(rg, s, v[k].key);
            }
        drawn++;
    }
    return drawn;
}

static void same_picture(tg_t *a, tg_t *b)
{
    uint8_t la[TG_WIDTH], lb[TG_WIDTH];
    for (int line = 0; line < TG_HEIGHT; line++) {
        tg_render_line_indices(a, line, la);
        tg_render_line_indices(b, line, lb);
        CHECK(memcmp(la, lb, TG_WIDTH) == 0);
    }
}

static void setup_sprites(tg_t *g, unsigned sl, unsigned depth, const uint8_t *tiles)
{
    tg_init(g);
    g->sprite_tile_log2 = (uint8_t)sl;
    g->sprite_depth = (uint8_t)depth;
    g->sprite_tiles = tiles;
    const int s = tg_sprite_alloc(g);          /* a sprite of the program's own: the actors must leave it alone */
    g->sprites[s] = (tg_sprite_t){.x = 20, .y = 30, .tile = 1, .flags = TG_SPRITE_ON};
}

/* wrap_w, wrap_h: the world's size in pixels if it wraps (0: not) */
static void test_draw(unsigned sl, unsigned depth, bool ysort, uint16_t cap, int scenes, int32_t wrap_w, int32_t wrap_h)
{
    const int32_t sz = 1 << sl;
    uint8_t *tiles = make_tiles(sl, depth);
    tg_actor_t *store = calloc(cap, sizeof *store);
    uint16_t *order = calloc(4 * (size_t)cap, sizeof *order);
    tg_actors_t p;
    tg_actors_init(&p, store, cap, order);
    p.flags = ysort ? TG_ACTORS_YSORT : 0;
    CHECK(tg_actors_set_wrap(&p, wrap_w, wrap_h));
    tg_t *g = malloc(sizeof *g), *rg = malloc(sizeof *rg);
    setup_sprites(g, sl, depth, tiles);

    for (int sc = 0; sc < scenes; sc++) {
        for (int i = 0; i < cap; i++)
            tg_actor_free(&p, i);
        const int count = cap > 100 ? rr(100, cap) : rr(1, cap);
        const int32_t cam_x = (wrap_w || wrap_h) ? rr(-100000, 100000) : rr(-300, 500), cam_y = (wrap_w || wrap_h) ? rr(-100000, 100000) : rr(-300, 500);
        for (int k = 0; k < count; k++) {
            const int id = tg_actor_alloc(&p);
            CHECK(id == k);
            tg_actor_t *a = tg_actor(&p, id);
            a->x = (cam_x + rr(-70, 350) + (wrap_w ? rr(-3, 3) * wrap_w : 0)) * 256 + rr(0, 255);      /* positions need not be within one world */
            a->y = (cam_y + rr(-70, 270) + (wrap_h ? rr(-3, 3) * wrap_h : 0)) * 256 + rr(0, 255);
            a->cols = (uint8_t)rr(1, cap > 100 ? 2 : 4);
            a->rows = (uint8_t)rr(1, cap > 100 ? 2 : 4);
            a->tile = (uint16_t)rr(0, NT - 16);
            a->pal = (uint8_t)rr(0, 15);
            a->above = (uint8_t)rr(0, 4);
            a->prio = (uint8_t)rr(0, 6);
            if (rnd() % 3 == 0) a->flags |= TG_ACTOR_HFLIP;
            if (rnd() % 3 == 0) a->flags |= TG_ACTOR_VFLIP;
            if (rnd() % 8 == 0) a->flags |= TG_ACTOR_HIDDEN;
        }
        /* draw a few times with different cameras: the sprites of the last draw must be given back each time */
        for (int pass = 0; pass < 3; pass++) {
            const int32_t cx = pass == 0 ? cam_x : cam_x + rr(-40, 40), cy = pass == 0 ? cam_y : cam_y + rr(-40, 40);
            const int drawn = tg_actors_draw(&p, g, cx, cy);
            setup_sprites(rg, sl, depth, tiles);
            const int rdrawn = reference_draw(rg, store, cap, cx, cy, ysort, sz, wrap_w, wrap_h);
            CHECK(drawn == rdrawn);
            CHECK(g->sprite_count == rg->sprite_count);
            CHECK(g->sprites[0].x == 20 && g->sprites[0].flags == TG_SPRITE_ON);   /* the program's sprite is still there */
            same_picture(g, rg);
        }
    }
    free(tiles); free(store); free(order); free(g); free(rg);
}

/* More sprites wanted than there are: it is the back-most actors that go without. */
static void test_exhaustion(void)
{
    uint8_t *tiles = make_tiles(4, 8);
    tg_actor_t store[200];
    uint16_t order[800];
    tg_actors_t p;
    tg_actors_init(&p, store, 200, order);
    tg_t *g = malloc(sizeof *g);
    setup_sprites(g, 4, 8, tiles);
    for (int i = 0; i < 150; i++) {
        const int id = tg_actor_alloc(&p);
        tg_actor(&p, id)->x = (i % 20) * 16 * 256;
        tg_actor(&p, id)->y = (i / 20) * 16 * 256;
        tg_actor(&p, id)->prio = (uint8_t)(i < 100 ? 10 : 5);      /* the last 50 are in front of the first 100 */
    }
    const int drawn = tg_actors_draw(&p, g, 0, 0);
    CHECK(drawn == 127);                       /* 128 slots, one is the program's */
    CHECK(g->sprite_count == 128);
    /* the 50 in front are all drawn, and of the 100 behind the first 77 by number */
    int in_front = 0, behind = 0;
    for (int k = 0; k < p.used_count; k++) {
        const tg_sprite_t *s = &g->sprites[p.used[k]];
        const int i = (s->y / 16) * 20 + s->x / 16;
        if (i >= 100) in_front++; else { behind++; CHECK(i < 77); }
    }
    CHECK(in_front == 50 && behind == 77);

    /* an actor whose tiles do not all fit is left out, and a smaller one behind it still gets its slot */
    for (int i = 0; i < 200; i++)
        tg_actor_free(&p, i);
    for (int i = 0; i < 126; i++) {
        const int id = tg_actor_alloc(&p);
        tg_actor(&p, id)->x = (i % 20) * 16 * 256;
        tg_actor(&p, id)->y = (i / 20) * 16 * 256;
    }
    int big = tg_actor_alloc(&p);              /* 2 x 2: needs 4 of the 2 slots left */
    tg_actor(&p, big)->cols = tg_actor(&p, big)->rows = 2;
    tg_actor(&p, big)->prio = 1;
    int small = tg_actor_alloc(&p);
    tg_actor(&p, small)->prio = 2;
    /* both behind the 126 plain ones (prio 0): the big one does not fit, the small one does */
    CHECK(tg_actors_draw(&p, g, 0, 0) == 127);
    CHECK(g->sprite_count == 128);
    free(tiles); free(g);
}

/* ---- collisions, against a model of the map ---- */

typedef struct {
    uint16_t *m;
    int32_t w, h;
    tg_border_t border;
    uint16_t be;
    uint8_t flags[TG_TILES_MAX];
} model_t;

static uint16_t model_get(const model_t *md, int32_t tx, int32_t ty)
{
    if (tx >= 0 && tx < md->w && ty >= 0 && ty < md->h)
        return md->m[ty * md->w + tx];
    switch (md->border) {
    case TG_BORDER_CLAMP: {
        const int32_t cx = tx < 0 ? 0 : tx >= md->w ? md->w - 1 : tx, cy = ty < 0 ? 0 : ty >= md->h ? md->h - 1 : ty;
        return md->m[cy * md->w + cx];
    }
    case TG_BORDER_WRAP:
        return md->m[posmod(ty, md->h) * md->w + posmod(tx, md->w)];
    default:
        return md->be;
    }
}

static uint8_t pixel_flags(const model_t *md, unsigned tl, int32_t x, int32_t y)
{
    return md->flags[model_get(md, floordiv(x, 1 << tl), floordiv(y, 1 << tl)) & TG_TILE_MASK];
}

static bool box_solid(const model_t *md, unsigned tl, int32_t x, int32_t y, int32_t w, int32_t h)
{
    for (int32_t py = y; py < y + h; py++)
        for (int32_t px = x; px < x + w; px++)
            if (pixel_flags(md, tl, px, py) & TG_TILE_SOLID)
                return true;
    return false;
}

/* Would a one pixel step down put the bottom edge of the box into a one-way tile? (Its bottom pixel row is the first row of a tile.) */
static bool oneway_step(const model_t *md, unsigned tl, int32_t x, int32_t y, int32_t w, int32_t h)   /* box after the step */
{
    const int32_t yb = y + h - 1;
    if (posmod(yb, 1 << tl) != 0)
        return false;
    for (int32_t px = x; px < x + w; px++)
        if (pixel_flags(md, tl, px, yb) & TG_TILE_ONEWAY)
            return true;
    return false;
}

static bool step_blocked(const model_t *md, unsigned tl, int32_t x, int32_t y, int32_t w, int32_t h, int dx, int dy)   /* box before the step */
{
    if (box_solid(md, tl, x + dx, y + dy, w, h))
        return true;
    return dy > 0 && oneway_step(md, tl, x + dx, y + dy, w, h);
}

static int64_t posmod64(int64_t v, int64_t m) { const int64_t r = v % m; return r < 0 ? r + m : r; }

/* after - before, as the shortest way round if the world (size_px pixels, 24.8) wraps */
static int64_t sdelta(int32_t after, int32_t before, int32_t size_px)
{
    int64_t d = (int64_t)after - before;
    if (size_px) {
        const int64_t m = (int64_t)size_px * 256;
        d = posmod64(d + m / 2, m) - m / 2;
    }
    return d;
}

static void test_move(void)
{
    const unsigned tl = 3;
    static uint16_t win_a[64 * 32], win_b[64 * 32];
    static uint32_t dirty[TG_WORLD_DIRTY_WORDS(6, 5)];
    uint8_t *tiles = make_tiles(4, 8);
    uint16_t *big = malloc(60 * 40 * sizeof *big);
    model_t md = {.w = 60, .h = 40, .be = 5};
    md.m = malloc(60 * 40 * sizeof *md.m);
    for (int i = 0; i < NT; i++)
        md.flags[i] = (uint8_t)((rnd() % 4 == 0 ? TG_TILE_SOLID : 0) | (rnd() % 6 == 0 ? TG_TILE_ONEWAY : 0));
    for (int i = 0; i < 60 * 40; i++)
        md.m[i] = big[i] = (uint16_t)(rnd() % NT);

    tg_actor_t store[4];
    uint16_t order[16];
    tg_actors_t p;
    tg_actors_init(&p, store, 4, order);

    for (int border = 0; border < 3; border++) {
        md.border = (tg_border_t)border;
        tg_t g;
        tg_init(&g);
        g.layers[0] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = tl, .depth = 8, .map_w_log2 = 6, .map_h_log2 = 5, .tiles = tiles};
        tg_world_t w;
        CHECK(tg_world_init(&w, &g.layers[0], win_a, win_b, dirty, big, md.w, md.h));
        tg_world_set_border(&w, md.border, md.be);
        tg_world_set_flags(&w, md.flags);
        int32_t wx = 0, wy = 0;                /* the world repeats with TG_BORDER_WRAP: the actors know its size */
        CHECK(tg_world_wrap_px(&w, &wx, &wy) == (border == 2));
        if (border == 2)
            CHECK(wx == 480 && wy == 320);
        CHECK(tg_actors_set_wrap(&p, wx, wy));

        int moved = 0, blocked_x = 0, blocked_y = 0;
        for (int trial = 0; trial < 6000; trial++) {
            const int id = tg_actor_alloc(&p);
            CHECK(id == 0);
            tg_actor_t *a = tg_actor(&p, id);
            a->cols = (uint8_t)rr(1, 2);
            a->rows = (uint8_t)rr(1, 2);
            if (rnd() % 2) {                   /* a hit box inside the grid */
                a->hit_w = (uint8_t)rr(3, 12);
                a->hit_h = (uint8_t)rr(3, 12);
                a->hit_x = (uint8_t)rr(0, 4);
                a->hit_y = (uint8_t)rr(0, 4);
            }
            int32_t ox = a->hit_w ? a->hit_x : 0, oy = a->hit_w ? a->hit_y : 0;
            const int32_t bw = a->hit_w ? a->hit_w : a->cols * 16, bh = a->hit_w ? a->hit_h : a->rows * 16;
            /* a start where the box is free */
            int32_t px = 0, py = 0;
            bool ok = false;
            for (int tries = 0; tries < 50 && !ok; tries++) {
                px = rr(-40, 60 * 8 + 40);
                py = rr(-40, 40 * 8 + 40);
                ok = !box_solid(&md, tl, px + ox, py + oy, bw, bh);
            }
            if (!ok) {
                tg_actor_free(&p, id);
                continue;
            }
            const bool whole = rnd() % 2;      /* whole pixels: compared with a pixel-by-pixel reference; else the properties */
            a->x = px * 256 + (whole ? 0 : rr(0, 255));
            a->y = py * 256 + (whole ? 0 : rr(0, 255));
            const int32_t x0 = a->x, y0 = a->y;
            const int32_t dx = whole ? rr(-40, 40) * 256 : rr(-9000, 9000), dy = whole ? rr(-40, 40) * 256 : rr(-9000, 9000);
            const uint8_t hit = tg_actor_move(&p, id, &w, dx, dy);
            const int32_t fx = floordiv(a->x, 256), fy = floordiv(a->y, 256);

            CHECK(!box_solid(&md, tl, fx + ox, fy + oy, bw, bh));          /* never ends inside a solid tile */
            if (whole) {
                int32_t ex = px, ey = py;
                uint8_t ehit = 0;
                const int sx = dx > 0 ? 1 : -1;
                for (int k = 0; k < (dx > 0 ? dx : -dx) / 256; k++) {
                    if (step_blocked(&md, tl, ex + ox, ey + oy, bw, bh, sx, 0)) { ehit |= sx > 0 ? TG_HIT_RIGHT : TG_HIT_LEFT; break; }
                    ex += sx;
                }
                const int sy = dy > 0 ? 1 : -1;
                for (int k = 0; k < (dy > 0 ? dy : -dy) / 256; k++) {
                    if (step_blocked(&md, tl, ex + ox, ey + oy, bw, bh, 0, sy)) { ehit |= sy > 0 ? TG_HIT_DOWN : TG_HIT_UP; break; }
                    ey += sy;
                }
                const int32_t nx = wx ? posmod(ex, wx) : ex, ny = wy ? posmod(ey, wy) : ey;     /* the actor is kept within the world */
                CHECK(fx == nx && fy == ny && a->x == nx * 256 && a->y == ny * 256);
                CHECK(hit == ehit);
            } else {
                if (!(hit & (TG_HIT_LEFT | TG_HIT_RIGHT)))
                    CHECK(a->x == (wx ? (int32_t)posmod64((int64_t)x0 + dx, (int64_t)wx * 256) : x0 + dx));    /* nothing in the way: the whole move, fraction included */
                else {
                    const int s = (hit & TG_HIT_RIGHT) ? 1 : -1;
                    CHECK((dx > 0) == (s > 0));
                }
                if (!(hit & (TG_HIT_UP | TG_HIT_DOWN)))
                    CHECK(a->y == (wy ? (int32_t)posmod64((int64_t)y0 + dy, (int64_t)wy * 256) : y0 + dy));
                else
                    CHECK((dy > 0) == ((hit & TG_HIT_DOWN) != 0));
                /* stopping never moves it against the way it was going */
                if (hit & TG_HIT_RIGHT) CHECK(sdelta(a->x, x0, wx) >= 0);
                if (hit & TG_HIT_LEFT) CHECK(sdelta(a->x, x0, wx) <= 0);
                if (hit & TG_HIT_DOWN) CHECK(sdelta(a->y, y0, wy) >= 0);
                if (hit & TG_HIT_UP) CHECK(sdelta(a->y, y0, wy) <= 0);
                /* where it was stopped, one more pixel the same way is blocked (the y was moved after the x, so only the last axis is exact) */
                if (hit & TG_HIT_DOWN) CHECK(step_blocked(&md, tl, fx + ox, fy + oy, bw, bh, 0, 1));
                if (hit & TG_HIT_UP) CHECK(step_blocked(&md, tl, fx + ox, fy + oy, bw, bh, 0, -1));
            }
            moved += hit == 0;
            blocked_x += (hit & (TG_HIT_LEFT | TG_HIT_RIGHT)) != 0;
            blocked_y += (hit & (TG_HIT_UP | TG_HIT_DOWN)) != 0;
            tg_actor_free(&p, id);
        }
        CHECK(moved > 200 && blocked_x > 200 && blocked_y > 200);   /* the test did exercise free moves and both kinds of stop */
    }

    /* one-way: through from below and sideways, stopped from above; inside it already, it lets go */
    memset(md.flags, 0, sizeof md.flags);
    md.flags[1] = TG_TILE_ONEWAY;
    for (int i = 0; i < 60 * 40; i++)
        md.m[i] = big[i] = 0;
    md.border = TG_BORDER_TILE;
    md.be = 0;
    for (int x = 0; x < 10; x++)
        md.m[10 * 60 + x] = big[10 * 60 + x] = 1;                         /* a row of one-way tiles at y = 80 */
    tg_t g;
    tg_init(&g);
    g.layers[0] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = tl, .depth = 8, .map_w_log2 = 6, .map_h_log2 = 5, .tiles = tiles};
    tg_world_t w;
    CHECK(tg_world_init(&w, &g.layers[0], win_a, win_b, dirty, big, 60, 40));
    tg_world_set_flags(&w, md.flags);
    CHECK(tg_actors_set_wrap(&p, 0, 0));
    const int id = tg_actor_alloc(&p);
    tg_actor_t *a = tg_actor(&p, id);
    a->cols = a->rows = 1;
    a->hit_w = 8; a->hit_h = 8;
    a->x = 8 * 256; a->y = 40 * 256;
    CHECK(tg_actor_move(&p, id, &w, 0, 100 * 256) == TG_HIT_DOWN);       /* lands on the row: its bottom at y = 80 */
    CHECK(a->y == 72 * 256);
    CHECK(tg_actor_move(&p, id, &w, 40 * 256, 0) == 0);                  /* sideways along it */
    a->x = 8 * 256; a->y = 120 * 256;
    CHECK(tg_actor_move(&p, id, &w, 0, -100 * 256) == 0);                /* up through it from below */
    CHECK(a->y == 20 * 256);
    a->y = 76 * 256;                           /* already inside the tile row: it lets go downwards */
    CHECK(tg_actor_move(&p, id, &w, 0, 30 * 256) == 0);
    free(tiles); free(big); free(md.m);
}

/* ---- overlap ---- */

static void test_overlap(void)
{
    tg_actor_t store[3];
    uint16_t order[12];
    tg_actors_t p;
    tg_actors_init(&p, store, 3, order);
    for (int trial = 0; trial < 20000; trial++) {
        int ids[2];
        int32_t bx[2], by[2], bw[2], bh[2];
        const int32_t wx = rnd() % 2 ? rr(192, 700) : 0, wy = rnd() % 2 ? rr(160, 500) : 0;     /* a world that wraps, or not */
        CHECK(tg_actors_set_wrap(&p, wx, wy));
        for (int k = 0; k < 2; k++) {
            ids[k] = tg_actor_alloc(&p);
            tg_actor_t *a = tg_actor(&p, ids[k]);
            a->cols = (uint8_t)rr(1, 3);
            a->rows = (uint8_t)rr(1, 3);
            if (rnd() % 2) {
                a->hit_w = (uint8_t)rr(1, 20);
                a->hit_h = (uint8_t)rr(1, 20);
                a->hit_x = (uint8_t)rr(0, 10);
                a->hit_y = (uint8_t)rr(0, 10);
            }
            a->x = rr(-60, 60 + wx) * 256 + rr(0, 255);
            a->y = rr(-60, 60 + wy) * 256 + rr(0, 255);
            bx[k] = floordiv(a->x, 256) + (a->hit_w ? a->hit_x : 0);
            by[k] = floordiv(a->y, 256) + (a->hit_w ? a->hit_y : 0);
            bw[k] = a->hit_w ? a->hit_w : a->cols * 16;
            bh[k] = a->hit_w ? a->hit_h : a->rows * 16;
        }
        /* the boxes, with the second one also a world (or two) away if the world wraps: is there a pixel in both? */
        bool expect = false;
        for (int kx = (wx ? -2 : 0); kx <= (wx ? 2 : 0) && !expect; kx++)
            for (int ky = (wy ? -2 : 0); ky <= (wy ? 2 : 0) && !expect; ky++) {
                const int32_t cx = bx[1] + kx * wx, cy = by[1] + ky * wy;
                if (bx[0] < cx + bw[1] && cx < bx[0] + bw[0] && by[0] < cy + bh[1] && cy < by[0] + bh[0])
                    expect = true;
            }
        CHECK(tg_actors_overlap(&p, ids[0], ids[1]) == expect);
        CHECK(tg_actors_overlap(&p, ids[1], ids[0]) == expect);
        tg_actor_free(&p, ids[0]);
        CHECK(!tg_actors_overlap(&p, ids[0], ids[1]));                   /* an inactive actor overlaps nothing */
        tg_actor_free(&p, ids[1]);
    }
    CHECK(tg_actors_set_wrap(&p, 0, 0));
    CHECK(!tg_actors_overlap(&p, -1, 0) && !tg_actors_overlap(&p, 0, 3));
}

int main(void)
{
    test_pool();
    for (unsigned sl = 3; sl <= 4; sl++)
        for (unsigned depth = 4; depth <= 8; depth += 4)
            for (int ys = 0; ys < 2; ys++) {
                test_draw(sl, depth, ys, 40, 40, 0, 0);
                test_draw(sl, depth, ys, 200, 6, 0, 0);
                test_draw(sl, depth, ys, 40, 30, 512, 256);      /* a world of 64 x 32 tiles: the size of the window */
                test_draw(sl, depth, ys, 40, 30, 192, 160);      /* the smallest: an actor is often seen twice */
                test_draw(sl, depth, ys, 200, 4, 384, 320);
                test_draw(sl, depth, ys, 40, 20, 400, 0);
                test_draw(sl, depth, ys, 40, 20, 0, 300);
            }
    test_exhaustion();
    test_move();
    test_overlap();
    printf("all %d checks passed\n", checks);
    return 0;
}
