/* SPDX-License-Identifier: MIT */
/*
 * The game's rules. Deterministic, in fixed point (1/256 of a pixel), and free of any display or sound: game_update takes
 * the buttons for one frame and changes the state, and sound-worthy things are reported as event bits for the caller.
 */
#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include <stdint.h>

#include "level.h"
#include "platform.h"   /* for the button bits only */

#define FP 8   /* fractional bits of positions and speeds */

/* Something that moves and bumps into tiles: a 16x16 sprite cell with a smaller hitbox inside it. */
typedef struct {
    int32_t x, y;       /* the top left of the 16x16 cell, in 1/256 pixel */
    int32_t vx, vy;     /* pixels per frame, in 1/256 */
    int8_t w, h, ox, oy;   /* the hitbox: size and offset inside the cell, in pixels */
    bool on_ground;
} body_t;

typedef enum { GS_PLAYING, GS_DYING, GS_GAMEOVER, GS_WIN } game_state_t;

enum {
    EV_JUMP = 1, EV_COIN = 2, EV_STOMP = 4, EV_DIE = 8, EV_WIN = 16, EV_GAMEOVER = 32, EV_RESPAWN = 64,
};

typedef struct {
    body_t b;
    bool facing_left;
    uint8_t coyote;        /* frames left in which a jump is still allowed after walking off an edge */
    uint8_t jump_buffer;   /* frames left in which a jump press is remembered */
    uint8_t anim;          /* grows while running; the view picks the frame */
} player_t;

typedef struct {
    body_t b;
    bool alive;
    bool facing_left;
    uint8_t squash;        /* frames left of the squashed look after a stomp */
} enemy_t;

typedef struct {
    level_t level;
    game_state_t state;
    player_t p;
    enemy_t enemy[MAX_ENEMIES];
    bool coin_taken[MAX_COINS];
    int score, coins, lives;
    int camera_x;          /* the world x pixel at the left of the screen */
    uint32_t frame;        /* frames played */
    int timer;             /* for the dying animation */
    uint32_t events;       /* EV_* bits from the last update */
} game_t;

void game_init(game_t *g);

/* One frame. `held` is what is down, `pressed` what went down this frame. Sets g->events. */
void game_update(game_t *g, plat_buttons_t held, plat_buttons_t pressed);

/* The player's hitbox in world pixels, for tests and the view. */
static inline int body_left(const body_t *b) { return (b->x >> FP) + b->ox; }
static inline int body_top(const body_t *b) { return (b->y >> FP) + b->oy; }

#endif
