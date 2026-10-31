/* SPDX-License-Identifier: MIT */
/* Extra access to the SDL backend, for tests. */
#ifndef PLAT_SDL_H
#define PLAT_SDL_H

#include <stdint.h>
#include <SDL2/SDL_keycode.h>
#include <SDL2/SDL_stdinc.h>

#include "platform.h"

/* The last tile mode frame that was composed, TG_WIDTH x TG_HEIGHT RGB565 (not updated in text mode). */
const uint16_t *plat_sdl_framebuffer(void);

/* The last frame in either mode as it is shown, TG_DISPLAY_WIDTH x TG_DISPLAY_HEIGHT (640 x 480) RGB565. */
const uint16_t *plat_sdl_display(void);

/* What a key press types when the window's text input does not (the keys that make no text), or -1: Enter, Backspace, Tab, Escape, Delete,
 * the cursor keys, F1..F12, and Ctrl with a letter or @ [ \ ] ^ _. `mod` is SDL's KMOD_* state. */
int plat_sdl_char_for_key(SDL_Keycode key, Uint16 mod);

/* The button a key stands for, or 0. Player 0 uses the arrows, Z X A S, Return and Right Shift. */
plat_buttons_t plat_sdl_button_for_key(SDL_Keycode key);

#endif
