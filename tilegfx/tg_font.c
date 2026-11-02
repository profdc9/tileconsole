/* SPDX-License-Identifier: MIT */
#include "tg_font.h"

/* ---- fonts and glyphs ---- */

const uint8_t *tg_font_glyph(const tg_font_t *font, uint32_t codepoint)
{
    unsigned lo = 0, hi = font->nruns;   /* the last run that starts at or before the codepoint */
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        if (font->runs[mid].first <= codepoint)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return NULL;
    const tg_glyph_run_t *run = &font->runs[lo - 1];
    if (codepoint - run->first >= run->count)
        return NULL;
    return font->glyphs + (size_t)(run->index + (codepoint - run->first)) * font->height;
}

size_t tg_tile_bytes(unsigned tile_log2, unsigned depth)
{
    return ((size_t)1 << (2 * tile_log2)) * depth / 8;
}

void tg_tile_from_glyph(uint8_t *tile, unsigned tile_log2, unsigned depth, const uint8_t *glyph, uint8_t fg, uint8_t bg)
{
    if ((tile_log2 != 3 && tile_log2 != 4) || (depth != 4 && depth != 8))
        return;
    const unsigned size = 1u << tile_log2, scale = size / 8;
    if (depth == 4) {
        fg &= 15;
        bg &= 15;
    }
    for (unsigned y = 0; y < size; y++) {
        const uint8_t bits = glyph[y / scale];
        for (unsigned x = 0; x < size; x++) {
            const uint8_t px = (bits >> (7 - x / scale)) & 1 ? fg : bg;
            const unsigned i = y * size + x;
            if (depth == 8)
                tile[i] = px;
            else if (i & 1)
                tile[i >> 1] |= px;
            else
                tile[i >> 1] = (uint8_t)(px << 4);
        }
    }
}

void tg_font_load_range(const tg_font_t *font, uint8_t *tiles, unsigned tile_log2, unsigned depth, unsigned first_tile,
                        uint32_t first_cp, unsigned count, unsigned half, uint8_t fg, uint8_t bg)
{
    static const uint8_t blank[8];
    const size_t bytes = tg_tile_bytes(tile_log2, depth);
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *g = tg_font_glyph(font, first_cp + i);
        tg_tile_from_glyph(tiles + (size_t)(first_tile + i) * bytes, tile_log2, depth,
                           g && 8 * (half + 1) <= font->height ? g + 8 * half : blank, fg, bg);
    }
}

/* ---- putting glyph numbers on a map ---- */

static void put_tile(uint16_t *map, unsigned map_w_log2, unsigned map_h_log2, int x, int y, int tile, unsigned pal)
{
    if (tile < 0 || tile > TG_TILE_MASK)
        return;
    map[(((unsigned)y & ((1u << map_h_log2) - 1)) << map_w_log2) | ((unsigned)x & ((1u << map_w_log2) - 1))] =
        TG_ENTRY(tile, pal, 0);
}

void tg_map_put_glyphs(uint16_t *map, unsigned map_w_log2, unsigned map_h_log2, int x, int y, const uint16_t *glyphs,
                       size_t count, int glyph_offset, unsigned pal)
{
    for (size_t i = 0; i < count; i++)
        put_tile(map, map_w_log2, map_h_log2, x + (int)i, y, (int)glyphs[i] + glyph_offset, pal);
}

void tg_map_put_string(uint16_t *map, unsigned map_w_log2, unsigned map_h_log2, int x, int y, const char *text,
                       int glyph_offset, unsigned pal)
{
    for (; *text; text++, x++)
        put_tile(map, map_w_log2, map_h_log2, x, y, (int)(uint8_t)*text + glyph_offset, pal);
}

/* ---- text on a layer ---- */

/* A tile layer that text can be loaded into and put on. */
static bool is_text_layer(const tg_layer_t *l)
{
    return l && l->kind == TG_LAYER_TILES && l->tiles && (l->tile_log2 == 3 || l->tile_log2 == 4) &&
           (l->depth == 4 || l->depth == 8);
}

/* The tile numbers first_tile .. first_tile + n - 1 must all be nameable by a map entry. */
static bool tiles_fit(unsigned first_tile, uint64_t n)
{
    return first_tile + n <= (uint64_t)TG_TILE_MASK + 1;
}

bool tg_layer_load_font_ptr(tg_layer_t *layer, const tg_font_t *font, uint32_t first_glyph, unsigned count,
                            unsigned first_tile, uint8_t fg, uint8_t bg)
{
    if (!font || !is_text_layer(layer) || count == 0)
        return false;
    const unsigned rows = font->height / 8;   /* tiles tall: 1 or 2 */
    if (!tiles_fit(first_tile, (uint64_t)count * rows))
        return false;
    /* The tile array is written to: for a layer used for text it is RAM, whatever its declared type. */
    uint8_t *tiles = (uint8_t *)layer->tiles;
    tg_font_load_range(font, tiles, layer->tile_log2, layer->depth, first_tile, first_glyph, count, 0, fg, bg);
    if (rows == 2)
        tg_font_load_range(font, tiles, layer->tile_log2, layer->depth, first_tile + count, first_glyph, count, 1, fg, bg);
    layer->glyphs = (tg_glyphmap_t){.offset = (int32_t)first_tile - (int32_t)first_glyph,
                                    .first = first_glyph,
                                    .count = count,
                                    .row_delta = rows == 2 ? (uint16_t)count : 0};
    return true;
}

bool tg_layer_load_glyphs(tg_layer_t *layer, const uint8_t *glyphs, unsigned rows_per_glyph, unsigned half, unsigned count,
                          unsigned first_tile, uint8_t fg, uint8_t bg)
{
    static const uint8_t blank[8];
    if (!glyphs || !is_text_layer(layer) || count == 0 || (rows_per_glyph != 8 && rows_per_glyph != 16) ||
        !tiles_fit(first_tile, count))
        return false;
    uint8_t *tiles = (uint8_t *)layer->tiles;
    const size_t bytes = tg_tile_bytes(layer->tile_log2, layer->depth);
    for (unsigned i = 0; i < count; i++)
        tg_tile_from_glyph(tiles + (size_t)(first_tile + i) * bytes, layer->tile_log2, layer->depth,
                           8 * (half + 1) <= rows_per_glyph ? glyphs + (size_t)i * rows_per_glyph + 8 * half : blank, fg, bg);
    return true;
}

void tg_layer_set_glyph_offset(tg_layer_t *layer, int offset)
{
    layer->glyphs = (tg_glyphmap_t){.offset = offset};
}

void tg_layer_set_glyph_range(tg_layer_t *layer, uint32_t first, uint32_t count)
{
    layer->glyphs.first = first;
    layer->glyphs.count = count;
}

/* One glyph at one map position (and the lower half of it, one row down, for a font two tiles tall). */
static void put_glyph(const tg_layer_t *l, int x, int y, uint32_t glyph, unsigned pal)
{
    const tg_glyphmap_t *m = &l->glyphs;
    if (m->count && (glyph < m->first || glyph - m->first >= m->count))
        return;
    const int64_t tile = (int64_t)glyph + m->offset;
    if (tile < 0 || tile > TG_TILE_MASK)
        return;
    uint16_t *map = (uint16_t *)l->map;
    put_tile(map, l->map_w_log2, l->map_h_log2, x, y, (int)tile, pal);
    if (m->row_delta)
        put_tile(map, l->map_w_log2, l->map_h_log2, x, y + 1, (int)(tile + m->row_delta), pal);
}

void tg_layer_put_glyphs(const tg_layer_t *layer, int x, int y, const uint16_t *glyphs, size_t count, unsigned pal)
{
    if (!is_text_layer(layer) || !layer->map)
        return;
    for (size_t i = 0; i < count; i++)
        put_glyph(layer, x + (int)i, y, glyphs[i], pal);
}

void tg_layer_put_string(const tg_layer_t *layer, int x, int y, const char *text, unsigned pal)
{
    if (!is_text_layer(layer) || !layer->map)
        return;
    for (; *text; text++, x++)
        put_glyph(layer, x, y, (uint8_t)*text, pal);
}

#define BAD_UTF8 0xFFFFFFFFu   /* not a codepoint */

/* The next codepoint of a UTF-8 string, advancing past it. Anything that is not well formed is one byte of BAD_UTF8. */
static uint32_t next_codepoint(const char **p)
{
    const uint8_t *s = (const uint8_t *)*p;
    const uint8_t b = s[0];
    unsigned extra;
    uint32_t cp, min;
    if (b < 0x80) {
        *p += 1;
        return b;
    } else if (b >= 0xC2 && b <= 0xDF) {
        extra = 1, cp = b & 0x1F, min = 0x80;
    } else if (b >= 0xE0 && b <= 0xEF) {
        extra = 2, cp = b & 0x0F, min = 0x800;
    } else if (b >= 0xF0 && b <= 0xF4) {
        extra = 3, cp = b & 0x07, min = 0x10000;
    } else {
        *p += 1;
        return BAD_UTF8;
    }
    for (unsigned i = 1; i <= extra; i++) {
        if ((s[i] & 0xC0) != 0x80) {   /* truncated (this also stops at the end of the string) */
            *p += 1;
            return BAD_UTF8;
        }
        cp = cp << 6 | (s[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {   /* overlong, too big, or a surrogate */
        *p += 1;
        return BAD_UTF8;
    }
    *p += extra + 1;
    return cp;
}

void tg_layer_put_utf8(const tg_layer_t *layer, int x, int y, const char *text, unsigned pal)
{
    if (!is_text_layer(layer) || !layer->map)
        return;
    while (*text) {
        const uint32_t cp = next_codepoint(&text);
        if (cp != BAD_UTF8)   /* a bad byte has no glyph: skipped like any glyph without a tile */
            put_glyph(layer, x, y, cp, pal);
        x++;
    }
}
