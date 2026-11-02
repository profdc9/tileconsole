/* SPDX-License-Identifier: MIT */
/*
 * The development kit on the board: the BASIC environment (MY-BASIC) and the editor (TDE) in one program, on the files of the SD card (/sd; a RAM disk if there is
 * no usable card). EDIT at the BASIC prompt runs TDE on the file; leaving TDE comes back to BASIC with the program loaded again. A USB keyboard is needed.
 *
 * Both want more stack than the 4 KB core 0 has (TDE keeps path-sized buffers in its functions), so everything runs on a stack of its own in ordinary RAM.
 *
 * On the desktop (no DEVKIT_BOARD) it is the same program: /sd is a folder of the host (the first argument, default ./devkit_files), the editor's configuration
 * comes from tde/config of the source tree the first time (run from the tilegame directory), and the window is SDL's.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "basic_env.h"
#include "platform.h"
#include "tde_os.h"
#include "tg_text.h"
#include "tilefs.h"
#include "tilegfx.h"

#ifdef DEVKIT_BOARD
#include "embedded.h"
#include "fs_fat.h"
#include "sd_spi_pico.h"
#define SD_HZ 12500000u
#define STACK_BYTES (48 * 1024)
#else
#include <sys/stat.h>
#include <sys/types.h>
FILE *__real_fopen(const char *name, const char *mode);
int __real_mkdir(const char *path, mode_t mode);   /* the C library's own (the editor's fopen is answered from tilefs) */
#endif

static tg_text_t screen;
static tg_t g;
static basic_env_t env;
#ifdef DEVKIT_BOARD
static fs_fat_t fat;
static fs_ram_t ram;
#define RAM_DISK_BYTES (96 * 1024)
static sd_spi_pico_t port;
static sd_spi_t sd;
static uint8_t stack[STACK_BYTES] __attribute__((aligned(8)));
#else
static fs_posix_t host;
static const char *host_dir = "devkit_files";
#endif

static void show(void)
{
    plat_present(&g);
}

static void frame(void)
{
    plat_wait_frame();
    plat_present(&g);
}

static void idle(void)
{
    frame();
}

#ifdef DEVKIT_BOARD
/* Mounts what holds the files at /sd; returns a line for the first screen if it is not the card. */
static const char *mount_files(void)
{
    sd_spi_pico_init(&port, &sd, spi0, SD_SPI_PICO_EMUBOARD2350, SD_HZ);
    const int e = fs_fat_init(&fat, 0, &sd_spi_dev, &sd);
    if (e == 0 && fs_mount("sd", &fs_fat_ops, &fat) == 0)
        return NULL;
    static char why[100];
    snprintf(why, sizeof why, "No usable SD card (%s): the files are in RAM and are lost at power off.\n", fs_strerror(e < 0 ? e : FS_EIO));
    fs_fat_done(&fat);
    uint8_t *pool = malloc(RAM_DISK_BYTES);   /* (only taken from the heap when it is used) */
    if (pool)
        fs_ram_init(&ram, pool, RAM_DISK_BYTES);
    fs_mount("sd", &fs_ram_ops, &ram);
    return why;
}

/* The editor's configuration into /sd/tde the first time (it looks there: TDEHOME). */
static void install_config(void)
{
    fs_stat_t st;
    if (fs_stat("/sd/tde/tde.cfg", &st) == 0)
        return;
    fs_mkdir("/sd/tde");
    for (const embedded_file_t *f = devkit_config_files; f->name; f++) {
        char path[64];
        snprintf(path, sizeof path, "/sd/tde/%s", f->name);
        const int fd = fs_open(path, FS_WRONLY | FS_CREAT | FS_TRUNC);
        if (fd >= 0) {
            fs_write(fd, f->data, f->size);
            fs_close(fd);
        }
    }
}
#else
static const char *mount_files(void)
{
    __real_mkdir(host_dir, 0777);
    if (fs_posix_init(&host, host_dir) != 0 || fs_mount("sd", &fs_posix_ops, &host) != 0) {
        static char why[200];
        snprintf(why, sizeof why, "Cannot use the folder %s.\n", host_dir);
        return why;
    }
    return NULL;
}

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
#endif

static int get_char(void)
{
#ifndef DEVKIT_BOARD
    if (plat_quit_requested())
        exit(0);
#endif
    return plat_getchar();
}

/* The two ways of reading the keyboard (typed characters, key presses) each keep what the other one was given: when one program hands over to the other, what is
 * still queued is what was typed to the first one, and is thrown away. */
static void drain_keys(void)
{
    plat_key_event_t ev;
    while (plat_getkey(&ev)) {}
    while (plat_getchar() >= 0) {}
}

static void run_editor(const char *name)
{
    static char program[] = "/sd/tde/tde";
    static char file[BASIC_NAME_MAX + 5];
    char *args[3] = {program, NULL, NULL};
    int n = 1;
    snprintf(file, sizeof file, "%s", name);
    if (*file)
        args[n++] = file;
    tde_console_attach(&screen, show);
    tde_console_set_idle(idle);
    setenv("TDEHOME", "/sd/tde", 1);
    drain_keys();
    tde_main(n, args);
    drain_keys();
}

static void devkit(void)
{
    static const basic_hooks_t hooks = {get_char, frame, plat_millis};
    if (!plat_init()) {
#ifdef DEVKIT_BOARD
        for (;;) {}
#else
        exit(1);
#endif
    }
    tg_text_load_cp437(&screen);
    tg_text_init(&screen);
    tg_init(&g);
    g.mode = TG_MODE_TEXT;
    g.text = &screen;
    const char *note = mount_files();
    fs_chdir("/sd");
    install_config();
    basic_env_init(&env, &screen, &hooks);
    if (note)
        tg_console_puts(&env.con, note);
    for (;;) {
        const int r = basic_env_run(&env);
        if (r == BASIC_EXIT_EDIT) {
            run_editor(env.edit_name);
            if (basic_env_load(&env, env.edit_name) < 0)
                env.length = 0;
            tg_console_puts(&env.con, "\n");
        } else {
#ifdef DEVKIT_BOARD
            tg_console_puts(&env.con, "Nothing to leave to: this is the whole machine. EDIT starts the editor.\n");
#else
            break;
#endif
        }
    }
}

int main(int argc, char **argv)
{
#ifdef DEVKIT_BOARD
    (void)argc;
    (void)argv;
    /* the stack grows down from the top of the array; interrupt handlers use it too */
    uint8_t *top = stack + sizeof stack;
    __asm volatile("msr msp, %0\n\tbx %1\n" ::"r"(top), "r"(devkit) : "memory");
    __builtin_unreachable();
#else
    if (argc > 1)
        host_dir = argv[1];
    devkit();
    basic_env_close(&env);
    plat_shutdown();
    return 0;
#endif
}
