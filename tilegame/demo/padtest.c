/* SPDX-License-Identifier: MIT */
/*
 * An input diagnostic screen: what the USB devices said about themselves, the raw bytes of the last gamepad report, and how
 * they were decoded into the platform's buttons. For finding out why a pad does not behave: press its buttons and watch.
 */
#include <stdio.h>
#include <string.h>

#include "platform.h"
#include "tg_font.h"
#include "tg_fonts.h"
#include "tilegfx.h"

#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

static tg_t g;
static uint8_t tiles[100 * 32];
static uint16_t maps[2][64 * 32];

static const struct { plat_buttons_t bit; const char *name; } names[] = {
    {PLAT_BTN_UP, "UP"},   {PLAT_BTN_DOWN, "DOWN"}, {PLAT_BTN_LEFT, "LEFT"}, {PLAT_BTN_RIGHT, "RIGHT"}, {PLAT_BTN_A, "A"},
    {PLAT_BTN_B, "B"},     {PLAT_BTN_X, "X"},       {PLAT_BTN_Y, "Y"},       {PLAT_BTN_L, "L"},         {PLAT_BTN_R, "R"},
    {PLAT_BTN_START, "START"}, {PLAT_BTN_SELECT, "SELECT"},
};

int main(void)
{
    if (!plat_init())
        return 1;
    tg_init(&g);
    g.palette[0] = RGB(0, 0, 40);
    g.palette[(1 << 4) | 1] = RGB(255, 255, 255);   /* sub-palette 1: white text */
    g.palette[(2 << 4) | 1] = RGB(255, 230, 80);    /* 2: yellow, for headings */
    g.palette[(3 << 4) | 1] = RGB(90, 255, 120);    /* 3: green, for what is pressed now */
    g.layers[0] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = 3, .depth = 4, .map_w_log2 = 6, .map_h_log2 = 5,
                               .map = maps[0], .tiles = tiles};
    tg_layer_load_font_ptr(&g.layers[0], &tg_font_unscii8, 32, 95, 1, 1, 0);
    plat_present(&g);

    int buf = 0;
    uint32_t presses = 0;
    while (!plat_quit_requested()) {
        plat_wait_frame();
        const plat_buttons_t held = plat_buttons(0), hit = plat_buttons_pressed(0);
        for (size_t i = 0; i < sizeof names / sizeof *names; i++)
            presses += (hit & names[i].bit) != 0;

        buf ^= 1;
        memset(maps[buf], 0, sizeof maps[buf]);
        g.layers[0].map = maps[buf];

        int row = 1;
        tg_layer_put_string(&g.layers[0], 2, row++, "INPUT DIAGNOSTIC", 2);
        row++;
        char lines[22][41];
        const int n = plat_input_diag(lines, 22);
        for (int i = 0; i < n; i++)
            tg_layer_put_string(&g.layers[0], 2, row++, lines[i], 1);
        row++;
        char text[48];
        tg_layer_put_string(&g.layers[0], 2, row++, "PLATFORM BUTTONS HELD NOW:", 2);
        int x = 2;
        for (size_t i = 0; i < sizeof names / sizeof *names; i++)
            if (held & names[i].bit) {
                tg_layer_put_string(&g.layers[0], x, row, names[i].name, 3);
                x += (int)strlen(names[i].name) + 1;
            }
        row += 2;
        snprintf(text, sizeof text, "PRESSES SEEN: %lu", (unsigned long)presses);
        tg_layer_put_string(&g.layers[0], 2, row++, text, 1);
        const plat_mouse_t m = plat_mouse();
        if (m.present) {
            snprintf(text, sizeof text, "MOUSE %3d,%3d BUTTONS %d", m.x, m.y, m.buttons);
            tg_layer_put_string(&g.layers[0], 2, row++, text, 1);
        }
        plat_present(&g);
    }
    plat_shutdown();
    return 0;
}
