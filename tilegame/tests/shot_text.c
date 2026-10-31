/* SPDX-License-Identifier: MIT */
/* Draw a page of the text mode demo and write the picture as a PPM:  shot_text page frame out.ppm */
#include <stdio.h>
#include <stdlib.h>

#include "textdemo_pages.h"
#include "tilegfx.h"

int main(int argc, char **argv)
{
    if (argc < 4)
        return 1;
    static tg_text_t screen;
    if (textdemo_init(&screen) != 0)
        return 2;
    const int page = atoi(argv[1]);
    const uint32_t last = (uint32_t)atoi(argv[2]);
    for (uint32_t f = 0; f <= last; f++)   /* the pages keep state from frame to frame */
        textdemo_draw(&screen, page, f);
    static tg_t g;
    tg_init(&g);
    g.mode = TG_MODE_TEXT;
    g.text = &screen;
    FILE *out = fopen(argv[3], "wb");
    if (!out)
        return 3;
    fprintf(out, "P6\n%d %d\n255\n", TG_DISPLAY_WIDTH, TG_DISPLAY_HEIGHT);
    for (int y = 0; y < TG_DISPLAY_HEIGHT; y++) {
        static uint32_t line[TG_WIDTH];
        tg_render_display_line(&g, y, line);
        for (int x = 0; x < TG_DISPLAY_WIDTH; x++) {
            const uint16_t c = (uint16_t)(line[x / 2] >> (16 * (x & 1)));
            const uint8_t rgb[3] = {(uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 63) << 2), (uint8_t)((c & 31) << 3)};
            fwrite(rgb, 1, 3, out);
        }
    }
    fclose(out);
    return 0;
}
