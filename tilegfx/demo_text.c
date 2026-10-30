/* SPDX-License-Identifier: MIT */
/*
 * Draws sample text in every font into a PPM image, through the real compositor:
 *   demo_text out.ppm
 */
#include <stdio.h>
#include <stdlib.h>

#include "tg_fonts.h"
#include "tilegfx.h"

static uint8_t tiles[1024 * 64], tiles2[1024 * 64];    /* two layers, each with its own 8x8 tiles at 8 bpp */
static uint16_t map[64 * 32], map2[64 * 32];           /* maps of 64 x 32 tiles, of which 40 x 30 show */

int main(int argc, char **argv)
{
    if (argc < 2)
        return 1;
    static tg_t g;
    tg_init(&g);
    g.palette[0] = 0x0008;                             /* dark blue */
    g.palette[1] = 0xFFFF;                             /* white */
    g.palette[2] = 0xFFE0;                             /* yellow */
    g.palette[3] = 0x07FF;                             /* cyan */
    g.palette[4] = 0xFB40;                             /* orange */
    g.palette[5] = 0x07E0;                             /* green */
    g.palette[6] = 0xF81F;                             /* magenta */
    g.palette[7] = 0xBDF7;                             /* grey */
    g.palette[8] = 0x4208;                             /* dark grey (a background) */

    static const struct { const tg_font_t *font; const char *name; } fonts[] = {
        {&tg_font_unscii8, "unscii-8"},           {&tg_font_unscii8_alt, "unscii-8-alt"},
        {&tg_font_unscii8_thin, "unscii-8-thin"}, {&tg_font_unscii8_fantasy, "unscii-8-fantasy"},
        {&tg_font_unscii8_mcr, "unscii-8-mcr"},   {&tg_font_unscii8_tall, "unscii-8-tall"},
        {&tg_font_unscii16, "unscii-16"},
    };
    unsigned next_tile = 1;                            /* tile 0 stays blank */
    int row = 1;
    for (size_t i = 0; i < sizeof fonts / sizeof *fonts; i++) {
        const tg_font_t *f = fonts[i].font;
        tg_charset_t ascii = tg_font_load(f, tiles, 3, 8, (uint16_t)next_tile, 0x20, 96, (uint8_t)(1 + i % 5), (i == 2 ? 8 : 0));
        next_tile += 96 * ascii.tiles_per_char;
        tg_text_put(map, 6, 5, 1, row, fonts[i].name, &ascii, 0);
        tg_text_put(map, 6, 5, 20, row, "Quick brown fox", &ascii, 0);
        row += (int)ascii.tiles_per_char;
        tg_text_put(map, 6, 5, 1, row, "ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789", &ascii, 0);
        row += (int)ascii.tiles_per_char;
    }
    /* Characters beyond ASCII, written as UTF-8 in the source: Latin-1, box drawing, blocks, Greek. */
    unsigned t2 = 1;
    tg_charset_t ascii2 = tg_font_load(&tg_font_unscii8, tiles2, 3, 8, (uint16_t)t2, 0x20, 96, 1, 0);
    t2 += 96;
    tg_charset_t latin = tg_font_load(&tg_font_unscii8, tiles2, 3, 8, (uint16_t)t2, 0xA0, 96, 3, 0);
    t2 += 96;
    tg_charset_t box = tg_font_load(&tg_font_unscii8, tiles2, 3, 8, (uint16_t)t2, 0x2500, 160, 2, 0);
    t2 += 160;
    tg_charset_t greek = tg_font_load(&tg_font_unscii8, tiles2, 3, 8, (uint16_t)t2, 0x390, 64, 5, 0);
    t2 += 64;
    /* One string, three charsets: each draws the characters it has and skips the rest. */
    static const char *const mixed = "Caf\xC3\xA9 \xC3\xA0 la cr\xC3\xA8me \xC2\xA9 \xC2\xBD";
    tg_text_put(map2, 6, 5, 1, row, mixed, &ascii2, 0);
    tg_text_put(map2, 6, 5, 1, row, mixed, &latin, 0);
    static const char *const boxes[] = {"\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x90 \xE2\x96\x88\xE2\x96\x93\xE2\x96\x92\xE2\x96\x91",
                                        "\xE2\x94\x82 \xE2\x96\xA0 \xE2\x94\x82 \xE2\x96\x80\xE2\x96\x84\xE2\x96\x8C\xE2\x96\x90",
                                        "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x98"};
    for (int i = 0; i < 3; i++)
        tg_text_put(map2, 6, 5, 1, row + 1 + i, boxes[i], &box, 0);
    tg_text_put(map2, 6, 5, 20, row + 1, "\xCE\x91\xCE\xB2\xCE\xB3\xCE\xB4 \xCF\x80 \xCE\xA9 = Greek", &greek, 0);
    tg_text_put(map2, 6, 5, 20, row + 1, "\xCE\x91\xCE\xB2\xCE\xB3\xCE\xB4 \xCF\x80 \xCE\xA9 = Greek", &ascii2, 0);
    g.layers[0] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = 3, .depth = 8, .map_w_log2 = 6, .map_h_log2 = 5,
                               .map = map, .tiles = tiles};
    g.layers[1] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = 3, .depth = 8, .map_w_log2 = 6, .map_h_log2 = 5,
                               .map = map2, .tiles = tiles2};
    FILE *out = fopen(argv[1], "wb");
    if (!out)
        return 1;
    fprintf(out, "P6\n%d %d\n255\n", TG_WIDTH, TG_HEIGHT);
    static uint32_t line[TG_WIDTH];
    for (int y = 0; y < TG_HEIGHT; y++) {
        tg_render_line(&g, y, line);
        for (int x = 0; x < TG_WIDTH; x++) {
            const uint16_t c = (uint16_t)line[x];
            const uint8_t rgb[3] = {(uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 63) << 2), (uint8_t)((c & 31) << 3)};
            fwrite(rgb, 1, 3, out);
        }
    }
    fclose(out);
    printf("layer 1 uses %u tiles, layer 2 uses %u\n", next_tile, t2);
    return 0;
}
