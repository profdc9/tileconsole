/* SPDX-License-Identifier: MIT */
/* A RAM disk for tilefs: directory entries in a fixed table, file data in FS_RAM_BLOCK-byte blocks chained through a next table. */
#include <string.h>

#include "tilefs.h"

#define NONE 0xFFFFu    /* no inode, or no block */
#define END 0xFFFFu     /* in next[]: the last block of a file */
#define FREE 0xFFFEu    /* in next[]: not part of any file */
#define MAX_BLOCKS 0xFFFDu
#define MODE_READ 1
#define MODE_WRITE 2

int fs_ram_init(fs_ram_t *r, void *pool, size_t pool_size)
{
    const uintptr_t at = ((uintptr_t)pool + 7u) & ~(uintptr_t)7u;
    const size_t lost = (size_t)(at - (uintptr_t)pool);
    if (!pool || pool_size < lost)
        return FS_ENOMEM;
    pool_size -= lost;
    size_t n_inodes = pool_size / 1024;
    if (n_inodes < 8)
        n_inodes = 8;
    if (n_inodes > 4096)
        n_inodes = 4096;
    const size_t inode_bytes = (n_inodes * sizeof(fs_ram_inode_t) + 7u) & ~(size_t)7u;
    if (pool_size < inode_bytes + 8 + FS_RAM_BLOCK + sizeof(uint16_t))
        return FS_ENOMEM;
    size_t n_blocks = (pool_size - inode_bytes - 8) / (FS_RAM_BLOCK + sizeof(uint16_t));   /* 8: room to align the blocks */
    if (n_blocks > MAX_BLOCKS)
        n_blocks = MAX_BLOCKS;
    memset(r, 0, sizeof *r);
    r->inode = (fs_ram_inode_t *)at;
    r->next = (uint16_t *)(at + inode_bytes);
    r->block = (uint8_t *)(((uintptr_t)(r->next + n_blocks) + 7u) & ~(uintptr_t)7u);
    r->n_inodes = (unsigned)n_inodes;
    r->n_blocks = (unsigned)n_blocks;
    memset(r->inode, 0, inode_bytes);
    for (unsigned i = 0; i < r->n_blocks; i++)
        r->next[i] = FREE;
    r->inode[0].used = 1;   /* the root directory */
    r->inode[0].dir = 1;
    r->inode[0].parent = NONE;
    r->inode[0].first = NONE;
    return FS_OK;
}

static unsigned find_free_block(fs_ram_t *r)
{
    for (unsigned i = 0; i < r->n_blocks; i++) {
        const unsigned b = (r->hint + i) % r->n_blocks;
        if (r->next[b] == FREE) {
            r->hint = b + 1;
            return b;
        }
    }
    return NONE;
}

static void free_chain(fs_ram_t *r, unsigned b)
{
    while (b != NONE) {
        const unsigned n = r->next[b];
        r->next[b] = FREE;
        b = n == END ? NONE : n;
    }
}

static int find_inode_slot(fs_ram_t *r)
{
    for (unsigned i = 1; i < r->n_inodes; i++)
        if (!r->inode[i].used)
            return (int)i;
    return -1;
}

static int find_child(const fs_ram_t *r, unsigned dir, const char *name, size_t n)
{
    for (unsigned i = 1; i < r->n_inodes; i++) {
        const fs_ram_inode_t *c = &r->inode[i];
        if (!c->used || c->parent != dir || strlen(c->name) != n)
            continue;
        size_t k = 0;
        while (k < n) {
            int a = (unsigned char)c->name[k], b = (unsigned char)name[k];
            a += a >= 'A' && a <= 'Z' ? 32 : 0;
            b += b >= 'A' && b <= 'Z' ? 32 : 0;
            if (a != b)
                break;
            k++;
        }
        if (k == n)
            return (int)i;
    }
    return -1;
}

/* Walks a path. On success *parent is the directory holding the last part, *leaf its name and *ino its inode (NONE if it does not
 * exist). For "/" there is no parent and no leaf and *ino is the root. */
static int walk(fs_ram_t *r, const char *path, unsigned *parent, const char **leaf, size_t *leaflen, unsigned *ino)
{
    if (path[1] == '\0') {
        *parent = NONE;
        *leaf = NULL;
        *leaflen = 0;
        *ino = 0;
        return FS_OK;
    }
    unsigned cur = 0;
    const char *p = path + 1;
    for (;;) {
        const char *e = p;
        while (*e && *e != '/')
            e++;
        const size_t n = (size_t)(e - p);
        if (n > FS_RAM_NAME_MAX)
            return FS_ENAMETOOLONG;
        const int c = find_child(r, cur, p, n);
        if (!*e) {
            *parent = cur;
            *leaf = p;
            *leaflen = n;
            *ino = c < 0 ? NONE : (unsigned)c;
            return FS_OK;
        }
        if (c < 0)
            return FS_ENOENT;
        if (!r->inode[c].dir)
            return FS_ENOTDIR;
        cur = (unsigned)c;
        p = e + 1;
    }
}

static fs_ram_handle_t *alloc_handle(fs_ram_t *r)
{
    for (int i = 0; i < FS_RAM_HANDLES; i++)
        if (!r->handle[i].used)
            return &r->handle[i];
    return NULL;
}

/* The block with number `ord` of a file (the chain must be at least that long), starting from the one the handle remembers. */
static unsigned block_at(fs_ram_handle_t *h, uint32_t ord)
{
    fs_ram_t *r = h->disk;
    unsigned b = r->inode[h->ino].first;
    uint32_t at = 0;
    if (h->cur != NONE && h->ord <= ord) {
        b = h->cur;
        at = h->ord;
    }
    while (at < ord) {
        b = r->next[b];
        at++;
    }
    h->cur = (uint16_t)b;
    h->ord = ord;
    return b;
}

static int ram_open(void *ctx, const char *path, int flags, void **out)
{
    fs_ram_t *r = ctx;
    unsigned parent, ino;
    const char *leaf;
    size_t n;
    int e = walk(r, path, &parent, &leaf, &n, &ino);
    if (e)
        return e;
    fs_ram_handle_t *h = alloc_handle(r);
    if (!h)
        return FS_EMFILE;
    if (ino != NONE) {
        if ((flags & FS_CREAT) && (flags & FS_EXCL))
            return FS_EEXIST;   /* before the directory test, as the host does */
        if (r->inode[ino].dir)
            return FS_EISDIR;
        if (flags & FS_TRUNC) {
            free_chain(r, r->inode[ino].first);
            r->inode[ino].first = NONE;
            r->inode[ino].size = 0;
        }
    } else {
        if (!(flags & FS_CREAT))
            return FS_ENOENT;
        const int slot = find_inode_slot(r);
        if (slot < 0)
            return FS_ENOSPC;
        fs_ram_inode_t *i = &r->inode[slot];
        memset(i, 0, sizeof *i);
        memcpy(i->name, leaf, n);
        i->parent = (uint16_t)parent;
        i->first = NONE;
        i->used = 1;
        ino = (unsigned)slot;
    }
    memset(h, 0, sizeof *h);
    h->used = 1;
    h->disk = r;
    h->ino = (uint16_t)ino;
    h->cur = NONE;
    h->mode = (uint8_t)(((flags & FS_RDONLY) ? MODE_READ : 0) | ((flags & FS_WRONLY) ? MODE_WRITE : 0));
    *out = h;
    return FS_OK;
}

static int ram_close(void *hp)
{
    ((fs_ram_handle_t *)hp)->used = 0;
    return FS_OK;
}

static int ram_read(void *hp, void *buf, size_t n)
{
    fs_ram_handle_t *h = hp;
    const fs_ram_inode_t *i = &h->disk->inode[h->ino];
    if (h->pos >= i->size)
        return 0;
    if (n > i->size - h->pos)
        n = i->size - h->pos;
    size_t done = 0;
    while (done < n) {
        const unsigned b = block_at(h, h->pos / FS_RAM_BLOCK);
        const size_t off = h->pos % FS_RAM_BLOCK;
        size_t k = FS_RAM_BLOCK - off;
        if (k > n - done)
            k = n - done;
        memcpy((uint8_t *)buf + done, h->disk->block + (size_t)b * FS_RAM_BLOCK + off, k);
        h->pos += (uint32_t)k;
        done += k;
    }
    return (int)done;
}

static int ram_write(void *hp, const void *buf, size_t n)
{
    fs_ram_handle_t *h = hp;
    fs_ram_t *r = h->disk;
    fs_ram_inode_t *i = &r->inode[h->ino];
    size_t done = 0;
    while (done < n) {
        const uint32_t ord = h->pos / FS_RAM_BLOCK;
        const uint32_t have = (i->size + FS_RAM_BLOCK - 1) / FS_RAM_BLOCK;
        if (ord >= have) {   /* at the end of the last block: add one (pos never passes the size) */
            const unsigned nb = find_free_block(r);
            if (nb == NONE)
                break;
            r->next[nb] = END;
            if (have == 0)
                i->first = (uint16_t)nb;
            else
                r->next[block_at(h, have - 1)] = (uint16_t)nb;
        }
        const unsigned b = block_at(h, ord);
        const size_t off = h->pos % FS_RAM_BLOCK;
        size_t k = FS_RAM_BLOCK - off;
        if (k > n - done)
            k = n - done;
        memcpy(r->block + (size_t)b * FS_RAM_BLOCK + off, (const uint8_t *)buf + done, k);
        h->pos += (uint32_t)k;
        done += k;
        if (h->pos > i->size)
            i->size = h->pos;
    }
    return done == 0 && n > 0 ? FS_ENOSPC : (int)done;
}

static int64_t ram_seek(void *hp, int64_t off, int whence)
{
    fs_ram_handle_t *h = hp;
    const int64_t size = h->disk->inode[h->ino].size;
    const int64_t base = whence == FS_SEEK_SET ? 0 : whence == FS_SEEK_CUR ? (int64_t)h->pos : size;
    const int64_t target = base + off;
    if (off > size + 1 || off < -size - 1 || target < 0 || target > size)   /* the first tests keep the sum from overflowing */
        return FS_EINVAL;
    h->pos = (uint32_t)target;
    return target;
}

static int ram_stat(void *ctx, const char *path, fs_stat_t *st)
{
    fs_ram_t *r = ctx;
    unsigned parent, ino;
    const char *leaf;
    size_t n;
    const int e = walk(r, path, &parent, &leaf, &n, &ino);
    if (e)
        return e;
    if (ino == NONE)
        return FS_ENOENT;
    memset(st, 0, sizeof *st);
    st->is_dir = r->inode[ino].dir;
    st->size = st->is_dir ? 0 : r->inode[ino].size;
    return FS_OK;
}

static int ram_unlink(void *ctx, const char *path)
{
    fs_ram_t *r = ctx;
    unsigned parent, ino;
    const char *leaf;
    size_t n;
    const int e = walk(r, path, &parent, &leaf, &n, &ino);
    if (e)
        return e;
    if (ino == NONE)
        return FS_ENOENT;
    if (r->inode[ino].dir)
        return FS_EISDIR;
    free_chain(r, r->inode[ino].first);
    r->inode[ino].used = 0;
    return FS_OK;
}

static int ram_mkdir(void *ctx, const char *path)
{
    fs_ram_t *r = ctx;
    unsigned parent, ino;
    const char *leaf;
    size_t n;
    const int e = walk(r, path, &parent, &leaf, &n, &ino);
    if (e)
        return e;
    if (ino != NONE)
        return FS_EEXIST;
    const int slot = find_inode_slot(r);
    if (slot < 0)
        return FS_ENOSPC;
    fs_ram_inode_t *i = &r->inode[slot];
    memset(i, 0, sizeof *i);
    memcpy(i->name, leaf, n);
    i->parent = (uint16_t)parent;
    i->first = NONE;
    i->dir = 1;
    i->used = 1;
    return FS_OK;
}

static int ram_rmdir(void *ctx, const char *path)
{
    fs_ram_t *r = ctx;
    unsigned parent, ino;
    const char *leaf;
    size_t n;
    const int e = walk(r, path, &parent, &leaf, &n, &ino);
    if (e)
        return e;
    if (ino == 0)
        return FS_EBUSY;
    if (ino == NONE)
        return FS_ENOENT;
    if (!r->inode[ino].dir)
        return FS_ENOTDIR;
    for (unsigned i = 1; i < r->n_inodes; i++)
        if (r->inode[i].used && r->inode[i].parent == ino)
            return FS_ENOTEMPTY;
    r->inode[ino].used = 0;
    return FS_OK;
}

static int ram_rename(void *ctx, const char *from, const char *to)
{
    fs_ram_t *r = ctx;
    unsigned fp, fi, tp, ti;
    const char *fl, *tl;
    size_t fn, tn;
    int e = walk(r, from, &fp, &fl, &fn, &fi);
    if (e)
        return e;
    if (fi == NONE)
        return FS_ENOENT;
    if (fi == 0)
        return FS_EBUSY;
    e = walk(r, to, &tp, &tl, &tn, &ti);
    if (e)
        return e;
    if (ti != NONE)
        return FS_EEXIST;
    fs_ram_inode_t *i = &r->inode[fi];
    memset(i->name, 0, sizeof i->name);
    memcpy(i->name, tl, tn);
    i->parent = (uint16_t)tp;
    return FS_OK;
}

static int ram_opendir(void *ctx, const char *path, void **out)
{
    fs_ram_t *r = ctx;
    unsigned parent, ino;
    const char *leaf;
    size_t n;
    int e = walk(r, path, &parent, &leaf, &n, &ino);
    if (e)
        return e;
    if (ino == NONE)
        return FS_ENOENT;
    if (!r->inode[ino].dir)
        return FS_ENOTDIR;
    fs_ram_handle_t *h = alloc_handle(r);
    if (!h)
        return FS_EMFILE;
    memset(h, 0, sizeof *h);
    h->used = 1;
    h->dir = 1;
    h->disk = r;
    h->ino = (uint16_t)ino;
    h->pos = 1;   /* the next inode to look at */
    *out = h;
    return FS_OK;
}

static int ram_readdir(void *hp, fs_dirent_t *e)
{
    fs_ram_handle_t *h = hp;
    const fs_ram_t *r = h->disk;
    while (h->pos < r->n_inodes) {
        const fs_ram_inode_t *i = &r->inode[h->pos++];
        if (i->used && i->parent == h->ino) {
            memset(e, 0, sizeof *e);
            strcpy(e->name, i->name);
            e->is_dir = i->dir;
            e->size = i->dir ? 0 : i->size;
            return 1;
        }
    }
    return 0;
}

static int ram_space(void *ctx, uint64_t *free_bytes, uint64_t *total_bytes)
{
    const fs_ram_t *r = ctx;
    unsigned n = 0;
    for (unsigned i = 0; i < r->n_blocks; i++)
        n += r->next[i] == FREE;
    *free_bytes = (uint64_t)n * FS_RAM_BLOCK;
    *total_bytes = (uint64_t)r->n_blocks * FS_RAM_BLOCK;
    return FS_OK;
}

const fs_ops_t fs_ram_ops = {
    .open = ram_open, .close = ram_close, .read = ram_read, .write = ram_write, .seek = ram_seek, .sync = NULL, .stat = ram_stat,
    .unlink = ram_unlink, .mkdir = ram_mkdir, .rmdir = ram_rmdir, .rename = ram_rename, .opendir = ram_opendir, .readdir = ram_readdir,
    .closedir = ram_close, .space = ram_space,
};
