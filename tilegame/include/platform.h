/* SPDX-License-Identifier: MIT */
/*
 * The platform layer: everything a game needs from the machine besides the compositor (tilegfx).
 * One header, one implementation per target (SDL on the desktop, the RP2350 board), so a game written against
 * this runs on both.
 *
 * A game's main loop looks like
 *
 *     plat_init();
 *     for (;;) {
 *         plat_wait_frame();                 // 60 Hz: shows what you presented, then reads the buttons
 *         buttons = plat_buttons(0);
 *         ...update the tg_t...
 *         plat_present(&g);                  // it is shown from the next frame on
 *     }
 *
 * and does its game logic once per frame (a fixed timestep).
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

#include "tilegfx.h"

/* ---- starting up ---- */

bool plat_init(void);        /* open the window, or bring up HDMI, input and audio */
void plat_shutdown(void);

/* ---- the frame loop ---- */

#define PLAT_FPS 60

/*
 * Wait for the next frame boundary, then sample the inputs for the frame that is about to be computed. Returns the
 * frame number (counting from 1). On the desktop this is also where window events are handled.
 */
uint32_t plat_wait_frame(void);

/*
 * Show this state from the next frame on. The state itself (the tg_t) is copied, so the caller may go on changing its own.
 * What it POINTS to is not: tile maps, tile graphics, per-line scroll tables and sprite graphics are shared with the
 * display, which on the board is another core reading them while the game runs. Anything that changes from frame to frame
 * should therefore be double-buffered: write to the buffer that was not just presented, then present a state that points
 * at it. (Data that never changes after start-up needs nothing.)
 */
void plat_present(const tg_t *g);

/* True once the user has closed the window. Always false on the board. */
bool plat_quit_requested(void);

/* ---- time ---- */

uint32_t plat_millis(void);   /* since start; wraps after 49 days */
uint32_t plat_frame(void);    /* the number plat_wait_frame last returned */

/* ---- input ---- */

#define PLAT_MAX_PLAYERS 2

#define PLAT_BTN_UP     0x0001
#define PLAT_BTN_DOWN   0x0002
#define PLAT_BTN_LEFT   0x0004
#define PLAT_BTN_RIGHT  0x0008
#define PLAT_BTN_A      0x0010
#define PLAT_BTN_B      0x0020
#define PLAT_BTN_X      0x0040
#define PLAT_BTN_Y      0x0080
#define PLAT_BTN_START  0x0100
#define PLAT_BTN_SELECT 0x0200

typedef uint16_t plat_buttons_t;

/* As sampled by the last plat_wait_frame, so a whole frame sees one consistent state. Players with no device give 0. */
plat_buttons_t plat_buttons(int player);            /* held now */
plat_buttons_t plat_buttons_pressed(int player);    /* went down since the previous frame */
plat_buttons_t plat_buttons_released(int player);   /* went up since the previous frame */

/* ---- audio ---- */

#define PLAT_AUDIO_RATE 48000

/*
 * Called when the output needs more sound: fill `frames` stereo frames into `lr` (left, right, left, right, ...,
 * signed 16 bit). It runs on another thread (the audio thread on the desktop, core 1 on the board), not in the game
 * loop, so share state with the game through single-word volatile fields or a queue, not through plain structures.
 * It must be quick and must not wait for anything.
 */
typedef void (*plat_audio_fn)(int16_t *lr, int frames, void *user);

/* Start (or, with NULL, stop) audio output. False if there is no audio. */
bool plat_audio_start(plat_audio_fn fn, void *user);

#endif
