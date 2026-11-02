/* SPDX-License-Identifier: MIT */
/*
 * Built-in fonts by number. This file refers to every font, so using it links them all unless TG_FONTS_ENABLED is
 * defined as a mask of the fonts to keep (bit n for font number n); the others are then NULL and are not linked.
 */
#include "tg_fonts.h"

#ifndef TG_FONTS_ENABLED
#define TG_FONTS_ENABLED 0xFFFFFFFFu
#endif

#define HAVE(id) ((TG_FONTS_ENABLED) >> (id) & 1u)

const tg_font_t *tg_font_by_id(tg_font_id_t id)
{
    switch (id) {
#define FONT(id, object)                \
    case id:                            \
        return HAVE(id) ? &object : NULL;
    FONT(TG_FONT_UNSCII8, tg_font_unscii8)
    FONT(TG_FONT_UNSCII8_ALT, tg_font_unscii8_alt)
    FONT(TG_FONT_UNSCII8_THIN, tg_font_unscii8_thin)
    FONT(TG_FONT_UNSCII8_FANTASY, tg_font_unscii8_fantasy)
    FONT(TG_FONT_UNSCII8_MCR, tg_font_unscii8_mcr)
    FONT(TG_FONT_UNSCII8_TALL, tg_font_unscii8_tall)
    FONT(TG_FONT_UNSCII16, tg_font_unscii16)
#undef FONT
    default:
        return NULL;
    }
}

bool tg_layer_load_font(tg_layer_t *layer, tg_font_id_t font, uint32_t first_glyph, unsigned count, unsigned first_tile,
                        uint8_t fg, uint8_t bg)
{
    return tg_layer_load_font_ptr(layer, tg_font_by_id(font), first_glyph, count, first_tile, fg, bg);
}
