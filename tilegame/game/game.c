/* SPDX-License-Identifier: MIT */
#include "game.h"

#include <string.h>

#define PX(pixels) ((int32_t)(pixels) * (1 << FP))   /* pixels to 1/256 pixel (a shift would be undefined for negative values) */

/* Speeds and accelerations in 1/256 pixel per frame (per frame squared). */
#define WALK_MAX 0x180
#define RUN_MAX 0x2A0
#define ACCEL 0x14
#define AIR_ACCEL 0x0E
#define FRICTION 0x12
#define AIR_FRICTION 0x04
#define SKID 0x28
#define GRAVITY 0x38
#define MAX_FALL 0x500
#define JUMP_SPEED 0x500          /* about 3.5 tiles with the button held */
#define JUMP_CUT 0x180        /* a jump cut short by releasing the button rises no faster than this */
#define STOMP_BOUNCE 0x300
#define STOMP_BOUNCE_HELD 0x4C0
#define ENEMY_SPEED 0x50
#define COYOTE_FRAMES 6
#define JUMP_BUFFER_FRAMES 6
#define DYING_FRAMES 90
#define SQUASH_FRAMES 24
#define START_LIVES 3

#define SCORE_COIN 10
#define SCORE_STOMP 100
#define SCORE_WIN 1000

/* ---- moving bodies about the level ---- */

static bool box_hits_solid(const level_t *lv, int left, int top, int w, int h)
{
    const int tx0 = left >> 4, tx1 = (left + w - 1) >> 4, ty0 = top >> 4, ty1 = (top + h - 1) >> 4;
    for (int ty = ty0; ty <= ty1; ty++)
        for (int tx = tx0; tx <= tx1; tx++)
            if (tile_is_solid(level_tile(lv, tx, ty)))
                return true;
    return false;
}

/* Move sideways by vx; stop against a wall. Returns true if it is against one. */
static bool move_x(const level_t *lv, body_t *b)
{
    const int32_t nx = b->x + b->vx;
    const int left = (nx >> FP) + b->ox, top = (b->y >> FP) + b->oy;
    if (box_hits_solid(lv, left, top, b->w, b->h)) {
        if (b->vx > 0)
            b->x = PX(((left + b->w - 1) >> 4) * TILE - b->w - b->ox);   /* flush against the tile to the right */
        else
            b->x = PX((((left >> 4) + 1) * TILE) - b->ox);                /* and to the left */
        b->vx = 0;
        return true;
    }
    b->x = nx;
    /* Moving less than a pixel a frame, a body pressed against a wall does not collide every frame: it is "against" the
     * wall when the next pixel over is solid, and then it has no sideways speed. */
    const int now = (b->x >> FP) + b->ox;
    if ((b->vx > 0 && box_hits_solid(lv, now + 1, top, b->w, b->h)) || (b->vx < 0 && box_hits_solid(lv, now - 1, top, b->w, b->h))) {
        b->vx = 0;
        return true;
    }
    return false;
}

/* Is there solid ground right under the feet? (Contact is decided by where the body is, not by whether it moved this frame:
 * on the ground gravity adds only a fraction of a pixel a frame, so a body at rest would otherwise flicker between
 * standing and falling.) */
static bool standing(const level_t *lv, const body_t *b)
{
    return b->vy >= 0 && box_hits_solid(lv, (b->x >> FP) + b->ox, (b->y >> FP) + b->oy + 1, b->w, b->h);
}

/* Apply gravity unless standing on something. */
static void fall(const level_t *lv, body_t *b)
{
    b->on_ground = standing(lv, b);
    if (b->on_ground) {
        b->vy = 0;
        return;
    }
    b->vy += GRAVITY;
    if (b->vy > MAX_FALL)
        b->vy = MAX_FALL;
}

/* Move up or down by vy. Landing sets on_ground; hitting a ceiling stops the rise. */
static void move_y(const level_t *lv, body_t *b)
{
    if (b->vy == 0)
        return;
    const int32_t ny = b->y + b->vy;
    const int left = (b->x >> FP) + b->ox, top = (ny >> FP) + b->oy;
    if (!box_hits_solid(lv, left, top, b->w, b->h)) {
        b->y = ny;
        b->on_ground = false;
        return;
    }
    if (b->vy > 0) {
        b->y = PX(((top + b->h - 1) >> 4) * TILE - b->h - b->oy);         /* onto the top of the tile */
        b->on_ground = true;
    } else {
        b->y = PX((((top >> 4) + 1) * TILE) - b->oy);                     /* under the tile */
    }
    b->vy = 0;
}

static int32_t approach(int32_t v, int32_t target, int32_t step)
{
    if (v < target)
        return v + step > target ? target : v + step;
    return v - step < target ? target : v - step;
}

static int sign(int32_t v) { return (v > 0) - (v < 0); }

static bool boxes_overlap(const body_t *a, const body_t *b)
{
    const int al = body_left(a), at = body_top(a), bl = body_left(b), bt = body_top(b);
    return al < bl + b->w && bl < al + a->w && at < bt + b->h && bt < at + a->h;
}

/* ---- setting up ---- */

static void place_player(game_t *g)
{
    g->p = (player_t){.b = {.x = PX(g->level.start_x * TILE), .y = PX(g->level.start_y * TILE - 16),
                            .w = 10, .h = 14, .ox = 3, .oy = 2}};
    g->camera_x = 0;
}

static void place_enemies(game_t *g)
{
    for (int i = 0; i < g->level.nenemies; i++) {
        g->enemy[i] = (enemy_t){.b = {.x = PX(g->level.enemy[i].x * TILE), .y = PX(g->level.enemy[i].y * TILE - 16),
                                      .vx = -ENEMY_SPEED, .w = 12, .h = 12, .ox = 2, .oy = 4},
                                .alive = true, .facing_left = true};
    }
}

void game_init(game_t *g)
{
    memset(g, 0, sizeof *g);
    level_build(&g->level);
    g->lives = START_LIVES;
    g->state = GS_PLAYING;
    place_player(g);
    place_enemies(g);
}

/* ---- one frame ---- */

static void update_camera(game_t *g)
{
    int cam = (g->p.b.x >> FP) + 8 - 160;
    const int max = LEVEL_W * TILE - 2 * 160;
    g->camera_x = cam < 0 ? 0 : (cam > max ? max : cam);
}

static void kill_player(game_t *g)
{
    g->state = GS_DYING;
    g->timer = DYING_FRAMES;
    g->p.b.vx = 0;
    g->p.b.vy = -0x480;   /* the classic hop upwards */
    g->events |= EV_DIE;
}

static void update_player(game_t *g, plat_buttons_t held, plat_buttons_t pressed)
{
    player_t *p = &g->p;
    body_t *b = &p->b;

    b->on_ground = standing(&g->level, b);

    /* Remember a jump press for a few frames, and allow a jump a few frames after leaving the ground. */
    if (pressed & GAME_BTN_JUMP)
        p->jump_buffer = JUMP_BUFFER_FRAMES;
    else if (p->jump_buffer)
        p->jump_buffer--;
    if (b->on_ground)
        p->coyote = COYOTE_FRAMES;
    else if (p->coyote)
        p->coyote--;

    /* Sideways. */
    const int dir = ((held & PLAT_BTN_RIGHT) ? 1 : 0) - ((held & PLAT_BTN_LEFT) ? 1 : 0);
    const int32_t max_speed = (held & GAME_BTN_RUN) ? RUN_MAX : WALK_MAX;
    if (dir == 0) {
        b->vx = approach(b->vx, 0, b->on_ground ? FRICTION : AIR_FRICTION);
    } else {
        int32_t step = b->on_ground ? ACCEL : AIR_ACCEL;
        if (sign(b->vx) == -dir)
            step = b->on_ground ? SKID : AIR_ACCEL * 2;               /* turning round */
        else if (sign(b->vx) == dir && (b->vx > max_speed || b->vx < -max_speed))
            step = FRICTION;                                           /* faster than allowed: ease off */
        b->vx = approach(b->vx, dir * max_speed, step);
        p->facing_left = dir < 0;
    }
    if (b->on_ground && b->vx != 0)
        p->anim++;
    else if (b->on_ground)
        p->anim = 0;

    /* Jumping, and cutting the jump short when the button is let go. */
    if (p->jump_buffer && (b->on_ground || p->coyote)) {
        b->vy = -JUMP_SPEED;
        b->on_ground = false;
        p->jump_buffer = 0;
        p->coyote = 0;
        g->events |= EV_JUMP;
    }
    if (!(held & GAME_BTN_JUMP) && b->vy < -JUMP_CUT)
        b->vy = -JUMP_CUT;

    if (b->on_ground) {
        b->vy = 0;
    } else {
        b->vy += GRAVITY;
        if (b->vy > MAX_FALL)
            b->vy = MAX_FALL;
    }

    move_x(&g->level, b);
    move_y(&g->level, b);

    if ((b->y >> FP) > LEVEL_H * TILE + TILE)   /* fell down a pit */
        kill_player(g);
}

static void update_enemies(game_t *g)
{
    player_t *p = &g->p;
    for (int i = 0; i < g->level.nenemies; i++) {
        enemy_t *e = &g->enemy[i];
        body_t *b = &e->b;
        if (!e->alive) {
            if (e->squash)
                e->squash--;
            continue;
        }

        /* Walk; turn round at a wall or the edge of a platform. */
        b->on_ground = standing(&g->level, b);
        if (b->on_ground) {
            const int front = (b->vx < 0) ? body_left(b) - 1 : body_left(b) + b->w;
            const int below = body_top(b) + b->h;
            if (!tile_is_solid(level_tile(&g->level, front >> 4, below >> 4)))
                b->vx = -b->vx;
        }
        fall(&g->level, b);
        if (move_x(&g->level, b))
            b->vx = (e->facing_left ? ENEMY_SPEED : -ENEMY_SPEED);
        move_y(&g->level, b);
        e->facing_left = b->vx < 0;
        if ((b->y >> FP) > LEVEL_H * TILE + TILE) {
            e->alive = false;
            continue;
        }

        /* The player lands on it, or is hurt by it. */
        if (g->state == GS_PLAYING && boxes_overlap(&p->b, b)) {
            const int player_bottom = body_top(&p->b) + p->b.h;
            if (p->b.vy > 0 && player_bottom <= body_top(b) + 8) {
                e->alive = false;
                e->squash = SQUASH_FRAMES;
                p->b.vy = -STOMP_BOUNCE_HELD;   /* a bounce; the jump-cut below softens it if the button is not held */
                p->b.on_ground = false;
                g->score += SCORE_STOMP;
                g->events |= EV_STOMP;
            } else {
                kill_player(g);
            }
        }
    }
}

static void collect(game_t *g)
{
    const body_t *pb = &g->p.b;
    const int pl = body_left(pb), pt = body_top(pb);
    for (int i = 0; i < g->level.ncoins; i++) {
        if (g->coin_taken[i])
            continue;
        const int cx = g->level.coin[i].x * TILE + 3, cy = g->level.coin[i].y * TILE + 3;   /* a 10x10 coin in its tile */
        if (pl < cx + 10 && cx < pl + pb->w && pt < cy + 10 && cy < pt + pb->h) {
            g->coin_taken[i] = true;
            g->coins++;
            g->score += SCORE_COIN;
            g->events |= EV_COIN;
        }
    }
    /* The flag pole: the whole height of one tile column. */
    if (pl < (g->level.goal_x + 1) * TILE && g->level.goal_x * TILE < pl + pb->w) {
        g->state = GS_WIN;
        g->score += SCORE_WIN;
        g->events |= EV_WIN;
    }
}

void game_update(game_t *g, plat_buttons_t held, plat_buttons_t pressed)
{
    g->events = 0;
    g->frame++;

    switch (g->state) {
    case GS_PLAYING:
        update_player(g, held, pressed);
        if (g->state == GS_PLAYING) {
            update_enemies(g);
            if (g->state == GS_PLAYING)
                collect(g);
        }
        break;
    case GS_DYING: {
        body_t *b = &g->p.b;   /* no collisions: it tumbles off the screen */
        b->vy += GRAVITY;
        b->y += b->vy;
        if (--g->timer <= 0) {
            if (--g->lives > 0) {
                place_player(g);
                place_enemies(g);
                g->state = GS_PLAYING;
                g->events |= EV_RESPAWN;
            } else {
                g->state = GS_GAMEOVER;
                g->events |= EV_GAMEOVER;
            }
        }
        break;
    }
    case GS_GAMEOVER:
    case GS_WIN:
        if (pressed & PLAT_BTN_START)
            game_init(g);
        break;
    }
    update_camera(g);
}
