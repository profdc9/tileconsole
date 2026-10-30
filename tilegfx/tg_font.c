/* SPDX-License-Identifier: MIT */
#include "tg_font.h"

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

tg_charset_t tg_font_load(const tg_font_t *font, uint8_t *tiles, unsigned tile_log2, unsigned depth, uint16_t first_tile,
                          uint32_t first_cp, uint16_t count, uint8_t fg, uint8_t bg)
{
    static const uint8_t blank[8];
    const unsigned per = font->height / 8;
    const size_t bytes = tg_tile_bytes(tile_log2, depth);
    for (unsigned c = 0; c < count; c++) {
        const uint8_t *g = tg_font_glyph(font, first_cp + c);
        for (unsigned half = 0; half < per; half++)
            tg_tile_from_glyph(tiles + ((size_t)first_tile + c * per + half) * bytes, tile_log2, depth,
                               g ? g + 8 * half : blank, fg, bg);
    }
    return (tg_charset_t){.font = font, .first_cp = first_cp, .count = count, .first_tile = first_tile,
                          .tiles_per_char = (uint8_t)per};
}

/* The next codepoint of a UTF-8 string, advancing past it. Anything that is not well formed is one byte of 0xFFFD. */
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
        return 0xFFFD;
    }
    for (unsigned i = 1; i <= extra; i++) {
        if ((s[i] & 0xC0) != 0x80) {   /* truncated (this also stops at the end of the string) */
            *p += 1;
            return 0xFFFD;
        }
        cp = cp << 6 | (s[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {   /* overlong, too big, or a surrogate */
        *p += 1;
        return 0xFFFD;
    }
    *p += extra + 1;
    return cp;
}

void tg_text_put(uint16_t *map, unsigned map_w_log2, unsigned map_h_log2, int x, int y, const char *text,
                 const tg_charset_t *cs, unsigned pal)
{
    const unsigned wmask = (1u << map_w_log2) - 1, hmask = (1u << map_h_log2) - 1, per = cs->tiles_per_char;
    while (*text) {
        const uint32_t cp = next_codepoint(&text);
        if (cp >= cs->first_cp && cp - cs->first_cp < cs->count) {
            const unsigned c = cp - cs->first_cp;
            for (unsigned half = 0; half < per; half++)
                map[(((unsigned)y + half) & hmask) << map_w_log2 | ((unsigned)x & wmask)] =
                    TG_ENTRY(cs->first_tile + c * per + half, pal, 0);
        }
        x++;
    }
}
