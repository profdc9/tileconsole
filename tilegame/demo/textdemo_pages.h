/* SPDX-License-Identifier: MIT */
/* The pages of the text mode demo, drawn into a tg_text_t. No platform code, so a desktop tool or a test can draw them too. */
#ifndef TEXTDEMO_PAGES_H
#define TEXTDEMO_PAGES_H

#include <stdint.h>

#include "tg_text.h"

#define TEXTDEMO_PAGES 5

/* The standard palette, the PC character set and a blank screen. Returns how many characters have no glyph (none). */
unsigned textdemo_init(tg_text_t *t);

/* Draw one page for this frame number. A page may change the glyphs (page 2 redefines one every few frames) and the screen is not
 * cleared between frames, so the same page can be drawn again and again. Changing page clears the screen first. */
void textdemo_draw(tg_text_t *t, int page, uint32_t frame);

#endif
