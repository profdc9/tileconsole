/* SPDX-License-Identifier: MIT */
#include "sdtest_logic.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ATTR_TEXT TG_TEXT_ATTR(TG_TEXT_LIGHT_GREY, TG_TEXT_BLACK)
#define ATTR_HEAD TG_TEXT_ATTR(TG_TEXT_YELLOW, TG_TEXT_BLACK)
#define ATTR_OK TG_TEXT_ATTR(TG_TEXT_LIGHT_GREEN, TG_TEXT_BLACK)
#define ATTR_BAD TG_TEXT_ATTR(TG_TEXT_LIGHT_RED, TG_TEXT_BLACK)

static void refresh(sdtest_t *d)
{
    if (d->refresh)
        d->refresh(d->user);
}

static void line(sdtest_t *d, uint8_t attr, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void line(sdtest_t *d, uint8_t attr, const char *fmt, ...)
{
    va_list ap;
    char text[100];
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    tg_console_set_attr(&d->con, attr);
    tg_console_puts(&d->con, text);
    tg_console_putc(&d->con, '\n');
    tg_console_set_attr(&d->con, ATTR_TEXT);
    refresh(d);
}

/* A step's result: PASS, or FAIL with the reason, and counted. */
static int result(sdtest_t *d, int error, const char *what)
{
    if (error >= 0) {
        line(d, ATTR_OK, "  PASS  %s", what);
        return 1;
    }
    line(d, ATTR_BAD, "  FAIL  %s: %s", what, fs_strerror(error));
    d->failures++;
    return 0;
}

static int verdict(sdtest_t *d, int ok, const char *what)
{
    return result(d, ok ? 0 : FS_EIO, what);
}

void sdtest_init(sdtest_t *d, tg_text_t *screen, const fs_blockdev_t *dev, void *dev_ctx, fs_fat_t *fat, uint32_t (*millis)(void),
                 void (*refresh_fn)(void *), void *user)
{
    memset(d, 0, sizeof *d);
    d->dev = dev;
    d->dev_ctx = dev_ctx;
    d->fat = fat;
    d->millis = millis;
    d->refresh = refresh_fn;
    d->user = user;
    tg_console_init(&d->con, screen, ATTR_TEXT);
    tg_console_clear(&d->con);
}

static const char *type_name(BYTE t)
{
    switch (t) {
    case FS_FAT12: return "FAT12";
    case FS_FAT16: return "FAT16";
    case FS_FAT32: return "FAT32";
    case FS_EXFAT: return "exFAT";
    default: return "unknown";
    }
}

static void unmount(sdtest_t *d)
{
    if (d->mounted) {
        fs_unmount("sd");
        fs_fat_done(d->fat);
        d->mounted = 0;
    }
}

static unsigned mb(uint64_t bytes)
{
    return (unsigned)(bytes / (1024 * 1024));
}

static unsigned kb_per_s(uint32_t bytes, uint32_t ms)
{
    return ms ? (unsigned)((uint64_t)bytes * 1000 / ms / 1024) : 0;
}

/* The byte of the test file at this position. */
static uint8_t pattern(uint32_t at)
{
    return (uint8_t)(at * 2654435761u >> 24);
}

static void fill(uint8_t *p, size_t n, uint32_t at)
{
    for (size_t i = 0; i < n; i++)
        p[i] = pattern(at + (uint32_t)i);
}

static void list_root(sdtest_t *d)
{
    fs_dirent_t e;
    const int dd = fs_opendir("/sd");
    int shown = 0, total = 0;
    if (!result(d, dd, "open the root directory"))
        return;
    while (fs_readdir(dd, &e) == 1) {
        total++;
        if (shown < 8) {
            shown++;
            if (e.is_dir)
                line(d, ATTR_TEXT, "        %-40.40s <DIR>", e.name);
            else
                line(d, ATTR_TEXT, "        %-40.40s %10lu", e.name, (unsigned long)e.size);
        }
    }
    fs_closedir(dd);
    if (total > shown)
        line(d, ATTR_TEXT, "        ... and %d more", total - shown);
    if (total == 0)
        line(d, ATTR_TEXT, "        (empty)");
}

static void big_file(sdtest_t *d)
{
    uint32_t t0 = d->millis();
    int fd = fs_open("/sd/TILEFS.TST", FS_WRONLY | FS_CREAT | FS_TRUNC);
    int ok = fd >= 0;
    for (uint32_t at = 0; ok && at < SDTEST_BIG_BYTES; at += sizeof d->buf) {
        fill(d->buf, sizeof d->buf, at);
        ok = fs_write(fd, d->buf, sizeof d->buf) == (int)sizeof d->buf;
    }
    if (fd >= 0)
        ok = fs_close(fd) == 0 && ok;
    const uint32_t write_ms = d->millis() - t0;
    if (!verdict(d, ok, "write a 128 KB file"))
        return;
    line(d, ATTR_TEXT, "        %u KB/s", kb_per_s(SDTEST_BIG_BYTES, write_ms));
    t0 = d->millis();
    fd = fs_open("/sd/TILEFS.TST", FS_RDONLY);
    ok = fd >= 0;
    uint32_t bad = 0;
    for (uint32_t at = 0; ok && at < SDTEST_BIG_BYTES; at += sizeof d->buf) {
        ok = fs_read(fd, d->buf, sizeof d->buf) == (int)sizeof d->buf;
        for (uint32_t i = 0; ok && i < sizeof d->buf; i++)
            bad += d->buf[i] != pattern(at + i);
    }
    if (fd >= 0)
        ok = fs_close(fd) == 0 && ok;
    const uint32_t read_ms = d->millis() - t0;
    if (verdict(d, ok && bad == 0, "read it back and compare"))
        line(d, ATTR_TEXT, "        %u KB/s", kb_per_s(SDTEST_BIG_BYTES, read_ms));
    fs_stat_t st;
    verdict(d, fs_stat("/sd/TILEFS.TST", &st) == 0 && st.size == SDTEST_BIG_BYTES, "its size is right");
}

static void small_files(sdtest_t *d)
{
    char name[64];
    int ok = fs_mkdir("/sd/TILEFS.D") == 0;
    for (int i = 0; ok && i < 10; i++) {
        snprintf(name, sizeof name, "/sd/TILEFS.D/File number %d.txt", i);
        const int fd = fs_open(name, FS_WRONLY | FS_CREAT);
        ok = fd >= 0 && fs_write(fd, "hello", 5) == 5 && fs_close(fd) == 0;
    }
    ok = ok && fs_rename("/sd/TILEFS.D/File number 3.txt", "/sd/TILEFS.D/Renamed.txt") == 0;
    ok = ok && fs_unlink("/sd/TILEFS.D/File number 4.txt") == 0;
    verdict(d, ok, "make a directory, 10 files, rename, delete");
    int count = 0;
    const int dd = fs_opendir("/sd/TILEFS.D");
    fs_dirent_t e;
    while (dd >= 0 && fs_readdir(dd, &e) == 1)
        count += e.size == 5;
    if (dd >= 0)
        fs_closedir(dd);
    verdict(d, count == 9, "the directory lists 9 files of 5 bytes");
    for (int i = 0; i < 10; i++) {
        snprintf(name, sizeof name, "/sd/TILEFS.D/File number %d.txt", i);
        fs_unlink(name);
    }
    fs_unlink("/sd/TILEFS.D/Renamed.txt");
    verdict(d, fs_rmdir("/sd/TILEFS.D") == 0 && fs_stat("/sd/TILEFS.D", &(fs_stat_t){0}) == FS_ENOENT, "remove them and the directory");
}

int sdtest_run(sdtest_t *d)
{
    uint64_t free_before, total, free_after;
    d->failures = 0;
    d->offer_format = d->confirm_format = 0;
    unmount(d);
    tg_console_clear(&d->con);
    line(d, ATTR_HEAD, "SD card test: FatFs on SPI");
    line(d, ATTR_TEXT, " ");
    int e = fs_fat_init(d->fat, 0, d->dev, d->dev_ctx);
    if (e == 0)
        e = fs_mount("sd", &fs_fat_ops, d->fat);
    if (!result(d, e, "start the card and mount the file system")) {
        if (e == FS_ENOMEDIA) {
            line(d, ATTR_TEXT, "        no card in the socket: put one in and press R");
        } else if (e == FS_ENODEV) {
            line(d, ATTR_TEXT, "        the card has no file system (or one that is not FAT)");
            line(d, ATTR_TEXT, "        F formats it (erasing it), R tries again");
            d->offer_format = 1;
        } else {
            line(d, ATTR_TEXT, "        R tries again");
        }
        return d->failures;
    }
    d->mounted = 1;
    line(d, ATTR_TEXT, "        %s, %u MB card, %u KB clusters", type_name(d->fat->fs.fs_type), mb((uint64_t)d->dev->sectors(d->dev_ctx) * 512),
         (unsigned)d->fat->fs.csize / 2);
    result(d, fs_space("/sd", &free_before, &total), "free space");
    line(d, ATTR_TEXT, "        %u MB free of %u MB", mb(free_before), mb(total));
    list_root(d);
    big_file(d);
    small_files(d);
    verdict(d, fs_unlink("/sd/TILEFS.TST") == 0, "delete the big file");
    result(d, fs_space("/sd", &free_after, &total), "free space again");
    verdict(d, free_after + 64 * 1024 >= free_before, "the space came back");
    line(d, ATTR_TEXT, " ");
    if (d->failures == 0)
        line(d, ATTR_OK, "ALL TESTS PASSED   (R runs it again)");
    else
        line(d, ATTR_BAD, "%d TESTS FAILED   (R runs it again)", d->failures);
    return d->failures;
}

void sdtest_char(sdtest_t *d, int ch)
{
    ch = toupper(ch);
    if (ch == 'R') {
        sdtest_run(d);
    } else if (ch == 'F' && d->offer_format) {
        d->confirm_format = 1;
        line(d, ATTR_BAD, "Press Y to erase the card and format it, any other key to cancel");
    } else if (d->confirm_format) {
        d->confirm_format = 0;
        if (ch != 'Y') {
            line(d, ATTR_TEXT, "Cancelled");
            return;
        }
        line(d, ATTR_TEXT, "Formatting ...");
        const int e = fs_fat_format(0, d->dev, d->dev_ctx, FS_FAT_ANY);
        if (e < 0) {
            line(d, ATTR_BAD, "Format failed: %s", fs_strerror(e));
            d->failures = 1;
        } else {
            sdtest_run(d);
        }
    }
}
