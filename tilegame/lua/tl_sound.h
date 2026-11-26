/* SPDX-License-Identifier: MIT */
/*
 * Sound functions for Lua programs: the 16 voices of tilesnd (tones, samples, envelopes, slides), numbered samples made from strings, tables or WAV files.
 * The machine gives the engine through the hooks (tl_hooks_t.snd), already playing on its audio thread; the functions only queue commands to it, on the
 * game thread. Without an engine they do nothing (samples are still made, so a program runs the same). lua/README.md lists the functions.
 */
#ifndef TL_SOUND_H
#define TL_SOUND_H

#include "tl_env.h"

struct lua_State;

/* Registers the functions (tl_env does, for every new state). */
void tl_sound_open(struct lua_State *L);

/* A new environment: everything forgotten (what an earlier one had is not given back). */
void tl_sound_init(tl_env_t *e);

/* RUN and NEW: every voice stops and goes back to its defaults, and the samples are given back. */
void tl_sound_reset(tl_env_t *e);

/* Ctrl+C, EDIT, BYE: every voice stops at once. */
void tl_sound_silence(tl_env_t *e);

/* Slides and notes with a length: called often on the game thread (tl_env does, while a program runs and at the prompt). */
void tl_sound_tick(tl_env_t *e);

#endif
