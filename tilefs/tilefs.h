/* SPDX-License-Identifier: MIT */
/*
 * tilefs: a small file system layer for console-style programs. One POSIX-flavoured API (open, read, write, seek, directories,
 * rename, ...) over interchangeable backends mounted under names: a host directory (fs_posix.c, for desktop builds and tests), a
 * RAM disk (fs_ram.c, for boards without storage) and, later, FatFs on an SD card. Plain C, no malloc, no dependencies.
 *
 * What the layer promises, so that a program behaves the same on every backend:
 *  - Paths are absolute ("/sd/save/game.dat") or relative to the current directory (fs_chdir). "/" is a virtual directory that lists
 *    the mounts; everything else lives under a mount point, which is a single name directly below "/". "." and ".." are resolved
 *    in the text of the path, before any backend sees it. Repeated and trailing slashes are ignored.
 *  - Names are portable FAT-style names: at most FS_NAME_MAX bytes, no control characters, none of  " * : < > ? \ |  and no
 *    trailing dot or space; names are matched without regard to case on backends that cannot tell (RAM, FAT), so a program must
 *    not use two names that differ only in case.
 *  - A file that is open cannot be unlinked, renamed, truncated or have its directory removed (FS_EBUSY), and a file open for
 *    writing cannot be opened a second time. This keeps FAT and POSIX alike.
 *  - Seeking outside 0..size is an error (FS_EINVAL): there are no holes. Writing at the end extends the file.
 *  - rename never replaces anything: if the new name exists the result is FS_EEXIST. It cannot cross mounts (FS_EXDEV).
 *  - Directory entries come in no particular order, and what a listing shows after its directory has been changed is unspecified.
 *  - Calls return a value >= 0 on success and a negative FS_E* code on failure.
 */
#ifndef TILEFS_H
#define TILEFS_H

#include <stddef.h>
#include <stdint.h>

#ifndef FS_MAX_MOUNTS
#define FS_MAX_MOUNTS 4
#endif
#ifndef FS_MAX_FILES
#define FS_MAX_FILES 8   /* open files at once, over all mounts */
#endif
#ifndef FS_MAX_DIRS
#define FS_MAX_DIRS 4
#endif
#define FS_NAME_MAX 255
#define FS_PATH_MAX 260   /* of the normalised absolute path, without the end of string */
#define FS_MOUNT_NAME_MAX 15

enum {
    FS_OK = 0,
    FS_EPERM = -1,         /* not allowed here: making a new top level directory */
    FS_ENOENT = -2,        /* no such file or directory */
    FS_EIO = -3,           /* the device failed */
    FS_EBADF = -4,         /* not an open handle, or not opened for this */
    FS_EBUSY = -5,         /* in use: open, a mount point, or the root */
    FS_EEXIST = -6,
    FS_EXDEV = -7,         /* rename across mounts */
    FS_ENOTDIR = -8,
    FS_EISDIR = -9,
    FS_EINVAL = -10,
    FS_EMFILE = -11,       /* out of handles */
    FS_ENOSPC = -12,       /* the disk or the directory is full */
    FS_ENAMETOOLONG = -13,
    FS_ENOTEMPTY = -14,
    FS_EBADNAME = -15,     /* a character a FAT name cannot have */
    FS_ENOMEM = -16,       /* the mount table is full, or a RAM disk's memory is too small */
};

/* Flags for fs_open: one of the access modes, plus any of the others. */
#define FS_RDONLY 1
#define FS_WRONLY 2
#define FS_RDWR 3
#define FS_CREAT 4     /* make the file if it is not there */
#define FS_EXCL 8      /* with FS_CREAT: fail with FS_EEXIST if it is there */
#define FS_TRUNC 16    /* empty it (needs write access) */
#define FS_APPEND 32   /* every write goes to the end of the file */

#define FS_SEEK_SET 0
#define FS_SEEK_CUR 1
#define FS_SEEK_END 2

typedef struct {
    int64_t size;
    uint32_t mtime;   /* seconds since 1970, or 0 if the backend has no clock */
    uint8_t is_dir;
} fs_stat_t;

typedef struct {
    char name[FS_NAME_MAX + 1];
    int64_t size;
    uint8_t is_dir;
} fs_dirent_t;

/* What a backend provides. Paths arrive normalised and relative to the mount: they start with '/', "/" is the mount's root, there
 * are no empty, "." or ".." parts and every name is valid. The backend reports existence and type errors itself. Handles are
 * the backend's own pointers. A backend does not enforce FS_APPEND, the busy rules or the mount namespace: the core does.
 * Everything but sync and space is required. */
typedef struct {
    int (*open)(void *ctx, const char *path, int flags, void **h);   /* flags: access mode, FS_CREAT, FS_EXCL, FS_TRUNC */
    int (*close)(void *h);
    int (*read)(void *h, void *buf, size_t n);                       /* 0 at the end of the file */
    int (*write)(void *h, const void *buf, size_t n);                /* may write less than n when full; FS_ENOSPC if none */
    int64_t (*seek)(void *h, int64_t off, int whence);               /* returns the new position; outside 0..size is FS_EINVAL */
    int (*sync)(void *h);
    int (*stat)(void *ctx, const char *path, fs_stat_t *st);
    int (*unlink)(void *ctx, const char *path);                      /* files only: FS_EISDIR for a directory */
    int (*mkdir)(void *ctx, const char *path);
    int (*rmdir)(void *ctx, const char *path);                       /* FS_ENOTEMPTY, FS_ENOTDIR, FS_EBUSY for the root */
    int (*rename)(void *ctx, const char *from, const char *to);      /* FS_EEXIST if `to` exists */
    int (*opendir)(void *ctx, const char *path, void **h);
    int (*readdir)(void *h, fs_dirent_t *e);                         /* 1 for an entry, 0 at the end; never "." or ".." */
    int (*closedir)(void *h);
    int (*space)(void *ctx, uint64_t *free_bytes, uint64_t *total_bytes);
} fs_ops_t;

/* Mounts. `name` is one valid name, with or without the leading slash ("sd" or "/sd"). */
int fs_mount(const char *name, const fs_ops_t *ops, void *ctx);
int fs_unmount(const char *name);   /* FS_EBUSY while anything on it is open */
void fs_reset(void);                /* close everything, unmount everything, current directory back to "/" */

int fs_open(const char *path, int flags);   /* the handle (>= 0) */
int fs_close(int fd);
int fs_read(int fd, void *buf, size_t n);   /* the count read; 0 at the end of the file */
int fs_write(int fd, const void *buf, size_t n);
int64_t fs_seek(int fd, int64_t off, int whence);
int fs_sync(int fd);

int fs_stat(const char *path, fs_stat_t *st);
int fs_mkdir(const char *path);
int fs_rmdir(const char *path);
int fs_unlink(const char *path);
int fs_rename(const char *from, const char *to);
/* The path only picks the mount. */
int fs_space(const char *path, uint64_t *free_bytes, uint64_t *total_bytes);

int fs_opendir(const char *path);   /* a directory handle (>= 0), separate from file handles */
int fs_readdir(int dd, fs_dirent_t *e);   /* 1 for an entry, 0 at the end */
int fs_closedir(int dd);

int fs_chdir(const char *path);
const char *fs_getcwd(void);

const char *fs_strerror(int code);

/* ---- backends ---- */

/* A RAM disk in a block of memory the caller provides. Names up to FS_RAM_NAME_MAX bytes (longer ones give FS_ENAMETOOLONG),
 * matched without regard to case; the disk is made of FS_RAM_BLOCK-byte blocks and fixed-size directory entries, so it never
 * fragments. No clock: mtime is 0. */
#define FS_RAM_BLOCK 256
#define FS_RAM_NAME_MAX 47
#define FS_RAM_HANDLES (FS_MAX_FILES + FS_MAX_DIRS)

typedef struct {
    char name[FS_RAM_NAME_MAX + 1];
    uint32_t size;
    uint16_t parent, first;
    uint8_t used, dir;
} fs_ram_inode_t;

typedef struct fs_ram fs_ram_t;

typedef struct {
    fs_ram_t *disk;
    uint32_t pos, ord;
    uint16_t ino, cur;   /* cur: the block number `ord` of the file, remembered to make sequential access fast */
    uint8_t used, dir, mode;
} fs_ram_handle_t;

struct fs_ram {
    fs_ram_inode_t *inode;
    uint8_t *block;
    uint16_t *next;
    unsigned n_inodes, n_blocks, hint;
    fs_ram_handle_t handle[FS_RAM_HANDLES];
};

int fs_ram_init(fs_ram_t *r, void *pool, size_t pool_size);   /* FS_ENOMEM if the pool is too small to hold anything */
extern const fs_ops_t fs_ram_ops;

/* A directory of the host's file system (desktop builds and tests). Names outside the root cannot be reached by ".." because the
 * core resolves that in the path text, but symbolic links inside the root are followed. */
#define FS_POSIX_ROOT_MAX 511

typedef struct {
    char root[FS_POSIX_ROOT_MAX + 1];
} fs_posix_t;

int fs_posix_init(fs_posix_t *p, const char *root_dir);   /* FS_ENOENT / FS_ENOTDIR if root_dir is not a directory */
extern const fs_ops_t fs_posix_ops;

#endif
