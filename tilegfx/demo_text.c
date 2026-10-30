/* SPDX-License-Identifier: MIT */
/*
 * Draws sample text in every font into a PPM image, through the real compositor, using the layer text API:
 *   demo_text out.ppm
 */
#include <stdio.h>
#include <stdlib.h>

#include "tg_font.h"
#include "tilegfx.h"

static uint8_t tiles[1024 * 64], tiles2[1024 * 64];    /* two layers, each with its own 8x8 tiles at 8 bpp */
static uint16_t map[64 * 32], map2[64 * 32];           /* maps of 64 x 32 tiles, of which 40 x 30 show */

int main(int argc, char **argv)
{
    if (argc < 2)
        return 1;
    static tg_t g;
    tg_init(&g);
    static const uint16_t colours[] = {0x0008, 0xFFFF, 0xFFE0, 0x07FF, 0xFB40, 0x07E0, 0xF81F, 0xBDF7, 0x4208};
    for (size_t i = 0; i < sizeof colours / sizeof *colours; i++)
        g.palette[i] = colours[i];   /* 0 dark blue (the backdrop), then white, yellow, cyan, orange, green, magenta, grey, dark grey */

    g.layers[0] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = 3, .depth = 8, .map_w_log2 = 6, .map_h_log2 = 5,
                               .map = map, .tiles = tiles};
    g.layers[1] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = 3, .depth = 8, .map_w_log2 = 6, .map_h_log2 = 5,
                               .map = map2, .tiles = tiles2};
    tg_layer_t *fonts = &g.layers[0], *extra = &g.layers[1];

    /* Layer 0: every font, by number. Each is loaded into its own tiles, and the offset is worked out and stored. */
    static const struct { tg_font_id_t id; const char *name; } list[] = {
        {TG_FONT_UNSCII8, "unscii-8"},           {TG_FONT_UNSCII8_ALT, "unscii-8-alt"},
        {TG_FONT_UNSCII8_THIN, "unscii-8-thin"}, {TG_FONT_UNSCII8_FANTASY, "unscii-8-fantasy"},
        {TG_FONT_UNSCII8_MCR, "unscii-8-mcr"},   {TG_FONT_UNSCII8_TALL, "unscii-8-tall"},
        {TG_FONT_UNSCII16, "unscii-16"},
    };
    unsigned next_tile = 1;                            /* tile 0 stays blank */
    int row = 1;
    for (size_t i = 0; i < sizeof list / sizeof *list; i++) {
        const unsigned rows = tg_font_by_id(list[i].id)->height / 8;
        if (!tg_layer_load_font(fonts, list[i].id, 0x20, 95, next_tile, (uint8_t)(1 + i % 5), (uint8_t)(i == 2 ? 8 : 0))) {
            fprintf(stderr, "could not load %s\n", list[i].name);
            return 1;
        }
        next_tile += 95 * rows;
        tg_layer_put_string(fonts, 1, row, list[i].name, 0);
        tg_layer_put_string(fonts, 20, row, "Quick brown fox", 0);
        row += (int)rows;
        tg_layer_put_string(fonts, 1, row, "ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789", 0);
        row += (int)rows;
    }

    /* Layer 1: one string, several glyph sets. Each load makes its glyphs the current ones and each put draws the
     * characters that have tiles, skipping the rest, so a mixed string takes one call per set. */
    static const char *const mixed = "Caf\xC3\xA9 \xC3\xA0 la cr\xC3\xA8me \xC2\xA9 \xC2\xBD";
    unsigned t2 = 1;
    tg_layer_load_font(extra, TG_FONT_UNSCII8, 0xA0, 96, t2, 3, 0);      /* Latin-1, in cyan */
    t2 += 96;
    tg_layer_put_utf8(extra, 1, row, mixed, 0);
    tg_layer_load_font(extra, TG_FONT_UNSCII8, 0x20, 95, t2, 1, 0);      /* ASCII, in white */
    t2 += 95;
    tg_layer_put_utf8(extra, 1, row, mixed, 0);
    static const char *const greek = "\xCE\x91\xCE\xB2\xCE\xB3\xCE\xB4 \xCF\x80 \xCE\xA9 = Greek";
    tg_layer_load_font(extra, TG_FONT_UNSCII8, 0x390, 64, t2, 5, 0);     /* Greek, in green */
    t2 += 64;
    tg_layer_put_utf8(extra, 20, row + 1, greek, 0);
    tg_layer_load_font(extra, TG_FONT_UNSCII8, 0x20, 95, t2, 1, 0);      /* and ASCII again for the rest of that string */
    t2 += 95;
    tg_layer_put_utf8(extra, 20, row + 1, greek, 0);

    /* Box drawing and blocks: U+2500..U+25FF. */
    tg_layer_load_font(extra, TG_FONT_UNSCII8, 0x2500, 256, t2, 2, 0);
    t2 += 256;
    static const char *const boxes[] = {
        "\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x90 \xE2\x96\x88\xE2\x96\x93\xE2\x96\x92\xE2\x96\x91",
        "\xE2\x94\x82 \xE2\x96\xA0 \xE2\x94\x82 \xE2\x96\x80\xE2\x96\x84\xE2\x96\x8C\xE2\x96\x90",
        "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x98"};
    for (int i = 0; i < 3; i++)
        tg_layer_put_utf8(extra, 1, row + 1 + i, boxes[i], 0);

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
    printf("layer 0 uses %u tiles, layer 1 uses %u\n", next_tile, t2);
    return 0;
}
