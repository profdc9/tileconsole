/* SPDX-License-Identifier: MIT */
/* The level: a grid of 16x16 tiles, 15 rows high (one screen), and where things start. */
#ifndef GAME_LEVEL_INCLUDED
#define GAME_LEVEL_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#define TILE 16
#define LEVEL_H 15
#define LEVEL_W 120
#define MAX_COINS 48
#define MAX_ENEMIES 12

enum { T_EMPTY = 0, T_GRASS, T_DIRT, T_BRICK, T_BLOCK, T_COUNT };

typedef struct {
    uint8_t tile[LEVEL_H][LEVEL_W];
    int ncoins, nenemies;
    struct { int16_t x, y; } coin[MAX_COINS];      /* tile coordinates */
    struct { int16_t x, y; } enemy[MAX_ENEMIES];   /* tile coordinates of the tile the enemy stands on the top of */
    int16_t start_x, start_y;                      /* tile coordinates where the player starts */
    int16_t goal_x;                                /* tile column of the flag */
} level_t;

void level_build(level_t *lv);

static inline bool tile_is_solid(uint8_t t) { return t >= T_GRASS && t < T_COUNT; }

/* The tile at tile coordinates. The edges of the level are walls; above and below it is open (so a pit can be fallen into). */
static inline uint8_t level_tile(const level_t *lv, int tx, int ty)
{
    if (tx < 0 || tx >= LEVEL_W)
        return T_BLOCK;
    if (ty < 0 || ty >= LEVEL_H)
        return T_EMPTY;
    return lv->tile[ty][tx];
}

#endif
