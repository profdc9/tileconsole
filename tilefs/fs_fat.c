/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "fs_fat.h"

int fs_fat_attach(unsigned pdrv, const fs_blockdev_t *dev, void *ctx);
void fs_fat_detach(unsigned pdrv);
int fs_fat_init_error(unsigned pdrv);

/* ---- time ---- */

static uint32_t (*clock_hook)(void);

void fs_fat_set_clock(uint32_t (*unix_time)(void))
{
    clock_hook = unix_time;
}

/* Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm) and back. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (*m <= 2));
}

uint32_t fs_fat_from_unix(uint32_t t)
{
    int y;
    unsigned m, d;
    civil_from_days(t / 86400, &y, &m, &d);
    if (y < 1980)
        return (1u << 21) | (1u << 16);   /* 1980-01-01 00:00:00, the earliest FAT can say */
    const uint32_t s = t % 86400;
    return ((uint32_t)(y - 1980) << 25) | (m << 21) | (d << 16) | ((s / 3600) << 11) | (((s / 60) % 60) << 5) | ((s % 60) / 2);
}

uint32_t fs_fat_to_unix(uint16_t fdate, uint16_t ftime)
{
    const unsigned y = 1980u + (fdate >> 9), m = (fdate >> 5) & 15, d = fdate & 31;
    if (m < 1 || m > 12 || d < 1)
        return 0;
    const int64_t days = days_from_civil(y, m, d);
    const int64_t t = days * 86400 + (int64_t)(ftime >> 11) * 3600 + (int64_t)((ftime >> 5) & 63) * 60 + (int64_t)(ftime & 31) * 2;
    return t > 0xFFFFFFFFll ? 0xFFFFFFFFu : (uint32_t)t;   /* FAT goes on to 2107, a 32-bit count of seconds stops in 2106 */
}

DWORD get_fattime(void)
{
    return clock_hook ? fs_fat_from_unix(clock_hook()) : fs_fat_from_unix(1767225600u);   /* 2026-01-01 00:00:00 */
}

/* ---- errors ---- */

static int map(FRESULT r)
{
    switch (r) {
    case FR_OK: return FS_OK;
    case FR_DISK_ERR: case FR_INT_ERR: case FR_NOT_READY: case FR_TIMEOUT: case FR_MKFS_ABORTED: return FS_EIO;
    case FR_NO_FILE: case FR_NO_PATH: return FS_ENOENT;
    case FR_INVALID_NAME: return FS_EBADNAME;
    case FR_DENIED: return FS_EPERM;
    case FR_EXIST: return FS_EEXIST;
    case FR_INVALID_OBJECT: return FS_EBADF;
    case FR_WRITE_PROTECTED: return FS_EPERM;
    case FR_NOT_ENABLED: case FR_NO_FILESYSTEM: return FS_ENODEV;
    case FR_LOCKED: return FS_EBUSY;
    case FR_NOT_ENOUGH_CORE: return FS_ENOMEM;
    case FR_TOO_MANY_OPEN_FILES: return FS_EMFILE;
    case FR_INVALID_PARAMETER: case FR_INVALID_DRIVE: return FS_EINVAL;
    default: return FS_EIO;
    }
}

/* The path as FatFs wants it: the drive number, then the path. */
static const char *fpath(fs_fat_t *f, const char *path)
{
    f->path[0] = (char)('0' + f->drive);
    f->path[1] = ':';
    strcpy(f->path + 2, path);
    return f->path;
}

/* FatFs says "no path" both for a missing directory and for a file used as one; tilefs tells them apart. Looks at each directory part
 * of `path` in turn. */
static int explain_no_path(fs_fat_t *f, const char *path)
{
    f->path2[0] = (char)('0' + f->drive);
    f->path2[1] = ':';
    for (size_t i = 1; path[i]; i++) {
        if (path[i] != '/')
            continue;
        memcpy(f->path2 + 2, path, i);
        f->path2[2 + i] = '\0';
        const FRESULT r = f_stat(f->path2, &f->info);
        if (r != FR_OK)
            return FS_ENOENT;
        if (!(f->info.fattrib & AM_DIR))
            return FS_ENOTDIR;
    }
    return FS_ENOENT;
}

static int fail(fs_fat_t *f, FRESULT r, const char *path)
{
    return r == FR_NO_PATH ? explain_no_path(f, path) : map(r);
}

static int is_root(const char *path)
{
    return path[1] == '\0';
}

/* ---- mounting ---- */

int fs_fat_format(unsigned drive, const fs_blockdev_t *dev, void *dev_ctx, int type)
{
    static BYTE work[FF_MAX_SS];
    char name[3] = {(char)('0' + drive), ':', '\0'};
    MKFS_PARM opt = {0};
    const int sfd = (type & FS_FAT_SFD) != 0;
    switch (type & ~FS_FAT_SFD) {
    case FS_FAT_ANY: opt.fmt = FM_ANY; break;
    case FS_FAT_FAT: opt.fmt = FM_FAT; break;
    case FS_FAT_FAT32: opt.fmt = FM_FAT32; break;
    case FS_FAT_EXFAT: opt.fmt = FM_EXFAT; break;
    default: return FS_EINVAL;
    }
    if (sfd)
        opt.fmt |= FM_SFD;
    int e = fs_fat_attach(drive, dev, dev_ctx);
    if (e)
        return e;
    f_unmount(name);
    const FRESULT r = f_mkfs(name, &opt, work, sizeof work);
    const int why = r == FR_NOT_READY ? fs_fat_init_error(drive) : 0;   /* the device's own reason (no card, ...) */
    f_unmount(name);
    fs_fat_detach(drive);
    return why ? why : map(r);
}

int fs_fat_init(fs_fat_t *f, unsigned drive, const fs_blockdev_t *dev, void *dev_ctx)
{
    char name[3] = {(char)('0' + drive), ':', '\0'};
    int e = fs_fat_attach(drive, dev, dev_ctx);
    if (e)
        return e;
    memset(f, 0, sizeof *f);
    f->drive = (uint8_t)drive;
    const FRESULT r = f_mount(&f->fs, name, 1);
    if (r != FR_OK) {
        const int why = r == FR_NOT_READY ? fs_fat_init_error(drive) : 0;   /* the device's own reason (no card, ...) */
        f_unmount(name);
        fs_fat_detach(drive);
        return why ? why : map(r);
    }
    f->mounted = 1;
    return FS_OK;
}

int fs_fat_done(fs_fat_t *f)
{
    char name[3] = {(char)('0' + f->drive), ':', '\0'};
    int e = FS_OK;
    if (f->mounted) {
        for (int i = 0; i < FS_MAX_FILES; i++)
            if (f->file[i].used)
                f_close(&f->file[i].f);
        e = map(f_unmount(name));
        f->mounted = 0;
    }
    fs_fat_detach(f->drive);
    return e;
}

/* ---- files ---- */

/* A file's size and time reach its directory entry only when it is synced or closed. Names are looked up in the directory, so what
 * is open for writing is synced first, to make stat and listings show what has been written. */
static int flush_writers(fs_fat_t *f)
{
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (f->file[i].used && (f->file[i].f.flag & FA_WRITE)) {
            const FRESULT r = f_sync(&f->file[i].f);
            if (r != FR_OK)
                return map(r);
        }
    }
    return FS_OK;
}

static int fat_open(void *ctx, const char *path, int flags, void **out)
{
    fs_fat_t *f = ctx;
    fs_fat_file_t *slot = NULL;
    for (int i = 0; i < FS_MAX_FILES && !slot; i++)
        if (!f->file[i].used)
            slot = &f->file[i];
    if (!slot)
        return FS_EMFILE;
    if (is_root(path))
        return (flags & FS_CREAT) && (flags & FS_EXCL) ? FS_EEXIST : FS_EISDIR;
    const char *fp = fpath(f, path);
    int exists = 0;
    const FRESULT sr = f_stat(fp, &f->info);
    if (sr == FR_OK) {
        if ((flags & FS_CREAT) && (flags & FS_EXCL))
            return FS_EEXIST;
        if (f->info.fattrib & AM_DIR)
            return FS_EISDIR;
        exists = 1;
    } else if (sr == FR_NO_FILE) {
        if (!(flags & FS_CREAT))
            return FS_ENOENT;
    } else {
        return fail(f, sr, path);
    }
    BYTE mode = (BYTE)(((flags & FS_RDONLY) ? FA_READ : 0) | ((flags & FS_WRONLY) ? FA_WRITE : 0));
    mode |= !exists ? FA_CREATE_NEW : (flags & FS_TRUNC) ? FA_CREATE_ALWAYS : FA_OPEN_EXISTING;
    const FRESULT r = f_open(&slot->f, fp, mode);
    if (r != FR_OK)
        return r == FR_DENIED && !exists ? FS_ENOSPC : fail(f, r, path);
    slot->used = 1;
    *out = slot;
    return FS_OK;
}

static int fat_close(void *h)
{
    fs_fat_file_t *s = h;
    const FRESULT r = f_close(&s->f);
    s->used = 0;
    return map(r);
}

static int fat_read(void *h, void *buf, size_t n)
{
    UINT got = 0;
    const FRESULT r = f_read(&((fs_fat_file_t *)h)->f, buf, (UINT)n, &got);
    return r == FR_OK ? (int)got : map(r);
}

static int fat_write(void *h, const void *buf, size_t n)
{
    UINT put = 0;
    const FRESULT r = f_write(&((fs_fat_file_t *)h)->f, buf, (UINT)n, &put);
    if (r != FR_OK)
        return map(r);
    return put == 0 && n > 0 ? FS_ENOSPC : (int)put;   /* FatFs reports a full disk as a short count */
}

static int64_t fat_seek(void *h, int64_t off, int whence)
{
    FIL *fil = &((fs_fat_file_t *)h)->f;
    const int64_t size = (int64_t)f_size(fil);
    const int64_t base = whence == FS_SEEK_SET ? 0 : whence == FS_SEEK_CUR ? (int64_t)f_tell(fil) : size;
    if (off > size + 1 || off < -size - 1 || base + off < 0 || base + off > size)
        return FS_EINVAL;
    const FRESULT r = f_lseek(fil, (FSIZE_t)(base + off));
    return r == FR_OK ? base + off : map(r);
}

static int fat_sync(void *h)
{
    return map(f_sync(&((fs_fat_file_t *)h)->f));
}

/* ---- names ---- */

static int fat_stat(void *ctx, const char *path, fs_stat_t *st)
{
    fs_fat_t *f = ctx;
    memset(st, 0, sizeof *st);
    if (is_root(path)) {
        st->is_dir = 1;
        return FS_OK;
    }
    const int fe = flush_writers(f);
    if (fe)
        return fe;
    const FRESULT r = f_stat(fpath(f, path), &f->info);
    if (r != FR_OK)
        return fail(f, r, path);
    st->is_dir = (f->info.fattrib & AM_DIR) != 0;
    st->size = st->is_dir ? 0 : (int64_t)f->info.fsize;
    st->mtime = fs_fat_to_unix(f->info.fdate, f->info.ftime);
    return FS_OK;
}

static int fat_unlink(void *ctx, const char *path)
{
    fs_fat_t *f = ctx;
    if (is_root(path))
        return FS_EISDIR;
    FRESULT r = f_stat(fpath(f, path), &f->info);
    if (r != FR_OK)
        return fail(f, r, path);
    if (f->info.fattrib & AM_DIR)
        return FS_EISDIR;
    r = f_unlink(f->path);
    return map(r);
}

static int fat_mkdir(void *ctx, const char *path)
{
    fs_fat_t *f = ctx;
    if (is_root(path))
        return FS_EEXIST;
    const FRESULT r = f_mkdir(fpath(f, path));
    return r == FR_DENIED ? FS_ENOSPC : fail(f, r, path);
}

static int fat_rmdir(void *ctx, const char *path)
{
    fs_fat_t *f = ctx;
    if (is_root(path))
        return FS_EBUSY;
    FRESULT r = f_stat(fpath(f, path), &f->info);
    if (r != FR_OK)
        return fail(f, r, path);
    if (!(f->info.fattrib & AM_DIR))
        return FS_ENOTDIR;
    r = f_unlink(f->path);
    return r == FR_DENIED ? FS_ENOTEMPTY : map(r);
}

static int fat_rename(void *ctx, const char *from, const char *to)
{
    fs_fat_t *f = ctx;
    if (is_root(from))
        return FS_EBUSY;
    FRESULT r = f_stat(fpath(f, from), &f->info);
    if (r != FR_OK)
        return fail(f, r, from);
    if (is_root(to))
        return FS_EEXIST;
    char old_path[FS_PATH_MAX + 8];   /* fpath uses one buffer, and rename needs two */
    strcpy(old_path, f->path);
    /* FatFs lets a name be "renamed" to itself or to another spelling of itself; tilefs says the new name exists */
    r = f_stat(fpath(f, to), &f->info);
    if (r == FR_OK)
        return FS_EEXIST;
    if (r != FR_NO_FILE)
        return fail(f, r, to);
    r = f_rename(old_path, f->path);
    return r == FR_DENIED ? FS_ENOSPC : fail(f, r, to);
}

/* ---- directories ---- */

static int fat_opendir(void *ctx, const char *path, void **out)
{
    fs_fat_t *f = ctx;
    fs_fat_dir_t *slot = NULL;
    for (int i = 0; i < FS_MAX_DIRS && !slot; i++)
        if (!f->dir[i].used)
            slot = &f->dir[i];
    if (!slot)
        return FS_EMFILE;
    const int fe = flush_writers(f);
    if (fe)
        return fe;
    if (!is_root(path)) {
        const FRESULT s = f_stat(fpath(f, path), &f->info);
        if (s != FR_OK)
            return fail(f, s, path);
        if (!(f->info.fattrib & AM_DIR))
            return FS_ENOTDIR;
    }
    const FRESULT r = f_opendir(&slot->d, fpath(f, path));
    if (r != FR_OK)
        return fail(f, r, path);
    slot->used = 1;
    *out = slot;
    return FS_OK;
}

static int fat_readdir(void *h, fs_dirent_t *e)
{
    fs_fat_dir_t *s = h;
    static FILINFO info;   /* not in the handle: the entries are copied out at once */
    const FRESULT r = f_readdir(&s->d, &info);
    if (r != FR_OK)
        return map(r);
    if (!info.fname[0])
        return 0;
    memset(e, 0, sizeof *e);
    size_t n = strlen(info.fname);
    if (n > FS_NAME_MAX)
        n = FS_NAME_MAX;
    memcpy(e->name, info.fname, n);
    e->is_dir = (info.fattrib & AM_DIR) != 0;
    e->size = e->is_dir ? 0 : (int64_t)info.fsize;
    return 1;
}

static int fat_closedir(void *h)
{
    fs_fat_dir_t *s = h;
    const FRESULT r = f_closedir(&s->d);
    s->used = 0;
    return map(r);
}

static int fat_space(void *ctx, uint64_t *free_bytes, uint64_t *total_bytes)
{
    fs_fat_t *f = ctx;
    DWORD free_clusters;
    FATFS *fsp;
    char name[3] = {(char)('0' + f->drive), ':', '\0'};
    const FRESULT r = f_getfree(name, &free_clusters, &fsp);
    if (r != FR_OK)
        return map(r);
    const uint64_t cluster = (uint64_t)fsp->csize * 512;
    *free_bytes = (uint64_t)free_clusters * cluster;
    *total_bytes = (uint64_t)(fsp->n_fatent - 2) * cluster;
    return FS_OK;
}

const fs_ops_t fs_fat_ops = {
    .open = fat_open, .close = fat_close, .read = fat_read, .write = fat_write, .seek = fat_seek, .sync = fat_sync, .stat = fat_stat,
    .unlink = fat_unlink, .mkdir = fat_mkdir, .rmdir = fat_rmdir, .rename = fat_rename, .opendir = fat_opendir, .readdir = fat_readdir,
    .closedir = fat_closedir, .space = fat_space,
};
