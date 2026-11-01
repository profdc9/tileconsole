/* SPDX-License-Identifier: MIT */
/*
 * consoleio: the console API of PicoParanoia (Daniel Marks' terminal programs, the editor and the menus), on top of tilegfx's text screen, console and
 * ANSI mode, and the keyboard of the platform layer. The functions are the same as those of PicoParanoia's consoleio.h, so programs written for it
 * (editor.c among them) build unchanged.
 *
 * Output is ANSI text: the console is in TG_CONSOLE_ANSI mode with TG_CONSOLE_LF_ONLY, so \n is a plain line feed and the programs send \r\n, as they
 * do for a terminal, and with TG_CONSOLE_NOWRAP, so a line that is too long is written over the last column and does not wrap. Input is a byte stream like a terminal's: Enter is \r, and the keys that are not characters arrive as the escape sequences the
 * editor looks for (the cursor keys as ESC [ A..D, and so on: see console_expand_key). A bare Escape key is the single byte 27.
 *
 * The program that uses this chooses where keys come from and what a frame is: console_use_platform() takes them from plat_getchar and shows the
 * screen with plat_present and plat_wait_frame, and tests give their own with console_set_hooks().
 */
#ifndef CONSOLEIO_H
#define CONSOLEIO_H

#include <stddef.h>

#include "tg_console.h"

#define MY_ITOASIZE 40

#ifdef __cplusplus
extern "C" {
#endif

int  console_inchar(void);      /* the next byte of input, or -1 if there is none */
int  console_getch(void);       /* wait for a byte of input */
int  console_waiting(void);     /* 1 if a byte of input is already there, else 0 (tilegame's: the editor tells a lone Escape from a key sequence by it) */
void console_putch(char c);
void console_puts(const char *c);
void console_init(void);        /* the text screen, cleared, with the PC character set loaded */
void console_printcrlf(void);
void console_clreol(void);
void console_clrscr(void);
void console_gotoxy(int x, int y);   /* column x, row y, both counted from 1 (below 1 is 1) */
void console_highvideo(void);
void console_lowvideo(void);
void console_printint(int n);
void console_printuint(unsigned int n);
void console_delayframes(unsigned short fr);
/* (selectmenu and getstring ignore the keys that arrive as escape sequences, such as the cursor keys; PicoParanoia's took them for typing.) */
char console_selectmenu(const char *menutext, const char *options);
int  console_getstring(char *c, int len, int row, int col, int wrap);
void console_press_space(void);
int  console_yes(int row);
void console_print_array(const char *c, unsigned char *a, int n);
char *myltoa(char *p, long n);
long  mystrtol(const char *str, const char **end);
char *strcpy_n(char *dest, const char *src, size_t len);
char *strcat_n(char *dest, const char *src, size_t len);
size_t strlen_n(const char *d);

/* ---- what is not in PicoParanoia's: the connection to the platform ---- */

typedef struct {
    int (*getkey)(void);   /* the next typed character as plat_getchar gives it (keymap.h), or -1 */
    void (*frame)(void);   /* show the screen, and wait for the next frame: called while the program waits for a key or a delay */
} console_hooks_t;

void console_set_hooks(const console_hooks_t *hooks);
void console_use_platform(void);   /* hooks for the platform layer; shows the console as a text mode screen */
tg_text_t *console_screen(void);   /* the text screen and the console that console_init sets up, for whoever shows or inspects them */
tg_console_t *console_console(void);

/* The bytes that a key arrives as: the key as plat_getchar gives it in, the bytes (up to 4) in out, and how many there are (0: the key is ignored). */
int console_expand_key(int key, char out[4]);

#ifdef __cplusplus
}
#endif

#endif /* CONSOLEIO_H */
