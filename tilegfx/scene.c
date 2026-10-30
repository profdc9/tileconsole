/* SPDX-License-Identifier: MIT */
/* The demo scene shared by the desktop demo and the board: generated graphics and a little animation. */
#include "scene.h"

#include <math.h>
#include <string.h>

#define BG_W_LOG2 6   /* 64 x 16 tiles of 16 px = 1024 x 256 pixels */
#define BG_H_LOG2 4
#define FG_W_LOG2 7   /* 128 x 32 tiles of 8 px = 1024 x 256 pixels */
#define FG_H_LOG2 5

static uint16_t bg_map[1 << (BG_W_LOG2 + BG_H_LOG2)];
static uint16_t fg_map[1 << (FG_W_LOG2 + FG_H_LOG2)];
static uint8_t bg_tiles[16 * 256];       /* 16 tiles, 16x16, 8 bpp */
static uint8_t fg_tiles[8 * 32];         /* 8 tiles, 8x8, 4 bpp */
static uint8_t spr_tiles[4 * 256];       /* 4 sprite tiles, 16x16, 8 bpp */
static int16_t wave[TG_HEIGHT];
#define NSPRITES 24
static int sprite_slot[NSPRITES];

static uint16_t rgb565(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

static void make_palette(tg_t *g)
{
    g->palette[0] = rgb565(20, 24, 60);
    for (int i = 1; i < 256; i++) {
        const double h = (i % 32) / 32.0 * 6.283185, v = 0.45 + 0.55 * (i / 256.0);
        g->palette[i] = rgb565((int)(255 * v * (0.5 + 0.5 * sin(h))), (int)(255 * v * (0.5 + 0.5 * sin(h + 2.1))),
                               (int)(255 * v * (0.5 + 0.5 * sin(h + 4.2))));
    }
}

static void make_graphics(void)
{
    for (int t = 0; t < 16; t++)             /* background: diagonal bands, each tile a different colour range */
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                bg_tiles[t * 256 + y * 16 + x] = (uint8_t)(((x + y) % 8 < 6) ? 32 + t * 12 + (x + y) % 8 : 0);
    for (int t = 0; t < 8; t++)              /* foreground: bricks in 4 bpp, 1..15 */
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x += 2) {
                const int mortar = y == 0 || y == 4 || ((x + (y < 4 ? 0 : 4)) % 8 == 0);
                const uint8_t a = (uint8_t)(mortar ? 2 : 9 + (t & 3)), b = (uint8_t)(mortar ? 2 : 9 + (t & 3));
                fg_tiles[t * 32 + y * 4 + x / 2] = (uint8_t)(a << 4 | b);
            }
    for (int t = 0; t < 4; t++)              /* sprites: discs with a transparent corner */
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                const int dx = x * 2 - 15, dy = y * 2 - 15;
                spr_tiles[t * 256 + y * 16 + x] = (dx * dx + dy * dy < 225) ? (uint8_t)(200 + t * 12 + (dx * dx + dy * dy) / 40) : 0;
            }
    for (int y = 0; y < 1 << BG_H_LOG2; y++)
        for (int x = 0; x < 1 << BG_W_LOG2; x++)
            bg_map[y << BG_W_LOG2 | x] = TG_ENTRY((x / 4 + y) % 16, 0, ((x ^ y) & 8) ? TG_HFLIP : 0);
}

void scene_setup(tg_t *g)
{
    tg_init(g);
    make_palette(g);
    make_graphics();
    /* Foreground tile 0 is "empty": all transparent. Tiles 1..7 are bricks. */
    memset(fg_tiles, 0, 32);
    for (int y = 0; y < 1 << FG_H_LOG2; y++)
        for (int x = 0; x < 1 << FG_W_LOG2; x++) {
            const int ground = y >= 26 || (y >= 24 && (x / 6) % 3 == 0);
            fg_map[y << FG_W_LOG2 | x] = ground ? TG_ENTRY(1 + (x + y) % 7, 1 + (x / 8) % 3, 0) : TG_ENTRY(0, 0, 0);
        }
    g->layers[0] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = 4, .depth = 8, .map_w_log2 = BG_W_LOG2,
                                .map_h_log2 = BG_H_LOG2, .map = bg_map, .tiles = bg_tiles};
    g->layers[1] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = 3, .depth = 4, .map_w_log2 = FG_W_LOG2,
                                .map_h_log2 = FG_H_LOG2, .map = fg_map, .tiles = fg_tiles};
    g->sprite_tile_log2 = 4;
    g->sprite_depth = 8;
    g->sprite_tiles = spr_tiles;
    for (int i = 0; i < NSPRITES; i++)
        sprite_slot[i] = tg_sprite_alloc(g);
}

void scene_animate(tg_t *g, int frame, int scroll, bool use_wave)
{
    g->layers[0].scroll_x = (uint32_t)scroll / 2;
    g->layers[0].scroll_y = 8;
    g->layers[1].scroll_x = (uint32_t)scroll;
    g->layers[1].scroll_y = 0;
    for (int i = 0; i < NSPRITES; i++) {
        const double t = frame * 0.03 + i * 0.7;
        g->sprites[sprite_slot[i]] = (tg_sprite_t){.x = (int16_t)(150 + 140 * sin(t)), .y = (int16_t)(100 + 60 * sin(t * 1.7 + i)), .tile = (uint16_t)(i & 3),
                                                   .flags = (uint8_t)(TG_SPRITE_ON | ((i & 4) ? TG_SPRITE_HFLIP : 0)),
                                                   .above = (uint8_t)(i % 3 ? 2 : 1)};
    }
    for (int l = 0; l < TG_HEIGHT; l++)
        wave[l] = use_wave ? (int16_t)(6 * sin(l * 0.15 + frame * 0.1)) : 0;
    g->layers[0].line_scroll_x = use_wave ? wave : NULL;
}

