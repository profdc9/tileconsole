/* SPDX-License-Identifier: MIT */
/*
 * Built with -DTG_FONTS_ENABLED=0x5 (fonts 0 and 2) and linked with only those two font files: that it links at all shows
 * the others are not referenced.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tg_font.h"

static int checks;
#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

int main(void)
{
    CHECK(tg_font_by_id(TG_FONT_UNSCII8) != NULL);
    CHECK(tg_font_by_id(TG_FONT_UNSCII8_THIN) != NULL);
    CHECK(tg_font_by_id(TG_FONT_UNSCII8_ALT) == NULL);
    CHECK(tg_font_by_id(TG_FONT_UNSCII8_FANTASY) == NULL);
    CHECK(tg_font_by_id(TG_FONT_UNSCII8_MCR) == NULL);
    CHECK(tg_font_by_id(TG_FONT_UNSCII8_TALL) == NULL);
    CHECK(tg_font_by_id(TG_FONT_UNSCII16) == NULL);

    static uint8_t tiles[100 * 64];
    static uint16_t map[16 * 8];
    tg_layer_t l = {.kind = TG_LAYER_TILES, .tile_log2 = 3, .depth = 8, .map_w_log2 = 4, .map_h_log2 = 3, .map = map, .tiles = tiles};
    CHECK(tg_layer_load_font(&l, TG_FONT_UNSCII8, 32, 95, 0, 1, 0));
    CHECK(!tg_layer_load_font(&l, TG_FONT_UNSCII16, 32, 95, 0, 1, 0));          /* left out of this build */
    CHECK(l.glyphs.offset == -32);                                              /* and the failed call changed nothing */
    printf("all %d checks passed\n", checks);
    return 0;
}
