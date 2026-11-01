/* SPDX-License-Identifier: MIT */
/*
 * The logic of the SD card test: mounts the card with FatFs (through tilefs), reports what it is, lists the root directory, writes and
 * reads back a file while timing it, exercises directories and renames, and prints PASS or FAIL for each step on a text console. It knows the
 * card only as an fs_blockdev_t, so a test can run it on a RAM disk or a disk image; see sdtest.c for the program around it.
 *
 * Keys: R runs it again; F (then Y) formats a card with no file system, erasing it.
 */
#ifndef SDTEST_LOGIC_H
#define SDTEST_LOGIC_H

#include <stdint.h>

#include "fs_fat.h"
#include "tg_console.h"
#include "tilefs.h"

#define SDTEST_BIG_BYTES (128 * 1024)

typedef struct {
    tg_console_t con;
    const fs_blockdev_t *dev;
    void *dev_ctx;
    fs_fat_t *fat;                       /* about 6 KB: the caller keeps it (not on the board's 2 KB stack) */
    uint32_t (*millis)(void);
    void (*refresh)(void *user);         /* called after each line, so that a slow step shows how far it has got; may be NULL */
    void *user;
    int mounted;
    int failures;                        /* of the last run */
    int offer_format;                    /* the card answered but holds no file system: F is accepted */
    int confirm_format;                  /* F was pressed: Y formats */
    uint8_t buf[4096];
} sdtest_t;

void sdtest_init(sdtest_t *d, tg_text_t *screen, const fs_blockdev_t *dev, void *dev_ctx, fs_fat_t *fat, uint32_t (*millis)(void),
                 void (*refresh)(void *), void *user);

/* Runs all the steps and returns how many failed (0: all passed). A card that cannot be mounted counts as one failure. */
int sdtest_run(sdtest_t *d);

/* A typed key: R, F, Y (any case). */
void sdtest_char(sdtest_t *d, int ch);

#endif
