/* SPDX-License-Identifier: MIT */
#include "editsd_logic.h"

#include <stdio.h>
#include <string.h>

#include "consoleio.h"
#include "editor.h"
#include "tilefs.h"

#define PROMPT_ROW 17

_Static_assert(EDITSD_FILE_MAX == AMAX - BMAX - 8, "the file limit follows the editor's buffer");

static char path[FS_PATH_MAX + 1];   /* the open file, absolute; "" if none */
static int is_new;                   /* it does not exist yet: the first save makes it */
static int crlf;                     /* its lines end with CR LF, and are saved so */
static int saves;
static const char *save_error;       /* why the last save failed, if it did */

/* What the editor reads through rf and writes through wf. */
static int rd_fd = -1;
static unsigned char rd_buf[512];
static int rd_len, rd_pos;

static int wr_fd = -1;
static char wr_tmp[FS_PATH_MAX + 8];
static unsigned char wr_buf[512];
static int wr_len;
static int wr_failed;
static int writing;

void editsd_init(void)
{
    path[0] = 0;
    is_new = crlf = saves = 0;
    save_error = NULL;
    rd_fd = wr_fd = -1;
    writing = 0;
}

const char *editsd_path(void) { return path; }
int editsd_saves(void) { return saves; }

/* ---- a message and a question, at the bottom of the screen ---- */

static void message(const char *a, const char *b)
{
    console_puts("\r\n");
    console_puts(a);
    if (b)
        console_puts(b);
    console_press_space();
}

/* A name as typed, as an absolute path without "." and ".." (tilefs's reading of it): "" if it cannot be one. */
static void cwd_with(const char *name, char *out, size_t size)
{
    if (fs_normalise(name, out, size) < 0)
        out[0] = 0;
}

/* ---- checking a file before the editor gets it ---- */

/* Reads the whole file: returns NULL if the editor can have it, or the reason it cannot. Sets *is_crlf (its first line ends with CR LF), *bytes and *lines. */
static const char *check_file(const char *file, long *bytes, long *lines, int *is_crlf)
{
    static char why[80];
    static unsigned char buf[512];   /* (static, like the other buffers here: the board's stack is small) */
    const int fd = fs_open(file, FS_RDONLY);
    if (fd < 0) {
        snprintf(why, sizeof why, "cannot read it: %s", fs_strerror(fd));
        return why;
    }
    long total = 0, nl = 0, col = 0, first_end = -1;
    int prev = 0, n;
    const char *result = NULL;
    while (!result && (n = fs_read(fd, buf, sizeof buf)) > 0) {
        for (int i = 0; i < n && !result; i++) {
            const int c = buf[i];
            total++;
            if (c == '\n') {
                nl++;
                if (first_end < 0)
                    first_end = prev == '\r';
                col = 0;
            } else if (c == '\r') {
                if (prev == '\r') {
                    snprintf(why, sizeof why, "a carriage return alone, at %ld", total - 2);
                    result = why;
                }
                /* only allowed before a line feed, which the next byte shows; it is not a character of the line */
            } else if (c == 0 || (c < ' ' && c != '\t') || c == 127) {
                snprintf(why, sizeof why, "not a text file (byte %d at %ld)", c, total - 1);
                result = why;
            } else {
                if (prev == '\r') {
                    snprintf(why, sizeof why, "a carriage return alone, at %ld", total - 2);
                    result = why;
                }
                col++;
            }
            if (!result && col > EDITSD_LINE_MAX) {
                snprintf(why, sizeof why, "line %ld is longer than %d characters", nl + 1, EDITSD_LINE_MAX);
                result = why;
            }
            prev = c;
        }
    }
    fs_close(fd);
    if (!result && n < 0) {
        snprintf(why, sizeof why, "cannot read it: %s", fs_strerror(n));
        result = why;
    }
    if (!result && total + (total > 0 && prev != '\n') > EDITSD_FILE_MAX) {   /* (the editor ends the last line with a line feed if it has none) */
        snprintf(why, sizeof why, "too big for the editor: %ld bytes, the most is %d", total, EDITSD_FILE_MAX);
        result = why;
    }
    if (!result && prev == '\r') {
        snprintf(why, sizeof why, "a carriage return alone, at the end");
        result = why;
    }
    *is_crlf = !result && first_end == 1;
    *bytes = total;
    *lines = nl + (!result && total > 0 && prev != '\n');
    return result;
}

/* ---- the editor's file callbacks ---- */

static int reader(void *v)
{
    (void)v;
    for (;;) {
        if (rd_pos >= rd_len) {
            if (rd_fd < 0)
                return 0;
            rd_len = fs_read(rd_fd, rd_buf, sizeof rd_buf);
            rd_pos = 0;
            if (rd_len <= 0) {   /* the end (or an error: the file was checked, so it is the end): let go of the file so that a save can replace it */
                fs_close(rd_fd);
                rd_fd = -1;
                rd_len = 0;
                return 0;
            }
        }
        const int c = rd_buf[rd_pos++];
        if (c != '\r')
            return c;
    }
}

static void put_byte(int c)
{
    if (wr_len == (int)sizeof wr_buf) {
        if (!wr_failed && fs_write(wr_fd, wr_buf, sizeof wr_buf) != (int)sizeof wr_buf)
            wr_failed = 1;
        wr_len = 0;
    }
    wr_buf[wr_len++] = (unsigned char)c;
}

static void finish_save(void)
{
    if (!wr_failed && wr_len && fs_write(wr_fd, wr_buf, (size_t)wr_len) != wr_len)
        wr_failed = 1;
    const int closed = fs_close(wr_fd);
    wr_fd = -1;
    wr_len = 0;
    writing = 0;
    if (closed < 0)
        wr_failed = 1;
    if (!wr_failed) {   /* the new file is complete: it takes the old one's place */
        fs_stat_t st;
        if (fs_stat(path, &st) == 0 && fs_unlink(path) < 0)
            wr_failed = 1;
        if (!wr_failed && fs_rename(wr_tmp, path) < 0) {
            wr_failed = 1;
            save_error = "the file could not be replaced: the new text is in the .TMP file next to it";
            return;
        }
    }
    if (wr_failed) {
        fs_unlink(wr_tmp);
        save_error = "the card or disk failed (full?): the file is as it was before";
        return;
    }
    is_new = 0;
    saves++;
}

static int writer(int c, void *v)
{
    (void)v;
    if (!writing) {   /* the first byte of a save: start the temporary file */
        writing = 1;
        wr_failed = 0;
        wr_len = 0;
        save_error = NULL;
        if (strlen(path) + 5 > FS_PATH_MAX) {
            wr_failed = 1;
            wr_fd = -1;
        } else {
            snprintf(wr_tmp, sizeof wr_tmp, "%s.TMP", path);
            wr_fd = fs_open(wr_tmp, FS_WRONLY | FS_CREAT | FS_TRUNC);
            if (wr_fd < 0)
                wr_failed = 1;
        }
    }
    if (c == -1) {   /* the end of the save */
        if (wr_fd < 0) {
            writing = 0;
            save_error = "the file could not be written";
            return 0;
        }
        finish_save();
    } else if (c > 0 && wr_fd >= 0) {
        if (c == '\n' && crlf)
            put_byte('\r');
        put_byte(c);
    }
    return 0;
}

/* ---- the menu's actions ---- */

static void edit_open_file(void)
{
    if (!path[0]) {
        message("No file is open: choose O first.", NULL);
        return;
    }
    rd_fd = -1;
    rd_len = rd_pos = 0;
    if (!is_new) {
        rd_fd = fs_open(path, FS_RDONLY);
        if (rd_fd < 0) {
            message("Cannot open the file: ", fs_strerror(rd_fd));
            return;
        }
    }
    rf = reader;
    rf_ptr = NULL;
    wf = writer;
    wf_ptr = NULL;
    writing = 0;
    save_error = NULL;
    editor();
    if (rd_fd >= 0) {
        fs_close(rd_fd);
        rd_fd = -1;
    }
    if (save_error) {
        console_clrscr();
        console_puts("SAVE FAILED: ");
        console_puts(save_error);
        console_press_space();
    }
}

/* Asks for a name at the bottom of the screen; returns its length, 0 if nothing was typed. */
static int ask(const char *what, char *out, int size)
{
    console_gotoxy(1, PROMPT_ROW);
    console_puts(what);
    const int col = (int)strlen(what) + 1;
    const int n = console_getstring(out, size - 1 > 79 - col ? 79 - col : size - 1, PROMPT_ROW, col, 80 - col);
    console_puts("\r\n");
    return n;
}

static void open_file(void)
{
    static char name[FS_PATH_MAX + 1], full[FS_PATH_MAX + 1];
    fs_stat_t st;
    if (!ask("File to open or make: ", name, 72))
        return;
    cwd_with(name, full, sizeof full);
    const int e = full[0] ? fs_stat(full, &st) : FS_EBADNAME;
    if (e == 0 && st.is_dir) {
        message("That is a folder: use C to go into it.", NULL);
        return;
    }
    if (e == 0) {
        long bytes, lines;
        int file_crlf;
        const char *why = check_file(full, &bytes, &lines, &file_crlf);
        if (why) {
            message("The editor cannot take this file: ", why);
            return;
        }
        is_new = 0;
        crlf = file_crlf;
    } else if (e == FS_ENOENT) {
        static char parent[FS_PATH_MAX + 4];
        snprintf(parent, sizeof parent, "%s/..", full);
        if (fs_stat(parent, &st) != 0 || !st.is_dir) {
            message("The folder it should be in does not exist.", NULL);
            return;
        }
        console_puts("There is no such file. Make it? (Y/N) ");
        if (console_selectmenu("", "YN") != 'Y')
            return;
        is_new = 1;
        crlf = 0;
    } else {
        message("Cannot use that name: ", fs_strerror(e));
        return;
    }
    snprintf(path, sizeof path, "%s", full);
    edit_open_file();
}

/* A page of text at a time: returns 0 if the user stopped it (Q). */
static int page_break(int *row)
{
    if (++*row < 22)
        return 1;
    console_puts("-- space for more, Q to stop --");
    int c;
    do {
        c = console_getch();
    } while (c != ' ' && c != 'q' && c != 'Q');
    console_clrscr();
    *row = 0;
    return c == ' ';
}

static void list_folder(void)
{
    fs_dirent_t e;
    const int dd = fs_opendir(fs_getcwd());
    console_clrscr();
    if (dd < 0) {
        message("Cannot list the folder: ", fs_strerror(dd));
        return;
    }
    console_puts(fs_getcwd());
    console_puts("\r\n");
    int row = 1, count = 0, going = 1;
    while (going && fs_readdir(dd, &e) == 1) {
        char line[96];
        count++;
        if (e.is_dir)
            snprintf(line, sizeof line, "  %-50.50s <DIR>", e.name);
        else
            snprintf(line, sizeof line, "  %-50.50s %9lu", e.name, (unsigned long)e.size);
        console_puts(line);
        console_puts("\r\n");
        going = page_break(&row);
    }
    fs_closedir(dd);
    if (!count)
        console_puts("  (empty)\r\n");
    console_press_space();
}

static void change_folder(void)
{
    static char name[FS_PATH_MAX + 1];
    if (!ask("Go to folder: ", name, 72))
        return;
    const int e = fs_chdir(name);
    if (e < 0)
        message("Cannot go there: ", fs_strerror(e));
}

static void view_file(void)
{
    long bytes, lines;
    static unsigned char buf[512];
    if (!path[0] || is_new) {
        message(path[0] ? "The file has not been saved yet." : "No file is open: choose O first.", NULL);
        return;
    }
    int file_crlf;
    const char *why = check_file(path, &bytes, &lines, &file_crlf);
    if (why) {
        message("Cannot show the file: ", why);
        return;
    }
    const int fd = fs_open(path, FS_RDONLY);
    if (fd < 0) {
        message("Cannot open the file: ", fs_strerror(fd));
        return;
    }
    console_clrscr();
    int row = 0, col = 0, n, going = 1;
    while (going && (n = fs_read(fd, buf, sizeof buf)) > 0) {
        for (int i = 0; i < n && going; i++) {
            const int c = buf[i];
            if (c == '\r')
                continue;
            if (c == '\n' || col >= 79) {
                console_printcrlf();
                col = 0;
                going = page_break(&row);
                if (c == '\n')
                    continue;
            }
            if (c == '\t') {
                do {
                    console_putch(' ');
                    col++;
                } while (col % 8 && col < 79);
            } else {
                console_putch((char)c);
                col++;
            }
        }
    }
    fs_close(fd);
    console_press_space();
}

static void information(void)
{
    long bytes = 0, lines = 0;
    uint64_t free_bytes = 0, total = 0;
    console_clrscr();
    console_puts("Folder: ");
    console_puts(fs_getcwd());
    console_puts("\r\nFile:   ");
    console_puts(path[0] ? path : "(none)");
    console_puts("\r\n");
    if (path[0] && !is_new) {
        int file_crlf;
        const char *why = check_file(path, &bytes, &lines, &file_crlf);
        if (why) {
            console_puts("\r\n  the editor cannot take it: ");
            console_puts(why);
            console_puts("\r\n");
        } else {
            console_puts("\r\n  bytes: ");
            console_printuint((unsigned)bytes);
            console_puts("\r\n  lines: ");
            console_printuint((unsigned)lines);
            console_puts(file_crlf ? "\r\n  line ends: CR LF (kept when saved)\r\n" : "\r\n  line ends: LF\r\n");
        }
    } else if (path[0]) {
        console_puts("\r\n  not saved yet\r\n");
    }
    console_puts("  saved: ");
    console_printint(saves);
    console_puts(" times in this session\r\n");
    if (fs_getcwd()[1] && fs_space(fs_getcwd(), &free_bytes, &total) == 0) {
        console_puts("  free:  ");
        console_printuint((unsigned)(free_bytes / 1024));
        console_puts(" KB of ");
        console_printuint((unsigned)(total / 1024));
        console_puts(" KB\r\n");
    }
    console_press_space();
}

static void delete_file(void)
{
    static char name[FS_PATH_MAX + 1], full[FS_PATH_MAX + 1];
    fs_stat_t st;
    if (!ask("File to delete: ", name, 72))
        return;
    cwd_with(name, full, sizeof full);
    const int e = full[0] ? fs_stat(full, &st) : FS_EBADNAME;
    if (e < 0) {
        message("Cannot find it: ", fs_strerror(e));
        return;
    }
    if (st.is_dir) {
        message("That is a folder: it is not deleted here.", NULL);
        return;
    }
    console_puts("Delete it? It cannot be undone. (Y/N) ");
    if (console_selectmenu("", "YN") != 'Y')
        return;
    const int r = fs_unlink(full);
    if (r < 0) {
        message("Cannot delete it: ", fs_strerror(r));
        return;
    }
    if (strcmp(full, path) == 0) {
        path[0] = 0;
        is_new = 0;
    }
}

void editsd_menu(void)
{
    for (;;) {
        console_clrscr();
        console_highvideo();
        console_puts("EDITSD");
        console_lowvideo();
        console_puts("  the editor of PicoParanoia on files\r\n\r\n");
        console_puts("  Folder: ");
        console_puts(fs_getcwd());
        console_puts("\r\n  File:   ");
        console_puts(path[0] ? path : "(none)");
        console_puts(is_new ? "  (new)" : "");
        console_puts("\r\n\r\n");
        console_puts("  O  open a file, or make a new one\r\n  L  list the folder\r\n  C  change folder\r\n  E  edit the open file\r\n");
        console_puts("  V  view the open file\r\n  I  information\r\n  D  delete a file\r\n  Q  quit\r\n\r\n");
        const char choice = console_selectmenu("Choice: ", "OLCEVIDQ");
        switch (choice) {
        case 'O': open_file(); break;
        case 'L': list_folder(); break;
        case 'C': change_folder(); break;
        case 'E': edit_open_file(); break;
        case 'V': view_file(); break;
        case 'I': information(); break;
        case 'D': delete_file(); break;
        default: return;
        }
    }
}
