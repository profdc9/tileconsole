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
 * Any other byte, including the ones the PC shows as faces and card suits, is drawn. A character that does not fit on the line goes to
 * the start of the next one, scrolling if need be. The wrap is put off until the next character is drawn, so a line of exactly 80
 * characters followed by \n does not leave a blank line. With TG_CONSOLE_NOWRAP the characters past the right edge are dropped instead.
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
#define TG_CONSOLE_PRINTF_MAX 256

typedef struct {
    tg_text_t *text;
    int col, row;   /* where the next character goes; col is TG_TEXT_COLS while a wrap is waiting for the next character */
    uint8_t attr;   /* the colours it is written in, and the rows that scroll in at the bottom are blank in */
    uint8_t flags;  /* TG_CONSOLE_* */
    char work[TG_CONSOLE_PRINTF_MAX];
} tg_console_t;

/* Attach a console to a text screen, at the top left. It does not touch the screen (see tg_console_clear). */
void tg_console_init(tg_console_t *c, tg_text_t *text, uint8_t attr);

void tg_console_clear(tg_console_t *c);                         /* blank the screen in the current colours and go to the top left */
void tg_console_set_attr(tg_console_t *c, uint8_t attr);        /* TG_TEXT_ATTR(foreground, background) */
void tg_console_gotoxy(tg_console_t *c, int col, int row);      /* clamped to the screen */

void tg_console_putc(tg_console_t *c, int ch);                  /* one byte (the low 8 bits of ch) */
void tg_console_puts(tg_console_t *c, const char *text);        /* a string; no newline is added */
int tg_console_printf(tg_console_t *c, const char *format, ...) __attribute__((format(printf, 2, 3)));
int tg_console_vprintf(tg_console_t *c, const char *format, va_list args);   /* both return how many characters were written */

#endif
