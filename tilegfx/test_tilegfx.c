/* SPDX-License-Identifier: MIT
 * Headless checks for the compositor. Exits non-zero on the first failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilegfx.h"

static int checks;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        checks++;                                                                      \
        if (!(cond)) {                                                                 \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                     \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

#define NTILES 64

/* A random tile set where about a quarter of the pixels are transparent. */
static uint8_t *make_tiles(unsigned tile_log2, unsigned depth, uint8_t *plain /* pixel values, tile-major, one byte each */)
{
    const size_t px_per_tile = (size_t)1 << (2 * tile_log2);
    uint8_t *packed = calloc(NTILES, px_per_tile);
    for (size_t t = 0; t < NTILES; t++)
        for (size_t p = 0; p < px_per_tile; p++) {
            uint8_t v = rnd() & 3 ? (uint8_t)(1 + rnd() % (depth == 8 ? 255 : 15)) : 0;
            plain[t * px_per_tile + p] = v;
            if (depth == 8)
                packed[t * px_per_tile + p] = v;
            else {
                size_t byte = (t * px_per_tile + p) / 2;
                packed[byte] |= (p & 1) ? v : (uint8_t)(v << 4);
            }
        }
    return packed;
}

static uint16_t *make_map_rich(unsigned w_log2, unsigned h_log2, bool rich)
{
    const size_t n = (size_t)1 << (w_log2 + h_log2);
    uint16_t *m = malloc(n * sizeof *m);
    for (size_t i = 0; i < n; i++)
        m[i] = rich ? TG_ENTRY(rnd() % NTILES, rnd() & 15, (rnd() & 1 ? TG_HFLIP : 0) | (rnd() & 1 ? TG_VFLIP : 0))
                    : TG_ENTRY(rnd() % NTILES, 0, 0);
    return m;
}

static uint16_t *make_map(unsigned w_log2, unsigned h_log2) { return make_map_rich(w_log2, h_log2, false); }

static void setup_layer(tg_t *g, unsigned tile_log2, unsigned depth, unsigned w_log2, unsigned h_log2)
{
    static uint8_t plain[NTILES * 256];
    tg_init(g);
    g->layers[0] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = (uint8_t)tile_log2, .depth = (uint8_t)depth,
                                .map_w_log2 = (uint8_t)w_log2, .map_h_log2 = (uint8_t)h_log2,
                                .map = make_map(w_log2, h_log2), .tiles = make_tiles(tile_log2, depth, plain)};
}

static void free_layer(tg_t *g)
{
    free((void *)g->layers[0].map);
    free((void *)g->layers[0].tiles);
}

static void test_scroll_all_modes(void)
{
    for (unsigned tl = 3; tl <= 4; tl++)
        for (unsigned depth = 4; depth <= 8; depth += 4) {
            tg_t g;
            setup_layer(&g, tl, depth, 5, 4);
            const uint32_t map_px_w = (uint32_t)1 << (5 + tl), map_px_h = (uint32_t)1 << (4 + tl);
            const uint32_t xs[] = {0, 1, 7, 8, 15, 16, 17, map_px_w - 9, map_px_w - 1, map_px_w, 0xFFFFFFF0u};
            for (size_t k = 0; k < sizeof xs / sizeof *xs; k++)
                for (int line = 0; line < TG_HEIGHT; line += 37) {
                    uint8_t a[TG_WIDTH], b[TG_WIDTH];
                    g.layers[0].scroll_x = xs[k];
                    tg_render_line_indices(&g, line, a);
                    g.layers[0].scroll_x = xs[k] + 1;   /* one pixel further right: picture moves one pixel left */
                    tg_render_line_indices(&g, line, b);
                    CHECK(memcmp(a + 1, b, TG_WIDTH - 1) == 0);
                }
            /* Scrolling a whole map width is invisible; so is a whole map height. */
            uint8_t a[TG_WIDTH], b[TG_WIDTH];
            g.layers[0].scroll_x = 5;
            tg_render_line_indices(&g, 100, a);
            g.layers[0].scroll_x = 5 + map_px_w;
            tg_render_line_indices(&g, 100, b);
            CHECK(memcmp(a, b, TG_WIDTH) == 0);
            g.layers[0].scroll_x = 5; g.layers[0].scroll_y = 0;
            tg_render_line_indices(&g, 10, a);
            g.layers[0].scroll_y = map_px_h;
            tg_render_line_indices(&g, 10, b);
            CHECK(memcmp(a, b, TG_WIDTH) == 0);
            /* Vertical: line L at scroll_y s+1 equals line L+1 at scroll_y s. */
            for (uint32_t s = 0; s < 40; s++) {
                g.layers[0].scroll_y = s + 1;
                tg_render_line_indices(&g, 50, a);
                g.layers[0].scroll_y = s;
                tg_render_line_indices(&g, 51, b);
                CHECK(memcmp(a, b, TG_WIDTH) == 0);
            }
            free_layer(&g);
        }
}

/* Every pixel on screen must come from the tile the map says is there, with flips and sub-palette applied. */
static uint8_t ref_pixel(const uint8_t *plain, unsigned depth, unsigned size, uint16_t e, unsigned tx, unsigned ty)
{
    if (e & TG_HFLIP)
        tx = size - 1 - tx;
    if (e & TG_VFLIP)
        ty = size - 1 - ty;
    uint8_t v = plain[(size_t)(e & TG_TILE_MASK) * size * size + ty * size + tx];
    if (depth == 4 && v)
        v |= (uint8_t)((e >> TG_PAL_SHIFT) << 4);
    return v;
}

static void test_pixels_match_source(void)
{
    for (unsigned tl = 3; tl <= 4; tl++)
        for (unsigned depth = 4; depth <= 8; depth += 4) {
            static uint8_t plain[NTILES * 256];
            tg_t g;
            tg_init(&g);
            const unsigned size = 1u << tl;
            uint8_t *packed = make_tiles(tl, depth, plain);
            uint16_t *map = make_map_rich(4, 4, true);
            g.layers[0] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = (uint8_t)tl, .depth = (uint8_t)depth,
                                       .map_w_log2 = 4, .map_h_log2 = 4, .map = map, .tiles = packed};
            for (uint32_t sx = 0; sx <= 2 * size + 1; sx++)
                for (uint32_t sy = 0; sy < 3; sy++) {
                    const uint32_t scroll_y = sy == 0 ? 0 : (sy == 1 ? 29 : 1000);
                    g.layers[0].scroll_x = sx + (sx & 1 ? 0xFFFFFF00u : 0);   /* also large and wrapped values */
                    g.layers[0].scroll_y = scroll_y;
                    for (int line = 0; line < TG_HEIGHT; line++) {
                        uint8_t idx[TG_WIDTH];
                        tg_render_line_indices(&g, line, idx);
                        for (int x = 0; x < TG_WIDTH; x++) {
                            const uint32_t wx = (uint32_t)x + g.layers[0].scroll_x, wy = (uint32_t)line + scroll_y;
                            const uint16_t e = map[((wy >> tl) & 15) * 16 + ((wx >> tl) & 15)];
                            CHECK(idx[x] == ref_pixel(plain, depth, size, e, wx & (size - 1), wy & (size - 1)));
                        }
                    }
                }
            free(map);
            free(packed);
        }
}

/* One sprite at random positions (including partly off either edge), with flips and sub-palettes, on a clear backdrop. */
static void test_sprites_match_source(void)
{
    for (unsigned tl = 3; tl <= 4; tl++)
        for (unsigned depth = 4; depth <= 8; depth += 4) {
            static uint8_t plain[NTILES * 256];
            tg_t g;
            tg_init(&g);
            const int size = 1 << tl;
            uint8_t *packed = make_tiles(tl, depth, plain);
            g.sprite_tile_log2 = (uint8_t)tl;
            g.sprite_depth = (uint8_t)depth;
            g.sprite_tiles = packed;
            CHECK(tg_sprite_alloc(&g) == 0);
            for (int trial = 0; trial < 3000; trial++) {
                const int x = (int)(rnd() % (TG_WIDTH + 40)) - 20, y = (int)(rnd() % (TG_HEIGHT - size));
                const unsigned flags = rnd() & 3;
                const uint16_t e = TG_ENTRY(rnd() % NTILES, rnd() & 15, (flags & 1 ? TG_HFLIP : 0) | (flags & 2 ? TG_VFLIP : 0));
                g.sprites[0] = (tg_sprite_t){.x = (int16_t)x, .y = (int16_t)y, .tile = e & TG_TILE_MASK,
                                             .flags = (uint8_t)(TG_SPRITE_USED | TG_SPRITE_ON | (flags & 1 ? TG_SPRITE_HFLIP : 0) |
                                                                (flags & 2 ? TG_SPRITE_VFLIP : 0)),
                                             .pal = (uint8_t)(e >> TG_PAL_SHIFT), .above = 0};
                const int line = y + (int)(rnd() % size);
                uint8_t idx[TG_WIDTH];
                tg_render_line_indices(&g, line, idx);
                for (int px = 0; px < TG_WIDTH; px++) {
                    uint8_t want = 0;
                    if (px >= x && px < x + size)
                        want = ref_pixel(plain, depth, (unsigned)size, e, (unsigned)(px - x), (unsigned)(line - y));
                    CHECK(idx[px] == want);
                }
            }
            free(packed);
        }
}

static void test_flips_and_subpalette(void)
{
    for (unsigned tl = 3; tl <= 4; tl++)
        for (unsigned depth = 4; depth <= 8; depth += 4) {
            static uint8_t plain[NTILES * 256];
            const unsigned size = 1u << tl;
            tg_t g;
            tg_init(&g);
            uint8_t *packed = make_tiles(tl, depth, plain);
            uint16_t map[1];
            g.layers[0] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = (uint8_t)tl, .depth = (uint8_t)depth,
                                       .map_w_log2 = 0, .map_h_log2 = 0, .map = map, .tiles = packed};
            for (unsigned f = 0; f < 4; f++) {
                const unsigned pal = depth == 4 ? 3 : 0;
                map[0] = TG_ENTRY(5, pal, (f & 1 ? TG_HFLIP : 0) | (f & 2 ? TG_VFLIP : 0));
                for (int line = 0; line < (int)size; line++) {
                    uint8_t idx[TG_WIDTH];
                    tg_render_line_indices(&g, line, idx);
                    for (unsigned x = 0; x < size; x++) {
                        const unsigned sx = f & 1 ? size - 1 - x : x, sy = f & 2 ? size - 1 - (unsigned)line : (unsigned)line;
                        uint8_t want = plain[5 * size * size + sy * size + sx];
                        if (want && depth == 4)
                            want |= (uint8_t)(pal << 4);
                        CHECK(idx[x] == want);
                    }
                }
            }
            free(packed);
        }
}

static void test_sprites_and_order(void)
{
    tg_t g;
    tg_init(&g);
    g.sprite_count = TG_MAX_SPRITES;   /* filled by hand below */
    g.sprite_tile_log2 = 3;
    g.sprite_depth = 8;
    uint8_t spr[64];
    memset(spr, 7, sizeof spr);          /* sprite tile 0: solid 7 */
    spr[0] = 0;                          /* top left pixel transparent */
    g.sprite_tiles = spr;

    uint8_t bg_tile[64];
    memset(bg_tile, 9, sizeof bg_tile);  /* layer tile 0: solid 9 */
    uint16_t map[1] = {0};
    g.layers[0] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = 3, .depth = 8, .map = map, .tiles = bg_tile};

    uint8_t idx[TG_WIDTH];
    /* In front of the layer, partly off the left edge. */
    g.sprites[0] = (tg_sprite_t){.x = -3, .y = 10, .tile = 0, .flags = TG_SPRITE_ON, .above = 1};
    tg_render_line_indices(&g, 10, idx);
    CHECK(idx[0] == 7 && idx[4] == 7 && idx[5] == 9);     /* sprite covers x -3..4 */
    tg_render_line_indices(&g, 11, idx);
    CHECK(idx[0] == 7);
    tg_render_line_indices(&g, 9, idx);                   /* above the sprite */
    CHECK(idx[0] == 9);
    tg_render_line_indices(&g, 18, idx);                  /* below the sprite */
    CHECK(idx[0] == 9);
    /* Transparent pixel shows the layer. */
    g.sprites[0].x = 20;
    tg_render_line_indices(&g, 10, idx);
    CHECK(idx[20] == 9 && idx[21] == 7);
    /* Behind the layer: the opaque layer hides it completely. */
    g.sprites[0].above = 0;
    tg_render_line_indices(&g, 10, idx);
    CHECK(idx[21] == 9);
    /* Lower sprite number is in front. */
    g.sprites[0].above = 1;
    g.sprites[1] = g.sprites[0];
    g.sprites[1].tile = 1;
    uint8_t spr2[128];
    memcpy(spr2, spr, 64);
    memset(spr2 + 64, 5, 64);
    g.sprite_tiles = spr2;
    tg_render_line_indices(&g, 12, idx);
    CHECK(idx[21] == 7);
    /* Clipped at the right edge. */
    g.sprites[1].flags = 0;
    g.sprites[0].x = TG_WIDTH - 2;
    tg_render_line_indices(&g, 12, idx);
    CHECK(idx[TG_WIDTH - 1] == 7 && idx[TG_WIDTH - 3] == 9);
    /* The per-line limit. */
    memset(g.sprites, 0, sizeof g.sprites);
    for (int i = 0; i < 40; i++)
        g.sprites[i] = (tg_sprite_t){.x = (int16_t)(i * 8), .y = 100, .tile = 0, .flags = TG_SPRITE_ON, .above = 1};
    tg_render_line_indices(&g, 103, idx);
    CHECK(idx[(TG_MAX_SPRITES_PER_LINE - 1) * 8 + 4] == 7);
    CHECK(idx[TG_MAX_SPRITES_PER_LINE * 8 + 4] == 9);
}

static void test_sprite_alloc(void)
{
    tg_t g;
    tg_init(&g);
    CHECK(g.sprite_count == 0);
    for (int i = 0; i < 5; i++)
        CHECK(tg_sprite_alloc(&g) == i);
    CHECK(g.sprite_count == 5);
    CHECK(g.sprites[3].flags == (TG_SPRITE_USED | TG_SPRITE_ON));

    /* A hole is reused first, and does not shrink the count while sprites above it are in use. */
    g.sprites[1].x = 99;
    tg_sprite_free(&g, 1);
    CHECK(g.sprite_count == 5);
    CHECK(tg_sprite_alloc(&g) == 1);
    CHECK(g.sprites[1].x == 0);        /* handed out cleared */
    CHECK(g.sprite_count == 5);

    /* Freeing from the top shrinks the count past any holes beneath. */
    tg_sprite_free(&g, 2);
    tg_sprite_free(&g, 4);
    CHECK(g.sprite_count == 4);
    tg_sprite_free(&g, 3);
    CHECK(g.sprite_count == 2);
    tg_sprite_free(&g, 1);
    tg_sprite_free(&g, 0);
    CHECK(g.sprite_count == 0);

    /* Bad slots are ignored, and the table fills up. */
    tg_sprite_free(&g, -1);
    tg_sprite_free(&g, TG_MAX_SPRITES);
    for (int i = 0; i < TG_MAX_SPRITES; i++)
        CHECK(tg_sprite_alloc(&g) == i);
    CHECK(g.sprite_count == TG_MAX_SPRITES);
    CHECK(tg_sprite_alloc(&g) == -1);

    /* The compositor looks only at allocated slots: a visible sprite past sprite_count is not drawn. */
    tg_init(&g);
    static uint8_t tile[64];
    memset(tile, 7, sizeof tile);
    g.sprite_tile_log2 = 3;
    g.sprite_depth = 8;
    g.sprite_tiles = tile;
    const int s = tg_sprite_alloc(&g);
    g.sprites[s].x = 10;
    g.sprites[s].y = 20;
    g.sprites[s].above = TG_MAX_LAYERS;
    g.sprites[50] = (tg_sprite_t){.x = 100, .y = 20, .flags = TG_SPRITE_ON, .above = TG_MAX_LAYERS};
    uint8_t idx[TG_WIDTH];
    tg_render_line_indices(&g, 22, idx);
    CHECK(idx[10] == 7 && idx[17] == 7 && idx[18] == 0);
    CHECK(idx[100] == 0);
    tg_sprite_free(&g, s);
    tg_render_line_indices(&g, 22, idx);   /* freed: gone, and the line is untouched */
    CHECK(idx[10] == 0);
}

static void test_bitmap_palette_and_hooks(void)
{
    tg_t g;
    tg_init(&g);
    static uint8_t bm[TG_WIDTH * TG_HEIGHT];
    for (size_t i = 0; i < sizeof bm; i++)
        bm[i] = (uint8_t)(i % 3);
    g.layers[0] = (tg_layer_t){.kind = TG_LAYER_BITMAP, .bitmap = bm};
    g.palette[0] = 0x1234; g.palette[1] = 0xF800; g.palette[2] = 0x07E0;
    uint32_t out[TG_WIDTH];
    tg_render_line(&g, 0, out);
    CHECK(out[0] == 0x12341234u && out[1] == 0xF800F800u && out[2] == 0x07E007E0u);

}

static int cb_calls;
static void count_cb(tg_t *g, int line, void *user)
{
    (void)g; (void)line; (void)user;
    cb_calls++;
}

static void test_line_hook_and_line_scroll(void)
{
    tg_t g;
    setup_layer(&g, 3, 8, 5, 5);
    g.line_cb = count_cb;
    uint8_t a[TG_WIDTH], b[TG_WIDTH];
    tg_render_line_indices(&g, 0, a);
    tg_render_line_indices(&g, 1, a);
    CHECK(cb_calls == 2);
    int16_t ls[TG_HEIGHT] = {0};
    ls[20] = 3; ls[21] = -2;
    g.layers[0].line_scroll_x = ls;
    g.layers[0].scroll_x = 100;
    tg_render_line_indices(&g, 20, a);
    g.layers[0].line_scroll_x = NULL;
    g.layers[0].scroll_x = 103;
    tg_render_line_indices(&g, 20, b);
    CHECK(memcmp(a, b, TG_WIDTH) == 0);
    g.layers[0].line_scroll_x = ls;
    g.layers[0].scroll_x = 100;
    tg_render_line_indices(&g, 21, a);
    g.layers[0].line_scroll_x = NULL;
    g.layers[0].scroll_x = 98;
    tg_render_line_indices(&g, 21, b);
    CHECK(memcmp(a, b, TG_WIDTH) == 0);
    free_layer(&g);
}

int main(void)
{
    test_scroll_all_modes();
    test_pixels_match_source();
    test_flips_and_subpalette();
    test_sprites_match_source();
    test_sprite_alloc();
    test_sprites_and_order();
    test_bitmap_palette_and_hooks();
    test_line_hook_and_line_scroll();
    printf("all %d checks passed\n", checks);
    return 0;
}
