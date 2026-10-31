/* SPDX-License-Identifier: MIT */
/* The console demo, on the desktop and on the board: type at the prompt (a USB keyboard on the board). Try HELP. */
#include "consdemo_logic.h"
#include "platform.h"
#include "tg_text.h"
#include "tilegfx.h"

static tg_text_t screen;
static consdemo_t demo;

int main(void)
{
    if (!plat_init())
        return 1;
    if (tg_text_load_cp437(&screen) != 0)
        return 2;
    tg_text_init(&screen);
    consdemo_init(&demo, &screen, plat_millis);
    static tg_t g;   /* not on the stack: it is 2.3 KB and the board's stack is 2 KB */
    tg_init(&g);
    g.mode = TG_MODE_TEXT;
    g.text = &screen;

    while (!plat_quit_requested()) {
        plat_wait_frame();
        int ch;
        while ((ch = plat_getchar()) >= 0)
            consdemo_char(&demo, ch);
        plat_present(&g);
    }
    plat_shutdown();
    return 0;
}
