/* SPDX-License-Identifier: MIT */
/* The editor demo, on the desktop and on the board: PicoParanoia's editor on the 80 x 25 text screen, with the file in memory. A keyboard is needed. */
#include "consoleio.h"
#include "editdemo_logic.h"
#include "platform.h"

int main(void)
{
    if (!plat_init())
        return 1;
    console_use_platform();
    console_init();
    editdemo_reset();
    for (;;) {                  /* (on the board there is nothing to quit to: the menu starts again) */
        editdemo_menu();
        if (plat_quit_requested())
            break;
    }
    plat_shutdown();
    return 0;
}
