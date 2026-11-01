/* SPDX-License-Identifier: MIT */
/*
 * The logic of the editor demo: a file in memory that the editor of PicoParanoia (editor/editor.c) edits, and a small menu around it. It uses only the
 * consoleio API and the editor, so a test can run it with scripted keys; see editdemo.c for the program around it.
 */
#ifndef EDITDEMO_LOGIC_H
#define EDITDEMO_LOGIC_H

#include <stddef.h>

#define EDITDEMO_FILE_MAX 0x6000   /* the largest file: a bit less than the editor's own buffer (editor.h AMAX) */

/* The file: to start with, a few lines about the editor. */
void editdemo_reset(void);

/* The text of the file (not ended with a zero byte: see the length), and its length in bytes. */
const char *editdemo_file(void);
size_t editdemo_length(void);
void editdemo_set(const char *text, size_t length);   /* for tests: put this text in the file (cut to EDITDEMO_FILE_MAX) */
int editdemo_saves(void);                             /* how many times the editor has saved the file */

/* The menu: E dit, V iew, N ew, I nformation, Q uit. Returns when Q is chosen. console_init must have been called. */
void editdemo_menu(void);

#endif
