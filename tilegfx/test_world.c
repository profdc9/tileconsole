/* SPDX-License-Identifier: MIT
 * Headless checks for tg_world: random camera walks and edits, compared with a reference that looks every tile up in a model of the big map,
 * and a check that the map the display is reading is never written. Exits non-zero on the first failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tg_world.h"

static int checks;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        checks++;                                                                      \
        if (!(cond)) {                                                                 \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                     \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

static uint32_t rng = 987654321u;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

#define NT 64                                  /* tiles in the tile set */
#define REF_LOG2 8                             /* the reference layer: 256 x 256 tiles, global tile -64 at its origin */
#define REF_BASE (-64)

static uint8_t *make_tiles(unsigned tile_log2, unsigned depth)
{
    const size_t px = (size_t)1 << (2 * tile_log2);
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

static uint16_t random_entry(void)
{
    return TG_ENTRY(rnd() % NT, rnd() & 15, (rnd() & 1 ? TG_HFLIP : 0) | (rnd() & 1 ? TG_VFLIP : 0));
}

/* The model of the big map: written separately from the library's, so that the two can disagree. */
typedef struct {
    uint16_t *m;
    int32_t w, h;
    tg_border_t border;
    uint16_t be;
    uint8_t flags[TG_TILES_MAX];
} model_t;

static int32_t posmod(int32_t v, int32_t s) { return ((v % s) + s) % s; }

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

static void model_set(model_t *md, int32_t tx, int32_t ty, uint16_t e)
{
    if (tx >= 0 && tx < md->w && ty >= 0 && ty < md->h)
        md->m[ty * md->w + tx] = e;
    else if (md->border == TG_BORDER_WRAP)
        md->m[posmod(ty, md->h) * md->w + posmod(tx, md->w)] = e;
}

static int32_t floordiv(int32_t v, int32_t s) { const int32_t q = v / s; return (v % s != 0 && v < 0) ? q - 1 : q; }

typedef struct {
    unsigned tl, depth;
    uint8_t *tiles;
    uint16_t *ref_map;                         /* 1 << (2 * REF_LOG2) entries */
    tg_t ref_g;
} ref_t;

/* What the screen should show: the visible tiles looked up in the model, drawn by the compositor from a map that is big enough not to wrap. */
static void expected_image(ref_t *r, const model_t *md, int32_t cam_x, int32_t cam_y, uint8_t img[TG_HEIGHT][TG_WIDTH])
{
    const int32_t ts = 1 << r->tl;
    const int32_t tx0 = floordiv(cam_x, ts), ty0 = floordiv(cam_y, ts);
    for (int32_t ty = ty0; ty < ty0 + TG_HEIGHT / ts + 2; ty++)
        for (int32_t tx = tx0; tx < tx0 + TG_WIDTH / ts + 2; tx++)
            r->ref_map[((size_t)(ty - REF_BASE) << REF_LOG2) + (size_t)(tx - REF_BASE)] = model_get(md, tx, ty);
    r->ref_g.layers[0].scroll_x = (uint32_t)(cam_x - REF_BASE * ts);
    r->ref_g.layers[0].scroll_y = (uint32_t)(cam_y - REF_BASE * ts);
    for (int line = 0; line < TG_HEIGHT; line++)
        tg_render_line_indices(&r->ref_g, line, img[line]);
}

static void check_image(tg_t *shown, uint8_t img[TG_HEIGHT][TG_WIDTH])
{
    uint8_t line[TG_WIDTH];
    for (int l = 0; l < TG_HEIGHT; l++) {
        tg_render_line_indices(shown, l, line);
        CHECK(memcmp(line, img[l], TG_WIDTH) == 0);
    }
}

static uint8_t brute_flags(const model_t *md, unsigned tl, int32_t x, int32_t y, int32_t w, int32_t h)
{
    const int32_t ts = 1 << tl;
    uint8_t f = 0;
    for (int32_t py = y; py < y + h; py++)
        for (int32_t px = x; px < x + w; px++)
            f |= md->flags[model_get(md, floordiv(px, ts), floordiv(py, ts)) & TG_TILE_MASK];
    return f;
}

static uint8_t img_prev[TG_HEIGHT][TG_WIDTH];

static void run(unsigned tl, unsigned depth, unsigned wl, unsigned hl, tg_border_t border, int frames)
{
    const int32_t ts = 1 << tl;
    const int32_t vis_w = TG_WIDTH / ts + 1, vis_h = TG_HEIGHT / ts + 1;

    static uint16_t ref_map[1u << (2 * REF_LOG2)];
    ref_t ref = {.tl = tl, .depth = depth, .tiles = make_tiles(tl, depth), .ref_map = ref_map};
    tg_init(&ref.ref_g);
    ref.ref_g.layers[0] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = (uint8_t)tl, .depth = (uint8_t)depth, .map_w_log2 = REF_LOG2,
                                       .map_h_log2 = REF_LOG2, .map = ref_map, .tiles = ref.tiles};

    model_t md = {.w = 3 + (int32_t)(rnd() % 98), .h = 3 + (int32_t)(rnd() % 98), .border = border, .be = random_entry()};
    md.m = malloc((size_t)md.w * (size_t)md.h * sizeof *md.m);
    uint16_t *big = malloc((size_t)md.w * (size_t)md.h * sizeof *big);
    for (int32_t i = 0; i < md.w * md.h; i++)
        md.m[i] = big[i] = random_entry();
    for (int i = 0; i < NT; i++)
        md.flags[i] = (uint8_t)rnd();

    const size_t entries = TG_WORLD_WIN_ENTRIES(wl, hl);
    uint16_t *win_a = malloc(entries * sizeof *win_a), *win_b = malloc(entries * sizeof *win_b);
    uint16_t *shown_copy = malloc(entries * sizeof *shown_copy);
    uint32_t *dirty = malloc(TG_WORLD_DIRTY_WORDS(wl, hl) * sizeof *dirty);

    tg_t g, prev_g;
    tg_init(&g);
    g.layers[0] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = (uint8_t)tl, .depth = (uint8_t)depth, .map_w_log2 = (uint8_t)wl,
                               .map_h_log2 = (uint8_t)hl, .tiles = ref.tiles};
    tg_world_t wd;
    CHECK(tg_world_init(&wd, &g.layers[0], win_a, win_b, dirty, big, md.w, md.h));
    tg_world_set_border(&wd, border, md.be);
    tg_world_set_flags(&wd, md.flags);
    bool have_prev = false;

    for (int f = 0; f < frames; f++) {
        tg_world_begin_frame(&wd);
        if (f > 0)                             /* (the first frame is open since init, with the border already changed) */
            CHECK(memcmp(win_a, win_b, entries * sizeof *win_a) == 0);   /* the replay made the hidden map equal to the shown one */

        if (f == frames / 3) {                 /* the look of the outside changes */
            md.border = (tg_border_t)((md.border + 1) % 3);
            md.be = random_entry();
            tg_world_set_border(&wd, md.border, md.be);
        }
        if (f == frames / 2) {                 /* the big map is changed behind the world's back */
            for (int k = 0; k < 60; k++) {
                const int32_t i = (int32_t)(rnd() % (uint32_t)(md.w * md.h));
                md.m[i] = big[i] = random_entry();
            }
            tg_world_reload(&wd);
        }

        const int edits = (int)(rnd() % 7);
        for (int k = 0; k < edits; k++) {
            const int32_t near_x = floordiv(wd.cam_x, ts) + (int32_t)(rnd() % (uint32_t)vis_w) - 2;
            const int32_t near_y = floordiv(wd.cam_y, ts) + (int32_t)(rnd() % (uint32_t)vis_h) - 2;
            const bool near = rnd() % 4 != 0;
            const int32_t tx = near ? near_x : (int32_t)(rnd() % (uint32_t)(md.w + 20)) - 10;
            const int32_t ty = near ? near_y : (int32_t)(rnd() % (uint32_t)(md.h + 20)) - 10;
            switch (rnd() % 3) {
            case 0: {
                const uint16_t e = random_entry();
                tg_world_set(&wd, tx, ty, e);
                model_set(&md, tx, ty, e);
                break;
            }
            case 1: {
                const uint16_t e = random_entry();
                const int32_t cols = 1 + (int32_t)(rnd() % 6), rows = 1 + (int32_t)(rnd() % 5);
                tg_world_fill(&wd, tx, ty, cols, rows, e);
                for (int32_t r = 0; r < rows; r++)
                    for (int32_t c = 0; c < cols; c++)
                        model_set(&md, tx + c, ty + r, e);
                break;
            }
            default: {
                uint16_t src[5 * 8];
                for (size_t i = 0; i < sizeof src / sizeof *src; i++)
                    src[i] = random_entry();
                const int32_t cols = 1 + (int32_t)(rnd() % 5), rows = 1 + (int32_t)(rnd() % 4);
                tg_world_blit(&wd, tx, ty, src, cols, rows, 8);
                for (int32_t r = 0; r < rows; r++)
                    for (int32_t c = 0; c < cols; c++)
                        model_set(&md, tx + c, ty + r, src[r * 8 + c]);
                break;
            }
            }
        }

        /* the camera: mostly small steps, sometimes a few tiles, sometimes a jump (negative positions included) */
        int32_t cx = wd.cam_x, cy = wd.cam_y;
        const uint32_t kind = rnd() % 10;
        if (kind < 7) {
            cx += (int32_t)(rnd() % 11) - 5;
            cy += (int32_t)(rnd() % 11) - 5;
        } else if (kind < 9) {
            cx += (int32_t)(rnd() % (uint32_t)(6 * ts + 1)) - 3 * ts;
            cy += (int32_t)(rnd() % (uint32_t)(6 * ts + 1)) - 3 * ts;
        } else {
            cx = ((int32_t)(rnd() % (uint32_t)(md.w + 80)) - 40) * ts + (int32_t)(rnd() % (uint32_t)ts);
            cy = ((int32_t)(rnd() % (uint32_t)(md.h + 80)) - 40) * ts + (int32_t)(rnd() % (uint32_t)ts);
        }
        if (cx < -40 * ts) cx = -40 * ts;
        if (cx > (md.w + 40) * ts) cx = (md.w + 40) * ts;
        if (cy < -40 * ts) cy = -40 * ts;
        if (cy > (md.h + 40) * ts) cy = (md.h + 40) * ts;
        tg_world_set_camera(&wd, cx, cy);

        for (int k = 0; k < 20; k++) {
            const int32_t tx = (int32_t)(rnd() % 160) - 30, ty = (int32_t)(rnd() % 160) - 30;
            CHECK(tg_world_get(&wd, tx, ty) == model_get(&md, tx, ty));
            CHECK(tg_world_flags_tile(&wd, tx, ty) == md.flags[model_get(&md, tx, ty) & TG_TILE_MASK]);
            const int32_t x = ((int32_t)(rnd() % 2000) - 400) * ts / 8, y = ((int32_t)(rnd() % 2000) - 400) * ts / 8;
            const int32_t bw = 1 + (int32_t)(rnd() % 40), bh = 1 + (int32_t)(rnd() % 40);
            CHECK(tg_world_flags_box(&wd, x, y, bw, bh) == brute_flags(&md, tl, x, y, bw, bh));
            CHECK(tg_world_flags_px(&wd, x, y) == brute_flags(&md, tl, x, y, 1, 1));
        }
        CHECK(tg_world_flags_box(&wd, 0, 0, 0, 5) == 0);

        /* the picture that was presented last frame must be unchanged by all this: nothing wrote to the map the display reads */
        if (have_prev) {
            CHECK(memcmp(shown_copy, wd.win[wd.shown], entries * sizeof *shown_copy) == 0);
            check_image(&prev_g, img_prev);
        }

        tg_world_end_frame(&wd);
        prev_g = g;                            /* plat_present copies the tg_t */
        expected_image(&ref, &md, cx, cy, img_prev);
        check_image(&prev_g, img_prev);        /* what is presented is right */
        memcpy(shown_copy, wd.win[wd.shown], entries * sizeof *shown_copy);
        have_prev = true;
    }

    free(ref.tiles); free(md.m); free(big); free(win_a); free(win_b); free(shown_copy); free(dirty);
}

static void test_init_rejects(void)
{
    static uint16_t a[1 << 10], b[1 << 10], map[16];
    static uint32_t d[32];
    tg_t g;
    tg_init(&g);
    g.layers[0] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = 3, .depth = 8, .map_w_log2 = 5, .map_h_log2 = 5};   /* 32 x 32: the screen is 41 x 31 tiles */
    tg_world_t w;
    CHECK(!tg_world_init(&w, &g.layers[0], a, b, d, map, 4, 4));
    g.layers[0].map_w_log2 = 6;
    CHECK(!tg_world_init(&w, &g.layers[0], a, b, d, map, 0, 4));
    CHECK(!tg_world_init(&w, &g.layers[0], a, b, d, NULL, 4, 4));
    g.layers[0].kind = TG_LAYER_BITMAP;
    CHECK(!tg_world_init(&w, &g.layers[0], a, b, d, map, 4, 4));
}

/* The lazy begin: an edit before begin_frame (after end_frame) still lands in the next frame, and a second end_frame does nothing. */
static void test_protocol(void)
{
    static uint16_t a[64 * 32], b[64 * 32], map[16 * 16];
    static uint32_t d[TG_WORLD_DIRTY_WORDS(6, 5)];
    tg_t g;
    tg_init(&g);
    g.layers[0] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = 3, .depth = 8, .map_w_log2 = 6, .map_h_log2 = 5};
    tg_world_t w;
    CHECK(tg_world_init(&w, &g.layers[0], a, b, d, map, 16, 16));
    tg_world_set(&w, 2, 3, 77);                /* frame is open after init */
    tg_world_end_frame(&w);
    const uint16_t *shown = g.layers[0].map;
    tg_world_end_frame(&w);                    /* ended already */
    CHECK(g.layers[0].map == shown);
    CHECK(shown[3 * 64 + 2] == 77);
    tg_world_set(&w, 2, 3, 78);                /* starts the next frame by itself: the other map is written */
    CHECK(shown[3 * 64 + 2] == 77);
    tg_world_end_frame(&w);
    CHECK(g.layers[0].map != shown && g.layers[0].map[3 * 64 + 2] == 78);
    tg_world_begin_frame(&w);
    tg_world_begin_frame(&w);                  /* a second begin must not lose or repeat anything */
    CHECK(memcmp(a, b, sizeof a) == 0);
    CHECK(tg_world_get(&w, 2, 3) == 78);
}

int main(void)
{
    test_init_rejects();
    test_protocol();
    for (unsigned tl = 3; tl <= 4; tl++)
        for (unsigned depth = 4; depth <= 8; depth += 4)
            for (unsigned size = 0; size < 3; size++)
                for (tg_border_t border = TG_BORDER_TILE; border <= TG_BORDER_WRAP; border++) {
                    unsigned wl, hl;
                    if (tl == 3) { wl = size == 2 ? 7 : 6; hl = size == 0 ? 5 : 6; }       /* 64 x 32, 64 x 64, 128 x 64 tiles */
                    else         { wl = size == 2 ? 6 : 5; hl = size == 0 ? 4 : 5; }       /* 32 x 16, 32 x 32, 64 x 32 tiles */
                    run(tl, depth, wl, hl, border, 150);
                }
    printf("all %d checks passed\n", checks);
    return 0;
}
