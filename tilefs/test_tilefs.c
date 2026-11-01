/* SPDX-License-Identifier: MIT */
/* Tests of tilefs: one conformance suite run against every backend, tests of the mount namespace, tests of the RAM disk's limits, and a
 * random differential test of the RAM disk against the host's file system, which stands in as the reference. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fs_fat.h"
#include "tilefs.h"

static unsigned long checks;

#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define EQ(a, b) do { const long long va_ = (long long)(a), vb_ = (long long)(b); checks++; \
    if (va_ != vb_) { fprintf(stderr, "FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); exit(1); } } while (0)

enum { RAM, POSIX, FAT16, FAT32, EXFAT, NKINDS };
static const char *const kind_name[NKINDS] = {"ram", "posix", "fat16", "fat32", "exfat"};

static char host_dir[256];
static fs_ram_t ram;
static fs_posix_t posix_fs;
static uint8_t pool[64 * 1024];

static int rm_cb(const char *path, const struct stat *sb, int type, struct FTW *f)
{
    (void)sb; (void)type; (void)f;
    return remove(path);
}

static void host_clean(void)
{
    if (host_dir[0])
        nftw(host_dir, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    host_dir[0] = '\0';
}

/* Disk images for the FAT kinds: two slots, each a drive of FatFs. */
static fs_fat_t fat[2];
static fs_blk_file_t blk[2];
static char img[2][300];
static int fat_is_up[2];

static void fat_down(int i)
{
    if (fat_is_up[i]) {
        fs_fat_done(&fat[i]);
        if (fat_is_up[i] == 1) {
            fs_blk_file_close(&blk[i]);
            unlink(img[i]);
        }
        fat_is_up[i] = 0;
    }
}

/* A new image of the kind, formatted, as FatFs drive i, mounted at `name`. */
static void fat_up(int i, int kind, const char *name, int sfd)
{
    static const uint32_t sectors[] = {32768, 131072, 65536};   /* 16 MB FAT16, 64 MB FAT32 (it needs 65525 clusters), 32 MB exFAT */
    static const int type[] = {FS_FAT_FAT, FS_FAT_FAT32, FS_FAT_EXFAT};
    const char *tmp = getenv("TMPDIR");
    fat_down(i);
    snprintf(img[i], sizeof img[i], "%s/tilefs_img_XXXXXX", tmp ? tmp : "/tmp");
    const int fd = mkstemp(img[i]);
    CHECK(fd >= 0);
    close(fd);
    unlink(img[i]);   /* fs_blk_file_open makes it, sparse */
    EQ(fs_blk_file_open(&blk[i], img[i], sectors[kind - FAT16]), 0);
    fat_is_up[i] = 1;
    EQ(fs_fat_format((unsigned)i, &fs_blk_file_dev, &blk[i], type[kind - FAT16] | (sfd ? FS_FAT_SFD : 0)), 0);
    EQ(fs_fat_init(&fat[i], (unsigned)i, &fs_blk_file_dev, &blk[i]), 0);
    EQ(fs_mount(name, &fs_fat_ops, &fat[i]), 0);
}

/* The same on a block of RAM instead of an image file, for tests that write a whole disk's worth. */
static uint8_t ram_disk[64u << 20];
static fs_blk_ram_t ram_blk;

static void fat_up_ram(int kind, const char *name)
{
    static const uint32_t sectors[] = {32768, 131072, 65536};
    static const int type[] = {FS_FAT_FAT, FS_FAT_FAT32, FS_FAT_EXFAT};
    fs_reset();
    fat_down(0);
    memset(ram_disk, 0, (size_t)sectors[kind - FAT16] * 512);
    EQ(fs_blk_ram_init(&ram_blk, ram_disk, (size_t)sectors[kind - FAT16] * 512), 0);
    EQ(fs_fat_format(0, &fs_blk_ram_dev, &ram_blk, type[kind - FAT16] | FS_FAT_SFD), 0);
    EQ(fs_fat_init(&fat[0], 0, &fs_blk_ram_dev, &ram_blk), 0);
    fat_is_up[0] = 2;   /* up, but with no image file to close */
    EQ(fs_mount(name, &fs_fat_ops, &fat[0]), 0);
}

/* A fresh, empty file system of the kind, mounted at /t. */
static void setup(int kind)
{
    fs_reset();
    fat_down(0);
    fat_down(1);
    if (kind == RAM) {
        EQ(fs_ram_init(&ram, pool, sizeof pool), 0);
        EQ(fs_mount("t", &fs_ram_ops, &ram), 0);
    } else if (kind == POSIX) {
        host_clean();
        const char *tmp = getenv("TMPDIR");
        snprintf(host_dir, sizeof host_dir, "%s/tilefs_test_XXXXXX", tmp ? tmp : "/tmp");
        CHECK(mkdtemp(host_dir) != NULL);
        EQ(fs_posix_init(&posix_fs, host_dir), 0);
        EQ(fs_mount("/t", &fs_posix_ops, &posix_fs), 0);
    } else {
        fat_up(0, kind, "t", kind == FAT32);   /* FAT32 without a partition table, the others with one */
    }
}

static int put(const char *path, const void *data, size_t n)
{
    const int fd = fs_open(path, FS_WRONLY | FS_CREAT | FS_TRUNC);
    if (fd < 0)
        return fd;
    const int w = fs_write(fd, data, n);
    fs_close(fd);
    return w;
}

static int slurp(const char *path, void *buf, size_t cap)
{
    const int fd = fs_open(path, FS_RDONLY);
    if (fd < 0)
        return fd;
    int total = 0, r;
    while ((r = fs_read(fd, (char *)buf + total, cap - (size_t)total)) > 0)
        total += r;
    fs_close(fd);
    return r < 0 ? r : total;
}

static int slurp_is(const char *path, const char *want)
{
    char buf[4096];
    const int n = slurp(path, buf, sizeof buf);
    return n == (int)strlen(want) && !memcmp(buf, want, (size_t)n);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The entries of a directory as "name" or "name/" (a directory), sorted, joined with spaces. */
static void listing(const char *path, char *out, size_t cap)
{
    char names[64][300];
    char *ptr[64];
    int n = 0;
    const int dd = fs_opendir(path);
    out[0] = '\0';
    if (dd < 0) {
        snprintf(out, cap, "error %d", dd);
        return;
    }
    fs_dirent_t e;
    while (fs_readdir(dd, &e) == 1) {
        snprintf(names[n], sizeof names[n], "%s%s", e.name, e.is_dir ? "/" : "");
        ptr[n] = names[n];
        n++;
    }
    fs_closedir(dd);
    qsort(ptr, (size_t)n, sizeof ptr[0], cmp_str);
    for (int i = 0; i < n; i++) {
        if (i)
            strcat(out, " ");
        strcat(out, ptr[i]);
    }
}

#define LISTS(path, want) do { char l_[1024]; listing(path, l_, sizeof l_); checks++; \
    if (strcmp(l_, want)) { fprintf(stderr, "FAIL %s:%d: listing of %s is \"%s\", want \"%s\"\n", __FILE__, __LINE__, path, l_, want); exit(1); } } while (0)

/* ---- the conformance suite ---- */

static void test_files(int kind)
{
    setup(kind);
    char buf[64];
    fs_stat_t st;
    int fd = fs_open("/t/a.txt", FS_RDWR | FS_CREAT);
    CHECK(fd >= 0);
    EQ(fs_write(fd, "hello", 5), 5);
    EQ(fs_seek(fd, 0, FS_SEEK_CUR), 5);
    EQ(fs_seek(fd, 0, FS_SEEK_SET), 0);
    EQ(fs_read(fd, buf, 3), 3);
    CHECK(!memcmp(buf, "hel", 3));
    EQ(fs_read(fd, buf, 64), 2);
    CHECK(!memcmp(buf, "lo", 2));
    EQ(fs_read(fd, buf, 64), 0);   /* at the end */
    EQ(fs_read(fd, buf, 0), 0);
    EQ(fs_write(fd, "", 0), 0);
    EQ(fs_close(fd), 0);
    EQ(fs_close(fd), FS_EBADF);
    EQ(fs_stat("/t/a.txt", &st), 0);
    EQ(st.size, 5);
    EQ(st.is_dir, 0);

    /* opening */
    EQ(fs_open("/t/none", FS_RDONLY), FS_ENOENT);
    EQ(fs_open("/t/a.txt", FS_RDWR | FS_CREAT | FS_EXCL), FS_EEXIST);
    EQ(fs_open("/t/nodir/f", FS_RDWR | FS_CREAT), FS_ENOENT);
    EQ(fs_open("/t/a.txt/f", FS_RDWR | FS_CREAT), FS_ENOTDIR);
    EQ(fs_open("/t/a.txt/f", FS_RDONLY), FS_ENOTDIR);
    EQ(fs_mkdir("/t/d"), 0);
    EQ(fs_open("/t/d", FS_RDONLY), FS_EISDIR);
    EQ(fs_open("/t/d", FS_RDWR | FS_CREAT), FS_EISDIR);
    EQ(fs_open("/t/d", FS_RDWR | FS_TRUNC), FS_EISDIR);
    EQ(fs_open("/t/d", FS_RDWR | FS_CREAT | FS_EXCL), FS_EEXIST);   /* exists is the first thing said */
    EQ(fs_open("/t/a.txt", 0), FS_EINVAL);
    EQ(fs_open("/t/a.txt", FS_RDONLY | FS_TRUNC), FS_EINVAL);
    EQ(fs_open("/t/a.txt", FS_RDONLY | FS_EXCL), FS_EINVAL);
    EQ(fs_open("/t/a.txt", FS_RDONLY | 64), FS_EINVAL);
    EQ(fs_open(NULL, FS_RDONLY), FS_ENOENT);
    EQ(fs_open("", FS_RDONLY), FS_ENOENT);
    EQ(fs_stat("/t/a.txt", &st), 0);   /* none of those failures touched the file */
    EQ(st.size, 5);

    /* access modes */
    fd = fs_open("/t/a.txt", FS_RDONLY);
    EQ(fs_write(fd, "x", 1), FS_EBADF);
    EQ(fs_read(fd, buf, 1), 1);
    EQ(fs_close(fd), 0);
    fd = fs_open("/t/a.txt", FS_WRONLY);
    EQ(fs_read(fd, buf, 1), FS_EBADF);
    EQ(fs_write(fd, "J", 1), 1);   /* overwrites the first byte */
    EQ(fs_close(fd), 0);
    CHECK(slurp_is("/t/a.txt", "Jello"));

    /* truncate and append */
    fd = fs_open("/t/a.txt", FS_WRONLY | FS_APPEND);
    EQ(fs_seek(fd, 0, FS_SEEK_SET), 0);
    EQ(fs_write(fd, "!!", 2), 2);   /* goes to the end all the same */
    EQ(fs_seek(fd, 0, FS_SEEK_CUR), 7);
    EQ(fs_close(fd), 0);
    CHECK(slurp_is("/t/a.txt", "Jello!!"));
    fd = fs_open("/t/a.txt", FS_WRONLY | FS_TRUNC);
    EQ(fs_close(fd), 0);
    CHECK(slurp_is("/t/a.txt", ""));
    EQ(put("/t/a.txt", "again", 5), 5);
    CHECK(slurp_is("/t/a.txt", "again"));
    fd = fs_open("/t/new", FS_RDONLY | FS_CREAT);   /* creating with a read-only handle is allowed */
    CHECK(fd >= 0);
    EQ(fs_close(fd), 0);
    EQ(fs_stat("/t/new", &st), 0);
    EQ(st.size, 0);
}

static void test_seek(int kind)
{
    setup(kind);
    char buf[16];
    EQ(put("/t/s", "0123456789", 10), 10);
    const int fd = fs_open("/t/s", FS_RDWR);
    EQ(fs_seek(fd, 4, FS_SEEK_SET), 4);
    EQ(fs_seek(fd, 2, FS_SEEK_CUR), 6);
    EQ(fs_seek(fd, -2, FS_SEEK_CUR), 4);
    EQ(fs_seek(fd, 0, FS_SEEK_END), 10);
    EQ(fs_seek(fd, -3, FS_SEEK_END), 7);
    EQ(fs_read(fd, buf, 16), 3);
    CHECK(!memcmp(buf, "789", 3));
    EQ(fs_seek(fd, 10, FS_SEEK_SET), 10);   /* the end itself is allowed */
    EQ(fs_seek(fd, 11, FS_SEEK_SET), FS_EINVAL);
    EQ(fs_seek(fd, -1, FS_SEEK_SET), FS_EINVAL);
    EQ(fs_seek(fd, 1, FS_SEEK_END), FS_EINVAL);
    EQ(fs_seek(fd, -11, FS_SEEK_END), FS_EINVAL);
    EQ(fs_seek(fd, 1, FS_SEEK_CUR), FS_EINVAL);
    EQ(fs_seek(fd, INT64_MAX, FS_SEEK_SET), FS_EINVAL);
    EQ(fs_seek(fd, INT64_MIN, FS_SEEK_CUR), FS_EINVAL);
    EQ(fs_seek(fd, 0, 7), FS_EINVAL);
    EQ(fs_seek(fd, 0, FS_SEEK_CUR), 10);   /* a failed seek leaves the position alone */
    EQ(fs_seek(fd, 5, FS_SEEK_SET), 5);
    EQ(fs_seek(fd, 3, FS_SEEK_CUR), 8);
    EQ(fs_seek(fd, -8, FS_SEEK_CUR), 0);
    EQ(fs_seek(fd, 99, FS_SEEK_CUR), FS_EINVAL);
    EQ(fs_seek(fd, 0, FS_SEEK_CUR), 0);
    EQ(fs_close(fd), 0);
    EQ(fs_seek(fd, 0, FS_SEEK_SET), FS_EBADF);
    EQ(fs_seek(-1, 0, FS_SEEK_SET), FS_EBADF);
    EQ(fs_seek(FS_MAX_FILES, 0, FS_SEEK_SET), FS_EBADF);
}

static void test_blocks(int kind)
{
    setup(kind);
    static uint8_t data[3000], back[3100];
    for (unsigned i = 0; i < sizeof data; i++)
        data[i] = (uint8_t)(i * 7 + (i >> 8));
    EQ(put("/t/big", data, sizeof data), sizeof data);
    EQ(slurp("/t/big", back, sizeof back), sizeof data);
    CHECK(!memcmp(data, back, sizeof data));
    /* reads in odd sized pieces and from odd places, crossing block boundaries */
    const int fd = fs_open("/t/big", FS_RDWR);
    const unsigned pieces[] = {1, 255, 256, 257, 511, 513, 100};
    unsigned pos = 0;
    for (int round = 0; round < 40; round++) {
        const unsigned want = pieces[round % 7];
        const unsigned n = pos + want > sizeof data ? (unsigned)sizeof data - pos : want;
        uint8_t got[600];
        EQ(fs_read(fd, got, want), n);
        CHECK(!memcmp(got, data + pos, n));
        pos += n;
        if (pos == sizeof data) {
            EQ(fs_seek(fd, 0, FS_SEEK_SET), 0);
            pos = 0;
        }
    }
    /* overwrite across a boundary, backwards seeks, then extend */
    EQ(fs_seek(fd, 250, FS_SEEK_SET), 250);
    memset(data + 250, 0xAA, 20);
    EQ(fs_write(fd, data + 250, 20), 20);
    EQ(fs_seek(fd, 2990, FS_SEEK_SET), 2990);
    memset(data + 2990, 0x55, 10);
    EQ(fs_write(fd, data + 2990, 10), 10);
    EQ(fs_write(fd, "tail", 4), 4);   /* the file grows */
    EQ(fs_seek(fd, 0, FS_SEEK_END), 3004);
    EQ(fs_close(fd), 0);
    EQ(slurp("/t/big", back, sizeof back), 3004);
    CHECK(!memcmp(data, back, 3000));
    CHECK(!memcmp(back + 3000, "tail", 4));
    /* exactly on a boundary */
    EQ(put("/t/b256", data, 256), 256);
    EQ(put("/t/b512", data, 512), 512);
    const int f = fs_open("/t/b256", FS_RDWR | FS_APPEND);
    EQ(fs_write(f, "z", 1), 1);
    EQ(fs_close(f), 0);
    EQ(slurp("/t/b256", back, sizeof back), 257);
    CHECK(!memcmp(back, data, 256) && back[256] == 'z');
}

static void test_dirs(int kind)
{
    setup(kind);
    fs_dirent_t e;
    LISTS("/t", "");
    EQ(fs_mkdir("/t/d1"), 0);
    EQ(fs_mkdir("/t/d1"), FS_EEXIST);
    EQ(fs_mkdir("/t/x/y"), FS_ENOENT);
    EQ(fs_mkdir("/t/d1/sub"), 0);
    EQ(put("/t/d1/f.txt", "abc", 3), 3);
    EQ(put("/t/top", "12345678", 8), 8);
    EQ(fs_mkdir("/t/top/z"), FS_ENOTDIR);
    EQ(fs_mkdir("/t/top"), FS_EEXIST);
    LISTS("/t", "d1/ top");
    LISTS("/t/d1", "f.txt sub/");
    LISTS("/t/d1/sub", "");
    EQ(fs_opendir("/t/top"), FS_ENOTDIR);
    EQ(fs_opendir("/t/nope"), FS_ENOENT);
    EQ(fs_opendir("/t/top/x"), FS_ENOTDIR);
    /* sizes and kinds in the entries */
    int dd = fs_opendir("/t");
    int seen = 0;
    while (fs_readdir(dd, &e) == 1) {
        if (!strcmp(e.name, "top")) {
            EQ(e.size, 8);
            EQ(e.is_dir, 0);
            seen |= 1;
        } else if (!strcmp(e.name, "d1")) {
            EQ(e.size, 0);
            EQ(e.is_dir, 1);
            seen |= 2;
        }
    }
    EQ(seen, 3);
    EQ(fs_readdir(dd, &e), 0);   /* stays at the end */
    EQ(fs_readdir(dd, &e), 0);
    EQ(fs_closedir(dd), 0);
    EQ(fs_closedir(dd), FS_EBADF);
    EQ(fs_readdir(dd, &e), FS_EBADF);
    /* a listing left half way is no trouble: every entry comes once, in some order */
    dd = fs_opendir("/t");
    EQ(fs_readdir(dd, &e), 1);
    char first[FS_NAME_MAX + 1];
    strcpy(first, e.name);
    EQ(fs_readdir(dd, &e), 1);
    CHECK(strcmp(first, e.name) != 0);
    EQ(fs_readdir(dd, &e), 0);
    EQ(fs_closedir(dd), 0);
    /* removing */
    EQ(fs_rmdir("/t/d1"), FS_ENOTEMPTY);
    EQ(fs_rmdir("/t/top"), FS_ENOTDIR);
    EQ(fs_rmdir("/t/nope"), FS_ENOENT);
    EQ(fs_rmdir("/t/d1/sub"), 0);
    EQ(fs_rmdir("/t/d1/sub"), FS_ENOENT);
    EQ(fs_unlink("/t/d1"), FS_EISDIR);
    EQ(fs_unlink("/t/d1/f.txt"), 0);
    EQ(fs_unlink("/t/d1/f.txt"), FS_ENOENT);
    EQ(fs_rmdir("/t/d1"), 0);
    LISTS("/t", "top");
    EQ(fs_unlink("/t/top/x"), FS_ENOTDIR);
    EQ(fs_unlink("/t/zz/x"), FS_ENOENT);
}

static void test_rename(int kind)
{
    setup(kind);
    EQ(put("/t/a", "AAA", 3), 3);
    EQ(put("/t/b", "BBB", 3), 3);
    EQ(fs_mkdir("/t/d"), 0);
    EQ(fs_mkdir("/t/d/e"), 0);
    EQ(put("/t/d/e/f", "FFF", 3), 3);
    EQ(fs_rename("/t/a", "/t/b"), FS_EEXIST);
    EQ(fs_rename("/t/a", "/t/d"), FS_EEXIST);
    EQ(fs_rename("/t/a", "/t/a"), FS_EEXIST);
    EQ(fs_rename("/t/none", "/t/c"), FS_ENOENT);
    EQ(fs_rename("/t/none", "/t/b"), FS_ENOENT);
    EQ(fs_rename("/t/a", "/t/no/c"), FS_ENOENT);
    EQ(fs_rename("/t/a", "/t/b/c"), FS_ENOTDIR);
    CHECK(slurp_is("/t/a", "AAA") && slurp_is("/t/b", "BBB"));
    EQ(fs_rename("/t/a", "/t/c"), 0);
    CHECK(slurp_is("/t/c", "AAA"));
    EQ(fs_open("/t/a", FS_RDONLY), FS_ENOENT);
    EQ(fs_rename("/t/c", "/t/d/c2"), 0);
    LISTS("/t", "b d/");
    LISTS("/t/d", "c2 e/");
    EQ(fs_rename("/t/d", "/t/moved"), 0);   /* a directory keeps what is inside it */
    CHECK(slurp_is("/t/moved/e/f", "FFF") && slurp_is("/t/moved/c2", "AAA"));
    EQ(fs_rename("/t/moved", "/t/moved/e/inside"), FS_EINVAL);
    EQ(fs_rename("/t/moved", "/t/moved/inside"), FS_EINVAL);
    EQ(fs_rename("/t/moved/e", "/t/e2"), 0);
    EQ(fs_rename("/t/moved", "/t/MOVED2/"), 0);   /* a trailing slash is ignored */
    LISTS("/t", "MOVED2/ b e2/");
    /* a sibling whose name starts like the directory is not inside it */
    EQ(fs_mkdir("/t/e"), 0);
    EQ(fs_rename("/t/e", "/t/e2/x"), 0);
    LISTS("/t/e2", "f x/");   /* e2 still has the file f it was moved with */
    EQ(fs_rename("/t/e2", "/t/e2/x/deeper"), FS_EINVAL);
}

static void test_busy(int kind)
{
    setup(kind);
    char buf[8];
    EQ(fs_mkdir("/t/d"), 0);
    EQ(put("/t/d/f", "data", 4), 4);
    int a = fs_open("/t/d/f", FS_RDONLY);
    CHECK(a >= 0);
    EQ(fs_unlink("/t/d/f"), FS_EBUSY);
    EQ(fs_rename("/t/d/f", "/t/d/g"), FS_EBUSY);
    EQ(fs_rename("/t/d", "/t/e"), FS_EBUSY);   /* something inside is open */
    EQ(fs_rmdir("/t/d"), FS_EBUSY);
    EQ(fs_unlink("/t/D/F"), FS_EBUSY);   /* names are compared without regard to case */
    const int b = fs_open("/t/d/f", FS_RDONLY);
    CHECK(b >= 0);   /* two readers are fine */
    EQ(fs_open("/t/d/f", FS_WRONLY), FS_EBUSY);
    EQ(fs_open("/t/d/f", FS_RDWR | FS_APPEND), FS_EBUSY);
    EQ(fs_open("/t/d/f", FS_WRONLY | FS_TRUNC), FS_EBUSY);
    CHECK(slurp_is("/t/d/f", "data"));   /* and the refused truncation did not happen */
    EQ(fs_close(a), 0);
    EQ(fs_unlink("/t/d/f"), FS_EBUSY);   /* one reader is still there */
    EQ(fs_close(b), 0);
    const int w = fs_open("/t/d/f", FS_WRONLY);
    EQ(fs_open("/t/d/f", FS_RDONLY), FS_EBUSY);   /* a writer shuts out readers too */
    EQ(fs_open("/t/d/f", FS_WRONLY), FS_EBUSY);
    EQ(fs_close(w), 0);
    a = fs_open("/t/d/f", FS_RDONLY);
    EQ(fs_read(a, buf, 8), 4);
    EQ(fs_close(a), 0);
    const int dd = fs_opendir("/t/d");   /* an open directory also holds its place */
    EQ(fs_rmdir("/t/d"), FS_EBUSY);
    EQ(fs_rename("/t/d", "/t/e"), FS_EBUSY);
    EQ(fs_closedir(dd), 0);
    EQ(fs_rename("/t/d", "/t/e"), 0);
    EQ(fs_unlink("/t/e/f"), 0);
    EQ(fs_rmdir("/t/e"), 0);
}

static void test_paths(int kind)
{
    setup(kind);
    char longname[300], longpath[600];
    EQ(fs_mkdir("/t/a"), 0);
    EQ(fs_mkdir("/t/a/b"), 0);
    EQ(put("/t/a/b/f", "x", 1), 1);
    CHECK(slurp_is("/t//a///b/f", "x"));
    CHECK(slurp_is("/t/a/./b/./f", "x"));
    CHECK(slurp_is("/t/a/b/../b/f", "x"));
    CHECK(slurp_is("/t/a/b/f/", "x"));   /* trailing slashes are ignored */
    CHECK(slurp_is("/../../t/a/b/f", "x"));   /* ".." stops at the root */
    CHECK(slurp_is("/t/a/b/../../a/b/f", "x"));
    EQ(strcmp(fs_getcwd(), "/"), 0);
    EQ(fs_chdir("/t/a"), 0);
    EQ(strcmp(fs_getcwd(), "/t/a"), 0);
    CHECK(slurp_is("b/f", "x"));
    CHECK(slurp_is("./b/f", "x"));
    CHECK(slurp_is("../a/b/f", "x"));
    EQ(put("g", "gg", 2), 2);
    CHECK(slurp_is("/t/a/g", "gg"));
    EQ(fs_chdir("b"), 0);
    EQ(strcmp(fs_getcwd(), "/t/a/b"), 0);
    EQ(fs_chdir("../.."), 0);
    EQ(strcmp(fs_getcwd(), "/t"), 0);
    EQ(fs_chdir("/t/a/g"), FS_ENOTDIR);
    EQ(fs_chdir("/t/zzz"), FS_ENOENT);
    EQ(strcmp(fs_getcwd(), "/t"), 0);   /* failures change nothing */
    EQ(fs_chdir("/"), 0);
    EQ(fs_chdir("t/.."), 0);
    EQ(strcmp(fs_getcwd(), "/"), 0);
    EQ(fs_chdir("../../.."), 0);
    EQ(strcmp(fs_getcwd(), "/"), 0);
    /* bad names */
    EQ(fs_open("/t/a:b", FS_RDWR | FS_CREAT), FS_EBADNAME);
    EQ(fs_open("/t/a*", FS_RDWR | FS_CREAT), FS_EBADNAME);
    EQ(fs_open("/t/a?", FS_RDWR | FS_CREAT), FS_EBADNAME);
    EQ(fs_open("/t/a<b", FS_RDWR | FS_CREAT), FS_EBADNAME);
    EQ(fs_open("/t/a>b", FS_RDWR | FS_CREAT), FS_EBADNAME);
    EQ(fs_open("/t/a|b", FS_RDWR | FS_CREAT), FS_EBADNAME);
    EQ(fs_open("/t/a\"b", FS_RDWR | FS_CREAT), FS_EBADNAME);
    EQ(fs_open("/t/a\\b", FS_RDWR | FS_CREAT), FS_EBADNAME);
    EQ(fs_open("/t/a\tb", FS_RDWR | FS_CREAT), FS_EBADNAME);
    EQ(fs_open("/t/a\x7f", FS_RDWR | FS_CREAT), FS_EBADNAME);
    EQ(fs_open("/t/a.", FS_RDWR | FS_CREAT), FS_EBADNAME);
    EQ(fs_open("/t/a ", FS_RDWR | FS_CREAT), FS_EBADNAME);
    EQ(fs_mkdir("/t/a:"), FS_EBADNAME);
    EQ(fs_stat("/t/bad|name/x", &(fs_stat_t){0}), FS_EBADNAME);
    int fd = fs_open("/t/.hidden", FS_RDWR | FS_CREAT);   /* a leading dot is fine */
    CHECK(fd >= 0);
    EQ(fs_close(fd), 0);
    fd = fs_open("/t/sp ace.t x", FS_RDWR | FS_CREAT);
    CHECK(fd >= 0);
    EQ(fs_close(fd), 0);
    LISTS("/t", ".hidden a/ sp ace.t x");
    /* long ones: 256 bytes is too long everywhere; 255 is for the backend to decide (the RAM disk keeps FS_RAM_NAME_MAX) */
    memset(longname, 'n', 256);
    longname[256] = '\0';
    snprintf(longpath, sizeof longpath, "/t/%s", longname);
    EQ(fs_open(longpath, FS_RDWR | FS_CREAT), FS_ENAMETOOLONG);
    longname[255] = '\0';
    snprintf(longpath, sizeof longpath, "/t/%s", longname);
    fd = fs_open(longpath, FS_RDWR | FS_CREAT);
    if (kind == RAM) {
        EQ(fd, FS_ENAMETOOLONG);
        EQ(fs_stat(longpath, &(fs_stat_t){0}), FS_ENAMETOOLONG);
    } else {
        CHECK(fd >= 0);
        EQ(fs_close(fd), 0);
        EQ(fs_unlink(longpath), 0);
    }
    longname[FS_RAM_NAME_MAX] = '\0';
    snprintf(longpath, sizeof longpath, "/t/%s", longname);   /* the longest the RAM disk takes */
    fd = fs_open(longpath, FS_RDWR | FS_CREAT);
    CHECK(fd >= 0);
    EQ(fs_close(fd), 0);
    EQ(fs_unlink(longpath), 0);
    char deep[400] = "/t";   /* a path over FS_PATH_MAX in short parts */
    for (int i = 0; i < 70; i++)
        strcat(deep, "/abc");
    EQ(fs_stat(deep, &(fs_stat_t){0}), FS_ENAMETOOLONG);
    EQ(fs_chdir("/t"), 0);
    char rel[400] = "";   /* and a relative one that is long once added to the current directory */
    for (int i = 0; i < 70; i++)
        strcat(rel, "abc/");
    EQ(fs_stat(rel, &(fs_stat_t){0}), FS_ENAMETOOLONG);
    EQ(fs_chdir("/"), 0);
    if (kind != POSIX) {   /* case does not matter on the RAM disk or FAT, and is kept */
        EQ(put("/t/CamelCase", "c", 1), 1);
        CHECK(slurp_is("/t/camelcase", "c") && slurp_is("/T/CAMELCASE", "c"));
        EQ(fs_open("/t/camelCASE", FS_RDWR | FS_CREAT | FS_EXCL), FS_EEXIST);
        LISTS("/t", ".hidden CamelCase a/ sp ace.t x");
        EQ(fs_rename("/t/camelcase", "/t/Other"), 0);
        EQ(fs_mkdir("/t/A"), FS_EEXIST);
    }
}

static void test_limits(int kind)
{
    setup(kind);
    int fd[FS_MAX_FILES + 1], dd[FS_MAX_DIRS + 1];
    char name[16];
    fs_dirent_t e;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        snprintf(name, sizeof name, "/t/f%d", i);
        fd[i] = fs_open(name, FS_RDWR | FS_CREAT);
        CHECK(fd[i] >= 0);
        for (int j = 0; j < i; j++)
            CHECK(fd[j] != fd[i]);
    }
    EQ(fs_open("/t/extra", FS_RDWR | FS_CREAT), FS_EMFILE);
    EQ(fs_stat("/t/extra", &(fs_stat_t){0}), FS_ENOENT);   /* the refused open made nothing */
    EQ(fs_close(fd[3]), 0);
    fd[3] = fs_open("/t/extra", FS_RDWR | FS_CREAT);
    CHECK(fd[3] >= 0);
    for (int i = 0; i < FS_MAX_FILES; i++)
        EQ(fs_close(fd[i]), 0);
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        dd[i] = fs_opendir("/t");
        CHECK(dd[i] >= 0);
    }
    EQ(fs_opendir("/t"), FS_EMFILE);
    EQ(fs_opendir("/"), FS_EMFILE);
    const int f = fs_open("/t/f0", FS_RDONLY);   /* files and directories have separate tables */
    CHECK(f >= 0);
    EQ(fs_close(f), 0);
    EQ(fs_closedir(dd[0]), 0);
    EQ(fs_readdir(dd[1], &e) >= 0, 1);
    for (int i = 1; i < FS_MAX_DIRS; i++)
        EQ(fs_closedir(dd[i]), 0);
    EQ(fs_close(-1), FS_EBADF);
    EQ(fs_close(FS_MAX_FILES), FS_EBADF);
    EQ(fs_read(-5, e.name, 1), FS_EBADF);
    EQ(fs_write(99, "x", 1), FS_EBADF);
    EQ(fs_sync(-1), FS_EBADF);
    EQ(fs_closedir(-1), FS_EBADF);
    EQ(fs_closedir(FS_MAX_DIRS), FS_EBADF);
    EQ(fs_readdir(-1, &e), FS_EBADF);
}

static void test_space(int kind)
{
    setup(kind);
    uint64_t fr, total;
    EQ(fs_space("/t", &fr, &total), 0);
    CHECK(fr <= total && total > 0);
    EQ(fs_space("/t/none", &fr, &total), 0);   /* the path only picks the mount */
    EQ(fs_space("/zz", &fr, &total), FS_ENOENT);
    EQ(fs_space("/", &fr, &total), FS_EINVAL);
    EQ(fs_space("/t", NULL, &total), FS_EINVAL);
    const int fd = fs_open("/t/s", FS_RDWR | FS_CREAT);
    CHECK(fd >= 0);
    EQ(fs_sync(fd), 0);
    EQ(fs_close(fd), 0);
    if (kind == RAM) {   /* the blocks are counted exactly: 1000 bytes take 4 blocks of FS_RAM_BLOCK = 256 */
        uint64_t before;
        uint8_t data[1000] = {0};
        EQ(fs_space("/t", &before, &total), 0);
        EQ(put("/t/s", data, sizeof data), sizeof data);
        EQ(fs_space("/t", &fr, &total), 0);
        EQ(before - fr, 4 * FS_RAM_BLOCK);
        EQ(put("/t/s", data, 10), 10);   /* put truncates first */
        EQ(fs_space("/t", &fr, &total), 0);
        EQ(before - fr, 1 * FS_RAM_BLOCK);
        EQ(fs_unlink("/t/s"), 0);
        EQ(fs_space("/t", &fr, &total), 0);
        EQ(fr, before);
    }
}

/* ---- what reaches a backend ---- */

static struct {
    char path[FS_PATH_MAX + 1], path2[FS_PATH_MAX + 1];
    int flags, calls, seeks_to_end, writes;
} spy;

static int spy_open(void *c, const char *p, int flags, void **h) { (void)c; spy.calls++; strcpy(spy.path, p); spy.flags = flags; *h = &spy; return 0; }
static int spy_close(void *h) { (void)h; return 0; }
static int spy_read(void *h, void *b, size_t n) { (void)h; (void)b; (void)n; return 0; }
static int spy_write(void *h, const void *b, size_t n) { (void)h; (void)b; spy.writes++; return (int)n; }
static int64_t spy_seek(void *h, int64_t o, int w) { (void)h; (void)o; if (w == FS_SEEK_END) spy.seeks_to_end++; return 0; }
static int spy_stat(void *c, const char *p, fs_stat_t *st) { (void)c; spy.calls++; strcpy(spy.path, p); memset(st, 0, sizeof *st); st->is_dir = 1; return 0; }
static int spy_path(void *c, const char *p) { (void)c; spy.calls++; strcpy(spy.path, p); return 0; }
static int spy_rename(void *c, const char *f, const char *t) { (void)c; spy.calls++; strcpy(spy.path, f); strcpy(spy.path2, t); return 0; }
static int spy_opendir(void *c, const char *p, void **h) { (void)c; spy.calls++; strcpy(spy.path, p); *h = &spy; return 0; }
static int spy_readdir(void *h, fs_dirent_t *e) { (void)h; (void)e; return 0; }

static const fs_ops_t spy_ops = {
    .open = spy_open, .close = spy_close, .read = spy_read, .write = spy_write, .seek = spy_seek, .stat = spy_stat, .unlink = spy_path,
    .mkdir = spy_path, .rmdir = spy_path, .rename = spy_rename, .opendir = spy_opendir, .readdir = spy_readdir, .closedir = spy_close,
};

#define SEES(want) do { checks++; if (strcmp(spy.path, want)) { fprintf(stderr, "FAIL %s:%d: backend saw \"%s\", want \"%s\"\n", __FILE__, __LINE__, spy.path, want); exit(1); } } while (0)

static void test_backend_view(void)
{
    fs_stat_t st;
    fs_reset();
    EQ(fs_mount("s", &spy_ops, NULL), 0);
    int fd = fs_open("/s/a//b/../c/.", FS_RDWR | FS_CREAT | FS_TRUNC | FS_APPEND);
    CHECK(fd >= 0);
    SEES("/a/c");
    EQ(spy.flags, FS_RDWR | FS_CREAT | FS_TRUNC);   /* the append is the core's business */
    EQ(fs_write(fd, "x", 1), 1);
    EQ(fs_write(fd, "y", 1), 1);
    EQ(spy.seeks_to_end, 2);   /* once before each write */
    EQ(spy.writes, 2);
    EQ(fs_close(fd), 0);
    fd = fs_open("/s/q", FS_WRONLY);
    EQ(fs_write(fd, "x", 1), 1);
    EQ(spy.seeks_to_end, 2);   /* no append, no seek */
    EQ(fs_close(fd), 0);
    fd = fs_open("/S", FS_RDONLY);   /* the mount root, whatever the case of its name */
    SEES("/");
    EQ(fs_close(fd), 0);
    EQ(fs_stat("/s/X/y/", &st), 0);
    SEES("/X/y");   /* case is passed on as written */
    EQ(fs_chdir("/s/d"), 0);
    SEES("/d");
    EQ(fs_mkdir("e"), 0);
    SEES("/d/e");
    EQ(fs_rmdir("../d/e"), 0);
    SEES("/d/e");
    EQ(fs_unlink("f"), 0);
    SEES("/d/f");
    EQ(fs_rename("f", "../g"), 0);
    SEES("/d/f");
    EQ(strcmp(spy.path2, "/g"), 0);
    const int dd = fs_opendir("."), calls = spy.calls;
    CHECK(dd >= 0);
    SEES("/d");
    EQ(fs_closedir(dd), 0);
    /* what is refused never gets as far as the backend */
    char name[300];
    memset(name, 'n', 256);
    name[256] = '\0';
    char path[400];
    snprintf(path, sizeof path, "/s/%s", name);
    EQ(fs_open(path, FS_RDONLY), FS_ENAMETOOLONG);
    EQ(fs_stat("/s/a:b", &st), FS_EBADNAME);
    EQ(fs_mkdir("/s/x/y*"), FS_EBADNAME);
    EQ(fs_open("/s/f", 0), FS_EINVAL);
    EQ(fs_rename("/s/a", "/s/b "), FS_EBADNAME);
    EQ(fs_unlink("/s/ends."), FS_EBADNAME);
    EQ(spy.calls, calls);
    fs_reset();
}

/* ---- the mount namespace ---- */

static void test_mounts(void)
{
    static fs_ram_t other;
    static uint8_t other_pool[8192];
    fs_dirent_t e;
    fs_stat_t st;
    setup(RAM);
    EQ(fs_ram_init(&other, other_pool, sizeof other_pool), 0);
    EQ(fs_mount("u", &fs_ram_ops, &other), 0);
    LISTS("/", "t/ u/");
    EQ(fs_stat("/", &st), 0);
    EQ(st.is_dir, 1);
    EQ(fs_mount("T", &fs_ram_ops, &other), FS_EEXIST);   /* names compare without regard to case */
    EQ(fs_mount("/t", &fs_ram_ops, &other), FS_EEXIST);
    EQ(fs_mount("", &fs_ram_ops, &other), FS_EINVAL);
    EQ(fs_mount("a/b", &fs_ram_ops, &other), FS_EINVAL);
    EQ(fs_mount("a:", &fs_ram_ops, &other), FS_EINVAL);
    EQ(fs_mount("abcdefghijklmnop", &fs_ram_ops, &other), FS_EINVAL);   /* 16 characters */
    EQ(fs_mount("abcdefghijklmno", &fs_ram_ops, &other), 0);           /* 15 */
    EQ(fs_mount(NULL, &fs_ram_ops, &other), FS_EINVAL);
    EQ(fs_mount("v", NULL, &other), FS_EINVAL);
    {
        fs_ops_t partial = fs_ram_ops;
        partial.rename = NULL;
        EQ(fs_mount("w", &partial, &other), FS_EINVAL);
        partial = fs_ram_ops;
        partial.sync = NULL;
        partial.space = NULL;
        EQ(fs_mount("w", &partial, &other), 0);   /* sync and space are optional; the table (4 mounts) is full now */
        EQ(fs_mount("x", &fs_ram_ops, &other), FS_ENOMEM);
        uint64_t a, b;
        EQ(fs_space("/w", &a, &b), FS_EINVAL);
        const int fd = fs_open("/w/f", FS_RDWR | FS_CREAT);
        EQ(fs_sync(fd), 0);
        EQ(fs_close(fd), 0);
        EQ(fs_unmount("w"), 0);
    }
    EQ(fs_unmount("abcdefghijklmno"), 0);
    EQ(fs_unmount("abcdefghijklmno"), FS_ENOENT);
    /* the root and mount points */
    EQ(fs_mkdir("/"), FS_EEXIST);
    EQ(fs_mkdir("/t"), FS_EEXIST);
    EQ(fs_mkdir("/foo"), FS_EPERM);
    EQ(fs_mkdir("/foo/bar"), FS_ENOENT);
    EQ(fs_rmdir("/"), FS_EBUSY);
    EQ(fs_rmdir("/t"), FS_EBUSY);
    EQ(fs_unlink("/"), FS_EISDIR);
    EQ(fs_unlink("/t"), FS_EISDIR);
    EQ(fs_open("/", FS_RDONLY), FS_EISDIR);
    EQ(fs_open("/t", FS_RDONLY), FS_EISDIR);
    EQ(fs_open("/foo", FS_RDWR | FS_CREAT), FS_ENOENT);
    EQ(fs_stat("/foo", &st), FS_ENOENT);
    EQ(fs_stat("/t", &st), 0);
    EQ(st.is_dir, 1);
    EQ(fs_rename("/t", "/u"), FS_EBUSY);
    EQ(fs_rename("/t", "/x"), FS_ENOENT);   /* there is no mount x */
    EQ(fs_rename("/t/a", "/t"), FS_EBUSY);
    EQ(fs_rename("/", "/t/a"), FS_EBUSY);
    EQ(fs_chdir("/foo"), FS_ENOENT);
    /* the root lists the mounts as they come and go */
    {
        const int dd = fs_opendir("/");
        EQ(fs_readdir(dd, &e), 1);
        EQ(e.is_dir, 1);
        EQ(fs_closedir(dd), 0);
    }
    /* across mounts */
    EQ(put("/t/f", "ff", 2), 2);
    EQ(fs_rename("/t/f", "/u/f"), FS_EXDEV);
    EQ(fs_mkdir("/u/d"), 0);
    EQ(put("/u/d/in", "in", 2), 2);
    CHECK(slurp_is("/t/f", "ff") && slurp_is("/u/d/in", "in"));
    EQ(fs_stat("/t/d", &st), FS_ENOENT);   /* the mounts are separate trees */
    /* unmounting */
    const int fd = fs_open("/u/d/in", FS_RDONLY);
    EQ(fs_unmount("u"), FS_EBUSY);
    EQ(fs_close(fd), 0);
    const int dd = fs_opendir("/u");
    EQ(fs_unmount("/u"), FS_EBUSY);
    EQ(fs_closedir(dd), 0);
    EQ(fs_chdir("/u/d"), 0);
    EQ(fs_unmount("u"), 0);
    EQ(strcmp(fs_getcwd(), "/"), 0);   /* the current directory was in it */
    EQ(fs_stat("/u/d", &st), FS_ENOENT);
    LISTS("/", "t/");
    EQ(fs_mount("u", &fs_ram_ops, &other), 0);   /* mounting again finds what was left on it */
    CHECK(slurp_is("/u/d/in", "in"));
    EQ(fs_chdir("/t"), 0);
    EQ(fs_unmount("u"), 0);
    EQ(strcmp(fs_getcwd(), "/t"), 0);   /* a different mount's current directory stays */
    EQ(fs_unmount("nonesuch"), FS_ENOENT);
    EQ(fs_unmount(NULL), FS_EINVAL);
    /* reset closes what is open, and the backends see the closes */
    EQ(fs_open("/t/f", FS_RDONLY) >= 0, 1);
    EQ(fs_opendir("/t") >= 0, 1);
    fs_reset();
    EQ(fs_open("/t/f", FS_RDONLY), FS_ENOENT);
    for (int i = 0; i < FS_RAM_HANDLES; i++)
        EQ(ram.handle[i].used, 0);
    EQ(strcmp(fs_getcwd(), "/"), 0);
}

/* ---- the RAM disk's limits ---- */

static void test_ram_limits(void)
{
    static fs_ram_t small;
    static uint8_t spool[8 * 1024 + 4];
    uint64_t fr, total;
    uint8_t chunk[1500];
    memset(chunk, 0x5A, sizeof chunk);
    fs_reset();
    EQ(fs_ram_init(&small, NULL, 100), FS_ENOMEM);
    EQ(fs_ram_init(&small, spool, 10), FS_ENOMEM);
    EQ(fs_ram_init(&small, spool, 200), FS_ENOMEM);   /* room for the inodes but not a block */
    EQ(fs_ram_init(&small, spool + 3, sizeof spool - 4), 0);   /* an odd start is aligned */
    EQ(fs_mount("r", &fs_ram_ops, &small), 0);
    EQ(fs_space("/r", &fr, &total), 0);
    EQ(fr, total);
    CHECK(total >= 20 * FS_RAM_BLOCK && total <= sizeof spool);
    const unsigned blocks = (unsigned)(total / FS_RAM_BLOCK);
    /* fill the disk with one file: it takes exactly all the blocks, then refuses */
    int fd = fs_open("/r/big", FS_WRONLY | FS_CREAT);
    uint64_t written = 0;
    int w;
    while ((w = fs_write(fd, chunk, sizeof chunk)) > 0)
        written += (unsigned)w;
    EQ(w, FS_ENOSPC);
    EQ(written, total);
    EQ(fs_space("/r", &fr, &total), 0);
    EQ(fr, 0);
    EQ(fs_write(fd, "x", 1), FS_ENOSPC);
    EQ(fs_close(fd), 0);
    fs_stat_t st;
    EQ(fs_stat("/r/big", &st), 0);
    EQ(st.size, written);
    EQ(fs_open("/r/second", FS_WRONLY | FS_CREAT) >= 0, 1);   /* an empty file needs no block */
    EQ(fs_close(0), 0);
    fd = fs_open("/r/second", FS_WRONLY);
    EQ(fs_write(fd, "x", 1), FS_ENOSPC);
    EQ(fs_close(fd), 0);
    /* deleting gives every block back, and it can be filled again */
    EQ(fs_unlink("/r/big"), 0);
    EQ(fs_space("/r", &fr, &total), 0);
    EQ(fr, total);
    /* a short write: the part that fits is written and counted */
    fd = fs_open("/r/second", FS_WRONLY | FS_TRUNC);
    EQ(fs_write(fd, chunk, 1), 1);
    uint8_t *huge = malloc(total + 5000);
    memset(huge, 0x33, total + 5000);
    EQ(fs_write(fd, huge, total + 5000), total - 1);
    EQ(fs_write(fd, huge, 10), FS_ENOSPC);
    EQ(fs_close(fd), 0);
    free(huge);
    EQ(fs_unlink("/r/second"), 0);
    /* many cycles of making and removing files of odd sizes never lose a block or an entry */
    unsigned seed = 12345;
    for (int round = 0; round < 300; round++) {
        char name[16];
        int made = 0;
        for (int i = 0; i < 6; i++) {
            seed = seed * 1103515245u + 12345u;
            const size_t n = (seed >> 8) % 1500;
            snprintf(name, sizeof name, "/r/f%d", i);
            EQ(put(name, chunk, n) >= 0 || put(name, chunk, n) == FS_ENOSPC, 1);
            made++;
        }
        for (int i = 0; i < made; i++) {
            snprintf(name, sizeof name, "/r/f%d", i);
            EQ(fs_unlink(name), 0);
        }
        EQ(fs_space("/r", &fr, &total), 0);
        EQ(fr, total);
    }
    /* running out of directory entries */
    int made = 0;
    char name[16];
    for (;; made++) {
        snprintf(name, sizeof name, "/r/n%d", made);
        const int f = fs_open(name, FS_WRONLY | FS_CREAT);
        if (f < 0) {
            EQ(f, FS_ENOSPC);
            break;
        }
        EQ(fs_close(f), 0);
        CHECK(made < 100000);
    }
    CHECK(made >= 7);
    EQ(fs_mkdir("/r/onemore"), FS_ENOSPC);
    EQ(fs_unlink("/r/n0"), 0);
    EQ(fs_mkdir("/r/onemore"), 0);   /* the freed entry is reused */
    (void)blocks;
    fs_reset();
}

/* ---- a random comparison of the RAM disk with the host's file system ---- */

static unsigned long rng_state = 88172645463325252ul;
static unsigned rnd(unsigned n)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (unsigned)((rng_state >> 11) % n);
}

static const char *const parts[] = {"a", "b", "c", "ab", "Ab"};   /* "Ab" and "ab" differ only in case: not used as such, see below */

static void random_path(char *out, const char *prefix)
{
    const int depth = 1 + (int)rnd(3);
    strcpy(out, prefix);
    for (int i = 0; i < depth; i++) {
        strcat(out, "/");
        strcat(out, parts[rnd(4)]);   /* only the all-lowercase names */
    }
    if (rnd(25) == 0)
        strcat(out, "/..");
    if (rnd(30) == 0)
        strcat(out, "/");
}

/* Removes everything below a directory, with the API itself. */
static void wipe(const char *dir)
{
    char names[256][64];
    int is_dir[256], n = 0;
    fs_dirent_t e;
    const int dd = fs_opendir(dir);
    CHECK(dd >= 0);
    while (fs_readdir(dd, &e) == 1) {
        CHECK(n < 256 && strlen(e.name) < 64);
        strcpy(names[n], e.name);
        is_dir[n++] = e.is_dir;
    }
    EQ(fs_closedir(dd), 0);
    for (int i = 0; i < n; i++) {
        char path[300];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        if (is_dir[i]) {
            wipe(path);
            EQ(fs_rmdir(path), 0);
        } else {
            EQ(fs_unlink(path), 0);
        }
    }
}

static void test_differential(int kind, unsigned long ops)
{
    static fs_ram_t big;
    static uint8_t bpool[4 * 1024 * 1024];   /* 4096 entries: renaming moves whole trees, so they grow deeper than the paths used */
    int fr[FS_MAX_FILES], fp[FS_MAX_FILES], dr[FS_MAX_DIRS], dp[FS_MAX_DIRS];
    for (int i = 0; i < FS_MAX_FILES; i++)
        fr[i] = fp[i] = -1;
    for (int i = 0; i < FS_MAX_DIRS; i++)
        dr[i] = dp[i] = -1;
    setup(POSIX);
    if (kind == RAM) {
        EQ(fs_ram_init(&big, bpool, sizeof bpool), 0);
        EQ(fs_mount("r", &fs_ram_ops, &big), 0);
    } else {
        fat_up(1, kind, "r", 0);
    }
    EQ(fs_mount("p", &fs_posix_ops, &posix_fs), 0);
    for (unsigned long step = 0; step < ops; step++) {
        if (step % 40000 == 39999) {   /* renames move whole trees, so now and then start again, or the RAM disk's entries run out */
            for (int i = 0; i < FS_MAX_FILES; i++)
                if (fr[i] >= 0) {
                    EQ(fs_close(fr[i]), 0);
                    EQ(fs_close(fp[i]), 0);
                    fr[i] = fp[i] = -1;
                }
            for (int i = 0; i < FS_MAX_DIRS; i++)
                if (dr[i] >= 0) {
                    EQ(fs_closedir(dr[i]), 0);
                    EQ(fs_closedir(dp[i]), 0);
                    dr[i] = dp[i] = -1;
                }
            wipe("/r");
            wipe("/p");
        }
        char pr[128], pp[128], qr[128], qp[128], path[64], path2[64];
        random_path(path, "");
        random_path(path2, "");
        snprintf(pr, sizeof pr, "/r%s", path);
        snprintf(pp, sizeof pp, "/p%s", path);
        snprintf(qr, sizeof qr, "/r%s", path2);
        snprintf(qp, sizeof qp, "/p%s", path2);
        const unsigned op = rnd(100);
        if (op < 12) {   /* open */
            const int flags = (int)(1 + rnd(3)) | (rnd(2) ? FS_CREAT : 0) | (rnd(5) == 0 ? FS_EXCL : 0) | (rnd(6) == 0 ? FS_TRUNC : 0) |
                              (rnd(6) == 0 ? FS_APPEND : 0);
            const int slot = (int)rnd(FS_MAX_FILES);
            if (fr[slot] >= 0)
                continue;
            const int a = fs_open(pr, flags), b = fs_open(pp, flags);
            if (a < 0 || b < 0) {
                if (a != b) { fprintf(stderr, "step %lu open %s flags %d: %d vs %d\n", step, path, flags, a, b); exit(1); }
            } else {
                fr[slot] = a;
                fp[slot] = b;
            }
        } else if (op < 18) {   /* close */
            const int slot = (int)rnd(FS_MAX_FILES);
            if (fr[slot] < 0)
                continue;
            EQ(fs_close(fr[slot]), fs_close(fp[slot]));
            fr[slot] = fp[slot] = -1;
        } else if (op < 36) {   /* write */
            const int slot = (int)rnd(FS_MAX_FILES);
            if (fr[slot] < 0)
                continue;
            uint8_t buf[700];
            const size_t n = rnd(4) == 0 ? rnd(700) : rnd(40);
            for (size_t i = 0; i < n; i++)
                buf[i] = (uint8_t)rnd(256);
            const int a = fs_write(fr[slot], buf, n), b = fs_write(fp[slot], buf, n);
            if (a != b) { fprintf(stderr, "step %lu write: %d vs %d\n", step, a, b); exit(1); }
        } else if (op < 52) {   /* read */
            const int slot = (int)rnd(FS_MAX_FILES);
            if (fr[slot] < 0)
                continue;
            uint8_t x[700], y[700];
            const size_t n = rnd(4) == 0 ? rnd(700) : rnd(60);
            const int a = fs_read(fr[slot], x, n), b = fs_read(fp[slot], y, n);
            if (a != b || (a > 0 && memcmp(x, y, (size_t)a))) { fprintf(stderr, "step %lu read: %d vs %d\n", step, a, b); exit(1); }
            checks++;
        } else if (op < 62) {   /* seek */
            const int slot = (int)rnd(FS_MAX_FILES);
            if (fr[slot] < 0)
                continue;
            const int whence = (int)rnd(4) == 3 ? (int)rnd(5) : (int)rnd(3);
            int64_t off = (int64_t)rnd(900) - 300;
            if (rnd(4) == 0)
                off = 0;
            const int64_t a = fs_seek(fr[slot], off, whence), b = fs_seek(fp[slot], off, whence);
            if (a != b) { fprintf(stderr, "step %lu seek %lld %d: %lld vs %lld\n", step, (long long)off, whence, (long long)a, (long long)b); exit(1); }
        } else if (op < 68) {
            EQ(fs_mkdir(pr), fs_mkdir(pp));
        } else if (op < 73) {
            EQ(fs_rmdir(pr), fs_rmdir(pp));
        } else if (op < 79) {
            EQ(fs_unlink(pr), fs_unlink(pp));
        } else if (op < 85) {
            EQ(fs_rename(pr, qr), fs_rename(pp, qp));
        } else if (op < 92) {
            fs_stat_t sa, sb;
            const int a = fs_stat(pr, &sa), b = fs_stat(pp, &sb);
            EQ(a, b);
            if (a == 0) {
                EQ(sa.is_dir, sb.is_dir);
                EQ(sa.size, sb.size);
            }
        } else if (op < 96) {   /* list a directory, in both, with every entry compared */
            char la[1024], lb[1024];
            listing(pr, la, sizeof la);
            listing(pp, lb, sizeof lb);
            if (strcmp(la, lb)) { fprintf(stderr, "step %lu list %s: \"%s\" vs \"%s\"\n", step, path, la, lb); exit(1); }
            checks++;
        } else if (op < 98) {   /* directory handles: open, step through a few entries, close */
            const int slot = (int)rnd(FS_MAX_DIRS);
            if (dr[slot] >= 0) {
                EQ(fs_closedir(dr[slot]), fs_closedir(dp[slot]));
                dr[slot] = dp[slot] = -1;
            } else {
                const int a = fs_opendir(pr), b = fs_opendir(pp);
                EQ(a < 0, b < 0);
                if (a < 0)
                    EQ(a, b);
                else {
                    dr[slot] = a;
                    dp[slot] = b;
                }
            }
        } else {   /* the directory in use. What a listing shows after the directory changed under it is not specified */
            const int slot = (int)rnd(FS_MAX_DIRS);
            if (dr[slot] < 0)
                continue;
            fs_dirent_t ea, eb;
            CHECK(fs_readdir(dr[slot], &ea) >= 0);
            CHECK(fs_readdir(dp[slot], &eb) >= 0);
        }
    }
    /* what is left is the same too */
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (fr[i] >= 0) {
            EQ(fs_close(fr[i]), 0);
            EQ(fs_close(fp[i]), 0);
        }
    for (int i = 0; i < FS_MAX_DIRS; i++)
        if (dr[i] >= 0) {
            EQ(fs_closedir(dr[i]), 0);
            EQ(fs_closedir(dp[i]), 0);
        }
    char la[1024], lb[1024];
    listing("/r", la, sizeof la);
    listing("/p", lb, sizeof lb);
    EQ(strcmp(la, lb), 0);
    (void)parts;
}

/* ---- the FAT file system against other people's tools ---- */

static int have_tool(const char *path)
{
    return access(path, X_OK) == 0;
}

static int run(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static int run(const char *fmt, ...)
{
    char cmd[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    strcat(cmd, " >/dev/null 2>&1");
    return system(cmd);
}

/* Detaches the image from tilefs but keeps the file, so that other tools can look at it. */
static void fat_release(int i)
{
    fs_reset();
    EQ(fs_fat_done(&fat[i]), 0);
    fs_blk_file_close(&blk[i]);
}

static void fill(uint8_t *p, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)((i * 31 + seed * 17 + (i >> 9)) & 0xFF);
}

/* Where the file system starts in an image: sector 0 if that sector is a boot sector (a jump instruction, then 0x55AA), else the start
 * of the first partition of the MBR there. -1 if neither makes sense. */
static long partition_start(const char *path)
{
    uint8_t s[512];
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return -1;
    const size_t n = fread(s, 1, sizeof s, fp);
    fclose(fp);
    if (n != sizeof s || s[510] != 0x55 || s[511] != 0xAA)
        return -1;
    if (s[0] == 0xEB || s[0] == 0xE9)
        return 0;
    if (s[0x1BE + 4] == 0)
        return -1;
    return (long)(s[0x1BE + 8] | s[0x1BE + 9] << 8 | s[0x1BE + 10] << 16 | (long)s[0x1BE + 11] << 24);
}

/* fsck reads a file system, not a disk: a partition is copied out of the image first. */
static void check_with_fsck(const char *fsck, const char *image, long first, const char *label)
{
    char part[400];
    snprintf(part, sizeof part, "%s.part", image);
    if (first > 0)
        EQ(run("dd if='%s' of='%s' bs=512 skip=%ld", image, part, first), 0);
    const int r = run("%s -n '%s'", fsck, first > 0 ? part : image);
    if (r != 0) {
        char cmd[900];
        snprintf(cmd, sizeof cmd, "%s -n '%s' >&2", fsck, first > 0 ? part : image);
        fprintf(stderr, "%s found a fault in the %s image:\n", fsck, label);
        if (system(cmd)) {}
    }
    unlink(part);
    EQ(r, 0);
}

/* Makes a tree with tilefs, then asks fsck whether the image is sound, and mtools (for FAT12/16/32) to read the files back. */
static void test_foreign_check(int kind, int sfd)
{
    static uint8_t data[100000], back[100000];
    const int exfat = kind == EXFAT;
    const char *fsck = exfat ? "/usr/sbin/fsck.exfat" : "/usr/sbin/fsck.vfat";
    if (!have_tool(fsck)) {
        printf("  (no %s: skipped)\n", fsck);
        return;
    }
    fs_reset();
    fat_up(0, kind, "t", sfd);
    EQ(fs_mkdir("/t/Documents"), 0);
    EQ(fs_mkdir("/t/Documents/Sub Folder"), 0);
    EQ(fs_mkdir("/t/Documents/Sub Folder/deeper"), 0);
    const size_t sizes[] = {0, 1, 511, 512, 513, 4095, 4096, 4097, 33000, 100000};
    char name[200];
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        fill(data, sizes[i], i);
        snprintf(name, sizeof name, "/t/Documents/Sub Folder/A Rather Long File Name Number %u.text", i);
        EQ(put(name, data, sizes[i]), (int)sizes[i]);
    }
    fill(data, 70000, 99);
    EQ(put("/t/Documents/deeper_file.bin", data, 70000), 70000);
    EQ(fs_rename("/t/Documents/deeper_file.bin", "/t/Documents/Sub Folder/deeper/moved.bin"), 0);
    EQ(fs_rename("/t/Documents/Sub Folder", "/t/Documents/Renamed Folder"), 0);
    EQ(fs_unlink("/t/Documents/Renamed Folder/A Rather Long File Name Number 2.text"), 0);
    EQ(fs_unlink("/t/Documents/Renamed Folder/A Rather Long File Name Number 8.text"), 0);
    for (int i = 0; i < 40; i++) {   /* a directory with enough entries to need more than one cluster */
        snprintf(name, sizeof name, "/t/many_entries_with_longish_names_%02d.dat", i);
        EQ(put(name, "x", 1), 1);
    }
    EQ(fs_rmdir("/t/Documents/Renamed Folder/deeper"), FS_ENOTEMPTY);
    {   /* grow a file in small steps, appending */
        const int fd = fs_open("/t/grow.log", FS_WRONLY | FS_CREAT | FS_APPEND);
        for (int i = 0; i < 300; i++)
            EQ(fs_write(fd, "line of text 0123456789\n", 24), 24);
        EQ(fs_close(fd), 0);
    }
    fat_release(0);
    long first = partition_start(img[0]);
    CHECK(first >= 0 && (first == 0) == (sfd != 0));
    check_with_fsck(fsck, img[0], first, kind_name[kind]);
    if (!exfat && have_tool("/usr/bin/mcopy")) {
        char out[400];
        snprintf(out, sizeof out, "%s.out", img[0]);
        fill(data, 100000, 9);
        EQ(run("mcopy -n -i '%s@@%ld' '::/Documents/Renamed Folder/A Rather Long File Name Number 9.text' '%s'", img[0], first * 512, out), 0);
        FILE *fp = fopen(out, "rb");
        CHECK(fp != NULL);
        EQ(fread(back, 1, sizeof back, fp), 100000);
        fclose(fp);
        CHECK(!memcmp(back, data, 100000));
        fill(data, 70000, 99);
        EQ(run("mcopy -n -i '%s@@%ld' '::/Documents/Renamed Folder/deeper/moved.bin' '%s'", img[0], first * 512, out), 0);
        fp = fopen(out, "rb");
        CHECK(fp != NULL);
        EQ(fread(back, 1, sizeof back, fp), 70000);
        fclose(fp);
        CHECK(!memcmp(back, data, 70000));
        unlink(out);
        EQ(run("mdir -i '%s@@%ld' '::/Documents/Renamed Folder/A Rather Long File Name Number 8.text'", img[0], first * 512) != 0, 1);   /* deleted */
    }
    fat_down(0);
}

/* The other direction: images made by mkfs, and by mtools, read and then written by tilefs. */
static void test_foreign_made(int kind)
{
    static uint8_t data[50000], back[50000];
    const int exfat = kind == EXFAT;
    const char *mkfs = exfat ? "/usr/sbin/mkfs.exfat" : "/usr/sbin/mkfs.vfat";
    const char *fsck = exfat ? "/usr/sbin/fsck.exfat" : "/usr/sbin/fsck.vfat";
    if (!have_tool(mkfs) || !have_tool(fsck) || (!exfat && !have_tool("/usr/bin/mcopy"))) {
        printf("  (tools missing: skipped)\n");
        return;
    }
    fs_reset();
    fat_down(0);
    const char *tmp = getenv("TMPDIR");
    snprintf(img[0], sizeof img[0], "%s/tilefs_img_XXXXXX", tmp ? tmp : "/tmp");
    const int fd0 = mkstemp(img[0]);
    CHECK(fd0 >= 0);
    CHECK(ftruncate(fd0, (off_t)(kind == FAT16 ? 32768 : kind == FAT32 ? 131072 : 65536) * 512) == 0);
    close(fd0);
    EQ(run("%s %s '%s'", mkfs, kind == FAT16 ? "-F 16" : kind == FAT32 ? "-F 32" : "", img[0]), 0);
    if (!exfat) {   /* put files in with mtools: a long name, a directory, a file of several clusters */
        char path[400];
        snprintf(path, sizeof path, "%s.src", img[0]);
        fill(data, 50000, 3);
        FILE *fp = fopen(path, "wb");
        CHECK(fp && fwrite(data, 1, 50000, fp) == 50000);
        fclose(fp);
        EQ(run("mmd -i '%s' '::/Some Directory'", img[0]), 0);
        EQ(run("mcopy -i '%s' '%s' '::/Some Directory/Made By Mtools With A Long Name.bin'", img[0], path), 0);
        EQ(run("mcopy -i '%s' '%s' '::/SHORT.BIN'", img[0], path), 0);
        unlink(path);
    }
    EQ(fs_blk_file_open(&blk[0], img[0], 0), 0);
    fat_is_up[0] = 1;
    EQ(fs_fat_init(&fat[0], 0, &fs_blk_file_dev, &blk[0]), 0);
    EQ(fs_mount("t", &fs_fat_ops, &fat[0]), 0);
    if (!exfat) {
        fill(data, 50000, 3);
        CHECK(slurp("/t/Some Directory/Made By Mtools With A Long Name.bin", back, sizeof back) == 50000 && !memcmp(back, data, 50000));
        CHECK(slurp("/t/short.bin", back, sizeof back) == 50000 && !memcmp(back, data, 50000));   /* an 8.3 name, any case */
        LISTS("/t", "SHORT.BIN Some Directory/");
    }
    fill(data, 20000, 4);
    EQ(put("/t/written by tilefs.dat", data, 20000), 20000);
    EQ(fs_mkdir("/t/New Dir"), 0);
    EQ(put("/t/New Dir/x", data, 100), 100);
    fat_release(0);
    check_with_fsck(fsck, img[0], 0, kind_name[kind]);
    fat_down(0);
}

/* ---- more of the FAT backend: times, faults, a full disk, odd names, two drives ---- */

static void test_fat_time(void)
{
    static const struct { unsigned y, mo, d, h, mi, s; uint32_t unix_time; } known[] = {
        {1980, 1, 1, 0, 0, 0, 315532800u},      {1999, 12, 31, 23, 59, 58, 946684798u}, {2000, 2, 29, 12, 34, 56, 951827696u},
        {2024, 2, 29, 23, 59, 58, 1709251198u}, {2026, 1, 1, 0, 0, 0, 1767225600u},     {2038, 1, 19, 3, 14, 6, 2147483646u},
        {2100, 3, 1, 0, 0, 0, 4107542400u},   /* 2100 is not a leap year */
    };
    for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++) {
        const uint32_t fat = fs_fat_from_unix(known[i].unix_time);
        EQ(fat >> 25, known[i].y - 1980);
        EQ((fat >> 21) & 15, known[i].mo);
        EQ((fat >> 16) & 31, known[i].d);
        EQ((fat >> 11) & 31, known[i].h);
        EQ((fat >> 5) & 63, known[i].mi);
        EQ((fat & 31) * 2, known[i].s);
        EQ(fs_fat_to_unix((uint16_t)(fat >> 16), (uint16_t)fat), known[i].unix_time);
    }
    EQ(fs_fat_from_unix(1700000001u), fs_fat_from_unix(1700000000u));   /* two second steps round down */
    EQ(fs_fat_to_unix((uint16_t)(fs_fat_from_unix(1700000001u) >> 16), (uint16_t)fs_fat_from_unix(1700000001u)), 1700000000u);
    EQ(fs_fat_from_unix(0), fs_fat_from_unix(315532800u));            /* before 1980: the earliest there is */
    EQ(fs_fat_to_unix((uint16_t)((127 << 9) | (12 << 5) | 31), (uint16_t)((23 << 11) | (59 << 5) | 29)), 0xFFFFFFFFu);   /* 2107 */
    EQ(fs_fat_to_unix(0, 0), 0);   /* a zero date is not a date */
    /* every even second over a few leap and non-leap years, both ways */
    for (uint32_t t = 315532800u; t < 315532800u + 86400u * 366 * 3; t += 86400 / 3 * 2 + 4) {
        const uint32_t fat = fs_fat_from_unix(t);
        EQ(fs_fat_to_unix((uint16_t)(fat >> 16), (uint16_t)fat), t & ~1u);
    }
    for (uint32_t t = 4107542400u - 86400u * 5; t < 4107542400u + 86400u * 5; t += 3600) {   /* around 2100-03-01 */
        const uint32_t fat = fs_fat_from_unix(t);
        EQ(fs_fat_to_unix((uint16_t)(fat >> 16), (uint16_t)fat), t & ~1u);
    }
}

static uint32_t fake_now;
static uint32_t fake_clock(void) { return fake_now; }

static void test_fat_clock(void)
{
    fs_stat_t st;
    fs_fat_set_clock(fake_clock);
    fake_now = 1700000001u;
    setup(FAT16);
    EQ(put("/t/f", "x", 1), 1);
    EQ(fs_stat("/t/f", &st), 0);
    EQ(st.mtime, 1700000000u);
    fake_now = 1709251198u;
    EQ(fs_mkdir("/t/d"), 0);
    EQ(fs_stat("/t/d", &st), 0);
    EQ(st.mtime, 1709251198u);
    fake_now = 1800000000u;
    EQ(put("/t/f", "yy", 2), 2);   /* a rewrite is a new time */
    EQ(fs_stat("/t/f", &st), 0);
    EQ(st.mtime, 1800000000u);
    EQ(fs_stat("/t/d", &st), 0);
    EQ(st.mtime, 1709251198u);
    fs_fat_set_clock(NULL);
    EQ(put("/t/g", "x", 1), 1);   /* with no clock: 2026-01-01 */
    EQ(fs_stat("/t/g", &st), 0);
    EQ(st.mtime, 1767225600u);
}

/* A block device that fails when told to. */
typedef struct {
    fs_blk_file_t *inner;
    int init_fails, reads_until_fail, writes_until_fail;   /* < 0: never */
} flaky_t;

static int flaky_init(void *c) { return ((flaky_t *)c)->init_fails ? FS_EIO : 0; }
static int flaky_read(void *c, void *b, uint32_t s, uint32_t n)
{
    flaky_t *f = c;
    if (f->reads_until_fail == 0)
        return FS_EIO;
    if (f->reads_until_fail > 0)
        f->reads_until_fail--;
    return fs_blk_file_dev.read(f->inner, b, s, n);
}
static int flaky_write(void *c, const void *b, uint32_t s, uint32_t n)
{
    flaky_t *f = c;
    if (f->writes_until_fail == 0)
        return FS_EIO;
    if (f->writes_until_fail > 0)
        f->writes_until_fail--;
    return fs_blk_file_dev.write(f->inner, b, s, n);
}
static uint32_t flaky_sectors(void *c) { return ((flaky_t *)c)->inner->sectors; }
static const fs_blockdev_t flaky_dev = {flaky_init, flaky_read, flaky_write, NULL, flaky_sectors};

static void test_fat_faults(void)
{
    static uint8_t buf[20000];
    flaky_t fl = {&blk[0], 0, -1, -1};
    fs_reset();
    fat_down(0);
    fat_up(0, FAT16, "t", 0);   /* a good image to start from */
    EQ(put("/t/keep", "kept", 4), 4);
    fat_release(0);
    /* the device is missing, or does not start */
    fl.init_fails = 1;
    EQ(fs_fat_init(&fat[0], 0, &flaky_dev, &fl), FS_EIO);
    fl.init_fails = 0;
    EQ(fs_blk_file_open(&blk[0], img[0], 0), 0);
    /* a read error while mounting */
    fl.reads_until_fail = 0;
    EQ(fs_fat_init(&fat[0], 0, &flaky_dev, &fl), FS_EIO);
    /* an image with nothing on it */
    fat_down(0);
    snprintf(img[0], sizeof img[0], "%s/tilefs_img_blank_%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    EQ(fs_blk_file_open(&blk[0], img[0], 4096), 0);
    fat_is_up[0] = 1;
    EQ(fs_fat_init(&fat[0], 0, &fs_blk_file_dev, &blk[0]), FS_ENODEV);
    EQ(fs_fat_format(0, &fs_blk_file_dev, &blk[0], FS_FAT_FAT | FS_FAT_SFD), 0);   /* 2 MB: it can be formatted, as FAT12 or FAT16 */
    EQ(fs_fat_init(&fat[0], 0, &fs_blk_file_dev, &blk[0]), 0);
    EQ(fs_fat_done(&fat[0]), 0);
    fat_down(0);
    /* errors in the middle of work */
    fat_up(0, FAT16, "t", 0);
    EQ(put("/t/f", buf, sizeof buf), (int)sizeof buf);
    fat_release(0);
    fl.reads_until_fail = fl.writes_until_fail = -1;
    EQ(fs_blk_file_open(&blk[0], img[0], 0), 0);
    EQ(fs_fat_init(&fat[0], 0, &flaky_dev, &fl), 0);
    EQ(fs_mount("t", &fs_fat_ops, &fat[0]), 0);
    fl.reads_until_fail = 0;
    {
        const int fd = fs_open("/t/f", FS_RDONLY);
        if (fd >= 0) {   /* the directory may be cached; the data is not */
            EQ(fs_read(fd, buf, sizeof buf), FS_EIO);
            EQ(fs_close(fd), 0);
        } else {
            EQ(fd, FS_EIO);
        }
    }
    fl.reads_until_fail = -1;
    EQ(slurp("/t/f", buf, sizeof buf), (int)sizeof buf);   /* and it recovers */
    fl.writes_until_fail = 0;
    {
        const int fd = fs_open("/t/new", FS_WRONLY | FS_CREAT);
        int e = fd;
        if (fd >= 0) {
            e = fs_write(fd, buf, sizeof buf);
            const int c = fs_close(fd);
            if (e >= 0)
                e = c;   /* the error may only come when the data is flushed */
        }
        EQ(e, FS_EIO);
    }
    fl.writes_until_fail = -1;
    EQ(fs_unlink("/t/new") <= 0, 1);   /* whatever state it was left in, the file system answers */
    fat_release(0);
    fat_down(0);
}

static void test_fat_full(int kind)
{
    static uint8_t chunk[65536];
    uint64_t fr, total, fr0;
    char name[64];
    fat_up_ram(kind, "t");
    memset(chunk, 0xA5, sizeof chunk);
    EQ(fs_space("/t", &fr0, &total), 0);
    CHECK(fr0 > 0 && fr0 <= total);
    if (kind == FAT16)
        EQ(fr0, total);   /* the FAT16 root directory is outside the data area: all of it is free */
    int fd = fs_open("/t/big", FS_WRONLY | FS_CREAT);
    uint64_t written = 0;
    int w;
    while ((w = fs_write(fd, chunk, sizeof chunk)) > 0)
        written += (unsigned)w;
    EQ(w, FS_ENOSPC);
    EQ(written, fr0);   /* the file takes every free byte, to the cluster */
    EQ(fs_close(fd), 0);
    EQ(fs_space("/t", &fr, &total), 0);
    EQ(fr, 0);
    EQ(fs_mkdir("/t/nospace"), FS_ENOSPC);
    fd = fs_open("/t/another", FS_WRONLY | FS_CREAT);
    if (fd >= 0) {   /* the entry may fit, the data cannot */
        EQ(fs_write(fd, chunk, 1), FS_ENOSPC);
        EQ(fs_close(fd), 0);
        EQ(fs_unlink("/t/another"), 0);
    } else {
        EQ(fd, FS_ENOSPC);
    }
    EQ(fs_unlink("/t/big"), 0);
    EQ(fs_space("/t", &fr, &total), 0);
    CHECK(fr >= fr0 - 4096);   /* what was free before is free again (the directory may have grown) */
    /* the root directory of FAT12/16 holds a fixed number of entries */
    int made = 0, e = 0;
    for (; made < 700; made++) {
        snprintf(name, sizeof name, "/t/file_with_a_long_name_%04d.txt", made);
        fd = fs_open(name, FS_WRONLY | FS_CREAT);
        if (fd < 0) {
            e = fd;
            break;
        }
        EQ(fs_close(fd), 0);
    }
    if (kind == FAT16) {
        EQ(e, FS_ENOSPC);
        CHECK(made > 100 && made < 512);
    } else {
        EQ(made, 700);   /* FAT32 and exFAT: the directory is a file that grows */
    }
    for (int i = 0; i < made; i++) {
        snprintf(name, sizeof name, "/t/file_with_a_long_name_%04d.txt", i);
        EQ(fs_unlink(name), 0);
    }
    LISTS("/t", "");
}

static void test_fat_names(int kind)
{
    char buf[16];
    setup(kind);
    /* bytes above 127 are the OEM code page 437: e-acute, a-umlaut, a box drawing character */
    const char *names[] = {"/t/caf\x82", "/t/\x84\x84.txt", "/t/box\xC4line", "/t/MiXeD Case.Name"};
    for (int i = 0; i < 4; i++) {
        EQ(put(names[i], "v", 1), 1);
        CHECK(slurp(names[i], buf, sizeof buf) == 1);
    }
    LISTS("/t", "MiXeD Case.Name box\xC4line caf\x82 \x84\x84.txt");
    CHECK(slurp("/t/MIXED CASE.NAME", buf, sizeof buf) == 1);
    CHECK(slurp("/t/CAF\x82", buf, sizeof buf) == 1);   /* case folding of the unaccented part */
    /* FatFs gives "caf\x82" the short name CAFE (its table turns an accented capital into the plain letter), and a name that fits a short
     * name is looked up by that alias: so on FAT the two spellings are one name. Portable programs keep to ASCII for names they make. */
    EQ(fs_stat("/t/cafe", &(fs_stat_t){0}), kind == EXFAT ? FS_ENOENT : 0);   /* exFAT has no short names */
    EQ(fs_rename("/t/caf\x82", "/t/cafe"), kind == EXFAT ? 0 : FS_EEXIST);
    if (kind == EXFAT)
        EQ(fs_rename("/t/cafe", "/t/caf\x82"), 0);
    EQ(fs_rename("/t/caf\x82", "/t/cafx"), 0);
    EQ(fs_unlink("/t/cafx"), 0);
    /* long names are whole */
    char longname[300];
    memset(longname, 'q', 200);
    longname[200] = '\0';
    char path[400];
    snprintf(path, sizeof path, "/t/%s", longname);
    EQ(put(path, "long", 4), 4);
    snprintf(path, sizeof path, "/t/%s", longname + 1);   /* one character shorter is a different file */
    EQ(fs_stat(path, &(fs_stat_t){0}), FS_ENOENT);
    /* names that need the long form: a dot first, several dots, spaces inside, a plus and other characters FAT short names lack */
    const char *odd[] = {"/t/.dotfirst", "/t/a.b.c.d", "/t/two  spaces", "/t/a+b,c;d=e[f]", "/t/%percent", "/t/long extension.markdown"};
    for (int i = 0; i < 6; i++) {
        EQ(put(odd[i], "o", 1), 1);
        CHECK(slurp(odd[i], buf, sizeof buf) == 1);
    }
    char want[1000];   /* in byte order: '%' '.' 'M' 'a' 'b' 'l' 'q' 't', then the bytes above 127 */
    snprintf(want, sizeof want, "%%percent .dotfirst MiXeD Case.Name a+b,c;d=e[f] a.b.c.d box\xC4line long extension.markdown %s two  spaces \x84\x84.txt", longname);
    {
        char l[2000];
        listing("/t", l, sizeof l);
        EQ(strcmp(l, want), 0);
    }
}

static void test_fat_open_sizes(int kind)
{
    fs_dirent_t e;
    fs_stat_t st;
    setup(kind);
    const int fd = fs_open("/t/growing", FS_WRONLY | FS_CREAT);
    CHECK(fd >= 0);
    EQ(fs_write(fd, "0123456789", 10), 10);
    EQ(fs_stat("/t/growing", &st), 0);   /* what an open file has written is what the directory says */
    EQ(st.size, 10);
    EQ(fs_write(fd, "0123456789", 10), 10);
    const int dd = fs_opendir("/t");
    EQ(fs_readdir(dd, &e), 1);
    EQ(e.size, 20);
    EQ(fs_closedir(dd), 0);
    EQ(fs_close(fd), 0);
}

static void test_block_bounds(void)
{
    static uint8_t mem[8192], buf[1024];
    fs_blk_ram_t r;
    fs_blk_file_t f;
    EQ(fs_blk_ram_init(&r, mem, sizeof mem), 0);
    EQ(r.sectors, 16);
    EQ(fs_blk_ram_dev.read(&r, buf, 15, 1), 0);
    EQ(fs_blk_ram_dev.read(&r, buf, 15, 2), FS_EINVAL);
    EQ(fs_blk_ram_dev.read(&r, buf, 16, 1), FS_EINVAL);
    EQ(fs_blk_ram_dev.write(&r, buf, 16, 1), FS_EINVAL);
    EQ(fs_blk_ram_dev.write(&r, buf, 0xFFFFFFFFu, 2), FS_EINVAL);
    EQ(fs_blk_ram_dev.write(&r, buf, 14, 2), 0);
    EQ(fs_blk_ram_init(&r, mem, 100), FS_EINVAL);   /* not even one sector */
    char path[300];
    snprintf(path, sizeof path, "%s/tilefs_bounds_%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    unlink(path);
    EQ(fs_blk_file_open(&f, path, 0), FS_ENOENT);   /* missing, and not to be made */
    EQ(fs_blk_file_open(&f, path, 16), 0);
    EQ(f.sectors, 16);
    EQ(fs_blk_file_dev.read(&f, buf, 15, 1), 0);
    EQ(fs_blk_file_dev.read(&f, buf, 15, 2), FS_EINVAL);
    EQ(fs_blk_file_dev.read(&f, buf, 16, 1), FS_EINVAL);
    EQ(fs_blk_file_dev.write(&f, buf, 16, 1), FS_EINVAL);
    EQ(fs_blk_file_dev.write(&f, buf, 15, 1), 0);
    EQ(fs_blk_file_dev.sync(&f), 0);
    fs_blk_file_close(&f);
    EQ(fs_blk_file_open(&f, path, 0), 0);   /* it was kept, and has its size */
    EQ(f.sectors, 16);
    fs_blk_file_close(&f);
    unlink(path);
}

static void test_fat_two_drives(void)
{
    char buf[16];
    setup(FAT16);
    fat_up(1, EXFAT, "u", 0);
    EQ(put("/t/f", "on drive 0", 10), 10);
    EQ(put("/u/f", "on drive 1", 10), 10);
    EQ(fs_mkdir("/u/d"), 0);
    CHECK(slurp("/t/f", buf, sizeof buf) == 10 && !memcmp(buf, "on drive 0", 10));
    CHECK(slurp("/u/f", buf, sizeof buf) == 10 && !memcmp(buf, "on drive 1", 10));
    EQ(fs_stat("/t/d", &(fs_stat_t){0}), FS_ENOENT);
    EQ(fs_rename("/t/f", "/u/g"), FS_EXDEV);
    fs_reset();
    fat_down(1);
    fat_down(0);
}

#include <time.h>
static void phase_time(const char *what)
{
    static struct timespec last;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (last.tv_sec || last.tv_nsec)
        printf("  [%.1f s] %s\n", (double)(now.tv_sec - last.tv_sec) + (double)(now.tv_nsec - last.tv_nsec) / 1e9, what);
    last = now;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int kind = RAM; kind < NKINDS; kind++) {
        phase_time("previous");
        printf("conformance: %s\n", kind_name[kind]);
        test_files(kind);
        test_seek(kind);
        test_blocks(kind);
        test_dirs(kind);
        test_rename(kind);
        test_busy(kind);
        test_paths(kind);
        test_limits(kind);
        test_space(kind);
    }
    test_backend_view();
    test_mounts();
    test_ram_limits();
    test_fat_time();
    test_fat_clock();
    test_fat_faults();
    test_fat_two_drives();
    test_block_bounds();
    for (int kind = FAT16; kind < NKINDS; kind++) {
        phase_time("previous");
        printf("FAT extras: %s\n", kind_name[kind]);
        test_fat_full(kind);
        test_fat_open_sizes(kind);
        test_fat_names(kind);
        phase_time("previous");
        printf("other tools: %s\n", kind_name[kind]);
        test_foreign_check(kind, 0);
        test_foreign_check(kind, 1);
        test_foreign_made(kind);
    }
    if (argc > 2)
        rng_state ^= strtoul(argv[2], NULL, 10) * 0x9E3779B97F4A7C15ul;   /* another run of the random test */
    const unsigned long ops = argc > 1 ? strtoul(argv[1], NULL, 10) : 30000;
    for (int kind = RAM; kind < NKINDS; kind++)
        if (kind != POSIX) {   /* the host against itself would prove nothing */
            phase_time("previous");
            printf("differential: %s against the host\n", kind_name[kind]);
            test_differential(kind, ops);
        }
    phase_time("previous");
    fs_reset();
    fat_down(0);
    fat_down(1);
    host_clean();
    printf("test_tilefs: %lu checks passed\n", checks);
    return 0;
}
