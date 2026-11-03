/* SPDX-License-Identifier: MIT */
#include "basic_env.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "my_basic/my_basic.h"
#include "platform.h"
#include "tilefs.h"

#define ATTR_TEXT TG_TEXT_ATTR(TG_TEXT_LIGHT_GREY, TG_TEXT_BLACK)
#define ATTR_HEAD TG_TEXT_ATTR(TG_TEXT_YELLOW, TG_TEXT_BLACK)
#define ATTR_ERROR TG_TEXT_ATTR(TG_TEXT_LIGHT_RED, TG_TEXT_BLACK)
#define ATTR_OK TG_TEXT_ATTR(TG_TEXT_LIGHT_GREEN, TG_TEXT_BLACK)
#define POLL_MS 16
#define PAGE_ROWS 22

static basic_env_t *cur;   /* MY-BASIC's callbacks carry no pointer of their own */

/* ---- the screen ---- */

static void with(basic_env_t *e, uint8_t attr, const char *text)
{
    const uint8_t was = e->con.attr;
    tg_console_set_attr(&e->con, attr);
    tg_console_puts(&e->con, text);
    tg_console_set_attr(&e->con, was);
}

static void say(basic_env_t *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(basic_env_t *e, const char *fmt, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    tg_console_puts(&e->con, text);
}

static void problem(basic_env_t *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void problem(basic_env_t *e, const char *fmt, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (e->con.col != 0)
        tg_console_putc(&e->con, '\n');
    with(e, ATTR_ERROR, text);
    tg_console_putc(&e->con, '\n');
}

/* ---- keys ---- */

/* The next key: from what was typed while a program ran, then from the keyboard, waiting (and showing the screen) if there is none. */
static int next_key(basic_env_t *e)
{
    for (;;) {
        if (e->typeahead_count) {
            const int ch = e->typeahead[0];
            memmove(e->typeahead, e->typeahead + 1, sizeof(int) * (size_t)(--e->typeahead_count));
            return ch;
        }
        const int ch = e->hooks.getchar();
        if (ch >= 0)
            return ch;
        e->hooks.frame();
    }
}

/* ---- reading a line, with the editing keys of the console demo ---- */

static void redraw(basic_env_t *e, int row, int col, const char *line, int pos, int shown)
{
    tg_console_gotoxy(&e->con, col, row);
    tg_console_printf(&e->con, "%-*s", shown, line);
    tg_console_gotoxy(&e->con, col + pos, row);
}

/* Reads a line into `out` (size bytes at most, and as many as fit on the screen line), with history if asked. 1 for Enter, 0 for Ctrl+C. */
static int read_line(basic_env_t *e, char *out, int size, int history)
{
    const int row = e->con.row, col = e->con.col;
    int limit = TG_TEXT_COLS - col - 1;
    int len = 0, pos = 0, browsing = -1;
    if (limit > size - 1)
        limit = size - 1;
    if (limit < 1)
        limit = 1;
    out[0] = 0;
    tg_text_show_cursor(e->con.text, true);
    for (;;) {
        const int ch = next_key(e);
        switch (ch) {
        case 10:
            tg_console_gotoxy(&e->con, col + len, row);
            tg_console_putc(&e->con, '\n');
            if (history && len && !(e->history_count && !strcmp(e->history[0], out))) {
                memmove(e->history[1], e->history[0], sizeof e->history[0] * (BASIC_HISTORY - 1));
                memcpy(e->history[0], out, (size_t)len + 1);
                if (e->history_count < BASIC_HISTORY)
                    e->history_count++;
            }
            return 1;
        case 3:
            tg_console_gotoxy(&e->con, col + len, row);
            tg_console_puts(&e->con, "^C\n");
            out[0] = 0;
            return 0;
        case 8:
            if (pos > 0) {
                memmove(out + pos - 1, out + pos, (size_t)(len - pos + 1));
                pos--;
                len--;
                redraw(e, row, col, out, pos, len + 1);
            }
            break;
        case 127:
            if (pos < len) {
                memmove(out + pos, out + pos + 1, (size_t)(len - pos));
                len--;
                redraw(e, row, col, out, pos, len + 1);
            }
            break;
        case PLAT_KEY_LEFT:
            if (pos > 0)
                tg_console_gotoxy(&e->con, col + --pos, row);
            break;
        case PLAT_KEY_RIGHT:
            if (pos < len)
                tg_console_gotoxy(&e->con, col + ++pos, row);
            break;
        case PLAT_KEY_HOME: case 1:
            pos = 0;
            tg_console_gotoxy(&e->con, col, row);
            break;
        case PLAT_KEY_END: case 5:
            pos = len;
            tg_console_gotoxy(&e->con, col + pos, row);
            break;
        case 11:
            out[pos] = 0;
            len = pos;
            redraw(e, row, col, out, pos, limit + 1);
            break;
        case 21:
            out[0] = 0;
            len = pos = 0;
            redraw(e, row, col, out, pos, limit + 1);
            break;
        case PLAT_KEY_UP: case PLAT_KEY_DOWN:
            if (history) {
                int to = browsing + (ch == PLAT_KEY_UP ? 1 : -1);
                if (to >= e->history_count)
                    to = e->history_count - 1;
                if (to < -1)
                    to = -1;
                if (to != browsing || to == -1) {
                    browsing = to;
                    const int old = len;
                    snprintf(out, (size_t)size, "%s", to < 0 ? "" : e->history[to]);
                    if ((int)strlen(out) > limit)
                        out[limit] = 0;
                    len = pos = (int)strlen(out);
                    redraw(e, row, col, out, pos, old > len ? old + 1 : len + 1);
                }
            }
            break;
        default:
            if (ch >= ' ' && ch < 127 && len < limit) {
                memmove(out + pos + 1, out + pos, (size_t)(len - pos + 1));
                out[pos++] = (char)ch;
                len++;
                redraw(e, row, col, out, pos, len);
            }
            break;
        }
    }
}

/* ---- MY-BASIC's side ---- */

static int on_print(struct mb_interpreter_t *s, const char *fmt, ...)
{
    char text[1024];
    va_list ap;
    (void)s;
    va_start(ap, fmt);
    const int n = vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    tg_console_puts(&cur->con, text);
    return n;
}

static int on_input(struct mb_interpreter_t *s, const char *prompt, char *buf, int size)
{
    (void)s;
    (void)prompt;   /* (printed by the interpreter) */
    tg_text_show_cursor(cur->con.text, true);
    if (!read_line(cur, buf, size, 0)) {
        cur->stop = 1;
        buf[0] = 0;
    }
    return (int)strlen(buf);
}

static void on_error(struct mb_interpreter_t *s, mb_error_e err, const char *msg, const char *func, int pos, unsigned short row, unsigned short col, int abort_code)
{
    (void)s;
    (void)pos;
    (void)abort_code;
    if (err == SE_NO_ERR)
        return;
    problem(cur, "Error: %s%s%s%s", msg ? msg : mb_get_error_desc(err), func && *func ? " (" : "", func && *func ? func : "", func && *func ? ")" : "");
    if (row)
        problem(cur, "  in line %d, column %d", (int)row, (int)col);
}

/* Called before every statement of a running program: now and then shows the screen and looks for Ctrl+C (other keys are kept for INPUT). */
static int on_step(struct mb_interpreter_t *s, void **ast, const char *source, int pos, unsigned short row, unsigned short col)
{
    (void)s;
    (void)ast;
    (void)source;
    (void)pos;
    (void)row;
    (void)col;
    basic_env_t *e = cur;
    if (e->stop)
        return MB_FUNC_BYE;
    const uint32_t now = e->hooks.millis();
    if (now - e->last_poll >= POLL_MS) {
        e->hooks.frame();
        int ch;
        while ((ch = e->hooks.getchar()) >= 0) {
            if (ch == 3)
                e->stop = 1;
            else if (e->typeahead_count < (int)(sizeof e->typeahead / sizeof *e->typeahead))
                e->typeahead[e->typeahead_count++] = ch;
        }
        e->last_poll = e->hooks.millis();
        if (e->stop)
            return MB_FUNC_BYE;
    }
    return MB_FUNC_OK;
}

/* ---- files ---- */

static int file_exists(const char *path)
{
    fs_stat_t st;
    return fs_stat(path, &st) == 0 && !st.is_dir;
}

int basic_env_load(basic_env_t *e, const char *path)
{
    char full[BASIC_NAME_MAX + 5];
    snprintf(full, sizeof full, "%s", path);
    if (!file_exists(full) && !strchr(path, '.') && strlen(path) < BASIC_NAME_MAX - 4) {   /* "LOAD game" finds game.bas */
        strcat(full, ".bas");
    }
    fs_stat_t st;
    int r = fs_stat(full, &st);
    if (r < 0)
        return r;
    if (st.is_dir)
        return FS_EISDIR;
    if (st.size > BASIC_PROGRAM_MAX)   /* too big: what is in memory stays as it is */
        return -100;
    const int fd = fs_open(full, FS_RDONLY);
    if (fd < 0)
        return fd;
    size_t n = 0;
    while ((r = fs_read(fd, e->program + n, BASIC_PROGRAM_MAX + 1 - n)) > 0)
        n += (size_t)r;
    fs_close(fd);
    if (r < 0 || n > BASIC_PROGRAM_MAX) {   /* a failure part way (or a file that grew): the program was overwritten, so there is none now */
        e->length = 0;
        e->program[0] = 0;
        e->name[0] = 0;
        return r < 0 ? r : -100;
    }
    e->program[n] = 0;
    e->length = n;
    snprintf(e->name, sizeof e->name, "%s", full);
    return 0;
}

/* Writes the program to a temporary file and then in place of the old one, so that a failure leaves the old file as it was. */
static int save_file(basic_env_t *e, const char *path)
{
    char tmp[BASIC_NAME_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.TMP", path);
    int fd = fs_open(tmp, FS_WRONLY | FS_CREAT | FS_TRUNC);
    if (fd < 0)
        return fd;
    size_t done = 0;
    int err = 0;
    while (done < e->length && !err) {
        const int w = fs_write(fd, e->program + done, e->length - done);
        if (w < 0)
            err = w;
        else
            done += (size_t)w;
    }
    const int closed = fs_close(fd);
    if (!err && closed < 0)
        err = closed;
    if (!err) {
        fs_stat_t st;
        if (fs_stat(path, &st) == 0)
            err = fs_unlink(path);
        if (!err)
            err = fs_rename(tmp, path);
    }
    if (err)
        fs_unlink(tmp);
    return err;
}

/* ---- the commands ---- */

static int more(basic_env_t *e, int *rows)
{
    if (++*rows < PAGE_ROWS)
        return 1;
    *rows = 0;
    with(e, ATTR_HEAD, "-- more --");
    const int ch = next_key(e);
    tg_console_puts(&e->con, "\r          \r");
    return ch != 27 && ch != 3 && ch != 'q' && ch != 'Q';
}

static void cmd_list(basic_env_t *e, const char *args)
{
    int first = 1, last = 1 << 30;
    const int got = sscanf(args, "%d %d", &first, &last);
    if (got == 1)
        last = first;
    else if (got <= 0)
        first = 1;
    if (!e->length) {
        say(e, "No program.\n");
        return;
    }
    int rows = 0, n = 1;
    for (const char *p = e->program; *p;) {
        const char *eol = strchr(p, '\n');
        const size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (n >= first && n <= last) {
            say(e, "%4d  %.*s\n", n, (int)(len > 72 ? 72 : len), p);
            if (!more(e, &rows))
                return;
        }
        n++;
        p += len + (eol != NULL);
    }
}

static void cmd_files(basic_env_t *e, const char *args)
{
    fs_dirent_t ent;
    const int dd = fs_opendir(*args ? args : fs_getcwd());
    if (dd < 0) {
        problem(e, "Cannot list %s: %s", *args ? args : fs_getcwd(), fs_strerror(dd));
        return;
    }
    int rows = 0, count = 0, going = 1;
    while (going && fs_readdir(dd, &ent) == 1) {
        count++;
        if (ent.is_dir)
            say(e, "  %-40.40s <DIR>\n", ent.name);
        else
            say(e, "  %-40.40s %9lu\n", ent.name, (unsigned long)ent.size);
        going = more(e, &rows);
    }
    fs_closedir(dd);
    if (!count)
        say(e, "  (empty)\n");
}

static void cmd_run(basic_env_t *e)
{
    if (!e->length) {
        problem(e, "No program: LOAD one, or NEW and EDIT it.");
        return;
    }
    e->stop = 0;
    e->typeahead_count = 0;
    e->last_poll = e->hooks.millis();
    mb_reset(&e->bas, false, true);
    int r = mb_load_string(e->bas, e->program, true);
    if (r == MB_FUNC_OK)
        r = mb_run(e->bas, true);
    if (e->con.col != 0)
        tg_console_putc(&e->con, '\n');
    if (e->stop)
        problem(e, "Break");
    else if (r == MB_FUNC_OK || r == MB_FUNC_END || r == MB_FUNC_BYE)
        with(e, ATTR_OK, "Ready.\n");
}

static void cmd_immediate(basic_env_t *e, const char *line)
{
    e->stop = 0;
    e->typeahead_count = 0;
    e->last_poll = e->hooks.millis();
    /*
     * Each line is a program of its own: MY-BASIC cannot run a line on its own among the variables of an earlier one (a string variable may be pointing into the
     * text of the line it was assigned in, which is freed when the next is loaded), so the variables go with the line. ":" puts several statements on one.
     */
    mb_reset(&e->bas, false, true);
    int r = mb_load_string(e->bas, line, false);
    if (r == MB_FUNC_OK)
        r = mb_run(e->bas, true);
    if (e->con.col != 0)
        tg_console_putc(&e->con, '\n');
    if (e->stop)
        problem(e, "Break");
}

static void trim(char *s)
{
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = 0;
}

static void show_help(basic_env_t *e)
{
    with(e, ATTR_HEAD, "Commands:\n");
    say(e, "  HELP             this list              CLS              clear the screen\n");
    say(e, "  NEW              forget the program     LIST [from [to]] show the program\n");
    say(e, "  RUN              run the program        Ctrl+C           stop a running program\n");
    say(e, "  LOAD file        load a program         SAVE [file]      save the program\n");
    say(e, "  FILES [folder]   list a folder          KILL file        delete a file\n");
    say(e, "  CD [folder]      change folder          EDIT [file]      edit a program\n");
    say(e, "  STACK            stack used so far      HEAP             memory left\n");
    say(e, "  VER              version                BYE              leave\n");
    say(e, "Anything else is BASIC and runs at once, each line on its own: PRINT 2 + 2     A = 5 : PRINT A * A\n");
    say(e, "In MY-BASIC PRINT a, b stays on the line and a semicolon ends it: PRINT a; PRINT a, \" \", b;\n");
    say(e, "Editing a line: arrows, Home, End, Backspace, Delete, Ctrl+A/E/K/U, Up and Down for history.\n");
}

/* Runs one line. Returns -1 to stay at the prompt, else the exit code. */
static int run_line(basic_env_t *e, char *line)
{
    while (isspace((unsigned char)*line))
        line++;
    trim(line);
    if (!*line)
        return -1;
    char *args = line;
    while (*args && !isspace((unsigned char)*args))
        args++;
    char word[16];
    const size_t wl = (size_t)(args - line) < sizeof word - 1 ? (size_t)(args - line) : sizeof word - 1;
    for (size_t i = 0; i < wl; i++)
        word[i] = (char)toupper((unsigned char)line[i]);
    word[wl] = 0;
    while (isspace((unsigned char)*args))
        args++;

    if (!strcmp(word, "HELP")) {
        show_help(e);
    } else if (!strcmp(word, "CLS")) {
        tg_console_clear(&e->con);
    } else if (!strcmp(word, "NEW")) {
        e->length = 0;
        e->program[0] = 0;
        e->name[0] = 0;
    } else if (!strcmp(word, "LIST")) {
        cmd_list(e, args);
    } else if (!strcmp(word, "RUN")) {
        cmd_run(e);
    } else if (!strcmp(word, "LOAD")) {
        if (!*args) {
            problem(e, "Usage: LOAD file");
        } else {
            const int r = basic_env_load(e, args);
            if (r == -100)
                problem(e, "That file is too big: the most is %d bytes.", BASIC_PROGRAM_MAX);
            else if (r < 0)
                problem(e, "Cannot load %s: %s", args, fs_strerror(r));
            else
                say(e, "Loaded %s, %lu bytes.\n", e->name, (unsigned long)e->length);
        }
    } else if (!strcmp(word, "SAVE")) {
        const char *target = *args ? args : e->name;
        if (!*target) {
            problem(e, "Usage: SAVE file");
        } else {
            const int r = save_file(e, target);
            if (r < 0) {
                problem(e, "Cannot save %s: %s", target, fs_strerror(r));
            } else {
                if (*args)
                    snprintf(e->name, sizeof e->name, "%s", args);
                say(e, "Saved %s, %lu bytes.\n", target, (unsigned long)e->length);
            }
        }
    } else if (!strcmp(word, "STACK") || !strcmp(word, "HEAP")) {
        char text[160];
        const char *what = word[0] == 'S' ? "stack" : "heap";
        if (e->hooks.info && e->hooks.info(what, text, sizeof text))
            say(e, "%s\n", text);
        else
            problem(e, "No %s figure on this machine", what);
    } else if (!strcmp(word, "FILES") || !strcmp(word, "DIR") || !strcmp(word, "LS")) {
        cmd_files(e, args);
    } else if (!strcmp(word, "KILL") || !strcmp(word, "DEL")) {
        if (!*args) {
            problem(e, "Usage: KILL file");
        } else {
            say(e, "Delete %s? (Y/N) ", args);
            const int ch = toupper(next_key(e));
            tg_console_putc(&e->con, '\n');
            if (ch == 'Y') {
                const int r = fs_unlink(args);
                if (r < 0)
                    problem(e, "Cannot delete %s: %s", args, fs_strerror(r));
            }
        }
    } else if (!strcmp(word, "CD")) {
        if (!*args) {
            say(e, "%s\n", fs_getcwd());
        } else {
            const int r = fs_chdir(args);
            if (r < 0)
                problem(e, "Cannot go to %s: %s", args, fs_strerror(r));
        }
    } else if (!strcmp(word, "EDIT")) {
        const char *target = *args ? args : e->name;
        if (!*target) {
            problem(e, "Usage: EDIT file");
        } else {
            snprintf(e->edit_name, sizeof e->edit_name, "%s", target);
            if (!strchr(target, '.') && strlen(target) < BASIC_NAME_MAX - 4 && !file_exists(target))   /* a new program is a .bas file */
                strcat(e->edit_name, ".bas");
            return BASIC_EXIT_EDIT;
        }
    } else if (!strcmp(word, "VER") || !strcmp(word, "VERSION")) {
        say(e, "MY-BASIC %s\n", mb_ver_string());
    } else if (!strcmp(word, "BYE") || !strcmp(word, "QUIT") || !strcmp(word, "EXIT")) {
        return BASIC_EXIT_BYE;
    } else {
        cmd_immediate(e, line);
    }
    return -1;
}

/* ---- the environment ---- */

void basic_env_init(basic_env_t *e, tg_text_t *screen, const basic_hooks_t *hooks)
{
    memset(e, 0, sizeof *e);
    e->hooks = *hooks;
    cur = e;
    tg_console_init(&e->con, screen, ATTR_TEXT);
    tg_console_clear(&e->con);
    mb_init();
    mb_open(&e->bas);
    mb_set_printer(e->bas, on_print);
    mb_set_inputer(e->bas, on_input);
    mb_set_error_handler(e->bas, on_error);
    mb_debug_set_stepped_handler(e->bas, on_step, NULL);
    with(e, ATTR_HEAD, "TILEGAME BASIC");
    say(e, "  MY-BASIC %s\n", mb_ver_string());
    say(e, "Type HELP for the commands, or BASIC at once.\n\n");
}

int basic_env_run(basic_env_t *e)
{
    cur = e;
    for (;;) {
        if (!e->at_prompt)
            with(e, ATTR_OK, "] ");
        e->at_prompt = 1;
        tg_text_show_cursor(e->con.text, true);
        const int got = read_line(e, e->line, sizeof e->line, 1);
        e->at_prompt = 0;
        if (!got)
            continue;
        memcpy(e->command, e->line, sizeof e->command);
        const int r = run_line(e, e->command);
        if (r >= 0) {
            e->exit_code = r;
            return r;
        }
    }
}

void basic_env_close(basic_env_t *e)
{
    if (e->bas)
        mb_close(&e->bas);
    mb_dispose();
}
