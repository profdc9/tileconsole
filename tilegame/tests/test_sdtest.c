/* SPDX-License-Identifier: MIT */
/* Tests of the SD card test logic: run it on RAM disks, an empty one, one with no card and ones that fail, and read what it printed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs_fat.h"
#include "sdtest_logic.h"
#include "tg_text.h"

static int checks;
#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static tg_text_t screen;
static sdtest_t test;
static fs_fat_t fat;
static uint8_t disk[66u << 20];   /* FAT32 needs at least 65525 clusters */
static fs_blk_ram_t ram;
static uint32_t now;
static int refreshes;

static uint32_t clock_ms(void) { return now += 7; }
static void refreshed(void *p) { (void)p; refreshes++; }

/* Does any row of the screen contain this text? */
static int shows(const char *text)
{
    char row[81];
    for (int r = 0; r < 25; r++) {
        for (int c = 0; c < 80; c++)
            row[c] = (char)(screen.cells[r * 80 + c] & 255);
        row[80] = 0;
        if (strstr(row, text))
            return 1;
    }
    return 0;
}

/* A device that wraps the RAM disk and fails as told. */
typedef struct {
    int init_error;            /* returned from init */
    int writes_ok;             /* writes allowed before they fail; -1 never fail */
    int corrupt_after_reads;   /* after this many reads, flip a bit in what is read; -1 never */
} bad_t;
static bad_t bad;

static int bad_init(void *c) { return ((bad_t *)c)->init_error ? ((bad_t *)c)->init_error : fs_blk_ram_dev.init(&ram); }
static int bad_read(void *c, void *buf, uint32_t s, uint32_t n)
{
    bad_t *b = c;
    const int e = fs_blk_ram_dev.read(&ram, buf, s, n);
    if (b->corrupt_after_reads >= 0 && b->corrupt_after_reads-- == 0) {
        ((uint8_t *)buf)[0] ^= 0x10;
        b->corrupt_after_reads = 0;
    }
    return e;
}
static int bad_write(void *c, const void *buf, uint32_t s, uint32_t n)
{
    bad_t *b = c;
    if (b->writes_ok == 0)
        return FS_EIO;
    if (b->writes_ok > 0)
        b->writes_ok--;
    return fs_blk_ram_dev.write(&ram, buf, s, n);
}
static uint32_t bad_sectors(void *c) { (void)c; return fs_blk_ram_dev.sectors(&ram); }
static const fs_blockdev_t bad_dev = {bad_init, bad_read, bad_write, NULL, bad_sectors};

static void fresh(const fs_blockdev_t *dev, void *ctx)
{
    fs_reset();
    memset(&screen, 0, sizeof screen);
    tg_text_load_cp437(&screen);
    tg_text_init(&screen);
    refreshes = 0;
    sdtest_init(&test, &screen, dev, ctx, &fat, clock_ms, refreshed, NULL);
}

static void formatted(int type)
{
    fs_reset();
    memset(disk, 0, sizeof disk);
    CHECK(fs_blk_ram_init(&ram, disk, sizeof disk) == 0);
    { const int e = fs_fat_format(0, &fs_blk_ram_dev, &ram, type); if (e) printf("format type %d: %d\n", type, e); CHECK(e == 0); }
}

int main(void)
{
    /* a good card, in each kind of file system */
    const int types[] = {FS_FAT_FAT | FS_FAT_SFD, FS_FAT_FAT32 | FS_FAT_SFD, FS_FAT_EXFAT | FS_FAT_SFD, FS_FAT_ANY};
    for (unsigned i = 0; i < sizeof types / sizeof types[0]; i++) {
        formatted(types[i]);
        fresh(&fs_blk_ram_dev, &ram);
        CHECK(sdtest_run(&test) == 0);
        CHECK(shows("ALL TESTS PASSED"));
        CHECK(shows("PASS  write a 128 KB file"));
        CHECK(shows("PASS  read it back and compare"));
        CHECK(shows("PASS  the space came back"));
        CHECK(shows("KB/s"));
        CHECK(!shows("FAIL"));
        CHECK(refreshes > 10);                  /* it showed its progress */
        CHECK(test.mounted);
        sdtest_char(&test, 'r');                /* again, from the keyboard */
        CHECK(test.failures == 0 && shows("ALL TESTS PASSED"));
    }
    formatted(FS_FAT_FAT | FS_FAT_SFD);
    fresh(&fs_blk_ram_dev, &ram);
    sdtest_run(&test);
    CHECK(shows("FAT16") || shows("FAT12"));
    formatted(FS_FAT_EXFAT | FS_FAT_SFD);
    fresh(&fs_blk_ram_dev, &ram);
    sdtest_run(&test);
    CHECK(shows("exFAT"));

    /* a card with a file already on it: it is listed, and left alone */
    formatted(FS_FAT_FAT32 | FS_FAT_SFD);
    fresh(&fs_blk_ram_dev, &ram);
    {
        static fs_fat_t pre;
        CHECK(fs_fat_init(&pre, 0, &fs_blk_ram_dev, &ram) == 0);
        CHECK(fs_mount("sd", &fs_fat_ops, &pre) == 0);
        const int fd = fs_open("/sd/Keep Me.txt", FS_WRONLY | FS_CREAT);
        CHECK(fs_write(fd, "precious", 8) == 8 && fs_close(fd) == 0);
        CHECK(fs_unmount("sd") == 0 && fs_fat_done(&pre) == 0);
    }
    CHECK(sdtest_run(&test) == 0);
    CHECK(shows("Keep Me.txt"));
    {
        char buf[16];
        const int fd = fs_open("/sd/Keep Me.txt", FS_RDONLY);
        CHECK(fd >= 0 && fs_read(fd, buf, 16) == 8 && !memcmp(buf, "precious", 8));
        CHECK(fs_close(fd) == 0);
    }

    /* a card with nothing on it: offered a format, which can be refused or accepted */
    memset(disk, 0, sizeof disk);
    CHECK(fs_blk_ram_init(&ram, disk, sizeof disk) == 0);
    fresh(&fs_blk_ram_dev, &ram);
    CHECK(sdtest_run(&test) == 1);
    CHECK(shows("FAIL") && shows("F formats"));
    CHECK(shows("no file system"));
    CHECK(test.offer_format && !test.mounted);
    sdtest_char(&test, 'y');                    /* Y without F does nothing */
    CHECK(shows("F formats") && !shows("Formatting"));
    sdtest_char(&test, 'F');
    CHECK(shows("Press Y"));
    sdtest_char(&test, 'n');
    CHECK(shows("Cancelled"));
    sdtest_char(&test, 'y');                    /* the question is over */
    CHECK(!shows("Formatting"));
    sdtest_char(&test, 'f');
    sdtest_char(&test, 'Y');
    CHECK(test.mounted);                        /* (the run that follows clears the screen, so "Formatting" is gone) */
    CHECK(test.failures == 0 && shows("ALL TESTS PASSED"));
    CHECK(!test.offer_format);
    sdtest_char(&test, 'F');                    /* nothing to format now: ignored */
    CHECK(!shows("Press Y to erase") || test.failures == 0);

    /* no card at all */
    bad = (bad_t){FS_ENOMEDIA, -1, -1};
    fresh(&bad_dev, &bad);
    CHECK(sdtest_run(&test) == 1);
    CHECK(shows("no card in the socket") && !test.offer_format);
    sdtest_char(&test, 'F');                    /* no card to format */
    CHECK(!shows("Press Y"));
    bad.init_error = FS_EIO;                    /* a card that does not work: not offered a format */
    CHECK(sdtest_run(&test) == 1);
    CHECK(!test.offer_format && shows("FAIL") && shows("i/o error"));
    sdtest_char(&test, 'F');
    CHECK(!shows("Press Y"));

    /* a card that stops taking writes part way */
    formatted(FS_FAT_FAT32 | FS_FAT_SFD);
    bad = (bad_t){0, 8, -1};
    fresh(&bad_dev, &bad);
    CHECK(sdtest_run(&test) > 0);
    CHECK(shows("FAIL") && shows("TESTS FAILED"));
    CHECK(!shows("ALL TESTS PASSED"));
    /* a card that reads back wrong */
    formatted(FS_FAT_FAT32 | FS_FAT_SFD);
    bad = (bad_t){0, -1, 3};
    fresh(&bad_dev, &bad);
    CHECK(sdtest_run(&test) > 0);
    CHECK(shows("FAIL"));
    /* a card that is full */
    formatted(FS_FAT_FAT | FS_FAT_SFD);
    {
        static uint8_t small[270 * 512];
        static fs_blk_ram_t small_ram;
        CHECK(fs_blk_ram_init(&small_ram, small, sizeof small) == 0);
        memset(small, 0, sizeof small);
        CHECK(fs_fat_format(0, &fs_blk_ram_dev, &small_ram, FS_FAT_FAT | FS_FAT_SFD) == 0);
        fresh(&fs_blk_ram_dev, &small_ram);
        CHECK(sdtest_run(&test) > 0);
        CHECK(shows("FAIL  write a 128 KB file"));
    }
    fs_reset();
    printf("test_sdtest: %d checks passed\n", checks);
    return 0;
}
