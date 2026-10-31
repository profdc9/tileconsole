/* SPDX-License-Identifier: MIT */
/*
 * A console on a text mode screen: characters are written at a position that moves along, the way a terminal does.
 *
 *   static tg_text_t screen;
 *   static tg_console_t con;
 *   tg_text_init(&screen); tg_text_load_cp437(&screen);
 *   tg_console_init(&con, &screen, TG_TEXT_ATTR(TG_TEXT_LIGHT_GREY, TG_TEXT_BLACK));
 *   tg_console_clear(&con);
 *   tg_console_printf(&con, "%d files, %s\n", 12, "ok");
 *
 * The characters are bytes, the codes of the PC character set (code page 437), so "\xC9\xCD\xBB" is a box corner; the printf formats
 * are the C library's. These controls act and are not drawn:
 *   \n  start of the next line (scrolling the screen up when it is on the last one)   \r  start of this line
 *   \b  one column to the left (it does not erase)   \t  the next multiple of 8 columns (it does not write; it stops at the last column)
 *   \a  nothing (there is no bell)
 * Any other byte, including the ones the PC shows as faces and card suits, is drawn (and \033, the escape, unless TG_CONSOLE_ANSI is on). A character that does not fit on the line goes to
 * the start of the next one, scrolling if need be. The wrap is put off until the next character is drawn, so a line of exactly 80
 * characters followed by \n does not leave a blank line. With TG_CONSOLE_NOWRAP the characters past the right edge are dropped instead.
 *
 * With TG_CONSOLE_ANSI (in flags, off by default) the console also interprets the ANSI/VT100 escape sequences, in the output of printf too, so a
 * program written for a terminal can use it. These are understood (n is a number, default 1; r and c are a row and column, counted from 1):
 *   ESC[nA B C D  cursor up, down, right, left (it stops at the edge of the screen, and does not scroll)   ESC[nE  ESC[nF  down, up, to column 1
 *   ESC[nG  ESC[n`  to column n   ESC[nd  to row n   ESC[r;cH  ESC[r;cf  to row r, column c (ESC[H is the top left)
 *   ESC[0J  clear from the cursor to the end of the screen   ESC[1J  to the start   ESC[2J  all of it, and the cursor goes to the top left
 *   ESC[0K  clear from the cursor to the end of the line     ESC[1K  to the start   ESC[2K  all of it
 *   ESC[nL  insert n blank lines at the cursor row, pushing the rest of the scroll region down   ESC[nM  delete n lines, pulling it up
 *   ESC[nP  delete n characters at the cursor, pulling the rest of the line left
 *   ESC[top;bottomr  set the scroll region (rows, from 1; the cursor goes to the top left); ESC[r makes it the whole screen
 *   ESC7 ESC8  save and restore the cursor position and the colours   ESC[s ESC[u  save and restore the position only
 *   ESC D  down a line, scrolling at the bottom of the region   ESC E  the same, to column 1   ESC M  up a line, scrolling at the top   ESC c  reset
 *   ESC[...m  colours: 0 normal, 1 bold (bright foreground), 22 not bold, 5 blink (bright background here), 25, 7 reverse, 27, 30-37 and 40-47 the
 *   ANSI colours (ANSI red is the PC's colour 4: the numbers are translated), 90-97 and 100-107 the bright ones, 39 and 49 the colours the console
 *   started with. 38 and 48 (extended colours) are skipped. Anything else that is a well formed sequence is ignored, and a malformed one is cut off.
 * Control characters that act in this mode: as above, and NUL is ignored, \f (form feed) clears the screen and goes to the top left, and \n is
 * a line feed that goes to column 1 as before, unless TG_CONSOLE_LF_ONLY (also in flags) makes it only move down, as on a terminal.
 * The scroll region (ESC[r, or tg_console_set_scroll_region) is what \n scrolls at the bottom of, as well: it is the whole screen until set.
 *
 * The text screen's own cursor (tg_text_set_cursor) follows the console's position; whether it shows is up to the caller
 * (tg_text_show_cursor). Nothing is clipped from the screen but what the flags say, and the work space for printf is in the console, not
 * on the stack: the output of one call is at most TG_CONSOLE_PRINTF_MAX - 1 characters, and the rest is dropped.
 */
#ifndef TG_CONSOLE_H
#define TG_CONSOLE_H

#include <stdarg.h>

#include "tg_text.h"

#define TG_CONSOLE_NOWRAP 1
#define TG_CONSOLE_ANSI 2
#define TG_CONSOLE_LF_ONLY 4
#define TG_CONSOLE_PRINTF_MAX 256
#define TG_CONSOLE_ANSI_PARAMS 16

typedef struct {
    tg_text_t *text;
    int col, row;   /* where the next character goes; col is TG_TEXT_COLS while a wrap is waiting for the next character */
    uint8_t attr;   /* the colours it is written in, and the rows that scroll in at the bottom are blank in */
    uint8_t flags;  /* TG_CONSOLE_* */
    int top, bottom;                  /* the scroll region: rows top..bottom (counted from 0, inclusive); the whole screen at first */

    /* What the ANSI mode keeps: the colours (attr is the colours as written, reversed if need be), the escape sequence being read, a saved cursor. */
    uint8_t default_attr, base_attr, reverse;
    uint8_t ansi_state, ansi_private, ansi_intermediate, ansi_digits;
    uint8_t ansi_count;               /* parameters read so far */
    int ansi_number;                  /* the one being read */
    uint16_t ansi_param[TG_CONSOLE_ANSI_PARAMS];
    int saved_col, saved_row;
    uint8_t saved_attr, saved_reverse;

    char work[TG_CONSOLE_PRINTF_MAX];
} tg_console_t;

/* Attach a console to a text screen, at the top left. It does not touch the screen (see tg_console_clear). */
void tg_console_init(tg_console_t *c, tg_text_t *text, uint8_t attr);

void tg_console_clear(tg_console_t *c);                         /* blank the screen in the current colours and go to the top left */
void tg_console_set_attr(tg_console_t *c, uint8_t attr);        /* TG_TEXT_ATTR(foreground, background) */
void tg_console_gotoxy(tg_console_t *c, int col, int row);      /* clamped to the screen */
/* Rows top..bottom (counted from 0) are what scrolls; top >= bottom, or anything off the screen, means the whole screen. */
void tg_console_set_scroll_region(tg_console_t *c, int top, int bottom);

void tg_console_putc(tg_console_t *c, int ch);                  /* one byte (the low 8 bits of ch) */
void tg_console_puts(tg_console_t *c, const char *text);        /* a string; no newline is added */
int tg_console_printf(tg_console_t *c, const char *format, ...) __attribute__((format(printf, 2, 3)));
int tg_console_vprintf(tg_console_t *c, const char *format, va_list args);   /* both return how many characters were written */

#endif
