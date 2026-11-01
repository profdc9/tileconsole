/* SPDX-License-Identifier: MIT */
#include "tilefs.h"

#include <limits.h>
#include <string.h>

#define KNOWN_FLAGS (FS_RDWR | FS_CREAT | FS_EXCL | FS_TRUNC | FS_APPEND)
#define MAX_IO 0x40000000u

typedef struct {
    const fs_ops_t *ops;
    void *ctx;
    char name[FS_MOUNT_NAME_MAX + 1];
    uint8_t used;
} mount_t;

typedef struct {
    void *h;
    int8_t mount;   /* the mount, or -1 for the virtual root directory */
    uint8_t used, flags;
    uint8_t vi;     /* the virtual root directory: the next mount to list */
    char path[FS_PATH_MAX + 1];
} slot_t;

static mount_t mounts[FS_MAX_MOUNTS];
static slot_t files[FS_MAX_FILES], dirs[FS_MAX_DIRS];
static char cwd[FS_PATH_MAX + 1] = "/";

static int fold(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
}

/* The same name, ignoring case, for the first `n` characters of a and all of b. */
static int same_name(const char *a, size_t n, const char *b)
{
    if (strlen(b) != n)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (fold((unsigned char)a[i]) != fold((unsigned char)b[i]))
            return 0;
    return 1;
}

/* Is b the path a, or something below it? Both normalised. */
static int covers(const char *a, const char *b)
{
    const size_t n = strlen(a);
    if (n == 1)   /* "/" */
        return 1;
    for (size_t i = 0; i < n; i++)
        if (fold((unsigned char)a[i]) != fold((unsigned char)b[i]))
            return 0;
    return b[n] == '\0' || b[n] == '/';
}

static int same_path(const char *a, const char *b)
{
    return same_name(a, strlen(a), b);
}

static int valid_name(const char *s, size_t n)
{
    if (n == 0)
        return FS_EINVAL;
    if (n > FS_NAME_MAX)
        return FS_ENAMETOOLONG;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F || strchr("\"*:<>?\\|", c))
            return FS_EBADNAME;
    }
    if (s[n - 1] == '.' || s[n - 1] == ' ')
        return FS_EBADNAME;
    return FS_OK;
}

/* Turns `path` into an absolute path with no "." / ".." / empty parts, in out (FS_PATH_MAX + 1 bytes). */
static int normalise(const char *path, char *out)
{
    size_t len = 0;
    if (!path || !*path)
        return FS_ENOENT;
    if (path[0] != '/') {
        len = strlen(cwd);
        memcpy(out, cwd, len + 1);
        if (len == 1)
            len = 0;   /* the root is kept as nothing, so that parts can be added with a leading slash */
    }
    out[len] = '\0';
    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        const char *e = p;
        while (*e && *e != '/')
            e++;
        const size_t n = (size_t)(e - p);
        if (n == 0) {
            break;
        } else if (n == 1 && p[0] == '.') {
            /* stays */
        } else if (n == 2 && p[0] == '.' && p[1] == '.') {
            while (len && out[len - 1] != '/')
                len--;
            if (len)
                len--;
            out[len] = '\0';
        } else {
            const int bad = valid_name(p, n);
            if (bad)
                return bad;
            if (len + 1 + n > FS_PATH_MAX)
                return FS_ENAMETOOLONG;
            out[len++] = '/';
            memcpy(out + len, p, n);
            len += n;
            out[len] = '\0';
        }
        p = e;
    }
    if (len == 0) {
        out[0] = '/';
        out[1] = '\0';
    }
    return FS_OK;
}

/* Finds the mount of a normalised path and the path inside it. -1 for "/" itself. */
static int locate(const char *norm, int *mount, const char **rel)
{
    if (norm[1] == '\0') {
        *mount = -1;
        *rel = norm;
        return FS_OK;
    }
    const char *name = norm + 1, *end = name;
    while (*end && *end != '/')
        end++;
    for (int i = 0; i < FS_MAX_MOUNTS; i++) {
        if (mounts[i].used && same_name(name, (size_t)(end - name), mounts[i].name)) {
            *mount = i;
            *rel = *end ? end : "/";
            return FS_OK;
        }
    }
    return FS_ENOENT;
}

static int resolve(const char *path, char *norm, int *mount, const char **rel)
{
    const int e = normalise(path, norm);
    return e ? e : locate(norm, mount, rel);
}

/* ---- mounts ---- */

int fs_mount(const char *name, const fs_ops_t *ops, void *ctx)
{
    if (!name || !ops || !ops->open || !ops->close || !ops->read || !ops->write || !ops->seek || !ops->stat || !ops->unlink || !ops->mkdir ||
        !ops->rmdir || !ops->rename || !ops->opendir || !ops->readdir || !ops->closedir)
        return FS_EINVAL;
    if (*name == '/')
        name++;
    const size_t n = strlen(name);
    if (strchr(name, '/') || valid_name(name, n) || n > FS_MOUNT_NAME_MAX)
        return FS_EINVAL;
    int slot = -1;
    for (int i = 0; i < FS_MAX_MOUNTS; i++) {
        if (mounts[i].used) {
            if (same_name(name, n, mounts[i].name))
                return FS_EEXIST;
        } else if (slot < 0) {
            slot = i;
        }
    }
    if (slot < 0)
        return FS_ENOMEM;
    mounts[slot].ops = ops;
    mounts[slot].ctx = ctx;
    memcpy(mounts[slot].name, name, n + 1);
    mounts[slot].used = 1;
    return FS_OK;
}

int fs_unmount(const char *name)
{
    if (!name)
        return FS_EINVAL;
    if (*name == '/')
        name++;
    for (int i = 0; i < FS_MAX_MOUNTS; i++) {
        if (mounts[i].used && same_name(name, strlen(name), mounts[i].name)) {
            for (int j = 0; j < FS_MAX_FILES; j++)
                if (files[j].used && files[j].mount == i)
                    return FS_EBUSY;
            for (int j = 0; j < FS_MAX_DIRS; j++)
                if (dirs[j].used && dirs[j].mount == i)
                    return FS_EBUSY;
            char top[FS_MOUNT_NAME_MAX + 2] = "/";
            strcat(top, mounts[i].name);
            if (covers(top, cwd))
                strcpy(cwd, "/");
            mounts[i].used = 0;
            return FS_OK;
        }
    }
    return FS_ENOENT;
}

void fs_reset(void)
{
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used)
            mounts[files[i].mount].ops->close(files[i].h);
        files[i].used = 0;
    }
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        if (dirs[i].used && dirs[i].mount >= 0)
            mounts[dirs[i].mount].ops->closedir(dirs[i].h);
        dirs[i].used = 0;
    }
    for (int i = 0; i < FS_MAX_MOUNTS; i++)
        mounts[i].used = 0;
    strcpy(cwd, "/");
}

/* ---- files ---- */

static slot_t *file_slot(int fd)
{
    return fd >= 0 && fd < FS_MAX_FILES && files[fd].used ? &files[fd] : NULL;
}

/* Is anything open at or below this path? */
static int in_use(const char *norm)
{
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (files[i].used && covers(norm, files[i].path))
            return 1;
    for (int i = 0; i < FS_MAX_DIRS; i++)
        if (dirs[i].used && dirs[i].mount >= 0 && covers(norm, dirs[i].path))
            return 1;
    return 0;
}

int fs_open(const char *path, int flags)
{
    char norm[FS_PATH_MAX + 1];
    int m;
    const char *rel;
    const int access = flags & FS_RDWR;
    if (!access || (flags & ~KNOWN_FLAGS) || ((flags & FS_TRUNC) && !(access & FS_WRONLY)) || ((flags & FS_EXCL) && !(flags & FS_CREAT)))
        return FS_EINVAL;
    int e = resolve(path, norm, &m, &rel);
    if (e)
        return e;
    if (m < 0)
        return FS_EISDIR;
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (files[i].used && same_path(files[i].path, norm) && ((files[i].flags | flags) & FS_WRONLY))
            return FS_EBUSY;
    int fd = -1;
    for (int i = 0; i < FS_MAX_FILES && fd < 0; i++)
        if (!files[i].used)
            fd = i;
    if (fd < 0)
        return FS_EMFILE;
    void *h = NULL;
    e = mounts[m].ops->open(mounts[m].ctx, rel, flags & ~FS_APPEND, &h);
    if (e < 0)
        return e;
    files[fd].used = 1;
    files[fd].mount = (int8_t)m;
    files[fd].flags = (uint8_t)flags;
    files[fd].h = h;
    strcpy(files[fd].path, norm);
    return fd;
}

int fs_close(int fd)
{
    slot_t *f = file_slot(fd);
    if (!f)
        return FS_EBADF;
    const int e = mounts[f->mount].ops->close(f->h);
    f->used = 0;
    return e < 0 ? e : FS_OK;
}

int fs_read(int fd, void *buf, size_t n)
{
    slot_t *f = file_slot(fd);
    if (!f || !(f->flags & FS_RDONLY))
        return FS_EBADF;
    if (n == 0)
        return 0;
    return mounts[f->mount].ops->read(f->h, buf, n > MAX_IO ? MAX_IO : n);
}

int fs_write(int fd, const void *buf, size_t n)
{
    slot_t *f = file_slot(fd);
    if (!f || !(f->flags & FS_WRONLY))
        return FS_EBADF;
    if (n == 0)
        return 0;
    const fs_ops_t *ops = mounts[f->mount].ops;
    if (f->flags & FS_APPEND) {
        const int64_t e = ops->seek(f->h, 0, FS_SEEK_END);
        if (e < 0)
            return (int)e;
    }
    return ops->write(f->h, buf, n > MAX_IO ? MAX_IO : n);
}

int64_t fs_seek(int fd, int64_t off, int whence)
{
    slot_t *f = file_slot(fd);
    if (!f)
        return FS_EBADF;
    if (whence != FS_SEEK_SET && whence != FS_SEEK_CUR && whence != FS_SEEK_END)
        return FS_EINVAL;
    return mounts[f->mount].ops->seek(f->h, off, whence);
}

int fs_sync(int fd)
{
    slot_t *f = file_slot(fd);
    if (!f)
        return FS_EBADF;
    const fs_ops_t *ops = mounts[f->mount].ops;
    return ops->sync ? ops->sync(f->h) : FS_OK;
}

/* ---- names ---- */

int fs_stat(const char *path, fs_stat_t *st)
{
    char norm[FS_PATH_MAX + 1];
    int m;
    const char *rel;
    const int e = resolve(path, norm, &m, &rel);
    if (e)
        return e;
    if (!st)
        return FS_EINVAL;
    if (m < 0) {
        memset(st, 0, sizeof *st);
        st->is_dir = 1;
        return FS_OK;
    }
    return mounts[m].ops->stat(mounts[m].ctx, rel, st);
}

int fs_mkdir(const char *path)
{
    char norm[FS_PATH_MAX + 1];
    int m;
    const char *rel;
    int e = normalise(path, norm);
    if (e)
        return e;
    e = locate(norm, &m, &rel);
    if (e == FS_ENOENT && !strchr(norm + 1, '/'))
        return FS_EPERM;   /* a new name directly below "/" would be a new mount */
    if (e)
        return e;
    if (m < 0)
        return FS_EEXIST;
    return mounts[m].ops->mkdir(mounts[m].ctx, rel);
}

int fs_rmdir(const char *path)
{
    char norm[FS_PATH_MAX + 1];
    int m;
    const char *rel;
    int e = resolve(path, norm, &m, &rel);
    if (e)
        return e;
    if (m < 0 || rel[1] == '\0')
        return FS_EBUSY;
    if (in_use(norm))
        return FS_EBUSY;
    return mounts[m].ops->rmdir(mounts[m].ctx, rel);
}

int fs_unlink(const char *path)
{
    char norm[FS_PATH_MAX + 1];
    int m;
    const char *rel;
    const int e = resolve(path, norm, &m, &rel);
    if (e)
        return e;
    if (m < 0 || rel[1] == '\0')
        return FS_EISDIR;
    if (in_use(norm))
        return FS_EBUSY;
    return mounts[m].ops->unlink(mounts[m].ctx, rel);
}

int fs_rename(const char *from, const char *to)
{
    char nf[FS_PATH_MAX + 1], nt[FS_PATH_MAX + 1];
    int mf, mt;
    const char *rf, *rt;
    int e = resolve(from, nf, &mf, &rf);
    if (e)
        return e;
    e = resolve(to, nt, &mt, &rt);
    if (e)
        return e;
    if (mf < 0 || mt < 0 || rf[1] == '\0' || rt[1] == '\0')
        return FS_EBUSY;   /* the root and the mount points stay where they are */
    if (mf != mt)
        return FS_EXDEV;
    if (in_use(nf))
        return FS_EBUSY;
    if (covers(nf, nt) && !same_path(nf, nt))
        return FS_EINVAL;   /* a directory cannot go into itself */
    return mounts[mf].ops->rename(mounts[mf].ctx, rf, rt);
}

int fs_space(const char *path, uint64_t *free_bytes, uint64_t *total_bytes)
{
    char norm[FS_PATH_MAX + 1];
    int m;
    const char *rel;
    const int e = resolve(path, norm, &m, &rel);
    if (e)
        return e;
    if (m < 0 || !free_bytes || !total_bytes || !mounts[m].ops->space)
        return FS_EINVAL;
    return mounts[m].ops->space(mounts[m].ctx, free_bytes, total_bytes);
}

/* ---- directories ---- */

static slot_t *dir_slot(int dd)
{
    return dd >= 0 && dd < FS_MAX_DIRS && dirs[dd].used ? &dirs[dd] : NULL;
}

int fs_opendir(const char *path)
{
    char norm[FS_PATH_MAX + 1];
    int m;
    const char *rel;
    int e = resolve(path, norm, &m, &rel);
    if (e)
        return e;
    int dd = -1;
    for (int i = 0; i < FS_MAX_DIRS && dd < 0; i++)
        if (!dirs[i].used)
            dd = i;
    if (dd < 0)
        return FS_EMFILE;
    void *h = NULL;
    if (m >= 0) {
        e = mounts[m].ops->opendir(mounts[m].ctx, rel, &h);
        if (e < 0)
            return e;
    }
    dirs[dd].used = 1;
    dirs[dd].mount = (int8_t)m;
    dirs[dd].h = h;
    dirs[dd].vi = 0;
    strcpy(dirs[dd].path, norm);
    return dd;
}

int fs_readdir(int dd, fs_dirent_t *e)
{
    slot_t *d = dir_slot(dd);
    if (!d || !e)
        return FS_EBADF;
    if (d->mount >= 0)
        return mounts[d->mount].ops->readdir(d->h, e);
    while (d->vi < FS_MAX_MOUNTS && !mounts[d->vi].used)
        d->vi++;
    if (d->vi >= FS_MAX_MOUNTS)
        return 0;
    memset(e, 0, sizeof *e);
    strcpy(e->name, mounts[d->vi++].name);
    e->is_dir = 1;
    return 1;
}

int fs_closedir(int dd)
{
    slot_t *d = dir_slot(dd);
    if (!d)
        return FS_EBADF;
    const int e = d->mount >= 0 ? mounts[d->mount].ops->closedir(d->h) : 0;
    d->used = 0;
    return e < 0 ? e : FS_OK;
}

int fs_chdir(const char *path)
{
    char norm[FS_PATH_MAX + 1];
    fs_stat_t st;
    int e = normalise(path, norm);
    if (e)
        return e;
    e = fs_stat(norm, &st);
    if (e)
        return e;
    if (!st.is_dir)
        return FS_ENOTDIR;
    strcpy(cwd, norm);
    return FS_OK;
}

const char *fs_getcwd(void)
{
    return cwd;
}

const char *fs_strerror(int code)
{
    switch (code) {
    case FS_OK: return "ok";
    case FS_EPERM: return "not allowed";
    case FS_ENOENT: return "no such file or directory";
    case FS_EIO: return "i/o error";
    case FS_EBADF: return "bad handle";
    case FS_EBUSY: return "busy";
    case FS_EEXIST: return "already exists";
    case FS_EXDEV: return "cannot rename across mounts";
    case FS_ENOTDIR: return "not a directory";
    case FS_EISDIR: return "is a directory";
    case FS_EINVAL: return "invalid argument";
    case FS_EMFILE: return "too many open files";
    case FS_ENOSPC: return "disk full";
    case FS_ENAMETOOLONG: return "name too long";
    case FS_ENOTEMPTY: return "directory not empty";
    case FS_EBADNAME: return "bad character in name";
    case FS_ENOMEM: return "out of memory";
    case FS_ENODEV: return "no file system on the device";
    case FS_ENOMEDIA: return "no card";
    default: return "unknown error";
    }
}
