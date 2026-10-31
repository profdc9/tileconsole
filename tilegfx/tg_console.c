/* SPDX-License-Identifier: MIT */
#include "tg_console.h"

#include <stdio.h>
#include <string.h>

static void sync_cursor(tg_console_t *c)
{
    tg_text_set_cursor(c->text, c->col < TG_TEXT_COLS ? c->col : TG_TEXT_COLS - 1, c->row);
}

static void new_line(tg_console_t *c)
{
    c->col = 0;
    if (c->row < TG_TEXT_ROWS - 1)
        c->row++;
    else
        tg_text_scroll_up(c->text, 1, c->attr);
}

static void put(tg_console_t *c, uint8_t ch)
{
    if (c->col >= TG_TEXT_COLS) {   /* a wrap was waiting for this character */
        if (c->flags & TG_CONSOLE_NOWRAP)
            return;
        new_line(c);
    }
    tg_text_put_cell(c->text, c->col, c->row, ch, c->attr);
    c->col++;
}

static void emit(tg_console_t *c, uint8_t ch)
{
    switch (ch) {
    case '\n': new_line(c); break;
    case '\r': c->col = 0; break;
    case '\b': if (c->col > 0) c->col = (c->col > TG_TEXT_COLS ? TG_TEXT_COLS : c->col) - 1; break;
    case '\t': c->col = (c->col / 8 + 1) * 8 < TG_TEXT_COLS ? (c->col / 8 + 1) * 8 : TG_TEXT_COLS - 1; break;
    case '\a': break;
    default: put(c, ch); break;
    }
}

void tg_console_init(tg_console_t *c, tg_text_t *text, uint8_t attr)
{
    c->text = text;
    c->col = c->row = 0;
    c->attr = attr;
    c->flags = 0;
    sync_cursor(c);
}

void tg_console_clear(tg_console_t *c)
{
    tg_text_clear(c->text, c->attr);
    c->col = c->row = 0;
    sync_cursor(c);
}

void tg_console_set_attr(tg_console_t *c, uint8_t attr) { c->attr = attr; }

void tg_console_gotoxy(tg_console_t *c, int col, int row)
{
    c->col = col < 0 ? 0 : col >= TG_TEXT_COLS ? TG_TEXT_COLS - 1 : col;
    c->row = row < 0 ? 0 : row >= TG_TEXT_ROWS ? TG_TEXT_ROWS - 1 : row;
    sync_cursor(c);
}

void tg_console_putc(tg_console_t *c, int ch)
{
    emit(c, (uint8_t)ch);
    sync_cursor(c);
}

void tg_console_puts(tg_console_t *c, const char *text)
{
    for (; *text; text++)
        emit(c, (uint8_t)*text);
    sync_cursor(c);
}

int tg_console_vprintf(tg_console_t *c, const char *format, va_list args)
{
    int n = vsnprintf(c->work, sizeof c->work, format, args);
    if (n < 0)
        return 0;
    if (n >= (int)sizeof c->work)
        n = (int)sizeof c->work - 1;   /* what vsnprintf kept */
    for (int i = 0; i < n; i++)
        emit(c, (uint8_t)c->work[i]);
    sync_cursor(c);
    return n;
}

int tg_console_printf(tg_console_t *c, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int n = tg_console_vprintf(c, format, args);
    va_end(args);
    return n;
}
