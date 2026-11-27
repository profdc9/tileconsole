/* SPDX-License-Identifier: MIT */
#include "tg_font.h"

#include <string.h>

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

/* ---- the character map ---- */

void tg_charmap_init(tg_charmap_t *cm, tg_char_t *storage, unsigned capacity)
{
    cm->chars = storage;
    cm->count = 0;
    cm->capacity = (uint16_t)(!storage ? 0 : capacity > 0xFFFF ? 0xFFFF : capacity);
}

void tg_charmap_clear(tg_charmap_t *cm)
{
    cm->count = 0;
}

/* The index of the first entry whose codepoint is not below cp (count if there is none). */
static unsigned lower_bound(const tg_charmap_t *cm, uint32_t cp)
{
    unsigned lo = 0, hi = cm->count;
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        if (cm->chars[mid].codepoint < cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

const tg_char_t *tg_charmap_find(const tg_charmap_t *cm, uint32_t codepoint)
{
    if (!cm)
        return NULL;
    const unsigned i = lower_bound(cm, codepoint);
    return i < cm->count && cm->chars[i].codepoint == codepoint ? &cm->chars[i] : NULL;
}

/* An entry in, new or replacing one (the caller has checked the tile numbers, and that a new one has room). */
static void put_entry(tg_charmap_t *cm, uint32_t cp, unsigned tile, unsigned lower)
{
    const unsigned i = lower_bound(cm, cp);
    if (i == cm->count || cm->chars[i].codepoint != cp) {
        memmove(&cm->chars[i + 1], &cm->chars[i], (cm->count - i) * sizeof *cm->chars);
        cm->count++;
    }
    cm->chars[i] = (tg_char_t){.codepoint = cp, .tile = (uint16_t)tile, .lower = (uint16_t)lower};
}

bool tg_charmap_set(tg_charmap_t *cm, uint32_t codepoint, unsigned tile, unsigned lower)
{
    if (!cm || tile > TG_TILE_MASK || (lower != TG_NO_TILE && lower > TG_TILE_MASK))
        return false;
    if (!tg_charmap_find(cm, codepoint) && cm->count >= cm->capacity)
        return false;
    put_entry(cm, codepoint, tile, lower);
    return true;
}

bool tg_charmap_remove(tg_charmap_t *cm, uint32_t codepoint)
{
    if (!tg_charmap_find(cm, codepoint))
        return false;
    const unsigned i = lower_bound(cm, codepoint);
    memmove(&cm->chars[i], &cm->chars[i + 1], (cm->count - i - 1) * sizeof *cm->chars);
    cm->count--;
    return true;
}

/* Would tg_charmap_set_range fit? (Only the characters the map does not have yet need room.) */
static bool range_fits(const tg_charmap_t *cm, uint32_t first, unsigned count, uint64_t last_tile)
{
    if (!cm || (uint64_t)first + count > (uint64_t)UINT32_MAX + 1 || last_tile > TG_TILE_MASK)
        return false;
    unsigned fresh = 0;
    for (unsigned i = 0; i < count; i++)
        fresh += !tg_charmap_find(cm, first + i);
    return (uint64_t)cm->count + fresh <= cm->capacity;
}

bool tg_charmap_set_range(tg_charmap_t *cm, uint32_t first, unsigned count, unsigned first_tile, unsigned lower_delta)
{
    if (count == 0)
        return cm != NULL;
    if (!range_fits(cm, first, count, (uint64_t)first_tile + count - 1 + lower_delta))
        return false;
    for (unsigned i = 0; i < count; i++)
        put_entry(cm, first + i, first_tile + i, lower_delta ? first_tile + i + lower_delta : TG_NO_TILE);
    return true;
}

/* ---- UTF-8 ---- */

uint32_t tg_utf8_next(const char **p)
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
        return TG_BAD_UTF8;
    }
    for (unsigned i = 1; i <= extra; i++) {
        if ((s[i] & 0xC0) != 0x80) {   /* truncated (this also stops at the end of the string) */
            *p += 1;
            return TG_BAD_UTF8;
        }
        cp = cp << 6 | (s[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {   /* overlong, too big, or a surrogate */
        *p += 1;
        return TG_BAD_UTF8;
    }
    *p += extra + 1;
    return cp;
}

/* ---- putting text on a map ---- */

static void put_tile(uint16_t *map, unsigned map_w_log2, unsigned map_h_log2, int x, int y, unsigned tile, unsigned pal)
{
    if (tile > TG_TILE_MASK)   /* (an entry set by hand, past what a map entry can name) */
        return;
    map[(((unsigned)y & ((1u << map_h_log2) - 1)) << map_w_log2) | ((unsigned)x & ((1u << map_w_log2) - 1))] =
        TG_ENTRY(tile, pal, 0);
}

/* One character at one map position, and its lower half one row down for a character two tiles tall. */
static void put_char(uint16_t *map, unsigned map_w_log2, unsigned map_h_log2, const tg_charmap_t *cm, int x, int y, uint32_t cp,
                     unsigned pal)
{
    const tg_char_t *c = tg_charmap_find(cm, cp);
    if (!c)
        return;
    put_tile(map, map_w_log2, map_h_log2, x, y, c->tile, pal);
    if (c->lower != TG_NO_TILE)
        put_tile(map, map_w_log2, map_h_log2, x, y + 1, c->lower, pal);
}

void tg_map_put_glyphs(uint16_t *map, unsigned map_w_log2, unsigned map_h_log2, const tg_charmap_t *cm, int x, int y,
                       const uint16_t *glyphs, size_t count, unsigned pal)
{
    if (!map || !cm)
        return;
    for (size_t i = 0; i < count; i++)
        put_char(map, map_w_log2, map_h_log2, cm, x + (int)i, y, glyphs[i], pal);
}

void tg_map_put_string(uint16_t *map, unsigned map_w_log2, unsigned map_h_log2, const tg_charmap_t *cm, int x, int y,
                       const char *text, unsigned pal)
{
    if (!map || !cm)
        return;
    for (; *text; text++, x++)
        put_char(map, map_w_log2, map_h_log2, cm, x, y, (uint8_t)*text, pal);
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

bool tg_layer_load_font_ptr(tg_layer_t *layer, tg_charmap_t *cm, const tg_font_t *font, uint32_t first_glyph, unsigned count,
                            unsigned first_tile, uint8_t fg, uint8_t bg)
{
    if (!font || !is_text_layer(layer) || count == 0)
        return false;
    const unsigned rows = font->height / 8;   /* tiles tall: 1 or 2 */
    if (!tiles_fit(first_tile, (uint64_t)count * rows) ||
        !range_fits(cm, first_glyph, count, (uint64_t)first_tile + (uint64_t)count * rows - 1))
        return false;
    /* The tile array is written to: for a layer used for text it is RAM, whatever its declared type. */
    uint8_t *tiles = (uint8_t *)layer->tiles;
    tg_font_load_range(font, tiles, layer->tile_log2, layer->depth, first_tile, first_glyph, count, 0, fg, bg);
    if (rows == 2)
        tg_font_load_range(font, tiles, layer->tile_log2, layer->depth, first_tile + count, first_glyph, count, 1, fg, bg);
    tg_charmap_set_range(cm, first_glyph, count, first_tile, rows == 2 ? count : 0);   /* (it fits: checked above) */
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

/* Is this the first time cp comes in the string (start up to `at`)? */
static bool first_time(const char *start, const char *at, uint32_t cp)
{
    while (start < at)
        if (tg_utf8_next(&start) == cp)
            return false;
    return true;
}

#define BIT(set, n) (((set)[(n) >> 3] >> ((n) & 7)) & 1)

int tg_font_add_chars(tg_charmap_t *cm, const tg_font_t *font, const char *text, uint8_t *tiles, unsigned tile_log2,
                      unsigned depth, unsigned lowest, unsigned highest, const uint8_t *keep, uint8_t fg, uint8_t bg)
{
    if (!cm || !font || !text || !tiles || (tile_log2 != 3 && tile_log2 != 4) || (depth != 4 && depth != 8) ||
        highest > TG_TILE_MASK || lowest > highest || (font->height != 8 && font->height != 16))
        return -1;
    const unsigned rows = font->height / 8;

    /* The characters to add: in the font, not in the map yet, the first time each comes. */
    unsigned fresh = 0;
    for (const char *p = text; *p;) {
        const char *at = p;
        const uint32_t cp = tg_utf8_next(&p);
        fresh += cp != TG_BAD_UTF8 && tg_font_glyph(font, cp) && !tg_charmap_find(cm, cp) && first_time(text, at, cp);
    }
    if (fresh == 0)
        return 0;
    if ((uint64_t)cm->count + fresh > cm->capacity)
        return -1;

    /* The free tiles: blank, not named by the map, not kept. The `need` of them nearest `highest` are taken. */
    uint8_t named[(TG_TILE_MASK + 1) / 8] = {0};
    for (unsigned i = 0; i < cm->count; i++) {
        if (cm->chars[i].tile <= TG_TILE_MASK)
            named[cm->chars[i].tile >> 3] |= (uint8_t)(1u << (cm->chars[i].tile & 7));
        if (cm->chars[i].lower <= TG_TILE_MASK)
            named[cm->chars[i].lower >> 3] |= (uint8_t)(1u << (cm->chars[i].lower & 7));
    }
    uint8_t is_free[(TG_TILE_MASK + 1) / 8] = {0};
    const size_t bytes = tg_tile_bytes(tile_log2, depth);
    const unsigned need = fresh * rows;
    unsigned found = 0, start = highest;
    for (unsigned t = highest + 1; t-- > lowest && found < need;) {
        if (BIT(named, t) || (keep && keep[t]))
            continue;
        const uint8_t *px = tiles + (size_t)t * bytes;
        size_t k = 0;
        while (k < bytes && !px[k])
            k++;
        if (k < bytes)
            continue;
        is_free[t >> 3] |= (uint8_t)(1u << (t & 7));
        found++;
        start = t;
    }
    if (found < need)
        return -1;

    /* Draw them, in rising tile order for the characters in the order of the string, and enter them in the map. */
    static const uint8_t blank[8];
    unsigned t = start;
    for (const char *p = text; *p;) {
        const char *at = p;
        const uint32_t cp = tg_utf8_next(&p);
        const uint8_t *g = cp == TG_BAD_UTF8 ? NULL : tg_font_glyph(font, cp);
        if (!g || tg_charmap_find(cm, cp) || !first_time(text, at, cp))
            continue;
        unsigned got[2] = {TG_NO_TILE, TG_NO_TILE};
        for (unsigned r = 0; r < rows; r++) {
            while (!BIT(is_free, t))
                t++;
            got[r] = t++;
            tg_tile_from_glyph(tiles + (size_t)got[r] * bytes, tile_log2, depth, r ? (font->height == 16 ? g + 8 : blank) : g, fg, bg);
        }
        put_entry(cm, cp, got[0], got[1]);
    }
    return (int)fresh;
}

int tg_layer_add_chars(tg_layer_t *layer, tg_charmap_t *cm, const tg_font_t *font, const char *text, unsigned lowest,
                       unsigned highest, const uint8_t *keep, uint8_t fg, uint8_t bg)
{
    if (!is_text_layer(layer))
        return -1;
    return tg_font_add_chars(cm, font, text, (uint8_t *)layer->tiles, layer->tile_log2, layer->depth, lowest, highest, keep, fg, bg);
}

void tg_layer_put_glyphs(const tg_layer_t *layer, const tg_charmap_t *cm, int x, int y, const uint16_t *glyphs, size_t count,
                         unsigned pal)
{
    if (!is_text_layer(layer) || !layer->map)
        return;
    tg_map_put_glyphs((uint16_t *)layer->map, layer->map_w_log2, layer->map_h_log2, cm, x, y, glyphs, count, pal);
}

void tg_layer_put_string(const tg_layer_t *layer, const tg_charmap_t *cm, int x, int y, const char *text, unsigned pal)
{
    if (!is_text_layer(layer) || !layer->map)
        return;
    tg_map_put_string((uint16_t *)layer->map, layer->map_w_log2, layer->map_h_log2, cm, x, y, text, pal);
}

void tg_layer_put_utf8(const tg_layer_t *layer, const tg_charmap_t *cm, int x, int y, const char *text, unsigned pal)
{
    if (!is_text_layer(layer) || !layer->map || !cm)
        return;
    while (*text) {
        const uint32_t cp = tg_utf8_next(&text);
        if (cp != TG_BAD_UTF8)   /* a bad byte has no tile: skipped like any character without one */
            put_char((uint16_t *)layer->map, layer->map_w_log2, layer->map_h_log2, cm, x, y, cp, pal);
        x++;
    }
}
