/* SPDX-License-Identifier: MIT */
/*
 * The file editor demo, on the desktop and on the board: PicoParanoia's editor on real files, through tilefs. On the board the files are on the SD card in
 * the EmuBoard2350's socket (mounted as /sd); with no usable card the demo says so and goes on with a RAM disk, whose files are lost at power off. On the
 * desktop /sd is a folder of the host (the first argument, default ./editsd_files, made if it is not there). A keyboard is needed.
 */
#include <stdio.h>

#include "consoleio.h"
#include "editsd_logic.h"
#include "fs_fat.h"
#include "platform.h"
#include "tilefs.h"

#ifdef EDITSD_BOARD
#include "sd_spi_pico.h"
#define SD_HZ 12500000u
#else
#include <sys/stat.h>
#endif

static fs_fat_t fat;
static fs_ram_t ram;
static uint8_t ram_pool[96 * 1024];
#ifdef EDITSD_BOARD
static sd_spi_pico_t port;
static sd_spi_t sd;
#else
static fs_posix_t host;
#endif

/* Mounts what holds the files at /sd; returns a line saying what it is, for the first screen. */
static const char *mount_files(const char *arg)
{
#ifdef EDITSD_BOARD
    (void)arg;
    sd_spi_pico_init(&port, &sd, spi0, SD_SPI_PICO_EMUBOARD2350, SD_HZ);
    const int e = fs_fat_init(&fat, 0, &sd_spi_dev, &sd);
    if (e == 0 && fs_mount("sd", &fs_fat_ops, &fat) == 0)
        return NULL;
    static char why[80];
    snprintf(why, sizeof why, "No usable SD card (%s): the files are in RAM and are lost at power off.", fs_strerror(e < 0 ? e : FS_EIO));
    fs_fat_done(&fat);
    fs_ram_init(&ram, ram_pool, sizeof ram_pool);
    fs_mount("sd", &fs_ram_ops, &ram);
    return why;
#else
    (void)fat;
    (void)ram_pool;
    (void)ram;
    const char *dir = arg ? arg : "editsd_files";
    mkdir(dir, 0777);
    if (fs_posix_init(&host, dir) != 0 || fs_mount("sd", &fs_posix_ops, &host) != 0) {
        fprintf(stderr, "cannot use the folder %s\n", dir);
        return "";
    }
    return NULL;
#endif
}

int main(int argc, char **argv)
{
    if (!plat_init())
        return 1;
    console_use_platform();
    console_init();
    const char *note = mount_files(argc > 1 ? argv[1] : NULL);
    if (note && !*note)
        return 3;
    fs_chdir("/sd");
    editsd_init();
    if (note) {
        console_puts(note);
        console_press_space();
    }
    for (;;) {
        editsd_menu();
        if (plat_quit_requested())
            break;
    }
    plat_shutdown();
    return 0;
}
