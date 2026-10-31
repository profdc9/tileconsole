/* SPDX-License-Identifier: MIT */
/*
 * The logic of the console demo: a prompt with a line editor (cursor keys, Home/End, Backspace and Delete, Ctrl+A/E/K/U/L/C, history on the
 * up and down keys) and a few commands. It reads the characters of plat_getchar() and writes to a tg_console_t, and knows nothing else
 * about the platform, so that a test can type into it. See consdemo.c for the program around it.
 */
#ifndef CONSDEMO_LOGIC_H
#define CONSDEMO_LOGIC_H

#include <stdint.h>

#include "tg_console.h"

#define CONSDEMO_LINE_MAX 70
#define CONSDEMO_HISTORY 8

typedef struct {
    tg_console_t con;
    uint32_t (*millis)(void);                 /* the clock for the UPTIME command */
    int prompt_row;                           /* the row of the prompt that is being edited */
    char line[CONSDEMO_LINE_MAX + 1];
    char command[CONSDEMO_LINE_MAX + 1];      /* the line being run (here and not on the stack, which is small on the board) */
    int len, pos;
    char history[CONSDEMO_HISTORY][CONSDEMO_LINE_MAX + 1];
    int history_count;
    int browsing;                             /* -1: editing a new line; else the history entry now shown (0 = newest) */
    int keys_mode;                            /* the KEYS command: every key is shown rather than typed, until Escape */
} consdemo_t;

/* Set up a console on this screen (which must have its glyphs loaded), clear it, print the banner and the first prompt. */
void consdemo_init(consdemo_t *d, tg_text_t *screen, uint32_t (*millis)(void));

/* One typed character, as plat_getchar() gives them. */
void consdemo_char(consdemo_t *d, int ch);

#endif
