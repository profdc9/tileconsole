/* SPDX-License-Identifier: MIT */
/*
 * The logic of the file editor demo: the editor of PicoParanoia (editor/editor.c) editing real files through tilefs, with a small menu around it to
 * open a file (or make a new one), list and change folders, view, delete and see information. The files can be on an SD card, a RAM disk or a folder of
 * the host: it only uses the tilefs API, with the current folder (fs_chdir) as the place names are looked up in. It uses the consoleio API and the editor,
 * so a test can run it with scripted keys; see editsd.c for the program around it.
 *
 * Opening a file first reads all of it to check that the editor can hold it without losing anything: at most EDITSD_FILE_MAX bytes, lines of at most
 * EDITSD_LINE_MAX characters, and text (no control characters other than tab, and a carriage return only before a line feed). A file with CR LF line ends
 * is shown with LF and saved with CR LF again. A save writes a temporary file beside the original (NAME.TMP) and then replaces the original with it, so a
 * failure (a full disk, a card taken out) leaves the file as it was.
 */
#ifndef EDITSD_LOGIC_H
#define EDITSD_LOGIC_H

#include <stddef.h>

#ifndef EDITSD_FILE_MAX
#define EDITSD_FILE_MAX 0x7DF8   /* the largest file the editor takes whole: its buffer (editor.h AMAX) less the block buffer (BMAX) and a few bytes (found by trying: a file
                                  * of 32254 makes it stop and ask for a key). It also limits what a save can write, so a file it saved can be opened again. */
#endif
#define EDITSD_LINE_MAX 250      /* the editor's lines are at most 255 characters */

/* No file open, and the first run of the menu. Call once before editsd_menu. */
void editsd_init(void);

/* The menu: O pen, L ist, C hange folder, E dit, V iew, I nformation, D elete, Q uit. Returns when Q is chosen. console_init must have been called and the
 * file system mounted. */
void editsd_menu(void);

/* For tests: the open file's full path ("" if none) and how many saves were made. */
const char *editsd_path(void);
int editsd_saves(void);

#endif
