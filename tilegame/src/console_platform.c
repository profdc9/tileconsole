/* SPDX-License-Identifier: MIT */
/* consoleio's connection to the platform layer: keys from plat_getchar, the screen shown as a text mode frame. */
#include "consoleio.h"
#include "platform.h"
#include "tilegfx.h"

static tg_t picture;   /* not on the stack: it is 2.3 KB and the board's stack is 2 KB */

static void frame(void)
{
    plat_present(&picture);   /* the platform layer copies the text screen, so it shows what has been written so far */
    plat_wait_frame();
}

void console_use_platform(void)
{
    tg_init(&picture);
    picture.mode = TG_MODE_TEXT;
    picture.text = console_screen();
    const console_hooks_t hooks = {plat_getchar, frame};
    console_set_hooks(&hooks);
}
