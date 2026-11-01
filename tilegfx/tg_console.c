/* SPDX-License-Identifier: MIT */
#include "tg_console.h"

#include <stdio.h>
#include <string.h>

enum { A_GROUND = 0, A_ESC, A_CSI, A_SKIP };   /* the ANSI state: not in a sequence, after ESC, in ESC[, or swallowing the one byte of ESC( and the like */

static void sync_cursor(tg_console_t *c)
{
    tg_text_set_cursor(c->text, c->col < TG_TEXT_COLS ? c->col : TG_TEXT_COLS - 1, c->row);
}

static uint16_t blank(const tg_console_t *c) { return (uint16_t)(' ' | c->attr << 8); }

static void fill_cells(tg_console_t *c, int first, int end)   /* the cells first .. end - 1 become blanks in the current colours */
{
    for (int i = first; i < end; i++)
        c->text->cells[i] = blank(c);
}

/* Move the rows top..bottom up (or down) by n rows; the rows that come in are blank. */
static void scroll_region(tg_console_t *c, int top, int bottom, int n, int up)
{
    const int height = bottom - top + 1;
    if (n > height)
        n = height;
    if (n <= 0)
        return;
    uint16_t *cells = c->text->cells;
    if (up) {
        memmove(cells + top * TG_TEXT_COLS, cells + (top + n) * TG_TEXT_COLS, (size_t)(height - n) * TG_TEXT_COLS * sizeof cells[0]);
        fill_cells(c, (bottom - n + 1) * TG_TEXT_COLS, (bottom + 1) * TG_TEXT_COLS);
    } else {
        memmove(cells + (top + n) * TG_TEXT_COLS, cells + top * TG_TEXT_COLS, (size_t)(height - n) * TG_TEXT_COLS * sizeof cells[0]);
        fill_cells(c, top * TG_TEXT_COLS, (top + n) * TG_TEXT_COLS);
    }
}

static void index_down(tg_console_t *c)   /* a line feed: down a row, scrolling the region up at its bottom */
{
    if (c->row == c->bottom)
        scroll_region(c, c->top, c->bottom, 1, 1);
    else if (c->row < TG_TEXT_ROWS - 1)
        c->row++;
}

static void index_up(tg_console_t *c)     /* a reverse line feed */
{
    if (c->row == c->top)
        scroll_region(c, c->top, c->bottom, 1, 0);
    else if (c->row > 0)
        c->row--;
}

static void new_line(tg_console_t *c)
{
    c->col = 0;
    index_down(c);
}

static void put(tg_console_t *c, uint8_t ch)
{
    if (c->flags & TG_CONSOLE_NOWRAP) {   /* no wrap: on the last column, each character is written over the one there, and the cursor stays */
        if (c->col >= TG_TEXT_COLS)
            c->col = TG_TEXT_COLS - 1;
        tg_text_put_cell(c->text, c->col, c->row, ch, c->attr);
        if (c->col < TG_TEXT_COLS - 1)
            c->col++;
        return;
    }
    if (c->col >= TG_TEXT_COLS)     /* a wrap was waiting for this character */
        new_line(c);
    tg_text_put_cell(c->text, c->col, c->row, ch, c->attr);
    c->col++;
}

/* ---- the ANSI mode ---- */

static void set_colours(tg_console_t *c)   /* attr is base_attr, with the colours swapped when reversed */
{
    c->attr = c->reverse ? (uint8_t)(c->base_attr << 4 | c->base_attr >> 4) : c->base_attr;
}

static void reset_ansi_state(tg_console_t *c)
{
    c->ansi_state = A_GROUND;
}

static void begin_csi(tg_console_t *c)
{
    c->ansi_state = A_CSI;
    c->ansi_private = c->ansi_intermediate = c->ansi_digits = c->ansi_count = 0;
    c->ansi_number = 0;
}

static void clear_screen_home(tg_console_t *c)
{
    fill_cells(c, 0, TG_TEXT_CELLS);
    c->col = c->row = 0;
}

static int clamp(int v, int low, int high) { return v < low ? low : v > high ? high : v; }

/* The numbers of the sequence: p(i, d) is parameter i, or d when it is missing or 0 (the way most sequences have it). */
static int p(const tg_console_t *c, int i, int dflt)
{
    return i < c->ansi_count && c->ansi_param[i] ? c->ansi_param[i] : dflt;
}

static int raw(const tg_console_t *c, int i) { return i < c->ansi_count ? c->ansi_param[i] : 0; }

static void move_to(tg_console_t *c, int col, int row)
{
    c->col = clamp(col, 0, TG_TEXT_COLS - 1);
    c->row = clamp(row, 0, TG_TEXT_ROWS - 1);
}

static void erase_in_display(tg_console_t *c)
{
    const int at = c->row * TG_TEXT_COLS + clamp(c->col, 0, TG_TEXT_COLS - 1);
    switch (raw(c, 0)) {
    case 0: fill_cells(c, at, TG_TEXT_CELLS); break;
    case 1: fill_cells(c, 0, at + 1); break;
    default: clear_screen_home(c); break;   /* 2: all of it, and the cursor goes home */
    }
}

static void erase_in_line(tg_console_t *c)
{
    const int start = c->row * TG_TEXT_COLS, at = start + clamp(c->col, 0, TG_TEXT_COLS - 1);
    switch (raw(c, 0)) {
    case 0: fill_cells(c, at, start + TG_TEXT_COLS); break;
    case 1: fill_cells(c, start, at + 1); break;
    default: fill_cells(c, start, start + TG_TEXT_COLS); break;
    }
}

static void delete_chars(tg_console_t *c, int n)
{
    const int col = clamp(c->col, 0, TG_TEXT_COLS - 1);
    uint16_t *line = c->text->cells + c->row * TG_TEXT_COLS;
    if (n > TG_TEXT_COLS - col)
        n = TG_TEXT_COLS - col;
    memmove(line + col, line + col + n, (size_t)(TG_TEXT_COLS - col - n) * sizeof line[0]);
    fill_cells(c, c->row * TG_TEXT_COLS + TG_TEXT_COLS - n, (c->row + 1) * TG_TEXT_COLS);
}

static void set_scroll_region_rows(tg_console_t *c, int top, int bottom)
{
    if (top < 0 || bottom >= TG_TEXT_ROWS || top >= bottom) {   /* not a region: the whole screen */
        top = 0;
        bottom = TG_TEXT_ROWS - 1;
    }
    c->top = top;
    c->bottom = bottom;
}

static const uint8_t pc_colour[8] = {0, 4, 2, 6, 1, 5, 3, 7};   /* ANSI colour number 0..7 -> the PC's: black red green yellow blue magenta cyan white */

static void sgr(tg_console_t *c)
{
    if (!c->ansi_count) {   /* ESC[m is ESC[0m */
        c->base_attr = c->default_attr;
        c->reverse = 0;
    }
    for (int i = 0; i < c->ansi_count; i++) {
        const int n = c->ansi_param[i];
        if (n == 0) {
            c->base_attr = c->default_attr;
            c->reverse = 0;
        } else if (n == 1) {
            c->base_attr |= 0x08;
        } else if (n == 22) {
            c->base_attr &= 0xF7;
        } else if (n == 5) {
            c->base_attr |= 0x80;
        } else if (n == 25) {
            c->base_attr &= 0x7F;
        } else if (n == 7) {
            c->reverse = 1;
        } else if (n == 27) {
            c->reverse = 0;
        } else if (n >= 30 && n <= 37) {
            c->base_attr = (uint8_t)((c->base_attr & 0xF8) | pc_colour[n - 30]);
        } else if (n >= 40 && n <= 47) {
            c->base_attr = (uint8_t)((c->base_attr & 0x8F) | pc_colour[n - 40] << 4);
        } else if (n >= 90 && n <= 97) {
            c->base_attr = (uint8_t)((c->base_attr & 0xF0) | 0x08 | pc_colour[n - 90]);
        } else if (n >= 100 && n <= 107) {
            c->base_attr = (uint8_t)((c->base_attr & 0x0F) | (0x08 | pc_colour[n - 100]) << 4);
        } else if (n == 39) {
            c->base_attr = (uint8_t)((c->base_attr & 0xF0) | (c->default_attr & 0x0F));
        } else if (n == 49) {
            c->base_attr = (uint8_t)((c->base_attr & 0x0F) | (c->default_attr & 0xF0));
        } else if (n == 38 || n == 48) {   /* an extended colour (5;n or 2;r;g;b): not supported, but its numbers are not colours either */
            i += i + 1 < c->ansi_count && c->ansi_param[i + 1] == 5 ? 2 : i + 1 < c->ansi_count && c->ansi_param[i + 1] == 2 ? 4 : 0;
        }
    }
    set_colours(c);
}

static void csi_dispatch(tg_console_t *c, uint8_t final)
{
    if (c->ansi_private || c->ansi_intermediate)   /* ESC[?25l and the like, or ESC[ q: nothing here understands them */
        return;
    const int col = clamp(c->col, 0, TG_TEXT_COLS - 1);   /* while a wrap waits, the cursor is on the last column */
    switch (final) {
    case 'A': move_to(c, col, c->row - p(c, 0, 1)); break;
    case 'B': case 'e': move_to(c, col, c->row + p(c, 0, 1)); break;
    case 'C': case 'a': move_to(c, col + p(c, 0, 1), c->row); break;
    case 'D': move_to(c, col - p(c, 0, 1), c->row); break;
    case 'E': move_to(c, 0, c->row + p(c, 0, 1)); break;
    case 'F': move_to(c, 0, c->row - p(c, 0, 1)); break;
    case 'G': case '`': move_to(c, p(c, 0, 1) - 1, c->row); break;
    case 'd': move_to(c, col, p(c, 0, 1) - 1); break;
    case 'H': case 'f': move_to(c, p(c, 1, 1) - 1, p(c, 0, 1) - 1); break;
    case 'J': erase_in_display(c); break;
    case 'K': erase_in_line(c); break;
    case 'L': if (c->row >= c->top && c->row <= c->bottom) scroll_region(c, c->row, c->bottom, p(c, 0, 1), 0); break;
    case 'M': if (c->row >= c->top && c->row <= c->bottom) scroll_region(c, c->row, c->bottom, p(c, 0, 1), 1); break;
    case 'P': delete_chars(c, p(c, 0, 1)); break;
    case 's': c->saved_col = col; c->saved_row = c->row; break;
    case 'u': move_to(c, c->saved_col, c->saved_row); break;
    case 'r':
        if (!c->ansi_count) {
            set_scroll_region_rows(c, 0, TG_TEXT_ROWS - 1);
        } else {
            const int top = p(c, 0, 1) - 1, bottom = p(c, 1, TG_TEXT_ROWS) - 1;
            if (top >= bottom || bottom >= TG_TEXT_ROWS)   /* not a region: the command is ignored */
                break;
            set_scroll_region_rows(c, top, bottom);
        }
        move_to(c, 0, 0);
        break;
    case 'm': sgr(c); break;
    default: break;
    }
}

/* A control character, in the ground state and inside a sequence alike. */
static void control(tg_console_t *c, uint8_t ch)
{
    switch (ch) {
    case '\n':
        if ((c->flags & (TG_CONSOLE_ANSI | TG_CONSOLE_LF_ONLY)) == (TG_CONSOLE_ANSI | TG_CONSOLE_LF_ONLY)) {   /* LF_ONLY is for the ANSI mode */
            c->col = clamp(c->col, 0, TG_TEXT_COLS - 1);
            index_down(c);
        } else {
            new_line(c);
        }
        break;
    case '\r': c->col = 0; break;
    case '\b': if (c->col > 0) c->col = (c->col > TG_TEXT_COLS ? TG_TEXT_COLS : c->col) - 1; break;
    case '\t': c->col = (c->col / 8 + 1) * 8 < TG_TEXT_COLS ? (c->col / 8 + 1) * 8 : TG_TEXT_COLS - 1; break;
    case '\f': clear_screen_home(c); break;
    default: break;   /* NUL, the bell and the rest of the controls do nothing */
    }
}

/* One byte through the ANSI state machine. Returns 1 if it was used up, 0 if it is an ordinary character for the rest of the console. */
static int ansi_byte(tg_console_t *c, uint8_t ch)
{
    if (ch == 0x1B) {                    /* ESC starts a sequence, and starts it again inside one */
        c->ansi_state = A_ESC;
        return 1;
    }
    if (c->ansi_state == A_GROUND) {
        if (ch == 0 || ch == '\f') {    /* the controls this mode treats differently from the plain console */
            control(c, ch);
            return 1;
        }
        return 0;   /* (a line feed that is only a line feed is control's business too, and is reached from emit) */
    }
    if (ch == 0x18 || ch == 0x1A) {      /* CAN and SUB cancel the sequence */
        reset_ansi_state(c);
        return 1;
    }
    if (ch < 0x20) {                     /* a control inside a sequence acts, and the sequence goes on */
        control(c, ch);
        return 1;
    }
    switch (c->ansi_state) {
    case A_ESC:
        reset_ansi_state(c);
        switch (ch) {
        case '[': begin_csi(c); break;
        case '7': c->saved_col = clamp(c->col, 0, TG_TEXT_COLS - 1); c->saved_row = c->row; c->saved_attr = c->base_attr; c->saved_reverse = c->reverse; break;
        case '8': move_to(c, c->saved_col, c->saved_row); c->base_attr = c->saved_attr; c->reverse = c->saved_reverse; set_colours(c); break;
        case 'D': c->col = clamp(c->col, 0, TG_TEXT_COLS - 1); index_down(c); break;
        case 'E': new_line(c); break;
        case 'M': c->col = clamp(c->col, 0, TG_TEXT_COLS - 1); index_up(c); break;
        case 'c':                        /* reset */
            c->base_attr = c->default_attr;
            c->reverse = 0;
            set_colours(c);
            set_scroll_region_rows(c, 0, TG_TEXT_ROWS - 1);
            clear_screen_home(c);
            c->saved_col = c->saved_row = 0;
            c->saved_attr = c->default_attr;
            c->saved_reverse = 0;
            break;
        case '(': case ')': case '*': case '+': case '#': case ' ':   /* one more byte belongs to these (a character set, a line size...) */
            c->ansi_state = A_SKIP;
            break;
        default: break;
        }
        return 1;
    case A_SKIP:
        reset_ansi_state(c);
        return 1;
    default:                             /* A_CSI */
        break;
    }
    if (ch >= '0' && ch <= '9') {
        c->ansi_digits = 1;
        c->ansi_number = c->ansi_number * 10 + (ch - '0');
        if (c->ansi_number > 9999)   /* a number cannot get out of hand (and not wrap round when it is kept in 16 bits) */
            c->ansi_number = 9999;
        return 1;
    }
    if (ch == ';' || (ch >= 0x40 && ch <= 0x7E)) {   /* the end of a number, or of the whole sequence */
        if (ch == ';' || c->ansi_digits || c->ansi_count) {
            if (c->ansi_count < TG_CONSOLE_ANSI_PARAMS)
                c->ansi_param[c->ansi_count++] = (uint16_t)c->ansi_number;
            c->ansi_number = 0;
            c->ansi_digits = 0;
        }
        if (ch != ';') {
            reset_ansi_state(c);
            csi_dispatch(c, ch);
        }
        return 1;
    }
    if (ch >= 0x3C && ch <= 0x3F) {      /* < = > ?: a private sequence */
        c->ansi_private = 1;
        return 1;
    }
    if (ch >= 0x20 && ch <= 0x2F) {      /* an intermediate byte: the sequence is not one that is understood */
        c->ansi_intermediate = 1;
        return 1;
    }
    reset_ansi_state(c);                 /* anything else (a byte of 0x80 or more, DEL): the sequence was malformed, and it is dropped */
    return 1;
}

static void emit(tg_console_t *c, uint8_t ch)
{
    if ((c->flags & TG_CONSOLE_ANSI) && ansi_byte(c, ch))
        return;
    switch (ch) {
    case '\n': case '\r': case '\b': case '\t': control(c, ch); break;
    case '\a': break;
    default: put(c, ch); break;
    }
}

void tg_console_init(tg_console_t *c, tg_text_t *text, uint8_t attr)
{
    memset(c, 0, offsetof(tg_console_t, work));   /* all but the printf work space */
    c->text = text;
    c->attr = c->base_attr = c->default_attr = c->saved_attr = attr;
    c->top = 0;
    c->bottom = TG_TEXT_ROWS - 1;
    sync_cursor(c);
}

void tg_console_clear(tg_console_t *c)
{
    tg_text_clear(c->text, c->attr);
    c->col = c->row = 0;
    sync_cursor(c);
}

void tg_console_set_attr(tg_console_t *c, uint8_t attr)
{
    c->attr = c->base_attr = attr;   /* not reversed: this is the colours asked for */
    c->reverse = 0;
}

void tg_console_gotoxy(tg_console_t *c, int col, int row)
{
    c->col = col < 0 ? 0 : col >= TG_TEXT_COLS ? TG_TEXT_COLS - 1 : col;
    c->row = row < 0 ? 0 : row >= TG_TEXT_ROWS ? TG_TEXT_ROWS - 1 : row;
    sync_cursor(c);
}

void tg_console_set_scroll_region(tg_console_t *c, int top, int bottom)
{
    set_scroll_region_rows(c, top, bottom);
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
