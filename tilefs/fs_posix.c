/* SPDX-License-Identifier: MIT */
/* A host directory as a tilefs mount, for desktop builds and tests. */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/statvfs.h>

#include "tilefs.h"

/* A program that takes over the C library's file functions with ld --wrap (the TDE port does) builds this file with -DTILEFS_WRAPPED_LIBC, so that
 * the host backend reaches the real ones, not its own caller. */
#ifdef TILEFS_WRAPPED_LIBC
int __real_stat(const char *path, struct stat *st);
int __real_unlink(const char *path);
int __real_mkdir(const char *path, mode_t mode);
int __real_rmdir(const char *path);
int __real_rename(const char *from, const char *to);
DIR *__real_opendir(const char *path);
struct dirent *__real_readdir(DIR *dir);
int __real_closedir(DIR *dir);
#define H_STAT __real_stat
#define H_UNLINK __real_unlink
#define H_MKDIR __real_mkdir
#define H_RMDIR __real_rmdir
#define H_RENAME __real_rename
#define H_OPENDIR __real_opendir
#define H_READDIR __real_readdir
#define H_CLOSEDIR __real_closedir
#else
#define H_STAT stat
#define H_UNLINK unlink
#define H_MKDIR mkdir
#define H_RMDIR rmdir
#define H_RENAME rename
#define H_OPENDIR opendir
#define H_READDIR readdir
#define H_CLOSEDIR closedir
#endif

typedef struct {
    DIR *d;
    char path[FS_POSIX_ROOT_MAX + FS_PATH_MAX + 2];
} posix_dir_t;

static int map_errno(int e)
{
    switch (e) {
    case ENOENT: return FS_ENOENT;
    case EEXIST: return FS_EEXIST;
    case ENOTDIR: return FS_ENOTDIR;
    case EISDIR: return FS_EISDIR;
    case ENOTEMPTY: return FS_ENOTEMPTY;
    case ENOSPC: case EDQUOT: case EFBIG: return FS_ENOSPC;
    case EMFILE: case ENFILE: return FS_EMFILE;
    case ENAMETOOLONG: return FS_ENAMETOOLONG;
    case EBUSY: case EXDEV: return FS_EBUSY;
    case EINVAL: return FS_EINVAL;
    case EACCES: case EPERM: case EROFS: return FS_EPERM;
    default: return FS_EIO;
    }
}

int fs_posix_init(fs_posix_t *p, const char *root_dir)
{
    struct stat st;
    if (!root_dir || strlen(root_dir) > FS_POSIX_ROOT_MAX)
        return FS_ENAMETOOLONG;
    if (H_STAT(root_dir, &st) < 0)
        return map_errno(errno);
    if (!S_ISDIR(st.st_mode))
        return FS_ENOTDIR;
    strcpy(p->root, root_dir);
    size_t n = strlen(p->root);
    while (n > 1 && p->root[n - 1] == '/')
        p->root[--n] = '\0';
    return FS_OK;
}

/* The host path of a path inside the mount. */
static void host_path(const fs_posix_t *p, const char *path, char *out, size_t size)
{
    if (path[1] == '\0')   /* the root itself: no trailing slash, which open() would take as a request for a directory */
        snprintf(out, size, "%s", p->root);
    else
        snprintf(out, size, "%s%s", p->root[1] == '\0' ? "" : p->root, path);
}

static int posix_open(void *ctx, const char *path, int flags, void **h)
{
    char host[FS_POSIX_ROOT_MAX + FS_PATH_MAX + 2];
    host_path(ctx, path, host, sizeof host);
    int of = (flags & FS_RDWR) == FS_RDWR ? O_RDWR : (flags & FS_WRONLY) ? O_WRONLY : O_RDONLY;
    of |= O_CLOEXEC | ((flags & FS_CREAT) ? O_CREAT : 0) | ((flags & FS_EXCL) ? O_EXCL : 0);
    /* The file is emptied only after it is known not to be a directory and the open succeeded. */
    const int fd = open(host, of, 0666);
    if (fd < 0)
        return map_errno(errno);
    struct stat st;
    if (fstat(fd, &st) < 0 || S_ISDIR(st.st_mode)) {
        const int e = S_ISDIR(st.st_mode) ? FS_EISDIR : map_errno(errno);
        close(fd);
        return e;
    }
    if ((flags & FS_TRUNC) && ftruncate(fd, 0) < 0) {
        const int e = map_errno(errno);
        close(fd);
        return e;
    }
    *h = (void *)(intptr_t)(fd + 1);
    return FS_OK;
}

static int fd_of(void *h)
{
    return (int)(intptr_t)h - 1;
}

static int posix_close(void *h)
{
    return close(fd_of(h)) < 0 ? map_errno(errno) : FS_OK;
}

static int posix_read(void *h, void *buf, size_t n)
{
    const ssize_t r = read(fd_of(h), buf, n);
    return r < 0 ? map_errno(errno) : (int)r;
}

static int posix_write(void *h, const void *buf, size_t n)
{
    const ssize_t r = write(fd_of(h), buf, n);
    if (r < 0)
        return map_errno(errno);
    return r == 0 && n > 0 ? FS_ENOSPC : (int)r;
}

static int64_t posix_seek(void *h, int64_t off, int whence)
{
    struct stat st;
    const int fd = fd_of(h);
    if (fstat(fd, &st) < 0)
        return map_errno(errno);
    const off_t cur = lseek(fd, 0, SEEK_CUR);
    if (cur < 0)
        return map_errno(errno);
    const int64_t size = st.st_size;
    const int64_t base = whence == FS_SEEK_SET ? 0 : whence == FS_SEEK_CUR ? (int64_t)cur : size;
    if (off > size + 1 || off < -size - 1 || base + off < 0 || base + off > size)
        return FS_EINVAL;
    const off_t r = lseek(fd, (off_t)(base + off), SEEK_SET);
    return r < 0 ? map_errno(errno) : (int64_t)r;
}

static int posix_sync(void *h)
{
    return fsync(fd_of(h)) < 0 ? map_errno(errno) : FS_OK;
}

static void fill_stat(const struct stat *st, fs_stat_t *out)
{
    memset(out, 0, sizeof *out);
    out->is_dir = S_ISDIR(st->st_mode) != 0;
    out->size = out->is_dir ? 0 : (int64_t)st->st_size;
    out->mtime = (uint32_t)st->st_mtime;
}

static int posix_stat(void *ctx, const char *path, fs_stat_t *out)
{
    char host[FS_POSIX_ROOT_MAX + FS_PATH_MAX + 2];
    struct stat st;
    host_path(ctx, path, host, sizeof host);
    if (H_STAT(host, &st) < 0)
        return map_errno(errno);
    fill_stat(&st, out);
    return FS_OK;
}

static int posix_unlink(void *ctx, const char *path)
{
    char host[FS_POSIX_ROOT_MAX + FS_PATH_MAX + 2];
    struct stat st;
    host_path(ctx, path, host, sizeof host);
    if (lstat(host, &st) < 0)
        return map_errno(errno);
    if (S_ISDIR(st.st_mode))
        return FS_EISDIR;
    return H_UNLINK(host) < 0 ? map_errno(errno) : FS_OK;
}

static int posix_mkdir(void *ctx, const char *path)
{
    char host[FS_POSIX_ROOT_MAX + FS_PATH_MAX + 2];
    host_path(ctx, path, host, sizeof host);
    return H_MKDIR(host, 0777) < 0 ? map_errno(errno) : FS_OK;
}

static int posix_rmdir(void *ctx, const char *path)
{
    char host[FS_POSIX_ROOT_MAX + FS_PATH_MAX + 2];
    host_path(ctx, path, host, sizeof host);
    if (H_RMDIR(host) < 0)
        return errno == EEXIST ? FS_ENOTEMPTY : map_errno(errno);
    return FS_OK;
}

static int posix_rename(void *ctx, const char *from, const char *to)
{
    char hf[FS_POSIX_ROOT_MAX + FS_PATH_MAX + 2], ht[FS_POSIX_ROOT_MAX + FS_PATH_MAX + 2];
    struct stat st;
    host_path(ctx, from, hf, sizeof hf);
    host_path(ctx, to, ht, sizeof ht);
    if (lstat(hf, &st) < 0)
        return map_errno(errno);
    if (lstat(ht, &st) == 0)
        return FS_EEXIST;
    if (errno != ENOENT)
        return map_errno(errno);
    return H_RENAME(hf, ht) < 0 ? map_errno(errno) : FS_OK;
}

static int posix_opendir(void *ctx, const char *path, void **h)
{
    posix_dir_t *d = malloc(sizeof *d);
    if (!d)
        return FS_ENOSPC;
    host_path(ctx, path, d->path, sizeof d->path);
    d->d = H_OPENDIR(d->path);
    if (!d->d) {
        const int e = map_errno(errno);
        free(d);
        return e;
    }
    *h = d;
    return FS_OK;
}

static int posix_readdir(void *h, fs_dirent_t *e)
{
    posix_dir_t *d = h;
    struct dirent *de;
    while ((de = H_READDIR(d->d)) != NULL) {
        char full[sizeof d->path + FS_NAME_MAX + 2];
        struct stat st;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        snprintf(full, sizeof full, "%s/%s", d->path, de->d_name);
        if (strlen(de->d_name) > FS_NAME_MAX || H_STAT(full, &st) < 0 || !(S_ISDIR(st.st_mode) || S_ISREG(st.st_mode)))
            continue;
        memset(e, 0, sizeof *e);
        strcpy(e->name, de->d_name);
        e->is_dir = S_ISDIR(st.st_mode) != 0;
        e->size = e->is_dir ? 0 : (int64_t)st.st_size;
        return 1;
    }
    return 0;
}

static int posix_closedir(void *h)
{
    posix_dir_t *d = h;
    const int e = H_CLOSEDIR(d->d) < 0 ? map_errno(errno) : FS_OK;
    free(d);
    return e;
}

static int posix_space(void *ctx, uint64_t *free_bytes, uint64_t *total_bytes)
{
    struct statvfs v;
    if (statvfs(((fs_posix_t *)ctx)->root, &v) < 0)
        return map_errno(errno);
    *free_bytes = (uint64_t)v.f_bavail * v.f_frsize;
    *total_bytes = (uint64_t)v.f_blocks * v.f_frsize;
    return FS_OK;
}

const fs_ops_t fs_posix_ops = {
    .open = posix_open, .close = posix_close, .read = posix_read, .write = posix_write, .seek = posix_seek, .sync = posix_sync,
    .stat = posix_stat, .unlink = posix_unlink, .mkdir = posix_mkdir, .rmdir = posix_rmdir, .rename = posix_rename,
    .opendir = posix_opendir, .readdir = posix_readdir, .closedir = posix_closedir, .space = posix_space,
};
