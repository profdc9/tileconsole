/* SPDX-License-Identifier: MIT */
/*
 * The SD card test, on the board and on the desktop. On the board the card is the one in the EmuBoard2350's socket (SPI0); on the desktop
 * it is a disk image file (the first argument, default sdcard.img, made 64 MB and empty if it does not exist), so that the whole stack
 * can be tried on a PC first. A keyboard gives R (run again) and F, Y (format a card that has no file system).
 */
#include <stdio.h>

#include "platform.h"
#include "sdtest_logic.h"
#include "tg_text.h"
#include "tilefs.h"
#include "tilegfx.h"

#ifdef SDTEST_BOARD
#include "sd_spi_pico.h"
#define SD_HZ 12500000u   /* a first try on a new board: half of what a standard card can take */
#endif

static tg_text_t screen;
static tg_t g;                 /* not on the stack: it is 2.3 KB and the board's stack is small */
static sdtest_t test;
static fs_fat_t fat;

static void show(void *unused)
{
    (void)unused;
    plat_present(&g);
}

int main(int argc, char **argv)
{
#ifdef SDTEST_BOARD
    static sd_spi_pico_t port;
    static sd_spi_t sd;
    (void)argc;
    (void)argv;
    const fs_blockdev_t *dev = &sd_spi_dev;
    void *dev_ctx = &sd;
#else
    static fs_blk_file_t image;
    const fs_blockdev_t *dev = &fs_blk_file_dev;
    void *dev_ctx = &image;
    const char *path = argc > 1 ? argv[1] : "sdcard.img";
    if (fs_blk_file_open(&image, path, 131072) != 0) {
        fprintf(stderr, "cannot open or make the disk image %s\n", path);
        return 3;
    }
#endif
    if (!plat_init())
        return 1;
#ifdef SDTEST_BOARD
    sd_spi_pico_init(&port, &sd, spi0, SD_SPI_PICO_EMUBOARD2350, SD_HZ);
#endif
    if (tg_text_load_cp437(&screen) != 0)
        return 2;
    tg_text_init(&screen);
    tg_init(&g);
    g.mode = TG_MODE_TEXT;
    g.text = &screen;
    sdtest_init(&test, &screen, dev, dev_ctx, &fat, plat_millis, show, NULL);
    plat_wait_frame();
    sdtest_run(&test);
    while (!plat_quit_requested()) {
        plat_wait_frame();
        int ch;
        while ((ch = plat_getchar()) >= 0)
            sdtest_char(&test, ch);
        plat_present(&g);
    }
    plat_shutdown();
    return 0;
}
