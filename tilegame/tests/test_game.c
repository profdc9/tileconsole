/* SPDX-License-Identifier: MIT */
/* Headless tests of the game rules: physics on small hand-made levels, level sanity, and a bot that must finish the level. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"

static int checks;
#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define GROUND_ROW 12

/* A flat world with nothing in it: ground on row 12 all the way along, the player standing at tile (5, 12). */
static void flat_world(game_t *g)
{
    game_init(g);
    memset(&g->level, 0, sizeof g->level);
    for (int x = 0; x < LEVEL_W; x++)
        g->level.tile[GROUND_ROW][x] = T_GRASS;
    g->level.start_x = 5;
    g->level.start_y = GROUND_ROW;
    g->level.goal_x = LEVEL_W - 2;
    memset(g->enemy, 0, sizeof g->enemy);
    g->p = (player_t){.b = {.x = (5 * TILE) << FP, .y = (GROUND_ROW * TILE - 16) << FP, .w = 10, .h = 14, .ox = 3, .oy = 2}};
}

static void run_frames(game_t *g, plat_buttons_t held, int n)
{
    static plat_buttons_t prev;
    for (int i = 0; i < n; i++) {
        game_update(g, held, held & (plat_buttons_t)~prev);
        prev = held;
    }
}

/* Frames with a press on the first one only. */
static void press_once(game_t *g, plat_buttons_t b)
{
    game_update(g, b, b);
}

static int feet(const game_t *g) { return body_top(&g->p.b) + g->p.b.h; }

static void test_standing_and_falling(void)
{
    game_t g;
    flat_world(&g);
    run_frames(&g, 0, 100);
    CHECK(g.p.b.on_ground && g.p.b.vx == 0 && g.p.b.vy == 0);
    CHECK(feet(&g) == GROUND_ROW * TILE);                         /* standing exactly on the ground */
    CHECK((g.p.b.x >> FP) == 5 * TILE);

    /* Dropped from height it lands exactly on the surface, and does not sink through at the maximum speed. */
    flat_world(&g);
    g.p.b.y = (3 * TILE) << FP;
    run_frames(&g, 0, 200);
    CHECK(g.p.b.on_ground && feet(&g) == GROUND_ROW * TILE);
    CHECK(g.state == GS_PLAYING);
}

static void test_walking_and_running(void)
{
    game_t g;
    flat_world(&g);
    run_frames(&g, PLAT_BTN_RIGHT, 120);
    CHECK(g.p.b.vx == 0x180 && !g.p.facing_left);                 /* walking speed */
    run_frames(&g, PLAT_BTN_RIGHT | GAME_BTN_RUN, 120);
    CHECK(g.p.b.vx == 0x2A0);                                      /* running speed */
    const int before = g.p.b.x;
    run_frames(&g, 0, 200);
    CHECK(g.p.b.vx == 0 && g.p.b.x > before);                      /* it slid to a stop, going on a little */
    run_frames(&g, PLAT_BTN_LEFT, 5);
    CHECK(g.p.facing_left && g.p.b.vx < 0);

    /* Reversing from a run is quicker than speeding up (the skid). */
    flat_world(&g);
    run_frames(&g, PLAT_BTN_RIGHT | GAME_BTN_RUN, 100);
    int frames_to_turn = 0;
    while (g.p.b.vx > 0 && frames_to_turn < 100) {
        run_frames(&g, PLAT_BTN_LEFT | GAME_BTN_RUN, 1);
        frames_to_turn++;
    }
    CHECK(frames_to_turn < 0x2A0 / 0x14);

    /* Letting go of the run button while fast eases back to a walk, without stopping dead. */
    flat_world(&g);
    run_frames(&g, PLAT_BTN_RIGHT | GAME_BTN_RUN, 100);
    run_frames(&g, PLAT_BTN_RIGHT, 1);
    CHECK(g.p.b.vx > 0x180 && g.p.b.vx < 0x2A0);
    run_frames(&g, PLAT_BTN_RIGHT, 100);
    CHECK(g.p.b.vx == 0x180);
}

static void test_walls_and_ceilings(void)
{
    game_t g;
    flat_world(&g);
    for (int y = 8; y < GROUND_ROW; y++)
        g.level.tile[y][10] = T_BLOCK;
    run_frames(&g, PLAT_BTN_RIGHT | GAME_BTN_RUN, 200);
    CHECK(g.p.b.vx == 0);
    CHECK(body_left(&g.p.b) + g.p.b.w == 10 * TILE);               /* flush against the wall */
    /* Pressing on does not push it in. */
    run_frames(&g, PLAT_BTN_RIGHT | GAME_BTN_RUN, 50);
    CHECK(body_left(&g.p.b) + g.p.b.w == 10 * TILE);

    flat_world(&g);
    for (int y = 8; y < GROUND_ROW; y++)
        g.level.tile[y][0] = T_BLOCK;
    run_frames(&g, PLAT_BTN_LEFT | GAME_BTN_RUN, 300);
    CHECK(body_left(&g.p.b) == 1 * TILE);                          /* the left wall, the other way round */

    /* The edges of the level are walls. */
    flat_world(&g);
    g.p.b.x = 0;
    run_frames(&g, PLAT_BTN_LEFT, 60);
    CHECK(body_left(&g.p.b) >= 0 && g.p.b.vx == 0);

    /* A ceiling stops the rise. */
    flat_world(&g);
    for (int x = 3; x < 8; x++)
        g.level.tile[9][x] = T_BRICK;
    press_once(&g, GAME_BTN_JUMP);
    run_frames(&g, GAME_BTN_JUMP, 30);
    CHECK(body_top(&g.p.b) >= 10 * TILE);                          /* never inside the brick row */
    CHECK(g.p.b.vy >= 0 || body_top(&g.p.b) == 10 * TILE);
}

static int rise_of_jump(int hold_frames)
{
    game_t g;
    flat_world(&g);
    const int start = feet(&g);
    int top = start;
    press_once(&g, GAME_BTN_JUMP);
    for (int f = 0; f < 90; f++) {
        run_frames(&g, f < hold_frames ? GAME_BTN_JUMP : 0, 1);
        if (feet(&g) < top)
            top = feet(&g);
    }
    CHECK(g.p.b.on_ground);
    return start - top;
}

static void test_jumping(void)
{
    const int tap = rise_of_jump(0), held = rise_of_jump(60);
    CHECK(held >= 52 && held <= 60);                                /* about 3.4 tiles with the button held: a 3-tile platform, not a 4-tile one */
    CHECK(tap >= 8 && tap < held - 10);                             /* a tap is a good deal lower */
    int last = 0;
    for (int h = 0; h < 30; h += 3) {                               /* and in between, the longer the button is held the higher */
        const int r = rise_of_jump(h);
        CHECK(r >= last);
        last = r;
    }

    /* A jump can be made and it announces itself. */
    game_t g;
    flat_world(&g);
    press_once(&g, GAME_BTN_JUMP);
    CHECK(g.events & EV_JUMP);
    CHECK(!g.p.b.on_ground && g.p.b.vy < 0);
    press_once(&g, GAME_BTN_JUMP);                                     /* not again in mid-air */
    CHECK(!(g.events & EV_JUMP));

    /* Coyote time: a jump just after walking off an edge works, a late one does not. */
    for (int late = 0; late < 2; late++) {
        flat_world(&g);
        for (int x = 8; x < LEVEL_W; x++)
            g.level.tile[GROUND_ROW][x] = T_EMPTY;
        run_frames(&g, PLAT_BTN_RIGHT, 1000);                       /* off the edge and falling */
        flat_world(&g);
        for (int x = 8; x < LEVEL_W; x++)
            g.level.tile[GROUND_ROW][x] = T_EMPTY;
        int frames = 0;
        while (g.p.b.on_ground || frames == 0) {
            run_frames(&g, PLAT_BTN_RIGHT, 1);
            frames++;
            if (frames > 200)
                break;
        }
        run_frames(&g, PLAT_BTN_RIGHT, late ? 8 : 3);               /* frames after leaving the ground */
        const int vy_before = g.p.b.vy;
        game_update(&g, PLAT_BTN_RIGHT | GAME_BTN_JUMP, GAME_BTN_JUMP);
        if (late)
            CHECK(!(g.events & EV_JUMP) && g.p.b.vy > vy_before - 1);
        else
            CHECK((g.events & EV_JUMP) && g.p.b.vy < 0);
    }

    /* Jump buffering: pressing a little before landing jumps on landing. */
    flat_world(&g);
    g.p.b.y = (GROUND_ROW * TILE - 16 - 3) << FP;                   /* 3 pixels up: lands in about 5 frames, inside the 6-frame buffer */
    g.p.b.on_ground = false;
    plat_buttons_t prev = 0;
    bool jumped = false;
    for (int f = 0; f < 20 && !jumped; f++) {
        const plat_buttons_t held_now = (f == 0) ? GAME_BTN_JUMP : 0;
        game_update(&g, held_now, held_now & (plat_buttons_t)~prev);
        prev = held_now;
        jumped = g.events & EV_JUMP;
    }
    CHECK(jumped);
}

static void test_pits_lives_and_game_over(void)
{
    game_t g;
    flat_world(&g);
    for (int x = 0; x < LEVEL_W; x++)
        g.level.tile[GROUND_ROW][x] = T_EMPTY;                      /* no ground at all */
    for (int life = 3; life >= 1; life--) {
        CHECK(g.lives == life && g.state == GS_PLAYING);
        bool died = false;
        for (int f = 0; f < 300 && !died; f++) {
            game_update(&g, 0, 0);
            died = g.events & EV_DIE;
        }
        CHECK(died && g.state == GS_DYING);
        bool ended = false;
        for (int f = 0; f < 200 && !ended; f++) {
            game_update(&g, 0, 0);
            ended = g.events & (EV_RESPAWN | EV_GAMEOVER);
        }
        CHECK(ended);
        if (life > 1)
            CHECK((g.events & EV_RESPAWN) && g.state == GS_PLAYING && g.lives == life - 1);
        else
            CHECK((g.events & EV_GAMEOVER) && g.state == GS_GAMEOVER && g.lives == 0);
        if (life > 1)
            CHECK((g.p.b.x >> FP) == g.level.start_x * TILE);        /* back at the start */
    }
    /* Game over stays until Start is pressed, and then it is a fresh game. */
    const int frame = (int)g.frame;
    run_frames(&g, PLAT_BTN_RIGHT | GAME_BTN_JUMP, 30);
    CHECK(g.state == GS_GAMEOVER && (int)g.frame > frame);
    game_update(&g, PLAT_BTN_START, PLAT_BTN_START);
    CHECK(g.state == GS_PLAYING && g.lives == 3 && g.score == 0 && g.coins == 0);
}

static void test_coins(void)
{
    game_t g;
    flat_world(&g);
    g.level.ncoins = 1;
    g.level.coin[0].x = 5;
    g.level.coin[0].y = 11;                                         /* in the player's tile */
    memset(g.coin_taken, 0, sizeof g.coin_taken);
    game_update(&g, 0, 0);
    CHECK((g.events & EV_COIN) && g.coins == 1 && g.score == 10 && g.coin_taken[0]);
    game_update(&g, 0, 0);
    CHECK(!(g.events & EV_COIN) && g.coins == 1 && g.score == 10);   /* only once */

    /* A coin out of reach is left alone. */
    flat_world(&g);
    g.level.ncoins = 1;
    g.level.coin[0].x = 5;
    g.level.coin[0].y = 5;
    memset(g.coin_taken, 0, sizeof g.coin_taken);
    run_frames(&g, 0, 30);
    CHECK(g.coins == 0);
}

static void test_enemies(void)
{
    game_t g;
    /* A walker stays on a short platform: it turns at both ends. */
    flat_world(&g);
    for (int x = 0; x < LEVEL_W; x++)
        g.level.tile[GROUND_ROW][x] = T_EMPTY;
    for (int x = 20; x <= 25; x++)
        g.level.tile[GROUND_ROW][x] = T_GRASS;
    g.level.nenemies = 1;
    g.level.enemy[0].x = 22;
    g.level.enemy[0].y = GROUND_ROW;
    g.p.b.x = (60 * TILE) << FP;                                     /* the player is out of the way */
    g.p.b.y = (GROUND_ROW * TILE - 40) << FP;
    for (int x = 55; x < 70; x++)
        g.level.tile[GROUND_ROW][x] = T_GRASS;
    g.enemy[0] = (enemy_t){.b = {.x = (22 * TILE) << FP, .y = (GROUND_ROW * TILE - 16) << FP, .vx = -0x50, .w = 12, .h = 12, .ox = 2, .oy = 4},
                           .alive = true, .facing_left = true};
    int lo = 9999, hi = -9999;
    for (int f = 0; f < 2000; f++) {
        game_update(&g, 0, 0);
        const int x = body_left(&g.enemy[0].b);
        lo = x < lo ? x : lo;
        hi = x + 12 > hi ? x + 12 : hi;
        CHECK(g.enemy[0].alive);
    }
    CHECK(lo >= 20 * TILE - 1 && hi <= 26 * TILE + 1);               /* never past the platform */
    CHECK(hi - lo > 4 * TILE);                                       /* and it did walk about */

    /* Landing on one from above kills it, and the player bounces. */
    flat_world(&g);
    g.level.nenemies = 1;
    g.enemy[0] = (enemy_t){.b = {.x = (8 * TILE) << FP, .y = (GROUND_ROW * TILE - 16) << FP, .vx = 0, .w = 12, .h = 12, .ox = 2, .oy = 4},
                           .alive = true};
    g.p.b.x = (8 * TILE) << FP;
    g.p.b.y = (GROUND_ROW * TILE - 16 - 30) << FP;
    g.p.b.vy = 0x200;
    g.p.b.on_ground = false;
    bool stomped = false;
    for (int f = 0; f < 40 && !stomped; f++) {
        game_update(&g, 0, 0);
        stomped = g.events & EV_STOMP;
    }
    CHECK(stomped && !g.enemy[0].alive && g.score == 100 && g.state == GS_PLAYING);
    CHECK(g.p.b.vy < 0);
    CHECK(g.enemy[0].squash > 0);

    /* Touching one from the side hurts. */
    flat_world(&g);
    g.level.nenemies = 1;
    g.enemy[0] = (enemy_t){.b = {.x = (8 * TILE) << FP, .y = (GROUND_ROW * TILE - 16) << FP, .vx = 0, .w = 12, .h = 12, .ox = 2, .oy = 4},
                           .alive = true};
    g.p.b.x = (7 * TILE) << FP;
    bool died = false;
    for (int f = 0; f < 120 && !died; f++) {
        game_update(&g, PLAT_BTN_RIGHT, 0);
        died = g.events & EV_DIE;
    }
    CHECK(died && g.state == GS_DYING && g.enemy[0].alive);
}

static void test_goal_and_camera(void)
{
    game_t g;
    flat_world(&g);
    run_frames(&g, 0, 2);
    CHECK(g.camera_x == 0);                                          /* at the left end the camera stays put */
    g.p.b.x = (30 * TILE) << FP;
    run_frames(&g, 0, 1);
    CHECK(g.camera_x == 30 * TILE + 8 - 160);                        /* in the middle the player is centred */
    g.p.b.x = ((LEVEL_W - 3) * TILE) << FP;
    run_frames(&g, 0, 1);
    CHECK(g.camera_x == LEVEL_W * TILE - 320);                       /* and at the right end it stops */

    flat_world(&g);
    g.level.goal_x = 7;
    int f = 0;
    while (g.state == GS_PLAYING && f++ < 200)
        game_update(&g, PLAT_BTN_RIGHT, 0);
    CHECK(g.state == GS_WIN && (g.events & EV_WIN) && g.score == 1000);
    const int frame = (int)g.frame;
    run_frames(&g, PLAT_BTN_RIGHT, 10);
    CHECK(g.state == GS_WIN && (int)g.frame == frame + 10);
    game_update(&g, PLAT_BTN_START, PLAT_BTN_START);
    CHECK(g.state == GS_PLAYING && g.score == 0);
}

/* ---- the real level ---- */

static void test_level_sanity(void)
{
    game_t g;
    game_init(&g);
    const level_t *lv = &g.level;
    for (int y = 0; y < LEVEL_H; y++)
        for (int x = 0; x < LEVEL_W; x++)
            CHECK(lv->tile[y][x] < T_COUNT);
    CHECK(lv->nenemies > 0 && lv->nenemies <= MAX_ENEMIES && lv->ncoins > 0 && lv->ncoins <= MAX_COINS);
    /* The player starts standing on something, not inside it. */
    CHECK(tile_is_solid(lv->tile[lv->start_y][lv->start_x]) && !tile_is_solid(lv->tile[lv->start_y - 1][lv->start_x]));
    for (int i = 0; i < lv->ncoins; i++)
        CHECK(!tile_is_solid(lv->tile[lv->coin[i].y][lv->coin[i].x]));        /* no coin inside a wall */
    for (int i = 0; i < lv->nenemies; i++) {
        CHECK(tile_is_solid(lv->tile[lv->enemy[i].y][lv->enemy[i].x]));        /* every enemy stands on something */
        CHECK(!tile_is_solid(lv->tile[lv->enemy[i].y - 1][lv->enemy[i].x]));
    }
    CHECK(lv->goal_x > 0 && lv->goal_x < LEVEL_W);
    /* The goal column is open all the way down to the ground, so the flag can be reached. */
    int open = 0;
    for (int y = 0; y < GROUND_ROW; y++)
        open += !tile_is_solid(lv->tile[y][lv->goal_x]);
    CHECK(open == GROUND_ROW);
    /* Standing still, the level does nothing wrong: the player is still there and alive after a while. */
    run_frames(&g, 0, 300);
    CHECK(g.state == GS_PLAYING || g.state == GS_DYING);
}

static bool solid_at_px(const game_t *g, int px, int py) { return tile_is_solid(level_tile(&g->level, px >> 4, py >> 4)); }

/* A simple player: run right, jump walls, gaps and enemies, and hold the jump while rising. */
static plat_buttons_t bot(const game_t *g)
{
    plat_buttons_t held = PLAT_BTN_RIGHT | GAME_BTN_RUN;
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
        held |= GAME_BTN_JUMP;
    else if (!b->on_ground && b->vy < 0)
        held |= GAME_BTN_JUMP;                                           /* keep holding it on the way up */
    return held;
}

static void test_bot_finishes_the_level(void)
{
    game_t g;
    game_init(&g);
    plat_buttons_t prev = 0;
    int frames = 0;
    while (g.state != GS_WIN && g.state != GS_GAMEOVER && frames < 9000) {
        const plat_buttons_t held = bot(&g);
        game_update(&g, held, held & (plat_buttons_t)~prev);
        prev = held;
        frames++;
    }
    if (g.state != GS_WIN)
        printf("  bot ended in state %d at frame %d, x=%d tiles, lives %d, coins %d\n", g.state, frames, (g.p.b.x >> FP) / TILE, g.lives, g.coins);
    CHECK(g.state == GS_WIN);
    CHECK(frames < 4500);                                             /* 75 seconds: a level that can be finished briskly */
    printf("  the bot finished in %d frames (%d seconds) with %d lives left, %d of %d coins, score %d\n", frames, frames / 60, g.lives,
           g.coins, g.level.ncoins, g.score);
}


/* ---- can a person actually get onto every platform? ---- */

static bool box_clear_at(const game_t *g, int cell_x, int cell_y)
{
    const int l = cell_x + 3, t = cell_y + 2;
    for (int dy = 0; dy < 14; dy += 13)
        for (int dx = 0; dx < 10; dx += 9)
            if (solid_at_px(g, l + dx, t + dy))
                return false;
    return true;
}

/* Is there a jump (a starting spot on some floor near it, a speed, a direction and a time to hold the button) that ends with
 * the player standing on the brick platform in row `ty`, columns x0..x1? */
static bool platform_reachable(int ty, int x0, int x1)
{
    static const int offsets[] = {0, 8};
    static const int speeds[] = {0, 0x180, 0x2A0};
    static const int holds[] = {8, 20, 32, 44, 60};
    game_t g0;
    game_init(&g0);
    for (int c = x0 - 9; c <= x1 + 9; c++)
        for (int r = ty + 1; r < LEVEL_H; r++) {
            /* A floor: a solid tile with air above it, below the platform. */
            if (c < 0 || c >= LEVEL_W || !tile_is_solid(g0.level.tile[r][c]) || tile_is_solid(g0.level.tile[r - 1][c]))
                continue;
            for (size_t oi = 0; oi < sizeof offsets / sizeof *offsets; oi++)
                for (int dir = -1; dir <= 1; dir += 2)
                    for (size_t si = 0; si < sizeof speeds / sizeof *speeds; si++)
                        for (size_t hi = 0; hi < sizeof holds / sizeof *holds; hi++) {
                            game_t g = g0;
                            const int cell_x = c * TILE + offsets[oi] - 3, cell_y = r * TILE - 16;
                            if (!box_clear_at(&g, cell_x, cell_y))
                                continue;
                            g.p.b.x = cell_x * 256;
                            g.p.b.y = cell_y * 256;
                            g.p.b.vx = dir * speeds[si];
                            memset(g.enemy, 0, sizeof g.enemy);
                            g.level.nenemies = 0;
                            g.level.ncoins = 0;
                            plat_buttons_t prev = 0;
                            for (int f = 0; f < 90; f++) {
                                plat_buttons_t held = (dir > 0 ? PLAT_BTN_RIGHT : PLAT_BTN_LEFT) | (speeds[si] > 0x180 ? GAME_BTN_RUN : 0);
                                if (f < holds[hi])
                                    held |= GAME_BTN_JUMP;
                                game_update(&g, held, held & (plat_buttons_t)~prev);
                                prev = held;
                                if (g.state != GS_PLAYING)
                                    break;
                                const int centre = body_left(&g.p.b) + g.p.b.w / 2;
                                if (g.p.b.on_ground && body_top(&g.p.b) + g.p.b.h == ty * TILE && centre >= x0 * TILE &&
                                    centre < (x1 + 1) * TILE)
                                    return true;
                            }
                        }
        }
    return false;
}

static void test_every_platform_can_be_reached(void)
{
    game_t g;
    game_init(&g);
    int platforms = 0;
    for (int y = 0; y < LEVEL_H; y++)
        for (int x = 0; x < LEVEL_W; x++) {
            if (g.level.tile[y][x] != T_BRICK || (x > 0 && g.level.tile[y][x - 1] == T_BRICK))
                continue;
            int x1 = x;
            while (x1 + 1 < LEVEL_W && g.level.tile[y][x1 + 1] == T_BRICK)
                x1++;
            const bool ok = platform_reachable(y, x, x1);
            if (!ok)
                printf("  the brick platform at row %d, columns %d..%d cannot be reached\n", y, x, x1);
            CHECK(ok);
            platforms++;
        }
    CHECK(platforms >= 5);
}

/* The same inputs always give the same game. */
static unsigned hash_state(const game_t *g)
{
    unsigned h = 2166136261u;
    const unsigned char *p = (const unsigned char *)g;
    for (size_t i = 0; i < sizeof *g; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static void test_determinism(void)
{
    unsigned hashes[2];
    for (int run = 0; run < 2; run++) {
        game_t g;
        game_init(&g);
        unsigned seed = 4242;
        plat_buttons_t prev = 0;
        for (int f = 0; f < 3000; f++) {
            seed = seed * 1664525u + 1013904223u;
            plat_buttons_t held = (plat_buttons_t)((seed >> 12) & (PLAT_BTN_UP | PLAT_BTN_DOWN | PLAT_BTN_LEFT | PLAT_BTN_RIGHT |
                                                                      GAME_BTN_JUMP | GAME_BTN_RUN | PLAT_BTN_START));
            held |= PLAT_BTN_RIGHT;
            game_update(&g, held, held & (plat_buttons_t)~prev);
            prev = held;
        }
        hashes[run] = hash_state(&g);
    }
    CHECK(hashes[0] == hashes[1]);
}

/* The buttons by their printed names: lower = A, right = B. The right one jumps and the lower one runs, whatever the pad. */
static void test_button_roles(void)
{
    CHECK(GAME_BTN_JUMP == PLAT_BTN_B && GAME_BTN_RUN == PLAT_BTN_A && GAME_BTN_JUMP != GAME_BTN_RUN);
}

int main(void)
{
    test_button_roles();
    test_standing_and_falling();
    test_walking_and_running();
    test_walls_and_ceilings();
    test_jumping();
    test_pits_lives_and_game_over();
    test_coins();
    test_enemies();
    test_goal_and_camera();
    test_level_sanity();
    test_bot_finishes_the_level();
    test_every_platform_can_be_reached();
    test_determinism();
    printf("all %d checks passed\n", checks);
    return 0;
}
