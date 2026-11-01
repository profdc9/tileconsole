/* SPDX-License-Identifier: MIT */
/*
 * The tilegame port of TDE (the Thomson-Davis Editor, public domain): what the editor's own sources need from the OS layer in os/. The editor is
 * built like its Unix version (-D__UNIX__) with -D__TILEGAME__, which takes the place of curses: the screen is a tilegfx text screen (80 x 25, CP437
 * glyphs, one 16-bit cell per character: the character in the low byte and the DOS attribute in the high byte), and the keys come from plat_getkey.
 */
#ifndef TDE_OS_H
#define TDE_OS_H

#include "tg_text.h"

/* A screen cell, as in the text mode of a PC: the character, and the attribute (foreground in the low nibble, background in the high one). */
typedef unsigned short chtype;
#define COLOR_PAIR(n) 0       /* (only in code that is compiled out) */
#define A_BOLD 0

/* The program that runs the editor gives it the screen and a function that shows what is on it (typically plat_present of a tg_t that has this screen);
 * call before tde_main. Without a screen the output goes nowhere. */
void tde_console_attach(tg_text_t *screen, void (*present)(void));

/* For the editor's own getkey while it waits: shows the screen and waits for the next frame (the program's choice: plat_wait_frame). */
void tde_console_set_idle(void (*idle)(void));

/* (the editor's Unix code saves and restores curses' cursor position round some output; our output does not move the cursor) */
#define stdscr 0
#define getyx(w, y, x) ((void)(w), (y) = 0, (x) = 0)
#define move(y, x) ((void)(y), (void)(x))
#define bkgdset(c) ((void)(c))

/* The editor throws away the keys typed ahead at some points (after a message, say). A program that feeds it keys on its own schedule (a test) can turn that off. */
void tde_console_set_flush(int enabled);

#include "platform.h"

/* The editor's key number, shift state and character for a key press (see console.c); 0 if the editor has no use for the key. Exposed for tests. */
int tde_event_key(const plat_key_event_t *event, int *shift_state, int *ch);

void refresh(void);   /* show the screen as it is now (curses' name for it, which the editor's Unix code uses) */

/* The editor's main(), renamed (-Dmain=tde_main for main.c). */
int tde_main(int argc, char **argv);

#endif
