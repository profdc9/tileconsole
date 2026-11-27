/* SPDX-License-Identifier: MIT */
#include "tl_env.h"

#include "tl_game.h"
#include "tl_io.h"
#include "tl_sound.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "platform.h"
#include "tilefs.h"

#define ATTR_TEXT TG_TEXT_ATTR(TG_TEXT_LIGHT_GREY, TG_TEXT_BLACK)
#define ATTR_HEAD TG_TEXT_ATTR(TG_TEXT_YELLOW, TG_TEXT_BLACK)
#define ATTR_ERROR TG_TEXT_ATTR(TG_TEXT_LIGHT_RED, TG_TEXT_BLACK)
#define ATTR_OK TG_TEXT_ATTR(TG_TEXT_LIGHT_GREEN, TG_TEXT_BLACK)
#define POLL_MS 16
#define PAGE_ROWS 22
#define COUNT_HOOK 10000   /* without ticks from the machine: Lua instructions between looks at the keys */

static tl_env_t *cur;            /* Lua's hook carries no pointer of its own */
static volatile int running;     /* a line or a program is running in cur->L: tl_env_tick may set the hook */

tl_env_t *tl_env_current(void)
{
    return cur;
}

/* ---- the screen ---- */

static void with(tl_env_t *e, uint8_t attr, const char *text)
{
    const uint8_t was = e->con.attr;
    tg_console_set_attr(&e->con, attr);
    tg_console_puts(&e->con, text);
    tg_console_set_attr(&e->con, was);
}

static void say(tl_env_t *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(tl_env_t *e, const char *fmt, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    tg_console_puts(&e->con, text);
}

static void problem(tl_env_t *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void problem(tl_env_t *e, const char *fmt, ...)
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

void tl_env_problem(tl_env_t *e, const char *fmt, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    problem(e, "%s", text);
}

/* ---- keys ---- */

/* The oldest of the keys typed while a program ran, taken out; -1 if there are none. */
static int take_typeahead(tl_env_t *e)
{
    if (!e->typeahead_count)
        return -1;
    const int ch = e->typeahead[0];
    memmove(e->typeahead, e->typeahead + 1, sizeof(int) * (size_t)(--e->typeahead_count));
    return ch;
}

/* The next key: from what was typed while a program ran, then from the keyboard, waiting (and showing the screen) if there is none. */
static int next_key(tl_env_t *e)
{
    for (;;) {
        int ch = take_typeahead(e);
        if (ch >= 0)
            return ch;
        ch = e->hooks.getchar();
        if (ch >= 0)
            return ch;
        tl_sound_tick(e);
        e->hooks.frame();
    }
}

/* ---- reading a line, with the editing keys of the console demo ---- */

static void redraw(tl_env_t *e, int row, int col, const char *line, int pos, int shown)
{
    tg_console_gotoxy(&e->con, col, row);
    tg_console_printf(&e->con, "%-*s", shown, line);
    tg_console_gotoxy(&e->con, col + pos, row);
}

/* Reads a line into `out` (size bytes at most, and as many as fit on the screen line), with history if asked. 1 for Enter, 0 for Ctrl+C. */
static int read_line(tl_env_t *e, char *out, int size, int history)
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
                memmove(e->history[1], e->history[0], sizeof e->history[0] * (TL_HISTORY - 1));
                memcpy(e->history[0], out, (size_t)len + 1);
                if (e->history_count < TL_HISTORY)
                    e->history_count++;
            }
            return 1;
        case 3:   /* (Ctrl+C also stops the sound) */
            tl_sound_silence(e);
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

int tl_env_read_line(tl_env_t *e, char *out, int size)
{
    if (!read_line(e, out, size, 0)) {
        e->stop = 1;
        return 0;
    }
    return 1;
}

/* ---- a running program: the keys, Ctrl+C ---- */

/* Now and then shows the screen (or keeps the machine going, while the game's picture is up) and looks for Ctrl+C; other keys are kept for io.read. */
void tl_env_poll(tl_env_t *e)
{
    tl_sound_tick(e);
    const uint32_t now = e->hooks.millis();
    if (now - e->last_poll < POLL_MS)
        return;
    if (!e->game_active)
        e->hooks.frame();
    else if (e->hooks.poll)
        e->hooks.poll();
    int ch;
    while ((ch = e->hooks.getchar()) >= 0) {
        if (ch == 3)
            e->stop = 1;
        else if (e->typeahead_count < (int)(sizeof e->typeahead / sizeof *e->typeahead))
            e->typeahead[e->typeahead_count++] = ch;
    }
    e->last_poll = e->hooks.millis();
}

int tl_env_key(tl_env_t *e)
{
    int ch = take_typeahead(e);
    if (ch < 0 && (ch = e->hooks.getchar()) == 3) {
        e->stop = 1;
        ch = -1;
    }
    return ch;
}

/* Lua's hook: with ticks from the machine it was set by tl_env_tick for this one look; without, it comes every COUNT_HOOK instructions. Once
 * Ctrl+C is seen it comes at every instruction, call and return, so that a pcall in the program cannot keep the error from reaching the top: the
 * first time it is raised outside one, the program stops. */
static void on_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    tl_env_t *e = cur;
    if (!e->stop) {
        if (e->hooks.async_ticks)
            lua_sethook(L, NULL, 0, 0);
        tl_env_poll(e);
        if (!e->stop)
            return;
    }
    lua_sethook(L, on_hook, LUA_MASKCALL | LUA_MASKRET | LUA_MASKCOUNT, 1);
    luaL_error(L, "Break");
}

void tl_env_tick(void)
{
    if (running && cur && cur->L && cur->hooks.async_ticks)
        lua_sethook(cur->L, on_hook, LUA_MASKCALL | LUA_MASKRET | LUA_MASKCOUNT, 1);
}

/* ---- the Lua state ---- */

static int l_print(lua_State *L)
{
    tl_env_t *e = cur;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        if (i > 1)
            tg_console_putc(&e->con, '\t');
        size_t len;
        const char *s = luaL_tolstring(L, i, &len);
        for (size_t k = 0; k < len; k++)
            tg_console_putc(&e->con, (unsigned char)s[k]);
        lua_pop(L, 1);
    }
    tg_console_putc(&e->con, '\n');
    return 0;
}

static void *plain_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;
    (void)osize;
    if (nsize == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, nsize);
}

static int open_libs(lua_State *L)
{
    static const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base}, {LUA_COLIBNAME, luaopen_coroutine}, {LUA_TABLIBNAME, luaopen_table},
                                    {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8},
                                    {NULL, NULL}};
    for (const luaL_Reg *lib = libs; lib->func; lib++) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }
    lua_register(L, "print", l_print);
    tl_io_open(L);
    tl_game_open(L);
    tl_sound_open(L);
    return 0;
}

static void close_state(tl_env_t *e)
{
    tl_game_reset(e);
    tl_sound_reset(e);
    if (e->L) {
        running = 0;
        lua_close(e->L);
        e->L = NULL;
    }
    e->chunk_len = 0;
}

/* A fresh state: what the last one had (variables, files, the game's graphics) is gone. 1 if it worked. */
static int open_state(tl_env_t *e)
{
    close_state(e);
    lua_State *L = lua_newstate(e->hooks.lua_alloc ? e->hooks.lua_alloc : plain_alloc, NULL);
    if (!L) {
        problem(e, "Not enough memory for Lua");
        return 0;
    }
    lua_pushcfunction(L, open_libs);
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        problem(e, "Lua could not start: %s", lua_tostring(L, -1));
        lua_close(L);
        return 0;
    }
    e->L = L;
    return 1;
}

void tl_env_collect(tl_env_t *e)
{
    if (e->L)
        lua_gc(e->L, LUA_GCCOLLECT);   /* (does nothing if the collector is running: Lua checks) */
}

/* What an error says, or "Break" for Ctrl+C. 1 for an end that is not an error (os.exit). */
static int report(tl_env_t *e, lua_State *L)
{
    if (tl_io_is_exit(L, -1))
        return 1;
    if (e->stop) {
        problem(e, "Break");
        return 0;
    }
    const char *msg = lua_tostring(L, -1);
    if (!msg) {
        if (luaL_callmeta(L, -1, "__tostring") && lua_type(L, -1) == LUA_TSTRING)
            msg = lua_tostring(L, -1);
        else
            msg = lua_pushfstring(L, "(the error is a %s)", luaL_typename(L, -1));
    }
    problem(e, "Error: %s", msg);
    return 0;
}

#define ACTIVITY(e, what) do { if ((e)->hooks.activity) (e)->hooks.activity(what); } while (0)

/* Calls the function on the top of the stack with no arguments, keeping `nresults` results (LUA_MULTRET: all) on the stack. 1 if it ended
 * well; otherwise the error is shown and the stack is as it was before the function was pushed. */
static int call(tl_env_t *e, int nresults)
{
    lua_State *L = e->L;
    e->stop = 0;
    e->typeahead_count = 0;
    e->last_poll = e->hooks.millis();
    ACTIVITY(e, 2);
    if (!e->hooks.async_ticks)
        lua_sethook(L, on_hook, LUA_MASKCOUNT, COUNT_HOOK);
    running = 1;
    const int r = lua_pcall(L, 0, nresults, 0);
    running = 0;
    lua_sethook(L, NULL, 0, 0);
    if (e->stop)   /* Ctrl+C stops the sound too (a program that ends, even with an error, leaves what it started playing) */
        tl_sound_silence(e);
    ACTIVITY(e, 1);
    e->game_active = 0;   /* (the text screen is back) */
    if (e->con.col != 0)
        tg_console_putc(&e->con, '\n');
    if (r == LUA_OK)
        return 1;
    const int fine = report(e, L);
    lua_settop(L, 0);
    tl_env_collect(e);   /* (what it built before the error is garbage now: give it back at once, for what is not Lua's and shares its memory) */
    return fine;
}

/* ---- files ---- */

static int file_exists(const char *path)
{
    fs_stat_t st;
    return fs_stat(path, &st) == 0 && !st.is_dir;
}

int tl_env_load(tl_env_t *e, const char *path)
{
    char full[TL_NAME_MAX + 5];
    snprintf(full, sizeof full, "%s", path);
    if (!file_exists(full) && !strchr(path, '.') && strlen(path) < TL_NAME_MAX - 4)   /* "LOAD game" finds game.lua */
        strcat(full, ".lua");
    fs_stat_t st;
    int r = fs_stat(full, &st);
    if (r < 0)
        return r;
    if (st.is_dir)
        return FS_EISDIR;
    if (st.size > TL_PROGRAM_MAX)   /* too big: what is in memory stays as it is */
        return -100;
    const int fd = fs_open(full, FS_RDONLY);
    if (fd < 0)
        return fd;
    size_t n = 0;
    while ((r = fs_read(fd, e->program + n, TL_PROGRAM_MAX + 1 - n)) > 0)
        n += (size_t)r;
    fs_close(fd);
    if (r < 0 || n > TL_PROGRAM_MAX) {   /* a failure part way (or a file that grew): the program was overwritten, so there is none now */
        e->length = 0;
        e->program[0] = 0;
        e->name[0] = 0;
        return r < 0 ? r : -100;
    }
    e->program[n] = 0;
    e->length = n;
    snprintf(e->name, sizeof e->name, "%.*s", TL_NAME_MAX, full);
    e->cart[0] = 0;   /* (RUN game.zip sets it again after loading main.lua) */
    return 0;
}

/* Writes the program to a temporary file and then in place of the old one, so that a failure leaves the old file as it was. */
static int save_file(tl_env_t *e, const char *path)
{
    char tmp[TL_NAME_MAX + 8];
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

static int more(tl_env_t *e, int *rows)
{
    if (++*rows < PAGE_ROWS)
        return 1;
    *rows = 0;
    with(e, ATTR_HEAD, "-- more --");
    const int ch = next_key(e);
    tg_console_puts(&e->con, "\r          \r");
    return ch != 27 && ch != 3 && ch != 'q' && ch != 'Q';
}

static void cmd_list(tl_env_t *e, const char *args)
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

static void cmd_files(tl_env_t *e, const char *args)
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

static void cmd_run(tl_env_t *e)
{
    if (!e->length) {
        problem(e, "No program: LOAD one, or NEW and EDIT it.");
        return;
    }
    if (!open_state(e))
        return;
    char chunkname[TL_NAME_MAX + 8];
    snprintf(chunkname, sizeof chunkname, "%s%.*s", e->name[0] ? "@" : "=", TL_NAME_MAX, e->name[0] ? e->name : "program");
    if (luaL_loadbuffer(e->L, e->program, e->length, chunkname) != LUA_OK) {
        problem(e, "Error: %s", lua_tostring(e->L, -1));
        lua_settop(e->L, 0);
        return;
    }
    if (call(e, 0))
        with(e, ATTR_OK, "Ready.\n");
}

/* RUN: in the archive the program came from, if it did (the current folder is the archive's while it runs, and as it was afterwards) */
static void cmd_run_here(tl_env_t *e)
{
    if (!e->cart[0] || !e->length) {
        cmd_run(e);
        return;
    }
    char back[FS_PATH_MAX + 1];
    snprintf(back, sizeof back, "%s", fs_getcwd());
    const int r = fs_chdir(e->cart);
    if (r < 0) {
        problem(e, "Cannot go to %s: %s (UNMOUNT took it away? RUN the archive again)", e->cart, fs_strerror(r));
        return;
    }
    cmd_run(e);
    if (fs_chdir(back) < 0)
        fs_chdir("/");
}

static int is_zip_name(const char *path)
{
    const size_t n = strlen(path);
    return n > 4 && !strcasecmp(path + n - 4, ".zip");
}

/* RUN file: a program loaded and run; RUN game.zip: the archive mounted at /ar and its main.lua run there, as a cartridge */
static void cmd_run_file(tl_env_t *e, const char *path)
{
    if (!is_zip_name(path)) {
        const int r = tl_env_load(e, path);
        if (r == -100)
            problem(e, "That file is too big: the most is %d bytes.", TL_PROGRAM_MAX);
        else if (r < 0)
            problem(e, "Cannot load %s: %s", path, fs_strerror(r));
        else
            cmd_run(e);
        return;
    }
    close_state(e);   /* (a file the last program left open in an archive at /ar would keep it from being replaced) */
    char why[120];
    if (tl_io_mount(e, path, TL_ARCHIVE_DEFAULT, why, sizeof why) < 0) {
        problem(e, "Cannot mount %s: %s", path, why);
        return;
    }
    const int r = tl_env_load(e, "/" TL_ARCHIVE_DEFAULT "/main.lua");
    if (r < 0) {
        if (r == FS_ENOENT)
            problem(e, "%s has no main.lua: it is mounted at /%s (FILES /%s shows what is in it).", path, TL_ARCHIVE_DEFAULT, TL_ARCHIVE_DEFAULT);
        else
            problem(e, "Cannot load main.lua from %s: %s", path, r == -100 ? "too big" : fs_strerror(r));
        return;
    }
    snprintf(e->cart, sizeof e->cart, "/%s", TL_ARCHIVE_DEFAULT);
    cmd_run_here(e);
}

/* MOUNT: the archives mounted; MOUNT archive [name]: one more (at /ar if no name) */
static void cmd_mount(tl_env_t *e, char *args)
{
    if (!*args) {
        const char *path;
        int i = 0;
        for (const char *name; (name = tl_io_archive(i, &path)) != NULL; i++) {
            if (i == 0)
                with(e, ATTR_HEAD, "Archives:\n");
            say(e, "  /%-15s %s\n", name, path);
        }
        if (i == 0)
            say(e, "No archives mounted. MOUNT game.zip puts one at /%s.\n", TL_ARCHIVE_DEFAULT);
        return;
    }
    char *name = args;
    while (*name && !isspace((unsigned char)*name))
        name++;
    if (*name) {
        *name++ = 0;
        while (isspace((unsigned char)*name))
            name++;
    }
    if (!*name)
        name = TL_ARCHIVE_DEFAULT;
    char why[120];
    if (tl_io_mount(e, args, name, why, sizeof why) < 0)
        problem(e, "Cannot mount %s: %s", args, why);
    else
        say(e, "%s is at /%s.\n", args, *name == '/' ? name + 1 : name);
}

/* Prints what a line typed at the prompt gave back (as Lua's own interpreter does: 2 + 2 prints 4). */
static void print_results(tl_env_t *e, lua_State *L)
{
    const int n = lua_gettop(L);
    if (n == 0)
        return;
    lua_pushcfunction(L, l_print);
    lua_insert(L, 1);
    if (lua_pcall(L, n, 0, 0) != LUA_OK)
        report(e, L);
    lua_settop(L, 0);
}

/*
 * A line typed at the prompt is Lua, run at once in the state the earlier lines used. An expression is shown ("2 + 2", or "= 2 + 2"). A statement that is
 * not finished on the line (a "for" without its "end", a string that goes on) goes on at the next line, with the prompt ">>"; Ctrl+C there gives it up.
 */
static void cmd_lua(tl_env_t *e, const char *line)
{
    if (!e->L && !open_state(e))
        return;
    lua_State *L = e->L;
    const size_t len = strlen(line);
    if (e->chunk_len + len + 2 > sizeof e->chunk) {
        problem(e, "That statement is too long to type at the prompt: put it in a program");
        e->chunk_len = 0;
        return;
    }
    if (e->chunk_len)
        e->chunk[e->chunk_len++] = '\n';
    memcpy(e->chunk + e->chunk_len, line, len + 1);
    e->chunk_len += len;
    if (e->chunk_len == len) {   /* (the first line: as an expression first) */
        const char *expr = line[0] == '=' ? line + 1 : line;
        lua_pushfstring(L, "return %s;", expr);
        const int r = luaL_loadbuffer(L, lua_tostring(L, -1), lua_rawlen(L, -1), "=line");
        lua_remove(L, -2);
        if (r == LUA_OK) {
            e->chunk_len = 0;
            if (call(e, LUA_MULTRET))
                print_results(e, L);
            return;
        }
        lua_pop(L, 1);
    }
    const int r = luaL_loadbuffer(L, e->chunk, e->chunk_len, "=line");
    if (r == LUA_ERRSYNTAX) {
        size_t mlen;
        const char *msg = lua_tolstring(L, -1, &mlen);
        if (mlen >= 5 && !strcmp(msg + mlen - 5, "<eof>")) {   /* not finished: the next line goes on with it */
            lua_pop(L, 1);
            return;
        }
    }
    e->chunk_len = 0;
    if (r != LUA_OK) {
        problem(e, "Error: %s", lua_tostring(L, -1));
        lua_settop(L, 0);
        return;
    }
    call(e, 0);
}

static void trim(char *s)
{
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = 0;
}

static void show_help(tl_env_t *e)
{
    with(e, ATTR_HEAD, "Commands:\n");
    say(e, "  HELP             this list              CLS              clear the screen\n");
    say(e, "  NEW              forget the program     LIST [from [to]] show the program\n");
    say(e, "  RUN [file]       run the program, or load the file and run it\n");
    say(e, "  RUN game.zip     mount the zip archive at /ar and run its main.lua there\n");
    say(e, "  MOUNT zip [name] the archive's files under /ar (or /name); MOUNT lists\n");
    say(e, "  UNMOUNT [name]   let go of an archive   Ctrl+C           stop a program\n");
    say(e, "  LOAD file        load a program         SAVE [file]      save the program\n");
    say(e, "  FILES [folder]   list a folder          KILL file        delete a file\n");
    say(e, "  CD [folder]      change folder          EDIT [file]      edit a program\n");
    say(e, "  STACK            stack used so far      HEAP             memory left\n");
    say(e, "  VIDEO            picture losses seen, saved in VIDEO.LOG (board only)\n");
    say(e, "  USBLOG           save the USB log to the card\n");
    say(e, "  SCRATCH [MARK]   watchdog registers at start; MARK fills 1-7 with a test pattern\n");
    say(e, "  VER              version                BYE              leave (board: back to the launcher)\n");
    say(e, "Anything else is Lua and runs at once: print(2 + 2)   x = 5   x * x   for i = 1, 3 do print(i) end\n");
    say(e, "Variables stay from line to line; RUN and NEW start afresh. A statement can go on over several lines.\n");
    say(e, "Editing a line: arrows, Home, End, Backspace, Delete, Ctrl+A/E/K/U, Up and Down for history.\n");
}

/* Is the line a command? The word must be one of them and what follows must not make it Lua: "list = 7", "run + 1" and "run()" are Lua. */
static int command_word(const char *line, const char *args, char *word, size_t size)
{
    const size_t wl = (size_t)(args - line);
    if (wl == 0 || wl >= size)
        return 0;
    for (size_t i = 0; i < wl; i++) {
        if (!isalpha((unsigned char)line[i]))
            return 0;
        word[i] = (char)toupper((unsigned char)line[i]);
    }
    word[wl] = 0;
    const char *rest = args;
    while (isspace((unsigned char)*rest))
        rest++;
    if (*rest && strchr("=+-*%^#<>~,:;[({\"'", *rest))   /* (an operator or a bracket after it: "list = 7", "run + 1"; a file name may start with a dot or a slash) */
        return 0;
    static const char *const words[] = {"HELP", "CLS", "NEW", "LIST", "RUN", "LOAD", "SAVE", "MOUNT", "UNMOUNT", "SCRATCH", "STACK", "HEAP", "USBLOG", "VIDEO", "FILES",
                                        "DIR", "LS", "KILL", "DEL", "CD", "EDIT", "VER", "VERSION", "BYE", "QUIT", "EXIT", NULL};
    for (int i = 0; words[i]; i++)
        if (!strcmp(word, words[i]))
            return 1;
    return 0;
}

/* Runs one line. Returns -1 to stay at the prompt, else the exit code. */
static int run_line(tl_env_t *e, char *line)
{
    if (e->chunk_len) {   /* the rest of a statement typed over several lines */
        cmd_lua(e, line);
        return -1;
    }
    while (isspace((unsigned char)*line))
        line++;
    trim(line);
    if (!*line)
        return -1;
    char *args = line;
    while (*args && !isspace((unsigned char)*args))
        args++;
    char word[16];
    if (!command_word(line, args, word, sizeof word)) {
        cmd_lua(e, line);
        return -1;
    }
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
        e->cart[0] = 0;
        open_state(e);
    } else if (!strcmp(word, "LIST")) {
        cmd_list(e, args);
    } else if (!strcmp(word, "RUN")) {
        if (*args)
            cmd_run_file(e, args);
        else
            cmd_run_here(e);
    } else if (!strcmp(word, "MOUNT")) {
        cmd_mount(e, args);
    } else if (!strcmp(word, "UNMOUNT")) {
        const char *name = *args ? args : TL_ARCHIVE_DEFAULT;
        const int r = tl_io_unmount(e, name);
        if (r == FS_ENOENT)
            problem(e, "No archive is mounted at /%s.", *name == '/' ? name + 1 : name);
        else if (r < 0)
            problem(e, "Cannot unmount /%s: a file in it is open.", *name == '/' ? name + 1 : name);
    } else if (!strcmp(word, "LOAD")) {
        if (!*args) {
            problem(e, "Usage: LOAD file");
        } else if (is_zip_name(args)) {
            problem(e, "%s is an archive: RUN it (its main.lua), or MOUNT it.", args);
        } else {
            const int r = tl_env_load(e, args);
            if (r == -100)
                problem(e, "That file is too big: the most is %d bytes.", TL_PROGRAM_MAX);
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
                if (*args) {
                    snprintf(e->name, sizeof e->name, "%s", args);
                    e->cart[0] = 0;
                }
                say(e, "Saved %s, %lu bytes.\n", target, (unsigned long)e->length);
            }
        }
    } else if (!strcmp(word, "SCRATCH")) {
        char text[160];
        if (e->hooks.info && e->hooks.info(!strcasecmp(args, "MARK") ? "scratchmark" : "scratch", text, sizeof text))
            say(e, "%s\n", text);
        else
            problem(e, "No scratch registers on this machine");
    } else if (!strcmp(word, "STACK") || !strcmp(word, "HEAP") || !strcmp(word, "USBLOG") || !strcmp(word, "VIDEO")) {
        char text[160];
        const char *what = word[0] == 'S' ? "stack" : word[0] == 'H' ? "heap" : word[0] == 'V' ? "video" : "usblog";
        if (word[0] == 'H')
            tl_env_collect(e);   /* (what is free once the garbage is gone) */
        const int got = e->hooks.info && e->hooks.info(what, text, sizeof text);
        if (got)
            say(e, "%s\n", text);
        if (word[0] == 'H' && e->L)
            say(e, "Lua is using %d KB.\n", lua_gc(e->L, LUA_GCCOUNT, 0));
        else if (!got)
            problem(e, "No %s on this machine", what);
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
            if (!strchr(target, '.') && strlen(target) < TL_NAME_MAX - 4 && !file_exists(target))   /* a new program is a .lua file */
                strcat(e->edit_name, ".lua");
            return TL_EXIT_EDIT;
        }
    } else if (!strcmp(word, "VER") || !strcmp(word, "VERSION")) {
        say(e, "%s, with 32-bit integers and floats\n", LUA_RELEASE);
    } else if (!strcmp(word, "BYE") || !strcmp(word, "QUIT") || !strcmp(word, "EXIT")) {
        return TL_EXIT_BYE;
    }
    return -1;
}

/* ---- the environment ---- */

void tl_env_init(tl_env_t *e, tg_text_t *screen, const tl_hooks_t *hooks)
{
    memset(e, 0, sizeof *e);
    e->hooks = *hooks;
    cur = e;
    tl_io_forget_archives(e);
    tg_console_init(&e->con, screen, ATTR_TEXT);
    tg_console_clear(&e->con);
    with(e, ATTR_HEAD, "TILEGAME LUA");
    say(e, "  %s\n", LUA_RELEASE);
    say(e, "Type HELP for the commands, or Lua at once.\n\n");
    tl_sound_init(e);
    open_state(e);
}

int tl_env_run(tl_env_t *e)
{
    cur = e;
    for (;;) {
        if (!e->at_prompt)
            with(e, ATTR_OK, e->chunk_len ? ">> " : "] ");
        e->at_prompt = 1;
        tg_text_show_cursor(e->con.text, true);
        const int got = read_line(e, e->line, sizeof e->line, 1);
        e->at_prompt = 0;
        if (!got) {
            e->chunk_len = 0;   /* (Ctrl+C gives up a statement that was going on over several lines) */
            continue;
        }
        memcpy(e->command, e->line, sizeof e->command);
        const int r = run_line(e, e->command);
        if (r >= 0) {
            tl_sound_silence(e);   /* (the editor, or what comes after BYE, starts quiet) */
            tl_env_collect(e);     /* (and has what Lua no longer uses: the editor takes its memory from the same pool on the board) */
            e->exit_code = r;
            return r;
        }
    }
}

void tl_env_close(tl_env_t *e)
{
    close_state(e);
    if (cur == e)
        cur = NULL;
}
