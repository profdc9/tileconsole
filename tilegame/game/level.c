/* SPDX-License-Identifier: MIT */
#include "level.h"

#include <string.h>

#define GROUND_ROW 12   /* the grass; dirt below it */

static void ground(level_t *lv, int x0, int x1)
{
    for (int x = x0; x <= x1; x++) {
        lv->tile[GROUND_ROW][x] = T_GRASS;
        lv->tile[GROUND_ROW + 1][x] = T_DIRT;
        lv->tile[GROUND_ROW + 2][x] = T_DIRT;
    }
}

static void bricks(level_t *lv, int x0, int x1, int y)
{
    for (int x = x0; x <= x1; x++)
        lv->tile[y][x] = T_BRICK;
}

/* A column of blocks standing on the ground, `h` high. */
static void tower(level_t *lv, int x, int h)
{
    for (int i = 1; i <= h; i++)
        lv->tile[GROUND_ROW - i][x] = T_BLOCK;
}

static void coins(level_t *lv, int x0, int x1, int y)
{
    for (int x = x0; x <= x1 && lv->ncoins < MAX_COINS; x++) {
        lv->coin[lv->ncoins].x = (int16_t)x;
        lv->coin[lv->ncoins].y = (int16_t)y;
        lv->ncoins++;
    }
}

static void enemy(level_t *lv, int x, int y)
{
    if (lv->nenemies < MAX_ENEMIES) {
        lv->enemy[lv->nenemies].x = (int16_t)x;
        lv->enemy[lv->nenemies].y = (int16_t)y;
        lv->nenemies++;
    }
}

void level_build(level_t *lv)
{
    memset(lv, 0, sizeof *lv);

    /* 1. A flat start with a few coins and a brick platform. */
    ground(lv, 0, 24);
    coins(lv, 8, 10, 10);
    bricks(lv, 14, 17, 9);
    coins(lv, 14, 17, 8);
    enemy(lv, 18, GROUND_ROW);

    /* 2. A short gap, then a staircase of blocks. */
    ground(lv, 28, 44);
    for (int i = 0; i < 4; i++)
        tower(lv, 33 + i, i + 1);
    coins(lv, 33, 36, 6);
    enemy(lv, 41, GROUND_ROW);

    /* 3. A wide gap crossed by a floating platform. */
    ground(lv, 49, 62);
    bricks(lv, 45, 46, 11);
    coins(lv, 45, 46, 10);
    enemy(lv, 54, GROUND_ROW);
    enemy(lv, 58, GROUND_ROW);

    /* 4. A pit with a single stepping stone, and a wall to jump. */
    ground(lv, 67, 80);
    bricks(lv, 63, 64, 11);
    coins(lv, 63, 64, 9);
    tower(lv, 72, 2);
    coins(lv, 71, 73, 8);
    enemy(lv, 76, GROUND_ROW);

    /* 5. High platforms. */
    ground(lv, 83, 99);
    bricks(lv, 86, 89, 9);
    bricks(lv, 91, 94, 7);
    coins(lv, 86, 89, 8);
    coins(lv, 91, 94, 6);
    enemy(lv, 96, GROUND_ROW);

    /* 6. The last stairs, and the flag. */
    ground(lv, 103, 119);
    for (int i = 0; i < 5; i++)
        tower(lv, 106 + i, i + 1);
    coins(lv, 108, 111, 5);
    lv->goal_x = 115;

    lv->start_x = 2;
    lv->start_y = GROUND_ROW;
}
