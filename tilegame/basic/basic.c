/* SPDX-License-Identifier: MIT */
/*
 * The BASIC environment on the desktop: ./tgbasic [-2 | --double | --scale 1..4] [FOLDER] (default ./basic_files, made if it is not there), whose files are /sd; -2 opens a window twice the picture's size. EDIT starts the editor ./tde_ed on
 * the file (the board restarts into the editor instead: see the README) and comes back with the program reloaded.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "basic_env.h"
#include "platform.h"
#include "tg_text.h"
#include "tilefs.h"
#include "tilegfx.h"

static tg_text_t screen;
static tg_t g;                 /* not on the stack: it is 2.3 KB and the board's stack is small */
static fs_posix_t host;
static basic_env_t env;

void plat_sdl_window_scale(int scale);   /* sdl/plat_sdl.c */

static void frame(void)
{
    plat_wait_frame();
    plat_present(&g);
}

static unsigned buttons(int player, int kind)
{
    return kind == 0 ? plat_buttons(player) : kind == 1 ? plat_buttons_pressed(player) : plat_buttons_released(player);
}

static int get_char(void)
{
    if (plat_quit_requested())
        exit(0);
    return plat_getchar();
}

int main(int argc, char **argv)
{
    const char *dir = "basic_files";
    const basic_hooks_t hooks = {get_char, frame, plat_millis, NULL, plat_wait_frame, plat_present, buttons};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-2") || !strcmp(argv[i], "--double")) {
            plat_sdl_window_scale(2);
        } else if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
            plat_sdl_window_scale(atoi(argv[++i]));
        } else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "usage: tgbasic [-2 | --double | --scale 1..4] [folder]\n");
            return 1;
        } else {
            dir = argv[i];
        }
    }
    mkdir(dir, 0777);
    if (fs_posix_init(&host, dir) != 0 || fs_mount("sd", &fs_posix_ops, &host) != 0) {
        fprintf(stderr, "cannot use the folder %s\n", dir);
        return 3;
    }
    fs_chdir("/sd");
    if (!plat_init())
        return 1;
    if (tg_text_load_cp437(&screen) != 0)
        return 2;
    tg_text_init(&screen);
    tg_init(&g);
    g.mode = TG_MODE_TEXT;
    g.text = &screen;
    basic_env_init(&env, &screen, &hooks);
    for (;;) {
        const int r = basic_env_run(&env);
        if (r == BASIC_EXIT_BYE)
            break;
        /* EDIT: the editor is another program on the desktop; it works in the same folder, with the same /sd */
        char cmd[600];
        snprintf(cmd, sizeof cmd, "./tde_ed '%s' '%s'", dir, env.edit_name);
        tg_console_puts(&env.con, "Starting the editor ...\n");
        frame();
        if (system(cmd) != 0)
            tg_console_puts(&env.con, "The editor could not be run (make tde_ed).\n");
        if (basic_env_load(&env, env.edit_name) < 0)
            env.length = 0;
        tg_console_puts(&env.con, "\n");
    }
    basic_env_close(&env);
    plat_shutdown();
    return 0;
}
