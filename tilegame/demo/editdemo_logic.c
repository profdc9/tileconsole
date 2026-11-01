/* SPDX-License-Identifier: MIT */
#include "editdemo_logic.h"

#include <stdio.h>
#include <string.h>

#include "consoleio.h"
#include "editor.h"

static char file[EDITDEMO_FILE_MAX];
static size_t length;
static size_t read_pos, write_len;
static int writing;
static int saves;

static const char welcome[] =
    "This is the editor of PicoParanoia, running on the 80 x 25 text mode.\n"
    "\n"
    "Keys:\n"
    "  arrows, Home, End, Page Up, Page Down   move\n"
    "  Delete, Backspace                       delete\n"
    "  Insert                                  overwrite or insert\n"
    "  F1  (Ctrl-\\)                            help for the Ctrl keys\n"
    "  F2                                      record a macro, Ctrl-Z ends it\n"
    "  Ctrl-C                                  play the macro\n"
    "  Ctrl-F                                  search\n"
    "  Ctrl-W                                  save and leave\n"
    "\n"
    "The file lives in memory. Leave with Ctrl-W, answer Y to save it,\n"
    "and choose V on the menu to look at it, or E to edit it again.\n";

void editdemo_set(const char *text, size_t n)
{
    if (n > EDITDEMO_FILE_MAX)
        n = EDITDEMO_FILE_MAX;
    memcpy(file, text, n);
    length = n;
}

void editdemo_reset(void)
{
    editdemo_set(welcome, sizeof welcome - 1);
    saves = 0;
}

const char *editdemo_file(void) { return file; }
size_t editdemo_length(void) { return length; }
int editdemo_saves(void) { return saves; }

/* What the editor reads and writes through: the bytes of the file, and 0 at its end; and the bytes it saves, ended by 0 and then -1. */
static int reader(void *v)
{
    (void)v;
    return read_pos < length ? (unsigned char)file[read_pos++] : 0;
}

static int writer(int c, void *v)
{
    (void)v;
    if (!writing) {   /* the first byte of a save: the old file is not wanted any more (the editor has it all in its own buffer) */
        writing = 1;
        write_len = 0;
    }
    if (c == -1) {    /* the end of the save */
        length = write_len;
        writing = 0;
        saves++;
    } else if (c > 0 && write_len < EDITDEMO_FILE_MAX) {
        file[write_len++] = (char)c;
    }
    return 0;
}

static void view(void)
{
    console_clrscr();
    int row = 1;
    for (size_t i = 0; i < length; i++) {
        if (file[i] == '\n') {
            console_printcrlf();
            if (++row >= 23) {
                console_puts("-- space for more --");
                while (console_getch() != ' ')
                    ;
                console_clrscr();
                row = 1;
            }
        } else if (file[i] >= ' ' && file[i] <= '~') {
            console_putch(file[i]);
        }
    }
    console_press_space();
}

static void information(void)
{
    size_t lines = 0;
    for (size_t i = 0; i < length; i++)
        lines += file[i] == '\n';
    console_clrscr();
    console_puts("The file in memory\r\n\r\n  bytes: ");
    console_printuint((unsigned)length);
    console_puts("\r\n  lines: ");
    console_printuint((unsigned)lines);
    console_puts("\r\n  saved: ");
    console_printint(saves);
    console_puts(" times\r\n  room:  ");
    console_printuint((unsigned)(EDITDEMO_FILE_MAX - length));
    console_puts(" bytes more\r\n");
    console_press_space();
}

void editdemo_menu(void)
{
    for (;;) {
        console_clrscr();
        console_highvideo();
        console_puts("EDITDEMO");
        console_lowvideo();
        console_puts("  the editor of PicoParanoia on an 80 x 25 text screen\r\n\r\n");
        console_puts("  E  edit the file\r\n  V  view the file\r\n  N  new, empty file\r\n  I  information\r\n  Q  quit\r\n\r\n");
        const char choice = console_selectmenu("Choice: ", "EVNIQ");
        if (choice == 'E') {
            rf = reader;
            rf_ptr = NULL;
            wf = writer;
            wf_ptr = NULL;
            read_pos = 0;
            writing = 0;
            editor();
        } else if (choice == 'V') {
            view();
        } else if (choice == 'N') {
            length = 0;
        } else if (choice == 'I') {
            information();
        } else {
            return;
        }
    }
}
