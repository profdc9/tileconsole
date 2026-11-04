/* SPDX-License-Identifier: MIT */
/*
 * The game functions of the BASIC environment: tiles, a scrolling map, actors (sprites that move by themselves and collide with tiles), buttons and the
 * frame loop, on top of tilegfx's tg_world and tg_actor. See tilegfx/WORLD.md for the design and basic/README.md for the functions.
 *
 * A program makes its picture with the functions and shows it with WAITFRAME(), which also waits for the next frame, moves the actors and returns the number
 * of frames that went by:
 *
 *     gfx(8) : newmap(64, 32) : tiledef(1, "...") : tile(3, 4, 1)
 *     while 1 : n = waitframe() : ... : wend
 *
 * Nothing is shown before the first WAITFRAME, and the picture goes away when the program ends (after a key, so that the last picture can be looked at).
 * The state belongs to the run: it is made on first use and freed at its end.
 */
#ifndef BASIC_GAME_H
#define BASIC_GAME_H

#include "basic_env.h"

struct mb_interpreter_t;

/* Registers the functions (basic_env_init does). */
void basic_game_register(struct mb_interpreter_t *s);

/* Before a run: forgets what a previous one left. After it: if a picture was shown, keeps showing the last one until a key is typed (not after Ctrl+C), then frees the state. */
void basic_game_begin_run(basic_env_t *e);
void basic_game_end_run(basic_env_t *e);

/* Frees the state. */
void basic_game_close(basic_env_t *e);

#endif
