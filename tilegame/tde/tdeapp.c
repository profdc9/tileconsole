/* SPDX-License-Identifier: MIT */
/*
 * TDE, the Thomson-Davis Editor, on the desktop: the editor's sources (tde/src, public domain) on the tilegame platform layer (SDL window, text mode) with
 * its files from a folder of the host mounted as /sd (the first argument, default ./tde_files, made if it is not there). Files named on the command line
 * after the folder are opened. The editor's configuration is looked for in /sd/tde.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "platform.h"
#include "tde_os.h"
#include "tg_text.h"
#include "tilefs.h"
#include "tilegfx.h"

static tg_text_t screen;
static tg_t g;                 /* not on the stack: it is 2.3 KB and the board's stack is small */
static fs_posix_t host;

FILE *__real_fopen(const char *name, const char *mode);
int __real_mkdir(const char *path, mode_t mode);   /* the C library's own (the editor's fopen is answered from tilefs) */

/* The editor's configuration into /sd/tde the first time: from the source tree's tde/config (run from the tilegame directory). */
static void install_config(void)
{
    static const char *const names[] = {"tde.cfg", "tde.hlp", "tde.shl"};
    static char data[100000];
    fs_stat_t st;
    if (fs_stat("/sd/tde/tde.cfg", &st) == 0)
        return;
    fs_mkdir("/sd/tde");
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        char host_path[100], path[100];
        snprintf(host_path, sizeof host_path, "tde/config/%s", names[i]);
        snprintf(path, sizeof path, "/sd/tde/%s", names[i]);
        FILE *fp = __real_fopen(host_path, "rb");
        if (!fp) {
            fprintf(stderr, "no %s: the editor starts without its configuration files\n", host_path);
            return;
        }
        const size_t n = fread(data, 1, sizeof data, fp);
        fclose(fp);
        const int fd = fs_open(path, FS_WRONLY | FS_CREAT | FS_TRUNC);
        if (fd >= 0) {
            fs_write(fd, data, n);
            fs_close(fd);
        }
    }
}

static void show(void)
{
    plat_present(&g);
}

static void idle(void)
{
    plat_wait_frame();
    plat_present(&g);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "tde_files";
    static char program[] = "/sd/tde/tde";
    char *args[16];
    int n = 0;

    __real_mkdir(dir, 0777);
    if (fs_posix_init(&host, dir) != 0 || fs_mount("sd", &fs_posix_ops, &host) != 0) {
        fprintf(stderr, "cannot use the folder %s\n", dir);
        return 3;
    }
    fs_chdir("/sd");
    install_config();
    if (!plat_init())
        return 1;
    if (tg_text_load_cp437(&screen) != 0)
        return 2;
    tg_text_init(&screen);
    tg_init(&g);
    g.mode = TG_MODE_TEXT;
    g.text = &screen;
    tde_console_attach(&screen, show);
    tde_console_set_idle(idle);
    args[n++] = program;
    for (int i = 2; i < argc && n < 15; i++)
        args[n++] = argv[i];
    args[n] = NULL;
    const int rc = tde_main(n, args);
    plat_shutdown();
    return rc;
}
