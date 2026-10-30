/* SPDX-License-Identifier: MIT */
/* Extra access to the SDL backend, for tests. */
#ifndef PLAT_SDL_H
#define PLAT_SDL_H

#include <stdint.h>
#include <SDL2/SDL_keycode.h>

#include "platform.h"

/* The last frame that was composed, TG_WIDTH x TG_HEIGHT RGB565. */
const uint16_t *plat_sdl_framebuffer(void);

/* The button a key stands for, or 0. Player 0 uses the arrows, Z X A S, Return and Right Shift. */
plat_buttons_t plat_sdl_button_for_key(SDL_Keycode key);

#endif
