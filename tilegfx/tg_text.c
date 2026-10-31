/* SPDX-License-Identifier: MIT */
#include "tg_text.h"

#include <string.h>

void tg_text_default_palette(uint16_t palette[16])
{
    static const uint8_t rgb[16][3] = {
        {0x00, 0x00, 0x00}, {0x00, 0x00, 0xAA}, {0x00, 0xAA, 0x00}, {0x00, 0xAA, 0xAA}, {0xAA, 0x00, 0x00}, {0xAA, 0x00, 0xAA},
        {0xAA, 0x55, 0x00}, {0xAA, 0xAA, 0xAA}, {0x55, 0x55, 0x55}, {0x55, 0x55, 0xFF}, {0x55, 0xFF, 0x55}, {0x55, 0xFF, 0xFF},
        {0xFF, 0x55, 0x55}, {0xFF, 0x55, 0xFF}, {0xFF, 0xFF, 0x55}, {0xFF, 0xFF, 0xFF},
    };
    for (int i = 0; i < 16; i++)
        palette[i] = TG_RGB(rgb[i][0], rgb[i][1], rgb[i][2]);
}

void tg_text_init(tg_text_t *t)
{
    tg_text_default_palette(t->palette);
    tg_text_clear(t, TG_TEXT_ATTR(TG_TEXT_LIGHT_GREY, TG_TEXT_BLACK));
}

void tg_text_set_glyph(tg_text_t *t, uint8_t ch, const uint8_t rows[TG_GLYPH_HEIGHT])
{
    memcpy(t->font + (size_t)ch * TG_GLYPH_HEIGHT, rows, TG_GLYPH_HEIGHT);
}

void tg_text_clear(tg_text_t *t, uint8_t attr)
{
    for (int i = 0; i < TG_TEXT_CELLS; i++)
        t->cells[i] = TG_TEXT_CELL(' ', attr);
}

void tg_text_put_cell(tg_text_t *t, int col, int row, uint8_t ch, uint8_t attr)
{
    if (col >= 0 && col < TG_TEXT_COLS && row >= 0 && row < TG_TEXT_ROWS)
        t->cells[row * TG_TEXT_COLS + col] = TG_TEXT_CELL(ch, attr);
}

void tg_text_put_string(tg_text_t *t, int col, int row, const char *text, uint8_t attr)
{
    if (row < 0 || row >= TG_TEXT_ROWS)
        return;
    for (; *text && col < TG_TEXT_COLS; text++, col++)
        tg_text_put_cell(t, col, row, (uint8_t)*text, attr);
}

void tg_text_scroll_up(tg_text_t *t, int rows, uint8_t attr)
{
    if (rows <= 0)
        return;
    if (rows > TG_TEXT_ROWS)
        rows = TG_TEXT_ROWS;
    memmove(t->cells, t->cells + rows * TG_TEXT_COLS, (size_t)(TG_TEXT_ROWS - rows) * TG_TEXT_COLS * sizeof t->cells[0]);
    for (int i = (TG_TEXT_ROWS - rows) * TG_TEXT_COLS; i < TG_TEXT_CELLS; i++)
        t->cells[i] = TG_TEXT_CELL(' ', attr);
}
