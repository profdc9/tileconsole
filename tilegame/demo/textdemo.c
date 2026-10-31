/* SPDX-License-Identifier: MIT */
/* Text mode demo, on the desktop and on the board: the PC character set, the colours, windows and scrolling. */
#include <stdio.h>

#include "platform.h"
#include "textdemo_pages.h"
#include "tilegfx.h"

static tg_text_t screen;

int main(void)
{
    if (!plat_init())
        return 1;
    if (textdemo_init(&screen) != 0)
        return 2;
    static tg_t g;   /* not on the stack: a tg_t is 2.3 KB and the board's default stack is 2 KB */
    tg_init(&g);
    g.mode = TG_MODE_TEXT;
    g.text = &screen;

    int page = 0;
    while (!plat_quit_requested()) {
        const uint32_t frame = plat_wait_frame();
        const plat_buttons_t hit = plat_buttons_pressed(0);
        if (hit & (PLAT_BTN_RIGHT | PLAT_BTN_A))
            page = (page + 1) % TEXTDEMO_PAGES;
        if (hit & (PLAT_BTN_LEFT | PLAT_BTN_B))
            page = (page + TEXTDEMO_PAGES - 1) % TEXTDEMO_PAGES;
        textdemo_draw(&screen, page, frame);
        plat_present(&g);   /* the screen is copied, so it can be drawn into again straight away */
    }
    plat_shutdown();
    return 0;
}
