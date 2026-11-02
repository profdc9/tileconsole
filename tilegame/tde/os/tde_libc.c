/* SPDX-License-Identifier: MIT */
/*
 * The files of the editor, from tilefs: the C library calls that the editor's sources make (fopen, stat, opendir, rename, getcwd, ...) are taken over
 * at link time (ld --wrap=NAME makes the editor's calls go to __wrap_NAME here), so that the editor's own code is unchanged and both the desktop
 * build and the board build read and write the same file system layer. stdio itself stays the C library's: fopen gives a FILE whose reads, writes and
 * seeks are tilefs's (fopencookie).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <utime.h>
#include <dirent.h>

#include "tilefs.h"

/* tilefs's errors as errno values */
static int to_errno(int e)
{
    switch (e) {
    case FS_EPERM: return EPERM;
    case FS_ENOENT: return ENOENT;
    case FS_EIO: return EIO;
    case FS_EBADF: return EBADF;
    case FS_EBUSY: return EBUSY;
    case FS_EEXIST: return EEXIST;
    case FS_EXDEV: return EXDEV;
    case FS_ENOTDIR: return ENOTDIR;
    case FS_EISDIR: return EISDIR;
    case FS_EINVAL: case FS_EBADNAME: return EINVAL;
    case FS_EMFILE: return EMFILE;
    case FS_ENOSPC: return ENOSPC;
    case FS_ENAMETOOLONG: return ENAMETOOLONG;
    case FS_ENOTEMPTY: return ENOTEMPTY;
    case FS_ENOMEM: return ENOMEM;
    case FS_ENODEV: case FS_ENOMEDIA: return ENODEV;
    default: return EIO;
    }
}

static int fail(int e)
{
    errno = to_errno(e);
    return -1;
}

/* ---- FILE over a tilefs handle ---- */

static ssize_t cookie_read(void *c, char *buf, size_t n)
{
    const int r = fs_read((int)(intptr_t)c, buf, n);
    if (r < 0) {
        errno = to_errno(r);
        return -1;
    }
    return r;
}

static ssize_t cookie_write(void *c, const char *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        const int w = fs_write((int)(intptr_t)c, buf + done, n - done);
        if (w < 0) {
            errno = to_errno(w);
            return -1;
        }
        done += (size_t)w;
    }
    return (ssize_t)n;
}

static int cookie_seek(void *c, off_t *off, int whence)
{
    const int64_t r = fs_seek((int)(intptr_t)c, *off, whence == SEEK_SET ? FS_SEEK_SET : whence == SEEK_CUR ? FS_SEEK_CUR : FS_SEEK_END);
    if (r < 0) {
        errno = to_errno((int)r);
        return -1;
    }
    *off = (off_t)r;
    return 0;
}

static int cookie_close(void *c)
{
    const int r = fs_close((int)(intptr_t)c);
    return r < 0 ? fail(r) : 0;
}

static const cookie_io_functions_t cookie_io = {cookie_read, cookie_write, cookie_seek, cookie_close};

void tde_crumb(unsigned id);

FILE *__wrap_fopen(const char *name, const char *mode)
{
    tde_crumb(100);
    int flags;
    const int plus = strchr(mode, '+') != NULL;
    switch (mode[0]) {
    case 'r': flags = plus ? FS_RDWR : FS_RDONLY; break;
    case 'w': flags = (plus ? FS_RDWR : FS_WRONLY) | FS_CREAT | FS_TRUNC; break;
    case 'a': flags = (plus ? FS_RDWR : FS_WRONLY) | FS_CREAT | FS_APPEND; break;
    default: errno = EINVAL; return NULL;
    }
    const int fd = fs_open(name, flags);
    if (fd < 0) {
        fail(fd);
        return NULL;
    }
    FILE *f = fopencookie((void *)(intptr_t)fd, mode, cookie_io);
    if (!f)
        fs_close(fd);
    return f;
}

/* ---- names ---- */

static void fill_stat(struct stat *st, const fs_stat_t *fs)
{
    memset(st, 0, sizeof *st);
    st->st_mode = fs->is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    st->st_size = (off_t)fs->size;
    st->st_mtime = (time_t)fs->mtime;
    st->st_atime = st->st_ctime = st->st_mtime;
    st->st_nlink = 1;
}

int __wrap_stat(const char *path, struct stat *st)
{
    fs_stat_t fs;
    const int e = fs_stat(path, &fs);
    if (e < 0)
        return fail(e);
    fill_stat(st, &fs);
    return 0;
}

int __wrap_chmod(const char *path, mode_t mode)
{
    (void)mode;
    fs_stat_t fs;
    const int e = fs_stat(path, &fs);
    return e < 0 ? fail(e) : 0;   /* FAT has no permissions: it can be asked and it does not mind */
}

int __wrap_utime(const char *path, const struct utimbuf *times)
{
    (void)times;
    fs_stat_t fs;
    const int e = fs_stat(path, &fs);
    return e < 0 ? fail(e) : 0;
}

int __wrap_access(const char *path, int mode)
{
    (void)mode;   /* there are no permissions: what exists can be read and written */
    fs_stat_t fs;
    const int e = fs_stat(path, &fs);
    return e < 0 ? fail(e) : 0;
}

int __wrap_unlink(const char *path)
{
    const int e = fs_unlink(path);
    return e < 0 ? fail(e) : 0;
}

int __wrap_remove(const char *path)
{
    fs_stat_t fs;
    int e = fs_stat(path, &fs);
    if (e < 0)
        return fail(e);
    e = fs.is_dir ? fs_rmdir(path) : fs_unlink(path);
    return e < 0 ? fail(e) : 0;
}

int __wrap_rmdir(const char *path)
{
    const int e = fs_rmdir(path);
    return e < 0 ? fail(e) : 0;
}

/* C's rename replaces the new name, tilefs's does not: take the old file of that name away first (a directory is left as it is). */
int __wrap_rename(const char *from, const char *to)
{
    fs_stat_t fs;
    if (fs_stat(to, &fs) == 0 && !fs.is_dir) {
        const int u = fs_unlink(to);
        if (u < 0)
            return fail(u);
    }
    const int e = fs_rename(from, to);
    return e < 0 ? fail(e) : 0;
}

int __wrap_mkdir(const char *path, mode_t mode)
{
    (void)mode;
    const int e = fs_mkdir(path);
    return e < 0 ? fail(e) : 0;
}

char *__wrap_getcwd(char *buf, size_t size)
{
    const char *cwd = fs_getcwd();
    if (!buf) {
        buf = malloc(strlen(cwd) + 1);
        if (!buf)
            return NULL;
    } else if (strlen(cwd) >= size) {
        errno = ERANGE;
        return NULL;
    }
    strcpy(buf, cwd);
    return buf;
}

int __wrap_chdir(const char *path)
{
    const int e = fs_chdir(path);
    return e < 0 ? fail(e) : 0;
}

char *__wrap_realpath(const char *path, char *resolved)
{
    char tmp[FS_PATH_MAX + 1];
    fs_stat_t fs;
    int e = fs_normalise(path, tmp, sizeof tmp);
    if (e < 0) {
        fail(e);
        return NULL;
    }
    e = fs_stat(tmp, &fs);   /* like realpath, the path has to exist */
    if (e < 0) {
        fail(e);
        return NULL;
    }
    if (!resolved) {
        resolved = malloc(strlen(tmp) + 1);
        if (!resolved)
            return NULL;
    }
    strcpy(resolved, tmp);
    return resolved;
}

/* ---- directories ---- */

struct tde_dir {
    int dd;
    int dots;              /* how many of "." and ".." have been given: tilefs does not list them, a Unix directory does */
    struct dirent entry;
};

DIR *__wrap_opendir(const char *path)
{
    const int dd = fs_opendir(path);
    if (dd < 0) {
        fail(dd);
        return NULL;
    }
    struct tde_dir *d = calloc(1, sizeof *d);
    if (!d) {
        fs_closedir(dd);
        errno = ENOMEM;
        return NULL;
    }
    d->dd = dd;
    return (DIR *)d;
}

struct dirent *__wrap_readdir(DIR *dir)
{
    struct tde_dir *d = (struct tde_dir *)dir;
    fs_dirent_t e;
    memset(&d->entry, 0, sizeof d->entry);
    if (d->dots < 2) {
        strcpy(d->entry.d_name, d->dots++ ? ".." : ".");
        d->entry.d_type = DT_DIR;
        return &d->entry;
    }
    if (fs_readdir(d->dd, &e) != 1)
        return NULL;
    strncpy(d->entry.d_name, e.name, sizeof d->entry.d_name - 1);
    d->entry.d_type = e.is_dir ? DT_DIR : DT_REG;
    return &d->entry;
}

int __wrap_closedir(DIR *dir)
{
    struct tde_dir *d = (struct tde_dir *)dir;
    fs_closedir(d->dd);
    free(d);
    return 0;
}
