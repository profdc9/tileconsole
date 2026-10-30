/* SPDX-License-Identifier: MIT */
/* Runs a bot through the level and saves frames as PPM files: shot <prefix> frame [frame ...]  (a frame of -1 is game over) */
#include <stdio.h>
#include <stdlib.h>

#include "game.h"
#include "view.h"

static bool solid_at_px(const game_t *g, int px, int py) { return tile_is_solid(level_tile(&g->level, px >> 4, py >> 4)); }

static plat_buttons_t bot(const game_t *g)
{
    plat_buttons_t held = PLAT_BTN_RIGHT | PLAT_BTN_B;
    const body_t *b = &g->p.b;
    const int front = body_left(b) + b->w, top = body_top(b), bottom = top + b->h;
    const bool wall = solid_at_px(g, front + 3, bottom - 4) || solid_at_px(g, front + 3, top + 2);
    const bool edge = !solid_at_px(g, front + 1, bottom + 2) && !solid_at_px(g, front - 6, bottom + 2);
    bool enemy_ahead = false;
    for (int i = 0; i < g->level.nenemies; i++) {
        const enemy_t *e = &g->enemy[i];
        if (!e->alive)
            continue;
        const int ex = body_left(&e->b), ey = body_top(&e->b);
        if (ex > front && ex - front < 34 && ey - top < 16 && top - ey < 16)
            enemy_ahead = true;
    }
    if (b->on_ground && (wall || edge || enemy_ahead))
        held |= PLAT_BTN_A;
    else if (!b->on_ground && b->vy < 0)
        held |= PLAT_BTN_A;
    return held;
}

static void save(view_t *v, const char *name)
{
    FILE *f = fopen(name, "wb");
    fprintf(f, "P6\n%d %d\n255\n", TG_WIDTH, TG_HEIGHT);
    static uint32_t line[TG_WIDTH];
    for (int y = 0; y < TG_HEIGHT; y++) {
        tg_render_line(&v->g, y, line);
        for (int x = 0; x < TG_WIDTH; x++) {
            const uint16_t c = (uint16_t)line[x];
            const uint8_t rgb[3] = {(uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 63) << 2), (uint8_t)((c & 31) << 3)};
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    static game_t g;
    static view_t v;
    game_init(&g);
    view_init(&v);
    printf("background uses %d of %d tiles\n", v.bg_tiles_used, BG_TILES_MAX);
    plat_buttons_t prev = 0;
    int frame = 0;
    for (int a = 2; a < argc; a++) {
        const int target = atoi(argv[a]);
        if (target < 0) {                                    /* game over: lose all the lives in the first pit */
            g.lives = 1;
            g.state = GS_PLAYING;
            g.p.b.y = (LEVEL_H * TILE + 40) << FP;
            for (int i = 0; i < 400 && g.state != GS_GAMEOVER; i++)
                game_update(&g, 0, 0);
        }
        while (frame < target) {
            const plat_buttons_t held = bot(&g);
            game_update(&g, held, held & (plat_buttons_t)~prev);
            prev = held;
            frame++;
        }
        view_draw(&v, &g);
        char name[256];
        snprintf(name, sizeof name, "%s%d.ppm", argv[1], a - 1);
        save(&v, name);
    }
    return 0;
}
