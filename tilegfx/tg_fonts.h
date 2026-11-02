/* SPDX-License-Identifier: MIT */
/* The font objects. Each is in its own source file in fonts/, so a program that names only some links only those
 * (font numbers, in tg_font.h, go through tg_fontlist.c, which refers to all of them unless TG_FONTS_ENABLED says otherwise). */
#ifndef TG_FONTS_H
#define TG_FONTS_H

#include "tg_font.h"

extern const tg_font_t tg_font_unscii8;           /* 8x8: the plain one */
extern const tg_font_t tg_font_unscii8_alt;       /* 8x8: alternative letterforms */
extern const tg_font_t tg_font_unscii8_thin;      /* 8x8: lighter strokes */
extern const tg_font_t tg_font_unscii8_fantasy;   /* 8x8: a fantasy look */
extern const tg_font_t tg_font_unscii8_mcr;       /* 8x8: after the MCR arcade fonts */
extern const tg_font_t tg_font_unscii8_tall;      /* 8x16: the 8x8 design stretched to double height */
extern const tg_font_t tg_font_unscii16;          /* 8x16: the full-height design (no double-width glyphs) */

#endif
