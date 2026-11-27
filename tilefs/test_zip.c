/* SPDX-License-Identifier: MIT */
/* Tests of the zip backend (fs_zip.c): archives made here entry by entry (good ones and broken ones) on a RAM disk, and two made by
 * real tools (Info-ZIP's zip -0 and Python's zipfile), all read through tilefs as programs would. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefs.h"

static unsigned long checks;

#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define EQ(a, b) do { const long long va_ = (long long)(a), vb_ = (long long)(b); checks++; \
    if (va_ != vb_) { fprintf(stderr, "FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); exit(1); } } while (0)

static fs_ram_t ram;
static uint8_t pool[1u << 20];

static void fresh(void)
{
    fs_reset();
    EQ(fs_ram_init(&ram, pool, sizeof pool), 0);
    EQ(fs_mount("sd", &fs_ram_ops, &ram), 0);
}

/* ---- making archives ---- */

typedef struct {
    const char *name;
    const void *data;
    uint32_t len;            /* the bytes stored */
    uint16_t method, flags;  /* 0 stored, 8 deflated (the bytes are then only a stand-in); flags bit 0: encrypted */
    uint16_t time, date;     /* MS-DOS */
    uint32_t usize;          /* the size it says it has once uncompressed (0: len) */
    const char *lextra, *cextra;   /* extra fields, local and central */
    uint16_t lxlen, cxlen;
} zent_t;

static uint8_t zbuf[400000];
static uint32_t zlen, z_local[32], z_central[32], z_end;

static void put8(uint8_t v) { zbuf[zlen++] = v; }
static void put16(uint16_t v) { put8((uint8_t)v); put8((uint8_t)(v >> 8)); }
static void put32(uint32_t v) { put16((uint16_t)v); put16((uint16_t)(v >> 16)); }
static void putn(const void *p, uint32_t n) { memcpy(zbuf + zlen, p, n); zlen += n; }

/* An archive of n entries in zbuf, after `stub` (stub_len bytes); offsets count the stub if `absolute` (as zip -A makes them), else
 * they are from the start of the archive proper. */
static uint32_t make_zip(const zent_t *e, int n, const char *stub, uint32_t stub_len, int absolute, const char *comment, uint32_t comment_len)
{
    zlen = 0;
    if (stub_len)
        putn(stub, stub_len);
    const uint32_t base = absolute ? 0 : stub_len;
    for (int i = 0; i < n; i++) {
        z_local[i] = zlen;
        put32(0x04034b50u);
        put16(10);
        put16(e[i].flags);
        put16(e[i].method);
        put16(e[i].time);
        put16(e[i].date);
        put32(0x12345678u);   /* (the CRC: the backend does not check it) */
        put32(e[i].len);
        put32(e[i].usize ? e[i].usize : e[i].len);
        put16((uint16_t)strlen(e[i].name));
        put16(e[i].lxlen);
        putn(e[i].name, (uint32_t)strlen(e[i].name));
        if (e[i].lxlen)
            putn(e[i].lextra, e[i].lxlen);
        if (e[i].len)
            putn(e[i].data, e[i].len);
    }
    const uint32_t cd = zlen;
    for (int i = 0; i < n; i++) {
        z_central[i] = zlen;
        put32(0x02014b50u);
        put16(0x031e);
        put16(10);
        put16(e[i].flags);
        put16(e[i].method);
        put16(e[i].time);
        put16(e[i].date);
        put32(0x12345678u);
        put32(e[i].len);
        put32(e[i].usize ? e[i].usize : e[i].len);
        put16((uint16_t)strlen(e[i].name));
        put16(e[i].cxlen);
        put16(0);   /* comment */
        put16(0);   /* disk */
        put16(0);
        put32(0);
        put32(z_local[i] - base);
        putn(e[i].name, (uint32_t)strlen(e[i].name));
        if (e[i].cxlen)
            putn(e[i].cextra, e[i].cxlen);
    }
    z_end = zlen;
    put32(0x06054b50u);
    put16(0);
    put16(0);
    put16((uint16_t)n);
    put16((uint16_t)n);
    put32(z_end - cd);
    put32(cd - base);
    put16((uint16_t)comment_len);
    if (comment_len)
        putn(comment, comment_len);
    return zlen;
}

static void put_file(const char *path, const void *data, uint32_t len)
{
    const int fd = fs_open(path, FS_WRONLY | FS_CREAT | FS_TRUNC);
    CHECK(fd >= 0);
    EQ(fs_write(fd, data, len), len);
    EQ(fs_close(fd), 0);
}

static fs_zip_t zip;
static uint8_t zindex[100000];

/* Opens and loads the archive and mounts it at /ar: 0, or the first failure (the archive is let go of then). */
static int mount_zip(const char *path)
{
    int e = fs_zip_open(&zip, path);
    if (e)
        return e;
    CHECK(zip.index_bytes <= sizeof zindex);
    e = fs_zip_load(&zip, zindex);
    if (!e)
        e = fs_mount("ar", &fs_zip_ops, &zip);
    if (e)
        fs_zip_close(&zip);
    return e;
}

static void unmount_zip(void)
{
    EQ(fs_unmount("ar"), 0);
    fs_zip_close(&zip);
}

/* The whole file, read in pieces of `step` bytes, into out; its length or an error. */
static long read_all(const char *path, uint8_t *out, size_t cap, size_t step)
{
    const int fd = fs_open(path, FS_RDONLY);
    if (fd < 0)
        return fd;
    long n = 0;
    for (;;) {
        const size_t want = step < cap - (size_t)n ? step : cap - (size_t)n;
        const int r = fs_read(fd, out + n, want);
        CHECK(r >= 0);
        if (r == 0)
            break;
        n += r;
    }
    EQ(fs_close(fd), 0);
    return n;
}

static int listed(const char *dir, const char *name, int is_dir, int64_t size)
{
    const int dd = fs_opendir(dir);
    CHECK(dd >= 0);
    fs_dirent_t e;
    int times = 0;
    while (fs_readdir(dd, &e) == 1)
        if (!strcmp(e.name, name)) {
            times++;
            CHECK(e.is_dir == is_dir && e.size == size);
        }
    EQ(fs_closedir(dd), 0);
    return times;
}

static int count_entries(const char *dir)
{
    const int dd = fs_opendir(dir);
    if (dd < 0)
        return dd;
    fs_dirent_t e;
    int n = 0;
    while (fs_readdir(dd, &e) == 1)
        n++;
    EQ(fs_closedir(dd), 0);
    return n;
}

#define DOS_DATE(y, m, d) (uint16_t)((((y) - 1980) << 9) | ((m) << 5) | (d))
#define DOS_TIME(h, mi, s) (uint16_t)(((h) << 11) | ((mi) << 5) | ((s) / 2))

static uint8_t big[100000], got[120000];

/* ---- tests ---- */

static void test_reading(void)
{
    fresh();
    for (uint32_t i = 0; i < sizeof big; i++)
        big[i] = (uint8_t)(i * 7 + (i >> 9));
    static const char bin[] = {'A', 0, 'B', 0, (char)0xFF};
    const zent_t e[] = {
        {.name = "main.lua", .data = "print(1)\n", .len = 9, .time = DOS_TIME(13, 45, 58), .date = DOS_DATE(2024, 2, 29)},
        {.name = "lib/util.lua", .data = "return {}\n", .len = 10},
        {.name = "Sprites/Hero.spr", .data = bin, .len = sizeof bin},
        {.name = "docs/", .time = DOS_TIME(12, 0, 0), .date = DOS_DATE(2023, 7, 4)},
        {.name = "docs/readme.txt", .data = "read me", .len = 7, .lextra = "\x55\x54\x05\x00\x01\x00\x00\x00\x00", .lxlen = 9},
        {.name = "empty.txt"},
        {.name = "big.bin", .data = big, .len = sizeof big},
    };
    put_file("/sd/game.zip", zbuf, make_zip(e, 7, NULL, 0, 0, NULL, 0));
    EQ(mount_zip("/sd/game.zip"), 0);
    EQ(zip.entries, 7);

    fs_stat_t st;
    EQ(fs_stat("/ar/main.lua", &st), 0);
    CHECK(!st.is_dir && st.size == 9 && st.mtime == 1709214358u);   /* 2024-02-29 13:45:58 */
    EQ(fs_stat("/ar/lib", &st), 0);
    CHECK(st.is_dir && st.mtime == 0);                               /* (no entry of its own: no time) */
    EQ(fs_stat("/ar/docs", &st), 0);
    CHECK(st.is_dir && st.mtime == 1688472000u);                     /* 2023-07-04 12:00:00, from "docs/" */
    EQ(fs_stat("/ar", &st), 0);
    CHECK(st.is_dir);
    EQ(fs_stat("/ar/empty.txt", &st), 0);
    CHECK(!st.is_dir && st.size == 0);
    EQ(fs_stat("/ar/nothere", &st), FS_ENOENT);
    EQ(fs_stat("/ar/main.lua/x", &st), FS_ENOENT);
    EQ(fs_stat("/ar/li", &st), FS_ENOENT);                           /* (a name that only begins like a folder's) */
    EQ(fs_stat("/ar/SPRITES/hero.SPR", &st), 0);                     /* names are matched without regard to case */
    CHECK(st.size == 5);

    EQ(read_all("/ar/main.lua", got, sizeof got, 100), 9);
    CHECK(!memcmp(got, "print(1)\n", 9));
    EQ(read_all("/ar/sprites/hero.spr", got, sizeof got, 2), 5);
    CHECK(!memcmp(got, bin, 5));
    EQ(read_all("/ar/docs/readme.txt", got, sizeof got, 3), 7);   /* (a local extra field the central header does not have) */
    CHECK(!memcmp(got, "read me", 7));
    EQ(read_all("/ar/empty.txt", got, sizeof got, 10), 0);
    EQ(read_all("/ar/big.bin", got, sizeof got, 4093), sizeof big);
    CHECK(!memcmp(got, big, sizeof big));
    EQ(read_all("/ar/lib", got, sizeof got, 10), FS_EISDIR);
    EQ(read_all("/ar/nothere", got, sizeof got, 10), FS_ENOENT);

    /* seeking, and files read in turns (they share the archive's handle) */
    const int a = fs_open("/ar/big.bin", FS_RDONLY), b = fs_open("/ar/BIG.BIN", FS_RDONLY), c = fs_open("/ar/main.lua", FS_RDONLY);
    CHECK(a >= 0 && b >= 0 && c >= 0);
    EQ(fs_seek(a, 50000, FS_SEEK_SET), 50000);
    EQ(fs_seek(b, -10, FS_SEEK_END), sizeof big - 10);
    uint8_t x[16];
    EQ(fs_read(a, x, 4), 4);
    CHECK(!memcmp(x, big + 50000, 4));
    EQ(fs_read(c, x, 5), 5);
    CHECK(!memcmp(x, "print", 5));
    EQ(fs_read(b, x, 16), 10);   /* (no more than there is) */
    CHECK(!memcmp(x, big + sizeof big - 10, 10));
    EQ(fs_read(b, x, 16), 0);
    EQ(fs_read(a, x, 4), 4);
    CHECK(!memcmp(x, big + 50004, 4));
    EQ(fs_seek(a, -8, FS_SEEK_CUR), 50000);
    EQ(fs_seek(a, 0, FS_SEEK_CUR), 50000);
    EQ(fs_seek(a, (int64_t)sizeof big + 1, FS_SEEK_SET), FS_EINVAL);   /* (outside the file: no holes, as everywhere in tilefs) */
    EQ(fs_seek(a, -1, FS_SEEK_SET), FS_EINVAL);
    EQ(fs_seek(a, 1, FS_SEEK_END), FS_EINVAL);
    EQ(fs_seek(a, INT64_MAX, FS_SEEK_CUR), FS_EINVAL);
    EQ(fs_seek(a, INT64_MIN, FS_SEEK_END), FS_EINVAL);
    EQ(fs_seek(a, 0, FS_SEEK_END), sizeof big);
    EQ(fs_read(a, x, 1), 0);
    EQ(fs_write(a, "z", 1), FS_EBADF);
    EQ(fs_read(c, x, 4), 4);
    CHECK(!memcmp(x, "(1)\n", 4));
    EQ(fs_close(a), 0);
    EQ(fs_close(b), 0);
    EQ(fs_close(c), 0);

    /* nothing changes */
    EQ(fs_open("/ar/main.lua", FS_WRONLY), FS_EROFS);
    EQ(fs_open("/ar/main.lua", FS_RDWR), FS_EROFS);
    EQ(fs_open("/ar/main.lua", FS_WRONLY | FS_TRUNC), FS_EROFS);
    EQ(fs_open("/ar/new.txt", FS_WRONLY | FS_CREAT), FS_EROFS);
    EQ(fs_open("/ar/new.txt", FS_RDONLY | FS_CREAT), FS_EROFS);
    EQ(fs_open("/ar/main.lua", FS_RDONLY | FS_CREAT | FS_EXCL), FS_EEXIST);
    const int r = fs_open("/ar/main.lua", FS_RDONLY | FS_CREAT);   /* (it is there: nothing to make) */
    CHECK(r >= 0);
    EQ(fs_close(r), 0);
    EQ(fs_unlink("/ar/main.lua"), FS_EROFS);
    EQ(fs_mkdir("/ar/new"), FS_EROFS);
    EQ(fs_rmdir("/ar/lib"), FS_EROFS);
    EQ(fs_rename("/ar/main.lua", "/ar/other.lua"), FS_EROFS);
    uint64_t fr, tot;
    EQ(fs_space("/ar", &fr, &tot), 0);
    CHECK(fr == 0 && tot == zlen);

    /* listing: each name once, folders that have no entry of their own included, and the folder's own entry left out */
    EQ(count_entries("/ar"), 6);
    EQ(listed("/ar", "main.lua", 0, 9), 1);
    EQ(listed("/ar", "lib", 1, 0), 1);
    EQ(listed("/ar", "Sprites", 1, 0), 1);
    EQ(listed("/ar", "docs", 1, 0), 1);
    EQ(listed("/ar", "empty.txt", 0, 0), 1);
    EQ(listed("/ar", "big.bin", 0, sizeof big), 1);
    EQ(count_entries("/ar/docs"), 1);
    EQ(listed("/ar/DOCS", "readme.txt", 0, 7), 1);
    EQ(count_entries("/ar/main.lua"), FS_ENOTDIR);
    EQ(count_entries("/ar/nothere"), FS_ENOENT);
    EQ(listed("/", "ar", 1, 0), 1);

    /* relative paths, as a program run from inside the archive uses them */
    EQ(fs_chdir("/ar/lib"), 0);
    EQ(read_all("util.lua", got, sizeof got, 64), 10);
    EQ(read_all("../main.lua", got, sizeof got, 64), 9);
    EQ(fs_chdir("/sd"), 0);

    /* the archive is busy while it is mounted, and the mount while a file in it is open */
    EQ(fs_unlink("/sd/game.zip"), FS_EBUSY);
    EQ(fs_rename("/sd/game.zip", "/sd/other.zip"), FS_EBUSY);
    EQ(fs_open("/sd/game.zip", FS_WRONLY), FS_EBUSY);
    const int also = fs_open("/sd/game.zip", FS_RDONLY);   /* (reading it as a file is fine) */
    CHECK(also >= 0);
    EQ(fs_close(also), 0);
    const int held = fs_open("/ar/main.lua", FS_RDONLY);
    CHECK(held >= 0);
    EQ(fs_unmount("ar"), FS_EBUSY);
    EQ(fs_close(held), 0);

    /* the archive's own handle is one of the FS_MAX_FILES */
    int fds[FS_MAX_FILES];
    for (int i = 0; i < FS_MAX_FILES - 1; i++) {
        fds[i] = fs_open("/ar/main.lua", FS_RDONLY);
        CHECK(fds[i] >= 0);
    }
    EQ(fs_open("/ar/main.lua", FS_RDONLY), FS_EMFILE);
    for (int i = 0; i < FS_MAX_FILES - 1; i++)
        EQ(fs_close(fds[i]), 0);

    unmount_zip();
    EQ(fs_stat("/ar/main.lua", &st), FS_ENOENT);
    EQ(fs_unlink("/sd/game.zip"), 0);   /* (let go of) */
}

static void test_entries(void)
{
    /* compressed and encrypted entries are there but cannot be read; names tilefs cannot reach are left out; backslashes are slashes */
    fresh();
    const zent_t e[] = {
        {.name = "packed.txt", .data = "xx", .len = 2, .method = 8, .usize = 1000},
        {.name = "secret.txt", .data = "yyyy", .len = 4, .flags = 1},
        {.name = "win\\path\\file.txt", .data = "from windows", .len = 12},
        {.name = "../evil.txt", .data = "e", .len = 1},
        {.name = "/abs.txt", .data = "e", .len = 1},
        {.name = "a//b.txt", .data = "e", .len = 1},
        {.name = "what?.txt", .data = "e", .len = 1},
        {.name = "dot./x.txt", .data = "e", .len = 1},
        {.name = "./here.txt", .data = "e", .len = 1},
        {.name = "fine.txt", .data = "ok", .len = 2},
    };
    put_file("/sd/e.zip", zbuf, make_zip(e, 10, NULL, 0, 0, NULL, 0));
    EQ(mount_zip("/sd/e.zip"), 0);
    fs_stat_t st;
    EQ(fs_stat("/ar/packed.txt", &st), 0);
    CHECK(st.size == 1000);
    EQ(fs_open("/ar/packed.txt", FS_RDONLY), FS_ENOTSUP);
    EQ(fs_open("/ar/secret.txt", FS_RDONLY), FS_ENOTSUP);
    CHECK(!strcmp(fs_strerror(FS_ENOTSUP), "not supported (a compressed or encrypted file?)") && !strcmp(fs_strerror(FS_EROFS), "read-only"));
    EQ(read_all("/ar/win/path/file.txt", got, sizeof got, 5), 12);
    CHECK(!memcmp(got, "from windows", 12));
    EQ(count_entries("/ar"), 4);   /* packed.txt, secret.txt, win, fine.txt */
    EQ(listed("/ar", "win", 1, 0), 1);
    EQ(listed("/ar", "packed.txt", 0, 1000), 1);
    EQ(listed("/ar", "fine.txt", 0, 2), 1);
    EQ(fs_stat("/ar/dot.", &st), FS_EBADNAME);   /* (the core says so first) */
    EQ(fs_stat("/ar/a", &st), FS_ENOENT);
    unmount_zip();

    /* a stored entry whose two sizes differ, a local header in the wrong place or with data running past the end: FS_EIO when opened */
    const zent_t f[] = {{.name = "a.txt", .data = "aaaa", .len = 4, .usize = 5}, {.name = "b.txt", .data = "bbbb", .len = 4},
                        {.name = "c.txt", .data = "cccc", .len = 4}, {.name = "d.txt", .data = "dddd", .len = 4}};
    make_zip(f, 4, NULL, 0, 0, NULL, 0);
    zbuf[z_local[1]] = 'X';                                                  /* b: not a local header */
    zbuf[z_central[2] + 42] = 0xF0;                                          /* c: its local header far outside the archive */
    zbuf[z_local[3] + 28] = 0xFF;                                            /* d: a local extra field longer than the archive */
    put_file("/sd/f.zip", zbuf, zlen);
    EQ(mount_zip("/sd/f.zip"), 0);
    EQ(fs_open("/ar/a.txt", FS_RDONLY), FS_EIO);
    EQ(fs_open("/ar/b.txt", FS_RDONLY), FS_EIO);
    EQ(fs_open("/ar/c.txt", FS_RDONLY), FS_EIO);
    EQ(fs_open("/ar/d.txt", FS_RDONLY), FS_EIO);
    unmount_zip();
}

static void test_archives(void)
{
    fresh();
    const zent_t one[] = {{.name = "x.txt", .data = "xyz", .len = 3}};
    fs_stat_t st;

    /* not archives */
    put_file("/sd/text.zip", "this is not an archive, only some text that goes on long enough", 63);
    EQ(mount_zip("/sd/text.zip"), FS_ENODEV);
    put_file("/sd/short.zip", "PK\5\6", 4);
    EQ(mount_zip("/sd/short.zip"), FS_ENODEV);
    EQ(mount_zip("/sd/none.zip"), FS_ENOENT);
    EQ(fs_mkdir("/sd/dir.zip"), 0);
    EQ(mount_zip("/sd/dir.zip"), FS_EISDIR);
    EQ(fs_unlink("/sd/text.zip"), 0);   /* (a failed open lets go of the file) */

    /* an archive with nothing in it */
    put_file("/sd/empty.zip", zbuf, make_zip(one, 0, NULL, 0, 0, NULL, 0));
    EQ(fs_zip_open(&zip, "/sd/empty.zip"), 0);
    CHECK(zip.index_bytes == 0 && zip.entries == 0);
    EQ(fs_zip_load(&zip, NULL), 0);
    EQ(fs_mount("ar", &fs_zip_ops, &zip), 0);
    EQ(count_entries("/ar"), 0);
    EQ(fs_stat("/ar", &st), 0);
    CHECK(st.is_dir);
    EQ(fs_stat("/ar/x.txt", &st), FS_ENOENT);
    unmount_zip();

    /* a stub in front (a self-extracting archive): offsets from the archive proper, or counting the stub */
    static char stub[300];
    memset(stub, 'M', sizeof stub);
    for (int absolute = 0; absolute < 2; absolute++) {
        put_file("/sd/stub.zip", zbuf, make_zip(one, 1, stub, sizeof stub, absolute, NULL, 0));
        EQ(mount_zip("/sd/stub.zip"), 0);
        EQ(zip.bias, absolute ? 0 : sizeof stub);
        EQ(read_all("/ar/x.txt", got, sizeof got, 8), 3);
        CHECK(!memcmp(got, "xyz", 3));
        unmount_zip();
    }

    /* a comment, the longest one, and one with something that looks like the end record in it */
    static char comment[65535];
    for (uint32_t i = 0; i < sizeof comment; i++)
        comment[i] = (char)('a' + i % 26);
    memcpy(comment + 1000, "PK\5\6", 4);
    put_file("/sd/comment.zip", zbuf, make_zip(one, 1, NULL, 0, 0, comment, sizeof comment));
    EQ(mount_zip("/sd/comment.zip"), 0);
    EQ(read_all("/ar/x.txt", got, sizeof got, 8), 3);
    unmount_zip();
    put_file("/sd/comment.zip", zbuf, make_zip(one, 1, NULL, 0, 0, comment, 2000));
    EQ(mount_zip("/sd/comment.zip"), 0);
    unmount_zip();
    put_file("/sd/comment.zip", zbuf, make_zip(one, 1, NULL, 0, 0, comment, 0));   /* (the end record not found: a comment one byte off) */
    zbuf[zlen - 2] = 1;
    put_file("/sd/comment.zip", zbuf, zlen);
    EQ(mount_zip("/sd/comment.zip"), FS_ENODEV);

    /* numbers that do not add up */
    const zent_t two[] = {{.name = "x.txt", .data = "xyz", .len = 3}, {.name = "y.txt", .data = "y", .len = 1}};
    make_zip(two, 2, NULL, 0, 0, NULL, 0);
    zbuf[z_end + 10] = 3;   /* the total says 3, the part here 2 */
    put_file("/sd/bad.zip", zbuf, zlen);
    EQ(mount_zip("/sd/bad.zip"), FS_ENODEV);
    zbuf[z_end + 8] = 3;    /* both 3: the central directory runs out */
    put_file("/sd/bad.zip", zbuf, zlen);
    EQ(mount_zip("/sd/bad.zip"), FS_ENODEV);
    make_zip(two, 2, NULL, 0, 0, NULL, 0);
    zbuf[z_end + 4] = 1;    /* part of an archive in several pieces */
    put_file("/sd/bad.zip", zbuf, zlen);
    EQ(mount_zip("/sd/bad.zip"), FS_ENODEV);
    make_zip(two, 2, NULL, 0, 0, NULL, 0);
    zbuf[z_end + 13] = 0x10;   /* a central directory bigger than what comes before the end */
    put_file("/sd/bad.zip", zbuf, zlen);
    EQ(mount_zip("/sd/bad.zip"), FS_ENODEV);
    make_zip(two, 2, NULL, 0, 0, NULL, 0);
    zbuf[z_central[1]] = 'Q';   /* the second header is not one */
    put_file("/sd/bad.zip", zbuf, zlen);
    EQ(mount_zip("/sd/bad.zip"), FS_ENODEV);
    make_zip(two, 2, NULL, 0, 0, NULL, 0);
    zbuf[z_central[1] + 28] = 0x40;   /* a name that runs past the central directory */
    put_file("/sd/bad.zip", zbuf, zlen);
    EQ(mount_zip("/sd/bad.zip"), FS_ENODEV);
    make_zip(two, 2, NULL, 0, 0, NULL, 0);
    zbuf[z_end + 10] = zbuf[z_end + 11] = zbuf[z_end + 8] = zbuf[z_end + 9] = 0xFF;   /* ZIP64 */
    put_file("/sd/bad.zip", zbuf, zlen);
    EQ(mount_zip("/sd/bad.zip"), FS_ENOTSUP);
    fs_zip_close(&zip);   /* (twice is harmless) */

    /* several archives at once, and a mount that refuses a second one of the same name */
    static fs_zip_t z2;
    static uint8_t ix2[1000];
    put_file("/sd/one.zip", zbuf, make_zip(one, 1, NULL, 0, 0, NULL, 0));
    put_file("/sd/two.zip", zbuf, make_zip(two, 2, NULL, 0, 0, NULL, 0));
    EQ(mount_zip("/sd/one.zip"), 0);
    EQ(fs_zip_open(&z2, "/sd/two.zip"), 0);
    EQ(fs_zip_load(&z2, ix2), 0);
    EQ(fs_mount("ar", &fs_zip_ops, &z2), FS_EEXIST);
    EQ(fs_mount("ar2", &fs_zip_ops, &z2), 0);
    EQ(read_all("/ar2/y.txt", got, sizeof got, 8), 1);
    EQ(read_all("/ar/x.txt", got, sizeof got, 8), 3);
    EQ(fs_stat("/ar/y.txt", &st), FS_ENOENT);
    EQ(fs_unmount("ar2"), 0);
    fs_zip_close(&z2);
    unmount_zip();
}

#include "test_zip_real.h"

static void test_real_tools(void)
{
    /* Info-ZIP: zip -0 -r infozip.zip main.lua Sub (extended timestamp and Unix extra fields, longer in the local headers) */
    fresh();
    put_file("/sd/infozip.zip", infozip_zip, sizeof infozip_zip);
    EQ(mount_zip("/sd/infozip.zip"), 0);
    EQ(read_all("/ar/main.lua", got, sizeof got, 7), 25);
    CHECK(!memcmp(got, "print(\"hello from main\")\n", 25));
    EQ(read_all("/ar/sub/data.bin", got, sizeof got, 7), 12);
    CHECK(!memcmp(got, "A\0B\0\377binary\n", 12));
    fs_stat_t st;
    EQ(fs_stat("/ar/main.lua", &st), 0);
    CHECK(st.mtime == 1709192758u);   /* 2024-02-29 07:45:58, the local time zip wrote */
    EQ(count_entries("/ar"), 2);
    EQ(listed("/ar", "Sub", 1, 0), 1);
    EQ(listed("/ar/Sub", "Data.bin", 0, 12), 1);
    unmount_zip();

    /* Python's zipfile (ZIP_STORED), with a folder entry and a comment */
    put_file("/sd/pyzip.zip", pyzip_zip, sizeof pyzip_zip);
    EQ(mount_zip("/sd/pyzip.zip"), 0);
    EQ(read_all("/ar/folder/note.txt", got, sizeof got, 100), 10);
    CHECK(!memcmp(got, "note text\n", 10));
    EQ(read_all("/ar/top.txt", got, sizeof got, 1), 3);
    EQ(fs_stat("/ar/folder", &st), 0);
    CHECK(st.is_dir && st.mtime == 1688472000u);
    EQ(fs_stat("/ar/folder/Note.TXT", &st), 0);
    CHECK(st.mtime == 1688473810u && st.size == 10);
    EQ(count_entries("/ar"), 2);
    EQ(count_entries("/ar/folder"), 1);
    unmount_zip();
}

int main(void)
{
    test_reading();
    test_entries();
    test_archives();
    test_real_tools();
    fs_reset();
    printf("test_zip: %lu checks passed\n", checks);
    return 0;
}
