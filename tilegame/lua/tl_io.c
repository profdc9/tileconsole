/* SPDX-License-Identifier: MIT */
#include "tl_io.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "lauxlib.h"
#include "lua.h"
#include "tilefs.h"
#include "tl_env.h"

#define FILE_META "tilegame.file"
#define READ_AHEAD 256

/* An open file: reads go through a small buffer (a byte at a time from the card would be slow); a write or a seek first gives back what was read ahead. */
typedef struct {
    int fd;                      /* -1 once closed */
    int rpos, rlen;
    unsigned char buf[READ_AHEAD];
} tl_file_t;

static char exit_key;            /* what os.exit raises: its address */

/* ---- zip archives ---- */

#define TL_ARCHIVES (FS_MAX_MOUNTS - 1)   /* (the card takes one mount) */

/* A mounted archive, in one block from the alloc hook (the PSRAM on the board), with the archive's directory after it */
typedef struct {
    fs_zip_t z;
    char name[FS_MOUNT_NAME_MAX + 1];
    char path[FS_PATH_MAX + 1];
} archive_t;

static archive_t *archives[TL_ARCHIVES];   /* NULL: a free slot */

static void *archive_mem(tl_env_t *e, size_t n) { return e->hooks.alloc ? e->hooks.alloc(n, 0) : malloc(n); }

static void archive_free(tl_env_t *e, int i)
{
    if (e->hooks.release)
        e->hooks.release(archives[i]);
    else
        free(archives[i]);
    archives[i] = NULL;
}

static int archive_named(const char *name)
{
    if (*name == '/')
        name++;
    for (int i = 0; i < TL_ARCHIVES; i++)
        if (archives[i] && !strcasecmp(archives[i]->name, name))
            return i;
    return -1;
}

int tl_io_unmount(tl_env_t *e, const char *name)
{
    const int i = archive_named(name);
    if (i < 0)
        return FS_ENOENT;
    int r = fs_unmount(archives[i]->name);
    if (r == FS_EBUSY) {   /* (a file a program left open, which nothing refers to any more, is closed by the collector) */
        tl_env_collect(e);
        r = fs_unmount(archives[i]->name);
    }
    if (r < 0 && r != FS_ENOENT)
        return r;
    if (r == 0)   /* (not mounted any more: the file system was reset, and its handle on the archive closed with everything else) */
        fs_zip_close(&archives[i]->z);
    archive_free(e, i);
    return 0;
}

int tl_io_mount(tl_env_t *e, const char *archive, const char *name, char *why, size_t n)
{
    char full[FS_PATH_MAX + 1];
    int r = fs_normalise(archive, full, sizeof full);
    if (*name == '/')
        name++;
    if (r < 0) {
        snprintf(why, n, "%s", fs_strerror(r));
        return r;
    }
    const int old = archive_named(name);
    if (old >= 0 && !strcasecmp(archives[old]->path, full))
        return 0;   /* (mounted there already) */
    if (old >= 0 && (r = tl_io_unmount(e, name)) < 0) {
        snprintf(why, n, "/%s is in use (a file in it is open)", archives[old]->name);
        return r;
    }
    int slot = -1;
    for (int i = 0; i < TL_ARCHIVES && slot < 0; i++)
        if (!archives[i])
            slot = i;
    fs_zip_t probe;
    if (slot < 0 || (r = fs_zip_open(&probe, full)) < 0) {
        r = slot < 0 ? FS_ENOMEM : r;
        snprintf(why, n, "%s", slot < 0 ? "too many archives mounted" : r == FS_ENODEV ? "not a zip archive" : r == FS_ENOTSUP ? "a ZIP64 archive (over 4 GB)" : fs_strerror(r));
        return r;
    }
    archive_t *a = archive_mem(e, sizeof *a + probe.index_bytes);
    if (!a) {
        fs_zip_close(&probe);
        snprintf(why, n, "not enough memory for its directory (%lu bytes)", (unsigned long)probe.index_bytes);
        return FS_ENOMEM;
    }
    a->z = probe;   /* (nothing in it points into it yet: no file in the archive is open) */
    snprintf(a->name, sizeof a->name, "%s", strlen(name) < sizeof a->name ? name : "");
    snprintf(a->path, sizeof a->path, "%s", full);
    archives[slot] = a;
    r = fs_zip_load(&a->z, a + 1);
    if (r == 0)
        r = fs_mount(name, &fs_zip_ops, &a->z);
    if (r < 0) {
        snprintf(why, n, "%s", r == FS_ENODEV ? "a broken zip archive" : r == FS_EEXIST ? "that name is taken (and not by an archive)"
                               : r == FS_EINVAL ? "not a name for a mount (one name, up to 15 letters)" : r == FS_ENOMEM ? "too many mounts" : fs_strerror(r));
        fs_zip_close(&a->z);
        archive_free(e, slot);
        return r;
    }
    return 0;
}

const char *tl_io_archive(int i, const char **path)
{
    for (int k = 0; k < TL_ARCHIVES; k++)
        if (archives[k] && i-- == 0) {
            *path = archives[k]->path;
            return archives[k]->name;
        }
    return NULL;
}

void tl_io_forget_archives(tl_env_t *e)
{
    for (int i = 0; i < TL_ARCHIVES; i++)
        if (archives[i])
            archive_free(e, i);
}

/* mount(archive [, name]): the zip archive's files under /name (/ar if not given); true, or nil and what went wrong */
static int l_mount(lua_State *L)
{
    const char *archive = luaL_checkstring(L, 1), *name = luaL_optstring(L, 2, TL_ARCHIVE_DEFAULT);
    char why[120];
    const int r = tl_io_mount(tl_env_current(), archive, name, why, sizeof why);
    if (r < 0) {
        luaL_pushfail(L);
        lua_pushfstring(L, "cannot mount %s: %s", archive, why);
        lua_pushinteger(L, r);
        return 3;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* unmount([name]): the archive at /name (/ar) goes; true, or nil and what went wrong */
static int l_unmount(lua_State *L)
{
    const char *name = luaL_optstring(L, 1, TL_ARCHIVE_DEFAULT);
    const int r = tl_io_unmount(tl_env_current(), name);
    if (r < 0) {
        luaL_pushfail(L);
        lua_pushfstring(L, r == FS_ENOENT ? "no archive is mounted at /%s" : "cannot unmount /%s: a file in it is open", *name == '/' ? name + 1 : name);
        lua_pushinteger(L, r);
        return 3;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int tl_io_is_exit(lua_State *L, int idx)
{
    return lua_islightuserdata(L, idx) && lua_touserdata(L, idx) == &exit_key;
}

/* nil, "name: what went wrong", the code: what a function gives back when it fails, as Lua's own do */
static int fail(lua_State *L, const char *name, int code)
{
    luaL_pushfail(L);
    if (name)
        lua_pushfstring(L, "%s: %s", name, fs_strerror(code));
    else
        lua_pushstring(L, fs_strerror(code));
    lua_pushinteger(L, code);
    return 3;
}

/* ---- the file object ---- */

static tl_file_t *to_file(lua_State *L, int i)
{
    tl_file_t *f = luaL_checkudata(L, i, FILE_META);
    if (f->fd < 0)
        luaL_error(L, "attempt to use a closed file");
    return f;
}

static int fill(tl_file_t *f)
{
    if (f->rpos < f->rlen)
        return 1;
    const int r = fs_read(f->fd, f->buf, sizeof f->buf);
    f->rpos = 0;
    f->rlen = r > 0 ? r : 0;
    return r;
}

/* The next byte, -1 at the end (or on an error). */
static int next_byte(tl_file_t *f)
{
    return fill(f) > 0 ? f->buf[f->rpos++] : -1;
}

/* Before a write or a seek: the file position goes back to what the program has read. */
static int give_back(tl_file_t *f)
{
    const int ahead = f->rlen - f->rpos;
    f->rpos = f->rlen = 0;
    return ahead > 0 ? (int)fs_seek(f->fd, -ahead, FS_SEEK_CUR) : 0;
}

static int read_line(lua_State *L, tl_file_t *f, int keep_end)
{
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    int c = -1, any = 0;
    while ((c = next_byte(f)) >= 0) {
        any = 1;
        if (c == '\n')
            break;
        luaL_addchar(&b, (char)c);
    }
    if (c == '\n' && keep_end)
        luaL_addchar(&b, '\n');
    luaL_pushresult(&b);
    return any;
}

static void read_all(lua_State *L, tl_file_t *f)
{
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    while (fill(f) > 0) {
        luaL_addlstring(&b, (const char *)f->buf + f->rpos, (size_t)(f->rlen - f->rpos));
        f->rpos = f->rlen;
    }
    luaL_pushresult(&b);
}

static int read_count(lua_State *L, tl_file_t *f, lua_Integer n)
{
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    lua_Integer got = 0;
    while (got < n && fill(f) > 0) {
        lua_Integer take = f->rlen - f->rpos;
        if (take > n - got)
            take = n - got;
        luaL_addlstring(&b, (const char *)f->buf + f->rpos, (size_t)take);
        f->rpos += (int)take;
        got += take;
    }
    luaL_pushresult(&b);
    return got > 0 || (n == 0 && fill(f) > 0);
}

/* A number written as Lua writes them (after spaces): pushes it and returns 1, or returns 0. */
static int read_number(lua_State *L, tl_file_t *f)
{
    char text[64];
    int n = 0, c;
    do
        c = next_byte(f);
    while (c == ' ' || c == '\t' || c == '\n' || c == '\r');
    while (c >= 0 && n < (int)sizeof text - 1 && strchr("0123456789+-.xXaAbBcCdDeEfFpP", c)) {
        text[n++] = (char)c;
        c = next_byte(f);
    }
    if (c >= 0)
        f->rpos--;   /* (the byte after the number stays to be read) */
    text[n] = 0;
    return n > 0 && lua_stringtonumber(L, text) != 0;
}

/* Reads the formats from index `first` on ("l", "L", "n", "a" or a count; "*l" as older Lua wrote it): pushes one result for each, stopping with nil at
 * the first that finds nothing. */
static int read_formats(lua_State *L, tl_file_t *f, int first)
{
    const int last = lua_gettop(L);
    if (last < first) {
        lua_pushliteral(L, "l");
        return read_formats(L, f, lua_gettop(L));
    }
    int ok = 1, n = 0;
    for (int i = first; i <= last && ok; i++, n++) {
        if (lua_type(L, i) == LUA_TNUMBER) {
            ok = read_count(L, f, luaL_checkinteger(L, i));
        } else {
            const char *p = luaL_checkstring(L, i);
            if (*p == '*')
                p++;
            switch (*p) {
            case 'n': ok = read_number(L, f); if (!ok) lua_pushnil(L); break;
            case 'l': ok = read_line(L, f, 0); break;
            case 'L': ok = read_line(L, f, 1); break;
            case 'a': read_all(L, f); ok = 1; break;
            default: return luaL_argerror(L, i, "invalid format");
            }
        }
        if (!ok) {
            lua_pop(L, 1);
            luaL_pushfail(L);
        }
    }
    return n;
}

static int f_read(lua_State *L)
{
    return read_formats(L, to_file(L, 1), 2);
}

static int f_write(lua_State *L)
{
    tl_file_t *f = to_file(L, 1);
    give_back(f);
    const int n = lua_gettop(L);
    for (int i = 2; i <= n; i++) {
        size_t len;
        const char *s = luaL_checklstring(L, i, &len);
        size_t done = 0;
        while (done < len) {
            const int w = fs_write(f->fd, s + done, len - done);
            if (w <= 0)
                return fail(L, NULL, w < 0 ? w : FS_EIO);
            done += (size_t)w;
        }
    }
    lua_settop(L, 1);
    return 1;
}

static int f_seek(lua_State *L)
{
    static const char *const names[] = {"set", "cur", "end", NULL};
    static const int whence[] = {FS_SEEK_SET, FS_SEEK_CUR, FS_SEEK_END};
    tl_file_t *f = to_file(L, 1);
    const int op = luaL_checkoption(L, 2, "cur", names);
    const lua_Integer off = luaL_optinteger(L, 3, 0);
    give_back(f);
    const int64_t pos = fs_seek(f->fd, off, whence[op]);
    if (pos < 0)
        return fail(L, NULL, (int)pos);
    lua_pushinteger(L, (lua_Integer)pos);
    return 1;
}

static int close_file(tl_file_t *f)
{
    if (f->fd < 0)
        return 0;
    const int r = fs_close(f->fd);
    f->fd = -1;
    return r;
}

static int f_close(lua_State *L)
{
    const int r = close_file(to_file(L, 1));
    if (r < 0)
        return fail(L, NULL, r);
    lua_pushboolean(L, 1);
    return 1;
}

static int f_flush(lua_State *L)
{
    tl_file_t *f = to_file(L, 1);
    const int r = fs_sync(f->fd);
    if (r < 0)
        return fail(L, NULL, r);
    lua_settop(L, 1);
    return 1;
}

static int f_setvbuf(lua_State *L)
{
    to_file(L, 1);
    lua_pushboolean(L, 1);
    return 1;
}

static int f_gc(lua_State *L)
{
    close_file(luaL_checkudata(L, 1, FILE_META));
    return 0;
}

static int f_tostring(lua_State *L)
{
    tl_file_t *f = luaL_checkudata(L, 1, FILE_META);
    if (f->fd < 0)
        lua_pushliteral(L, "file (closed)");
    else
        lua_pushfstring(L, "file (%p)", (void *)f);
    return 1;
}

/* The iterator of file:lines and io.lines: upvalues the file, whether to close it at the end, the count of formats, the formats. */
static int lines_next(lua_State *L)
{
    tl_file_t *f = luaL_checkudata(L, lua_upvalueindex(1), FILE_META);
    if (f->fd < 0)
        return luaL_error(L, "file is already closed");
    const int nformats = (int)lua_tointeger(L, lua_upvalueindex(3));
    lua_settop(L, 0);
    for (int i = 1; i <= nformats; i++)
        lua_pushvalue(L, lua_upvalueindex(3 + i));
    const int n = read_formats(L, f, 1);
    if (lua_toboolean(L, -n))
        return n;
    if (lua_toboolean(L, lua_upvalueindex(2)))
        close_file(f);
    return 0;
}

/* Pushes the iterator for the file at idx with the formats from `first` on. */
static void push_lines(lua_State *L, int idx, int first, int to_close)
{
    const int nformats = lua_gettop(L) - first + 1;
    luaL_argcheck(L, nformats <= 250, first + 250, "too many arguments");
    lua_pushvalue(L, idx);
    lua_pushboolean(L, to_close);
    lua_pushinteger(L, nformats);
    for (int i = 0; i < nformats; i++)
        lua_pushvalue(L, first + i);
    lua_pushcclosure(L, lines_next, 3 + nformats);
}

static int f_lines(lua_State *L)
{
    to_file(L, 1);
    push_lines(L, 1, 2, 0);
    return 1;
}

static tl_file_t *new_file(lua_State *L)
{
    tl_file_t *f = lua_newuserdatauv(L, sizeof *f, 0);
    f->fd = -1;
    f->rpos = f->rlen = 0;
    luaL_setmetatable(L, FILE_META);
    return f;
}

/* ---- io ---- */

/* io.open(name [, mode]): "r", "w" (empties it), "a" (writes at the end), each with "+" to read and write; "b" makes no difference. */
static int io_open(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    const char *mode = luaL_optstring(L, 2, "r");
    int flags;
    const char *m = mode;
    switch (*m++) {
    case 'r': flags = FS_RDONLY; break;
    case 'w': flags = FS_WRONLY | FS_CREAT | FS_TRUNC; break;
    case 'a': flags = FS_WRONLY | FS_CREAT | FS_APPEND; break;
    default: return luaL_argerror(L, 2, "invalid mode");
    }
    if (*m == '+') {
        flags = (flags & ~(FS_RDONLY | FS_WRONLY)) | FS_RDWR;
        m++;
    }
    while (*m == 'b')
        m++;
    luaL_argcheck(L, *m == 0, 2, "invalid mode");
    tl_file_t *f = new_file(L);
    f->fd = fs_open(name, flags);
    if (f->fd < 0) {
        const int code = f->fd;
        f->fd = -1;
        return fail(L, name, code);
    }
    return 1;
}

/* io.lines(name, ...): the lines of a file, which is closed at the end; io.lines() reads lines typed at the console. */
static int io_lines(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) {
        lua_getglobal(L, "io");
        lua_getfield(L, -1, "read");
        return 1;
    }
    const char *name = luaL_checkstring(L, 1);
    tl_file_t *f = new_file(L);
    f->fd = fs_open(name, FS_RDONLY);
    if (f->fd < 0) {
        const int code = f->fd;
        f->fd = -1;
        return luaL_error(L, "%s: %s", name, fs_strerror(code));
    }
    lua_replace(L, 1);
    push_lines(L, 1, 2, 1);
    lua_pushnil(L);
    lua_pushnil(L);
    lua_pushvalue(L, 1);   /* (a to-be-closed value for the generic for: the file is closed if the loop is left early) */
    return 4;
}

/* io.read(...): lines typed at the console ("l", "L", or "n" for a number); the text screen comes up for it. Ctrl+C stops the program. */
static int io_read(lua_State *L)
{
    tl_env_t *e = tl_env_current();
    const int last = lua_gettop(L) ? lua_gettop(L) : 1;
    int n = 0;
    for (int i = 1; i <= last; i++, n++) {
        const char *fmt = lua_isnoneornil(L, i) ? "l" : luaL_checkstring(L, i);
        if (*fmt == '*')
            fmt++;
        if (*fmt != 'l' && *fmt != 'L' && *fmt != 'n')
            return luaL_argerror(L, i, "the console reads \"l\", \"L\" or \"n\"");
        if (e->game_active) {
            e->game_active = 0;
            e->hooks.frame();
        }
        char text[TL_LINE_MAX + 2];
        if (!tl_env_read_line(e, text, TL_LINE_MAX + 1))
            return luaL_error(L, "Break");
        if (*fmt == 'n') {
            if (!lua_stringtonumber(L, text)) {
                luaL_pushfail(L);
                return n + 1;
            }
        } else {
            if (*fmt == 'L')
                strcat(text, "\n");
            lua_pushstring(L, text);
        }
    }
    return n;
}

static int io_write(lua_State *L)
{
    tl_env_t *e = tl_env_current();
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        size_t len;
        const char *s = luaL_checklstring(L, i, &len);
        for (size_t k = 0; k < len; k++)
            tg_console_putc(&e->con, (unsigned char)s[k]);
    }
    return 0;
}

static int io_type(lua_State *L)
{
    luaL_checkany(L, 1);
    tl_file_t *f = luaL_testudata(L, 1, FILE_META);
    if (!f)
        luaL_pushfail(L);
    else if (f->fd < 0)
        lua_pushliteral(L, "closed file");
    else
        lua_pushliteral(L, "file");
    return 1;
}

/* ---- os ---- */

static int os_remove(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    int r = fs_unlink(name);
    if (r == FS_EISDIR)
        r = fs_rmdir(name);
    if (r < 0)
        return fail(L, name, r);
    lua_pushboolean(L, 1);
    return 1;
}

static int os_rename(lua_State *L)
{
    const char *from = luaL_checkstring(L, 1), *to = luaL_checkstring(L, 2);
    const int r = fs_rename(from, to);
    if (r < 0)
        return fail(L, from, r);
    lua_pushboolean(L, 1);
    return 1;
}

static int os_clock(lua_State *L)
{
    tl_env_t *e = tl_env_current();
    lua_pushnumber(L, (lua_Number)e->hooks.millis() / 1000);
    return 1;
}

/* os.time(): seconds since the machine started (the board has no clock of the day) */
static int os_time(lua_State *L)
{
    tl_env_t *e = tl_env_current();
    luaL_argcheck(L, lua_isnoneornil(L, 1), 1, "there is no calendar: os.time() is the seconds since the start");
    lua_pushinteger(L, (lua_Integer)(e->hooks.millis() / 1000));
    return 1;
}

/* os.exit(): ends the program, as if it had come to its end */
static int os_exit(lua_State *L)
{
    lua_pushlightuserdata(L, &exit_key);
    return lua_error(L);
}

/* os.dir([folder]): what is in a folder (the current one if none is given), a list of {name =, size =, dir = true or false} */
static int os_dir(lua_State *L)
{
    const char *path = luaL_optstring(L, 1, fs_getcwd());
    const int dd = fs_opendir(path);
    if (dd < 0)
        return fail(L, path, dd);
    lua_newtable(L);
    fs_dirent_t ent;
    int n = 0;
    while (fs_readdir(dd, &ent) == 1) {
        lua_createtable(L, 0, 3);
        lua_pushstring(L, ent.name);
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, (lua_Integer)ent.size);
        lua_setfield(L, -2, "size");
        lua_pushboolean(L, ent.is_dir);
        lua_setfield(L, -2, "dir");
        lua_rawseti(L, -2, ++n);
    }
    fs_closedir(dd);
    return 1;
}

/* ---- loading Lua files ---- */

/* Loads the file as a chunk: pushes the function, or the error message and returns the error code. */
static int load_file(lua_State *L, const char *name, const char *mode)
{
    const int fd = fs_open(name, FS_RDONLY);
    if (fd < 0) {
        lua_pushfstring(L, "cannot open %s: %s", name, fs_strerror(fd));
        return LUA_ERRFILE;
    }
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (;;) {
        char *p = luaL_prepbuffer(&b);
        const int r = fs_read(fd, p, LUAL_BUFFERSIZE);
        if (r < 0) {
            fs_close(fd);
            lua_pushfstring(L, "cannot read %s: %s", name, fs_strerror(r));
            return LUA_ERRFILE;
        }
        if (r == 0)
            break;
        luaL_addsize(&b, (size_t)r);
    }
    fs_close(fd);
    luaL_pushresult(&b);
    size_t len;
    const char *text = lua_tolstring(L, -1, &len);
    lua_pushfstring(L, "@%s", name);
    const int r = luaL_loadbufferx(L, text, len, lua_tostring(L, -1), mode);
    lua_remove(L, -2);   /* (the chunk name) */
    lua_remove(L, -2);   /* (the text) */
    return r;
}

static int l_loadfile(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    const char *mode = luaL_optstring(L, 2, NULL);
    const int env = !lua_isnone(L, 3) ? 3 : 0;
    if (load_file(L, name, mode) != LUA_OK) {
        luaL_pushfail(L);
        lua_insert(L, -2);
        return 2;
    }
    if (env) {
        lua_pushvalue(L, env);
        if (!lua_setupvalue(L, -2, 1))
            lua_pop(L, 1);
    }
    return 1;
}

static int l_dofile(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    lua_settop(L, 1);
    if (load_file(L, name, NULL) != LUA_OK)
        return lua_error(L);
    lua_call(L, 0, LUA_MULTRET);
    return lua_gettop(L) - 1;
}

/* require(name): runs name.lua (a dot in the name is a folder: "lib.maths" is lib/maths.lua) once; what it returns is kept in package.loaded and given
 * to every require of the same name. */
static int l_require(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    lua_settop(L, 1);
    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);   /* 2 */
    lua_getfield(L, 2, name);
    if (lua_toboolean(L, -1))
        return 1;
    lua_pop(L, 1);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (const char *p = name; *p; p++)
        luaL_addchar(&b, *p == '.' ? '/' : *p);
    luaL_addstring(&b, ".lua");
    luaL_pushresult(&b);   /* 3: the file */
    if (load_file(L, lua_tostring(L, 3), NULL) != LUA_OK)
        return luaL_error(L, "module '%s' not found: %s", name, lua_tostring(L, -1));
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 3);
    lua_call(L, 2, 1);
    if (!lua_isnil(L, -1))
        lua_setfield(L, 2, name);
    else
        lua_pop(L, 1);
    lua_getfield(L, 2, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
        lua_pushvalue(L, -1);
        lua_setfield(L, 2, name);
    }
    lua_pushvalue(L, 3);
    return 2;
}

void tl_io_open(lua_State *L)
{
    static const luaL_Reg file_methods[] = {{"read", f_read},   {"write", f_write},     {"lines", f_lines}, {"seek", f_seek},
                                            {"close", f_close}, {"flush", f_flush},     {"setvbuf", f_setvbuf}, {NULL, NULL}};
    static const luaL_Reg file_meta[] = {{"__gc", f_gc}, {"__close", f_gc}, {"__tostring", f_tostring}, {NULL, NULL}};
    luaL_newmetatable(L, FILE_META);
    luaL_setfuncs(L, file_meta, 0);
    luaL_newlib(L, file_methods);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    static const luaL_Reg io[] = {{"open", io_open}, {"lines", io_lines}, {"read", io_read}, {"write", io_write}, {"type", io_type}, {NULL, NULL}};
    luaL_newlib(L, io);
    lua_setglobal(L, "io");
    static const luaL_Reg os[] = {{"remove", os_remove}, {"rename", os_rename}, {"clock", os_clock}, {"time", os_time},
                                  {"exit", os_exit},     {"dir", os_dir},       {NULL, NULL}};
    luaL_newlib(L, os);
    lua_setglobal(L, "os");

    lua_register(L, "mount", l_mount);
    lua_register(L, "unmount", l_unmount);
    lua_register(L, "loadfile", l_loadfile);
    lua_register(L, "dofile", l_dofile);
    lua_register(L, "require", l_require);
    lua_newtable(L);
    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_setfield(L, -2, "loaded");
    lua_pushliteral(L, "?.lua");
    lua_setfield(L, -2, "path");
    lua_setglobal(L, "package");
}
