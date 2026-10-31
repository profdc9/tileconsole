/* SPDX-License-Identifier: MIT */
/* Type into the console demo and write the picture as a PPM:  shot_cons out.ppm "help\n" "test\n" ...   (\n and \e in the arguments are Enter and Escape) */
#include <stdio.h>
#include <string.h>

#include "consdemo_logic.h"
#include "tg_text.h"

static uint32_t millis(void) { return 123456; }

int main(int argc, char **argv)
{
    if (argc < 2)
        return 1;
    static tg_text_t screen;
    static consdemo_t demo;
    if (tg_text_load_cp437(&screen) != 0)
        return 2;
    tg_text_init(&screen);
    consdemo_init(&demo, &screen, millis);
    for (int a = 2; a < argc; a++)
        for (const char *p = argv[a]; *p; p++) {
            if (p[0] == '\\' && p[1] == 'n') { consdemo_char(&demo, 10); p++; }
            else if (p[0] == '\\' && p[1] == 'e') { consdemo_char(&demo, 27); p++; }
            else consdemo_char(&demo, (unsigned char)*p);
        }
    static tg_t g;
    tg_init(&g);
    g.mode = TG_MODE_TEXT;
    g.text = &screen;
    FILE *out = fopen(argv[1], "wb");
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
