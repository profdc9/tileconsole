/* SPDX-License-Identifier: MIT */
/* TGF files: see tgf.h and docs/tgf-format.md. */
#include "tgf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define HEAD 12   /* TGAC, the tag, the length */

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void set16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void set32(uint8_t *p, uint32_t v) { set16(p, v); set16(p + 2, v >> 16); }

static const char *const part_tag[TGF_KINDS] = {"PALT", "TILE", "SPRT", "FLAG"};

static int item_bytes(int kind, int tile_size)
{
    return kind == TGF_PALT ? 3 : kind == TGF_FLAG ? 1 : tile_size * tile_size;
}

static int fail(char *why, size_t n, int code, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int fail(char *why, size_t n, int code, const char *fmt, ...)
{
    if (why && n) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(why, n, fmt, ap);
        va_end(ap);
    }
    return code;
}

/* Exactly n bytes from pos (the caller has checked that they are in the file). */
static int read_at(const tgf_io_t *io, uint32_t pos, void *buf, uint32_t n)
{
    int e = io->seek(io->ctx, pos);
    if (e < 0)
        return e;
    uint32_t got = 0;
    while (got < n) {
        e = io->read(io->ctx, (uint8_t *)buf + got, n - got);
        if (e < 0)
            return e;
        if (e == 0)
            return TGF_EIO;   /* (shorter than its size said) */
        got += (uint32_t)e;
    }
    return 0;
}

static int io_fail(char *why, size_t n, int e)
{
    return fail(why, n, e, "the file could not be read");
}

/* ---- checking ---- */

int tgf_check(const tgf_io_t *io, tgf_info_t *info, char *why, size_t n)
{
    memset(info, 0, sizeof *info);
    const uint32_t size = io->size;
    uint32_t pos = 0;
    int ended = 0;
    char prev[5] = "";
    for (;;) {
        if (pos == size) {
            if (!ended)
                return fail(why, n, pos ? TGF_EBROKEN : TGF_ENOTTGF, pos ? "cut short: no END chunk" : "not a TGF file (it is empty)");
            return 0;
        }
        if (ended)
            return fail(why, n, TGF_EBROKEN, "something after the END chunk");
        uint8_t h[HEAD];
        if (size - pos < HEAD) {
            if (pos == 0)
                return fail(why, n, TGF_ENOTTGF, "not a TGF file (it does not begin with TGAC TGF1)");
            return fail(why, n, TGF_EBROKEN, "cut short: the end of the file in the middle of a chunk");
        }
        int e = read_at(io, pos, h, HEAD);
        if (e < 0)
            return io_fail(why, n, e);
        char tag[5];
        memcpy(tag, h + 4, 4);
        tag[4] = 0;
        if (memcmp(h, "TGAC", 4) != 0 || (pos == 0 && strcmp(tag, "TGF1") != 0)) {
            if (pos == 0)
                return fail(why, n, TGF_ENOTTGF, "not a TGF file (it does not begin with TGAC TGF1)");
            return fail(why, n, TGF_EBROKEN, "no TGAC where the %s chunk's length says the next chunk begins", prev);
        }
        const uint32_t len = get32(h + 8), p = pos + HEAD;
        if (len > size - p)
            return fail(why, n, TGF_EBROKEN, "the %s chunk is longer than the rest of the file", tag);
        if (pos != 0 && !strcmp(tag, "TGF1"))
            return fail(why, n, TGF_EBROKEN, "a second TGF1 header");

        uint8_t f[HEAD];
        if (!strcmp(tag, "TGF1")) {
            if (len < 4)
                return fail(why, n, TGF_EBROKEN, "the TGF1 header is shorter than its fields");
            if ((e = read_at(io, p, f, 4)) < 0)
                return io_fail(why, n, e);
            if (f[0] != 8 && f[0] != 16)
                return fail(why, n, TGF_ERANGE, "a tile size of %d (it is 8 or 16)", f[0]);
            info->tile_size = f[0];
        } else if (!strcmp(tag, "END ")) {
            ended = 1;
        } else if (!strcmp(tag, "ANIM")) {
            if (len < 4)
                return fail(why, n, TGF_EBROKEN, "an ANIM chunk shorter than its fields");
            if ((e = read_at(io, p, f, 4)) < 0)
                return io_fail(why, n, e);
            const int t = f[0], mode = f[1], count = f[2];
            if (t < 1 || t > TGF_TABLES)
                return fail(why, n, TGF_ERANGE, "animation table %d (they are 1 to %d)", t, TGF_TABLES);
            if (mode > 2)
                return fail(why, n, TGF_ERANGE, "animation table %d's mode %d (0 loop, 1 once, 2 pingpong)", t, mode);
            if (count > TGF_FRAMES)
                return fail(why, n, TGF_ERANGE, "animation table %d has %d frames (%d at most)", t, count, TGF_FRAMES);
            if (len < 4u + 4u * (uint32_t)count)
                return fail(why, n, TGF_EBROKEN, "animation table %d's chunk is shorter than its %d frames", t, count);
            tgf_table_t *tb = &info->table[t - 1];
            if (tb->present)
                return fail(why, n, TGF_EBROKEN, "animation table %d twice", t);
            uint8_t fr[4 * TGF_FRAMES];
            if (count && (e = read_at(io, p + 4, fr, 4u * (uint32_t)count)) < 0)
                return io_fail(why, n, e);
            for (int i = 0; i < count; i++)
                if (get16(fr + 4 * i) >= TGF_SET)
                    return fail(why, n, TGF_ERANGE, "animation table %d shows sprite %u (they are 0 to %d)", t, get16(fr + 4 * i), TGF_SET - 1);
            tb->present = 1;
            tb->mode = (uint8_t)mode;
            tb->count = (uint8_t)count;
            tb->at = p + 4;
            info->tables++;
        } else if (!strcmp(tag, "MAP ")) {
            if (len < 12)
                return fail(why, n, TGF_EBROKEN, "a MAP chunk shorter than its fields");
            if ((e = read_at(io, p, f, 12)) < 0)
                return io_fail(why, n, e);
            const int layer = f[0];
            const uint32_t w = get32(f + 4), hgt = get32(f + 8);
            if (layer < 1 || layer > TGF_LAYERS)
                return fail(why, n, TGF_ERANGE, "a map for layer %d (they are 1 to %d)", layer, TGF_LAYERS);
            if (w < 1 || hgt < 1 || (uint64_t)w * hgt > TGF_MAP_MAX)
                return fail(why, n, TGF_ERANGE, "layer %d's map is %lu x %lu tiles (at least 1 x 1, at most %lu tiles)", layer, (unsigned long)w,
                            (unsigned long)hgt, (unsigned long)TGF_MAP_MAX);
            if (len < 12 + 2 * w * hgt)
                return fail(why, n, TGF_EBROKEN, "layer %d's MAP chunk is shorter than its %lu x %lu tiles", layer, (unsigned long)w, (unsigned long)hgt);
            tgf_map_t *m = &info->map[layer - 1];
            if (m->present)
                return fail(why, n, TGF_EBROKEN, "two maps for layer %d", layer);
            uint8_t buf[512];
            for (uint32_t done = 0, total = 2 * w * hgt; done < total;) {
                const uint32_t k = total - done < sizeof buf ? total - done : (uint32_t)sizeof buf;
                if ((e = read_at(io, p + 12 + done, buf, k)) < 0)
                    return io_fail(why, n, e);
                for (uint32_t i = 0; i < k; i += 2) {
                    const uint16_t v = get16(buf + i);
                    if ((v & 0x3FF) >= TGF_SET || (v & 0xF000))
                        return fail(why, n, TGF_ERANGE, "layer %d's map has an entry %04X at %lu, %lu (tiles are 0 to %d, bits 12 to 15 zero)", layer, v,
                                    (unsigned long)((done + i) / 2 % w), (unsigned long)((done + i) / 2 / w), TGF_SET - 1);
                }
                done += k;
            }
            m->present = 1;
            m->flags = f[1];
            m->w = w;
            m->h = hgt;
            m->at = p + 12;
            info->maps++;
        } else {
            for (int k = 0; k < TGF_KINDS; k++) {
                if (strcmp(tag, part_tag[k]) != 0)
                    continue;
                const uint32_t fields = k == TGF_TILE || k == TGF_SPRT ? 8 : 4;
                if (len < fields)
                    return fail(why, n, TGF_EBROKEN, "a %s chunk shorter than its fields", tag);
                if ((e = read_at(io, p, f, fields)) < 0)
                    return io_fail(why, n, e);
                const uint32_t first = get16(f), count = get16(f + 2);
                if (fields == 8 && f[4] != 8)
                    return fail(why, n, TGF_ERANGE, "%s: a depth of %d bits (8)", tag, f[4]);
                if (first + count > TGF_SET)
                    return fail(why, n, TGF_ERANGE, "%s: %lu from %lu does not fit (0 to %d)", tag, (unsigned long)count, (unsigned long)first, TGF_SET - 1);
                if (len < fields + count * (uint32_t)item_bytes(k, info->tile_size))
                    return fail(why, n, TGF_EBROKEN, "the %s chunk is shorter than its %lu items", tag, (unsigned long)count);
                if (info->part[k].at)
                    return fail(why, n, TGF_EBROKEN, "two %s chunks", tag);
                info->part[k].at = p + fields;
                info->part[k].first = (uint16_t)first;
                info->part[k].count = (uint16_t)count;
            }
        }
        memcpy(prev, tag, 5);
        pos = p + len;
    }
}

/* ---- reading ---- */

int tgf_read_part(const tgf_io_t *io, const tgf_info_t *info, int kind, void *dest)
{
    if (kind < 0 || kind >= TGF_KINDS)
        return TGF_ERANGE;
    const tgf_range_t *r = &info->part[kind];
    if (!r->count)
        return 0;
    const uint32_t item = (uint32_t)item_bytes(kind, info->tile_size);
    return read_at(io, r->at, (uint8_t *)dest + r->first * item, r->count * item);
}

int tgf_read_table(const tgf_io_t *io, const tgf_info_t *info, int n, tgf_frame_t *frames)
{
    if (n < 1 || n > TGF_TABLES)
        return TGF_ERANGE;
    const tgf_table_t *t = &info->table[n - 1];
    uint8_t fr[4 * TGF_FRAMES];
    if (!t->count)
        return 0;
    const int e = read_at(io, t->at, fr, 4u * t->count);
    if (e < 0)
        return e;
    for (int i = 0; i < t->count; i++) {
        frames[i].sprite = get16(fr + 4 * i);
        frames[i].ticks = fr[4 * i + 2];
    }
    return 0;
}

int tgf_read_map(const tgf_io_t *io, const tgf_info_t *info, int n, uint16_t *entries)
{
    if (n < 1 || n > TGF_LAYERS)
        return TGF_ERANGE;
    const tgf_map_t *m = &info->map[n - 1];
    if (!m->present)
        return 0;
    const uint32_t count = m->w * m->h;
    const int e = read_at(io, m->at, entries, 2 * count);
    if (e < 0)
        return e;
    uint8_t *b = (uint8_t *)entries;   /* (little-endian in the file: each entry is put back where its two bytes were) */
    for (uint32_t i = 0; i < count; i++)
        entries[i] = get16(b + 2 * i);
    return 0;
}

/* ---- writing ---- */

static int write_all(const tgf_io_t *io, const void *buf, uint32_t n)
{
    if (n == 0)
        return 0;
    const int e = io->write(io->ctx, buf, n);
    return e < 0 ? e : (uint32_t)e == n ? 0 : TGF_EIO;
}

static int chunk(const tgf_io_t *io, const char *tag, uint32_t len)
{
    uint8_t h[HEAD];
    memcpy(h, "TGAC", 4);
    memcpy(h + 4, tag, 4);
    set32(h + 8, len);
    return write_all(io, h, HEAD);
}

int tgf_write_header(const tgf_io_t *io, int tile_size)
{
    const uint8_t f[4] = {(uint8_t)tile_size, 0, 0, 0};
    const int e = chunk(io, "TGF1", 4);
    return e ? e : write_all(io, f, 4);
}

int tgf_write_part(const tgf_io_t *io, int kind, int first, int count, int tile_size, const void *items)
{
    if (kind < 0 || kind >= TGF_KINDS || first < 0 || count < 0 || first + count > TGF_SET)
        return TGF_ERANGE;
    const uint32_t fields = kind == TGF_TILE || kind == TGF_SPRT ? 8 : 4, bytes = (uint32_t)count * (uint32_t)item_bytes(kind, tile_size);
    uint8_t f[8] = {0};
    set16(f, (uint32_t)first);
    set16(f + 2, (uint32_t)count);
    f[4] = 8;   /* (the depth, for TILE and SPRT) */
    int e = chunk(io, part_tag[kind], fields + bytes);
    if (!e)
        e = write_all(io, f, fields);
    return e ? e : write_all(io, items, bytes);
}

int tgf_write_table(const tgf_io_t *io, int n, int mode, int count, const tgf_frame_t *frames)
{
    if (n < 1 || n > TGF_TABLES || mode < 0 || mode > 2 || count < 0 || count > TGF_FRAMES)
        return TGF_ERANGE;
    uint8_t b[4 + 4 * TGF_FRAMES] = {(uint8_t)n, (uint8_t)mode, (uint8_t)count, 0};
    for (int i = 0; i < count; i++) {
        set16(b + 4 + 4 * i, frames[i].sprite);
        b[4 + 4 * i + 2] = frames[i].ticks;
        b[4 + 4 * i + 3] = 0;
    }
    const int e = chunk(io, "ANIM", 4u + 4u * (uint32_t)count);
    return e ? e : write_all(io, b, 4u + 4u * (uint32_t)count);
}

int tgf_write_map(const tgf_io_t *io, int n, int flags, uint32_t w, uint32_t h, const uint16_t *entries)
{
    if (n < 1 || n > TGF_LAYERS || w < 1 || h < 1 || (uint64_t)w * h > TGF_MAP_MAX)
        return TGF_ERANGE;
    uint8_t f[12] = {(uint8_t)n, (uint8_t)flags, 0, 0};
    set32(f + 4, w);
    set32(f + 8, h);
    int e = chunk(io, "MAP ", 12 + 2 * w * h);
    if (!e)
        e = write_all(io, f, 12);
    uint8_t buf[512];
    for (uint32_t i = 0, count = w * h; !e && i < count;) {
        uint32_t k = 0;
        for (; k < sizeof buf / 2 && i < count; k++, i++)
            set16(buf + 2 * k, entries[i]);
        e = write_all(io, buf, 2 * k);
    }
    return e;
}

int tgf_write_end(const tgf_io_t *io)
{
    return chunk(io, "END ", 0);
}
