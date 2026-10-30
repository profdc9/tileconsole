/* SPDX-License-Identifier: MIT */
/*
 * Turns the game's state into something tilegfx can draw: three tile layers (hills and clouds, the level, the text HUD) and
 * the sprites. All the art is made in code at start-up. The level layer and the HUD are double-buffered, because the
 * display reads their maps while the game writes the next frame's (see plat_present).
 */
#ifndef VIEW_H
#define VIEW_H

#include "game.h"
#include "tilegfx.h"

#define BG_TILES_MAX 200
#define COIN_POOL 24

/* Sub-palettes (16 colours each, picked by the tile entry or sprite). */
enum { PAL_BG = 0, PAL_TILES, PAL_PLAYER, PAL_ENEMY, PAL_COIN, PAL_TEXT, PAL_TEXT_HI };

/* Sprite graphics, 16x16 at 4 bits, by frame number. */
enum { SPR_PLAYER_IDLE = 0, SPR_PLAYER_RUN1, SPR_PLAYER_RUN2, SPR_PLAYER_JUMP, SPR_COIN0, SPR_COIN1, SPR_COIN2, SPR_COIN3,
       SPR_ENEMY0, SPR_ENEMY1, SPR_ENEMY_SQUASHED, SPR_COUNT };

typedef struct {
    tg_t g;   /* what to present */

    uint8_t bg_tiles[BG_TILES_MAX * 128];
    uint8_t main_tiles[T_COUNT * 128];
    uint8_t sprite_gfx[SPR_COUNT * 128];
    uint8_t hud_tiles[100 * 32];
    uint16_t bg_map[32 * 16];
    uint16_t main_map[2][32 * 16];
    uint16_t hud_map[2][64 * 32];
    int bg_tiles_used;
    int buf;   /* which of the double buffers the next frame is written to */
    int player_slot, enemy_slot[MAX_ENEMIES], coin_slot[COIN_POOL];
} view_t;

void view_init(view_t *v);

/* Draw the game into v->g. */
void view_draw(view_t *v, const game_t *g);

#endif
