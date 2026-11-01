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

#include "tilefs.h"

static unsigned long checks;

#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define EQ(a, b) do { const long long va_ = (long long)(a), vb_ = (long long)(b); checks++; \
    if (va_ != vb_) { fprintf(stderr, "FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); exit(1); } } while (0)

enum { RAM, POSIX };

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

/* A fresh, empty file system of the kind, mounted at /t. */
static void setup(int kind)
{
    fs_reset();
    if (kind == RAM) {
        EQ(fs_ram_init(&ram, pool, sizeof pool), 0);
        EQ(fs_mount("t", &fs_ram_ops, &ram), 0);
    } else {
        host_clean();
        const char *tmp = getenv("TMPDIR");
        snprintf(host_dir, sizeof host_dir, "%s/tilefs_test_XXXXXX", tmp ? tmp : "/tmp");
        CHECK(mkdtemp(host_dir) != NULL);
        EQ(fs_posix_init(&posix_fs, host_dir), 0);
        EQ(fs_mount("/t", &fs_posix_ops, &posix_fs), 0);
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
    if (kind == RAM) {   /* case does not matter on the RAM disk, and is kept */
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

static void test_differential(unsigned long ops)
{
    static fs_ram_t big;
    static uint8_t bpool[4 * 1024 * 1024];   /* 4096 entries: renaming moves whole trees, so they grow deeper than the paths used */
    int fr[FS_MAX_FILES], fp[FS_MAX_FILES], dr[FS_MAX_DIRS], dp[FS_MAX_DIRS];
    for (int i = 0; i < FS_MAX_FILES; i++)
        fr[i] = fp[i] = -1;
    for (int i = 0; i < FS_MAX_DIRS; i++)
        dr[i] = dp[i] = -1;
    setup(POSIX);
    EQ(fs_ram_init(&big, bpool, sizeof bpool), 0);
    EQ(fs_mount("r", &fs_ram_ops, &big), 0);
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

int main(int argc, char **argv)
{
    for (int kind = RAM; kind <= POSIX; kind++) {
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
    if (argc > 2)
        rng_state ^= strtoul(argv[2], NULL, 10) * 0x9E3779B97F4A7C15ul;   /* another run of the random test */
    test_differential(argc > 1 ? strtoul(argv[1], NULL, 10) : 300000);
    fs_reset();
    host_clean();
    printf("test_tilefs: %lu checks passed\n", checks);
    return 0;
}
