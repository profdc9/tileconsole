/* SPDX-License-Identifier: MIT */
/*
 * A zip archive as a read-only backend (see tilefs.h). The archive is a file of another mount, read through tilefs with one handle that
 * every file opened in it shares (each read seeks first). Its central directory is read into memory the caller gives (fs_zip_load) and
 * looked up there; only entries stored without compression can be read. Folders are what the entries' paths say they are: an archive
 * need not have an entry for a folder.
 */
#include <string.h>

#include "tilefs.h"

#define SIG_END 0x06054b50u     /* end of central directory record */
#define SIG_CENTRAL 0x02014b50u /* central directory file header */
#define SIG_LOCAL 0x04034b50u   /* local file header */
#define END_SIZE 22
#define CENTRAL_SIZE 46
#define LOCAL_SIZE 30
#define COMMENT_MAX 0xFFFFu

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int fold(int c) { return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c; }

static int same_fold(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (fold((unsigned char)a[i]) != fold((unsigned char)b[i]))
            return 0;
    return 1;
}

/* Exactly n bytes of the archive from `at`; FS_EIO if it ends first. */
static int read_at(fs_zip_t *z, uint32_t at, void *buf, size_t n)
{
    const int64_t s = fs_seek(z->fd, at, FS_SEEK_SET);
    if (s < 0)
        return s == FS_EINVAL ? FS_EIO : (int)s;
    size_t got = 0;
    while (got < n) {
        const int r = fs_read(z->fd, (uint8_t *)buf + got, n - got);
        if (r < 0)
            return r;
        if (r == 0)
            return FS_EIO;
        got += (size_t)r;
    }
    return FS_OK;
}

/* Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm), for the entries' MS-DOS times. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static uint32_t dos_time(uint16_t date, uint16_t time)
{
    const unsigned y = 1980u + (date >> 9), m = (date >> 5) & 15, d = date & 31;
    if (m < 1 || m > 12 || d < 1)
        return 0;
    const int64_t t = days_from_civil(y, m, d) * 86400 + (int64_t)(time >> 11) * 3600 + (int64_t)((time >> 5) & 63) * 60 + (int64_t)(time & 31) * 2;
    return t > 0xFFFFFFFFll ? 0xFFFFFFFFu : (uint32_t)t;
}

/* ---- opening ---- */

int fs_zip_open(fs_zip_t *z, const char *archive)
{
    memset(z, 0, sizeof *z);
    z->fd = -1;
    fs_stat_t st;
    int e = fs_stat(archive, &st);
    if (e < 0)
        return e;
    if (st.is_dir)
        return FS_EISDIR;
    if (st.size < END_SIZE)
        return FS_ENODEV;
    if (st.size > 0xFFFFFFFFll)
        return FS_ENOTSUP;   /* (ZIP64) */
    const int fd = fs_open(archive, FS_RDONLY);
    if (fd < 0)
        return fd;
    z->fd = fd;
    z->size = (uint32_t)st.size;

    /* The end record is the last thing in the archive but for its comment (up to 64 KB): look for it from the end, a window at a time. */
    uint8_t buf[256 + END_SIZE];
    const uint32_t lowest = z->size - END_SIZE > COMMENT_MAX ? z->size - END_SIZE - COMMENT_MAX : 0;
    uint32_t hi = z->size - END_SIZE, end = 0;
    int found = 0;
    for (;;) {
        const uint32_t we = hi + END_SIZE, ws = we - lowest > sizeof buf ? we - (uint32_t)sizeof buf : lowest;
        e = read_at(z, ws, buf, we - ws);
        if (e)
            goto fail;
        for (uint32_t p = hi + 1; p-- > ws;) {
            const uint8_t *r = buf + (p - ws);
            if (u32(r) == SIG_END && (uint32_t)u16(r + 20) == z->size - p - END_SIZE) {
                end = p;
                found = 1;
                break;
            }
        }
        if (found || ws == lowest)
            break;
        hi = ws - 1;
    }
    e = FS_ENODEV;
    if (!found)
        goto fail;
    e = read_at(z, end, buf, END_SIZE);
    if (e)
        goto fail;
    const uint16_t disk = u16(buf + 4), cd_disk = u16(buf + 6), here = u16(buf + 8), total = u16(buf + 10);
    const uint32_t cd_size = u32(buf + 12), cd_offset = u32(buf + 16);
    e = FS_ENOTSUP;
    if (total == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_offset == 0xFFFFFFFFu)
        goto fail;   /* (ZIP64) */
    e = FS_ENODEV;
    if (disk != 0 || cd_disk != 0 || here != total || cd_size > end || cd_offset > end - cd_size)
        goto fail;   /* (an archive in several parts, or one whose numbers do not fit) */
    z->cd_start = end - cd_size;              /* where the central directory is ... */
    z->bias = z->cd_start - cd_offset;        /* ... and how far that is from where the archive says: what is in front of it (a stub) */
    z->index_bytes = cd_size;
    z->entries = total;
    return FS_OK;
fail:
    fs_close(z->fd);
    z->fd = -1;
    return e;
}

int fs_zip_load(fs_zip_t *z, void *index)
{
    if (z->fd < 0 || (!index && z->index_bytes))
        return FS_EINVAL;
    uint8_t *ix = index;
    if (z->index_bytes) {
        const int e = read_at(z, z->cd_start, ix, z->index_bytes);
        if (e)
            return e;
    }
    uint32_t at = 0;
    for (uint32_t i = 0; i < z->entries; i++) {
        if (z->index_bytes - at < CENTRAL_SIZE || u32(ix + at) != SIG_CENTRAL)
            return FS_ENODEV;
        uint8_t *c = ix + at;
        const uint32_t nlen = u16(c + 28), len = CENTRAL_SIZE + nlen + u16(c + 30) + u16(c + 32);
        if (z->index_bytes - at < len)
            return FS_ENODEV;
        for (uint32_t k = 0; k < nlen; k++)   /* (an archive made on Windows may separate its folders with backslashes) */
            if (c[CENTRAL_SIZE + k] == '\\')
                c[CENTRAL_SIZE + k] = '/';
        at += len;
    }
    z->index = ix;
    z->index_used = at;
    return FS_OK;
}

void fs_zip_close(fs_zip_t *z)
{
    if (z->fd >= 0)
        fs_close(z->fd);
    z->fd = -1;
    z->index = NULL;
}

/* ---- entries ---- */

typedef struct {
    const uint8_t *c;    /* the central directory header */
    const char *name;
    uint32_t nlen, len;  /* the name's length; the header's, with its name, extra field and comment */
} entry_t;

static void entry_at(const fs_zip_t *z, uint32_t at, entry_t *en)
{
    en->c = z->index + at;
    en->name = (const char *)en->c + CENTRAL_SIZE;
    en->nlen = u16(en->c + 28);
    en->len = CENTRAL_SIZE + en->nlen + u16(en->c + 30) + u16(en->c + 32);
}

/* A name tilefs can reach: parts that are valid names (none empty, "." or ".."), and at most a slash at the end (a folder's entry). */
static int entry_ok(const char *n, uint32_t nl)
{
    if (nl == 0 || n[0] == '/')
        return 0;
    uint32_t i = 0;
    while (i < nl) {
        uint32_t j = i;
        while (j < nl && n[j] != '/')
            j++;
        const uint32_t cl = j - i;
        if (cl == 0 || cl > FS_NAME_MAX || (cl == 1 && n[i] == '.') || (cl == 2 && n[i] == '.' && n[i + 1] == '.'))
            return 0;
        for (uint32_t k = i; k < j; k++) {
            const unsigned char ch = (unsigned char)n[k];
            if (ch < 0x20 || ch == 0x7F || strchr("\"*:<>?\\|", ch))
                return 0;
        }
        if (n[j - 1] == '.' || n[j - 1] == ' ')
            return 0;
        if (j == nl)
            break;
        i = j + 1;
    }
    return 1;
}

/* What the entry is to the folder or file p (pl bytes, no slash at either end; 0 bytes for the root): 1 the file itself, 2 something in
 * the folder p (or the folder's own entry), 0 neither. */
static int relation(const entry_t *en, const char *p, uint32_t pl)
{
    if (pl == 0)
        return 2;
    if (en->nlen < pl || !same_fold(en->name, p, pl))
        return 0;
    if (en->nlen == pl)
        return 1;
    return en->name[pl] == '/' ? 2 : 0;
}

/* The path as the core gives it ("/" or "/a/b") without its leading slash. */
static const char *inside(const char *path, uint32_t *len)
{
    const char *p = path[0] == '/' ? path + 1 : path;
    *len = (uint32_t)strlen(p);
    return p;
}

/* Looks the path up: FS_OK with *file the file's entry (and is_dir 0), or with is_dir 1 for a folder (*file its own entry if it has one,
 * else NULL); FS_ENOENT if it is neither. A file of that name wins over a folder (in an archive that has both). */
static int lookup(const fs_zip_t *z, const char *path, entry_t *file, int *is_dir)
{
    uint32_t pl;
    const char *p = inside(path, &pl);
    *is_dir = 0;
    int dir = pl == 0, have_own = 0;
    entry_t own = {0};
    for (uint32_t at = 0; at < z->index_used;) {
        entry_t en;
        entry_at(z, at, &en);
        at += en.len;
        if (!entry_ok(en.name, en.nlen))
            continue;
        const int r = relation(&en, p, pl);
        if (r == 1) {
            *file = en;
            return FS_OK;
        }
        if (r == 2) {
            dir = 1;
            if (!have_own && en.nlen == pl + 1) {   /* "p/": the folder's own entry, which has its time */
                own = en;
                have_own = 1;
            }
        }
    }
    if (!dir)
        return FS_ENOENT;
    *is_dir = 1;
    *file = own;   /* (c is NULL if there is no entry of its own) */
    return FS_OK;
}

/* ---- the backend ---- */

static int zip_stat(void *ctx, const char *path, fs_stat_t *st)
{
    const fs_zip_t *z = ctx;
    entry_t en;
    int is_dir;
    const int e = lookup(z, path, &en, &is_dir);
    if (e)
        return e;
    memset(st, 0, sizeof *st);
    st->is_dir = (uint8_t)is_dir;
    if (en.c) {
        st->mtime = dos_time(u16(en.c + 14), u16(en.c + 12));
        if (!is_dir)
            st->size = u32(en.c + 24);
    }
    return FS_OK;
}

static int zip_open(void *ctx, const char *path, int flags, void **h)
{
    fs_zip_t *z = ctx;
    entry_t en;
    int is_dir;
    const int found = lookup(z, path, &en, &is_dir);
    if (found == FS_ENOENT)
        return flags & FS_CREAT ? FS_EROFS : FS_ENOENT;
    if (found)
        return found;
    if (is_dir)
        return FS_EISDIR;
    if ((flags & FS_CREAT) && (flags & FS_EXCL))
        return FS_EEXIST;
    if (flags & FS_WRONLY)
        return FS_EROFS;
    const uint16_t gp = u16(en.c + 8), method = u16(en.c + 10);
    const uint32_t csize = u32(en.c + 20), size = u32(en.c + 24);
    if (method != 0 || (gp & 1) || size == 0xFFFFFFFFu)
        return FS_ENOTSUP;   /* compressed, encrypted, or ZIP64 */
    if (csize != size)
        return FS_EIO;
    uint8_t lh[LOCAL_SIZE];
    const uint64_t local = (uint64_t)z->bias + u32(en.c + 42);
    if (local + LOCAL_SIZE > z->size)
        return FS_EIO;
    int e = read_at(z, (uint32_t)local, lh, sizeof lh);
    if (e)
        return e;
    const uint64_t start = local + LOCAL_SIZE + u16(lh + 26) + u16(lh + 28);
    if (u32(lh) != SIG_LOCAL || start + size > z->size)
        return FS_EIO;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        fs_zip_file_t *f = &z->file[i];
        if (!f->used) {
            f->used = 1;
            f->zip = z;
            f->start = (uint32_t)start;
            f->size = size;
            f->pos = 0;
            *h = f;
            return FS_OK;
        }
    }
    return FS_EMFILE;
}

static int zip_close(void *h)
{
    ((fs_zip_file_t *)h)->used = 0;
    return FS_OK;
}

static int zip_read(void *h, void *buf, size_t n)
{
    fs_zip_file_t *f = h;
    const uint32_t left = f->size - f->pos;
    if (n > left)
        n = left;
    if (n == 0)
        return 0;
    const int e = read_at(f->zip, f->start + f->pos, buf, n);
    if (e)
        return e;
    f->pos += (uint32_t)n;
    return (int)n;
}

static int zip_write(void *h, const void *buf, size_t n)
{
    (void)h;
    (void)buf;
    (void)n;
    return FS_EBADF;   /* (no file here is open for writing) */
}

static int64_t zip_seek(void *h, int64_t off, int whence)
{
    fs_zip_file_t *f = h;
    const int64_t base = whence == FS_SEEK_SET ? 0 : whence == FS_SEEK_CUR ? (int64_t)f->pos : (int64_t)f->size;
    if (off < -base || off > (int64_t)f->size - base)
        return FS_EINVAL;
    f->pos = (uint32_t)(base + off);
    return f->pos;
}

static int zip_unlink(void *ctx, const char *path)
{
    (void)ctx;
    (void)path;
    return FS_EROFS;
}

static int zip_rename(void *ctx, const char *from, const char *to)
{
    (void)ctx;
    (void)from;
    (void)to;
    return FS_EROFS;
}

static int zip_opendir(void *ctx, const char *path, void **h)
{
    fs_zip_t *z = ctx;
    entry_t en;
    int is_dir;
    const int e = lookup(z, path, &en, &is_dir);
    if (e)
        return e;
    if (!is_dir)
        return FS_ENOTDIR;
    uint32_t pl;
    const char *p = inside(path, &pl);
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        fs_zip_dir_t *d = &z->dir[i];
        if (!d->used) {
            d->used = 1;
            d->zip = z;
            d->next = 0;
            d->plen = pl;
            memcpy(d->prefix, p, pl);
            *h = d;
            return FS_OK;
        }
    }
    return FS_EMFILE;
}

/* The part of the entry's name just below the folder, and whether more follows it (it is a folder). 0 bytes: the folder's own entry. */
static uint32_t child_of(const entry_t *en, uint32_t pl, const char **child, int *is_dir)
{
    const uint32_t from = pl ? pl + 1 : 0;
    *child = en->name + from;
    uint32_t cl = 0;
    while (from + cl < en->nlen && (*child)[cl] != '/')
        cl++;
    *is_dir = from + cl < en->nlen;
    return cl;
}

static int zip_readdir(void *h, fs_dirent_t *e)
{
    fs_zip_dir_t *d = h;
    const fs_zip_t *z = d->zip;
    while (d->next < z->index_used) {
        const uint32_t at = d->next;
        entry_t en;
        entry_at(z, at, &en);
        d->next = at + en.len;
        if (!entry_ok(en.name, en.nlen) || relation(&en, d->prefix, d->plen) != 2)
            continue;
        const char *child;
        int is_dir;
        const uint32_t cl = child_of(&en, d->plen, &child, &is_dir);
        if (cl == 0)
            continue;
        int seen = 0;   /* listed already (the folder of an earlier entry, or the same name twice) */
        for (uint32_t b = 0; b < at && !seen;) {
            entry_t eb;
            entry_at(z, b, &eb);
            b += eb.len;
            const char *cb;
            int db;
            seen = entry_ok(eb.name, eb.nlen) && relation(&eb, d->prefix, d->plen) == 2 && child_of(&eb, d->plen, &cb, &db) == cl &&
                   same_fold(cb, child, cl);
        }
        if (seen)
            continue;
        memcpy(e->name, child, cl);
        e->name[cl] = '\0';
        e->is_dir = (uint8_t)is_dir;
        e->size = is_dir ? 0 : u32(en.c + 24);
        return 1;
    }
    return 0;
}

static int zip_closedir(void *h)
{
    ((fs_zip_dir_t *)h)->used = 0;
    return FS_OK;
}

static int zip_space(void *ctx, uint64_t *free_bytes, uint64_t *total_bytes)
{
    *free_bytes = 0;
    *total_bytes = ((const fs_zip_t *)ctx)->size;
    return FS_OK;
}

const fs_ops_t fs_zip_ops = {
    .open = zip_open,
    .close = zip_close,
    .read = zip_read,
    .write = zip_write,
    .seek = zip_seek,
    .stat = zip_stat,
    .unlink = zip_unlink,
    .mkdir = zip_unlink,
    .rmdir = zip_unlink,
    .rename = zip_rename,
    .opendir = zip_opendir,
    .readdir = zip_readdir,
    .closedir = zip_closedir,
    .space = zip_space,
};
