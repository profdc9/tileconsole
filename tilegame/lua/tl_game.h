/* SPDX-License-Identifier: MIT */
/*
 * Game functions for Lua programs: up to four tile layers with maps that scroll (tilegfx's tg_world), with parallax, actors (tg_actor) with animation tables, a camera that can follow one,
 * the buttons, and the frame (waitframe, or a routine that run() calls once a frame). The graphics are made by the first of the functions that needs them
 * and kept until the next RUN or NEW; while they are on the screen the text screen is not shown, and it comes back when the program or line ends.
 * lua/README.md lists the functions.
 */
#ifndef TL_GAME_H
#define TL_GAME_H

#include "tl_env.h"

struct lua_State;

/* Registers the functions (tl_env does, for every new state). */
void tl_game_open(struct lua_State *L);

/* Frees the game's state (tl_env does before it closes a Lua state). */
void tl_game_reset(tl_env_t *e);

#endif
