/* SPDX-License-Identifier: MIT */
#include "basic_file.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "basic_env.h"
#include "my_basic/my_basic.h"
#include "tilefs.h"

#define BF_READ_BUFFER 256
#define BF_STRING_MAX 1000          /* the longest string FREAD_LINE gives (in one piece) */
#define BF_READ_MAX 0x100000        /* the most bytes FREAD(f, n) reads at once */

/* Reads go through a small buffer (a byte at a time from the card would be slow); anything else first gives back what was read ahead. */
typedef struct {
    int used;
    int fd;
    int rpos, rlen;
    uint8_t buf[BF_READ_BUFFER];
} bf_file_t;

static bf_file_t files[BASIC_FILES];
static char text[BF_STRING_MAX + 1];

static int fail(struct mb_interpreter_t *s, void **l, mb_error_e err, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int fail(struct mb_interpreter_t *s, void **l, mb_error_e err, const char *fmt, ...)
{
    char msg[120];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    basic_env_problem(basic_env_current(), "%s", msg);
    return mb_raise_error(s, l, err, MB_FUNC_ERR);
}

#define FUNC(n) static int bf_##n(struct mb_interpreter_t *s, void **l)
#define CK(x) do { const int ck_ = (x); if (ck_ != MB_FUNC_OK) return ck_; } while (0)
#define OPEN() CK(mb_attempt_open_bracket(s, l))
#define CLOSE() CK(mb_attempt_close_bracket(s, l))
#define RET(v) CK(mb_push_int(s, l, (int_t)(v)))

void basic_file_close_all(void)
{
    for (int i = 0; i < BASIC_FILES; i++) {
        if (files[i].used)
            fs_close(files[i].fd);
        files[i].used = 0;
    }
}

static int arg_int(struct mb_interpreter_t *s, void **l, int *v)
{
    int_t t = 0;
    const int r = mb_pop_int(s, l, &t);
    *v = (int)t;
    return r;
}

/* The file of the number the program gave. */
static int arg_file(struct mb_interpreter_t *s, void **l, const char *who, bf_file_t **out)
{
    int h;
    CK(arg_int(s, l, &h));
    if (h < 1 || h > BASIC_FILES || !files[h - 1].used)
        return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "%s: %d is not an open file (FOPEN gives the number)", who, h);
    *out = &files[h - 1];
    return MB_FUNC_OK;
}

/* Back to where the program is: the bytes read ahead are given back. */
static void unread(bf_file_t *f)
{
    if (f->rpos < f->rlen)
        fs_seek(f->fd, -(int64_t)(f->rlen - f->rpos), FS_SEEK_CUR);
    f->rpos = f->rlen = 0;
}

static int next_byte(bf_file_t *f)
{
    if (f->rpos >= f->rlen) {
        const int n = fs_read(f->fd, f->buf, sizeof f->buf);
        f->rpos = 0;
        f->rlen = n > 0 ? n : 0;
        if (!f->rlen)
            return -1;
    }
    return f->buf[f->rpos++];
}

static int push_text(struct mb_interpreter_t *s, void **l, size_t n)
{
    text[n] = 0;
    return mb_push_string(s, l, mb_memdup(text, (unsigned)(strlen(text) + 1)));
}

/* FOPEN(name, mode): the file's number, or 0 if it could not be opened. The mode is C's: "r", "w", "a", each with "+" (and "b", which makes no difference). */
FUNC(fopen)
{
    char *name = NULL, *mode = NULL;
    OPEN();
    CK(mb_pop_string(s, l, &name));
    CK(mb_pop_string(s, l, &mode));
    CLOSE();
    int flags;
    const int plus = mode && strchr(mode, '+') != NULL;
    switch (mode ? mode[0] : 0) {
    case 'r': flags = plus ? FS_RDWR : FS_RDONLY; break;
    case 'w': flags = (plus ? FS_RDWR : FS_WRONLY) | FS_CREAT | FS_TRUNC; break;
    case 'a': flags = (plus ? FS_RDWR : FS_WRONLY) | FS_CREAT | FS_APPEND; break;
    default: return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "FOPEN(name, mode): the mode is \"r\", \"w\" or \"a\", with \"+\" to do both");
    }
    int h = 0;
    while (h < BASIC_FILES && files[h].used)
        h++;
    if (h == BASIC_FILES)
        return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "FOPEN: %d files are open already, FCLOSE one", BASIC_FILES);
    const int fd = fs_open(name ? name : "", flags);
    if (fd < 0) {
        RET(0);
        return MB_FUNC_OK;
    }
    files[h].used = 1;
    files[h].fd = fd;
    files[h].rpos = files[h].rlen = 0;
    RET(h + 1);
    return MB_FUNC_OK;
}

FUNC(fclose)
{
    bf_file_t *f;
    OPEN();
    CK(arg_file(s, l, "FCLOSE(file)", &f));
    CLOSE();
    const int r = fs_close(f->fd);
    f->used = 0;
    RET(r < 0 ? -1 : 0);
    return MB_FUNC_OK;
}

/* The position in the file, or -1. */
FUNC(fpeek)
{
    bf_file_t *f;
    OPEN();
    CK(arg_file(s, l, "FPEEK(file)", &f));
    CLOSE();
    unread(f);
    const int64_t p = fs_seek(f->fd, 0, FS_SEEK_CUR);
    RET(p < 0 || p > INT32_MAX ? -1 : p);
    return MB_FUNC_OK;
}

/* Moves to a position counted from the start: 0, or -1. */
FUNC(fpoke)
{
    bf_file_t *f;
    int pos;
    OPEN();
    CK(arg_file(s, l, "FPOKE(file, position)", &f));
    CK(arg_int(s, l, &pos));
    CLOSE();
    unread(f);
    RET(pos < 0 || fs_seek(f->fd, pos, FS_SEEK_SET) < 0 ? -1 : 0);
    return MB_FUNC_OK;
}

/* The length of the file in bytes, or -1. */
FUNC(fsize)
{
    bf_file_t *f;
    OPEN();
    CK(arg_file(s, l, "FSIZE(file)", &f));
    CLOSE();
    unread(f);
    const int64_t here = fs_seek(f->fd, 0, FS_SEEK_CUR);
    const int64_t end = here < 0 ? -1 : fs_seek(f->fd, 0, FS_SEEK_END);
    if (here >= 0)
        fs_seek(f->fd, here, FS_SEEK_SET);
    RET(end < 0 || end > INT32_MAX ? -1 : end);
    return MB_FUNC_OK;
}

/* 1 if there is nothing more to read. */
FUNC(feof)
{
    bf_file_t *f;
    OPEN();
    CK(arg_file(s, l, "FEOF(file)", &f));
    CLOSE();
    const int b = next_byte(f);
    if (b >= 0)
        f->rpos--;
    RET(b < 0);
    return MB_FUNC_OK;
}

/* FREAD(file): the next byte, or -1 at the end. FREAD(file, n): up to n bytes as a string ("" at the end; a zero byte ends the string). */
FUNC(fread)
{
    bf_file_t *f;
    int n = 0, counted = 0;
    OPEN();
    CK(arg_file(s, l, "FREAD(file [, count])", &f));
    if (mb_has_arg(s, l)) {
        CK(arg_int(s, l, &n));
        counted = 1;
    }
    CLOSE();
    if (!counted) {
        RET(next_byte(f));
        return MB_FUNC_OK;
    }
    if (n < 1 || n > BF_READ_MAX)
        return fail(s, l, SE_RN_INDEX_OUT_OF_BOUND, "FREAD(file, count): the count is 1 to %d", BF_READ_MAX);
    /* The bytes are read into a block of the bulk memory (the PSRAM on the board), and the string made from them is a copy in the interpreter's. */
    basic_env_t *e = basic_env_current();
    char *tmp = e->hooks.alloc ? e->hooks.alloc((size_t)n + 1, 0) : malloc((size_t)n + 1);
    if (!tmp)
        return fail(s, l, SE_RN_OUT_OF_MEMORY, "FREAD(file, count): not enough memory for %d bytes", n);
    size_t got = 0;
    while (got < (size_t)n && f->rpos < f->rlen) {                        /* what was read ahead */
        tmp[got++] = (char)f->buf[f->rpos++];
    }
    while (got < (size_t)n) {                                             /* the rest straight from the file */
        const int r = fs_read(f->fd, tmp + got, (size_t)n - got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    tmp[got] = 0;
    char *str = mb_memdup(tmp, (unsigned)(strlen(tmp) + 1));
    if (e->hooks.release)
        e->hooks.release(tmp);
    else
        free(tmp);
    if (!str)
        return fail(s, l, SE_RN_OUT_OF_MEMORY, "FREAD(file, count): not enough memory for %d bytes", n);
    return mb_push_string(s, l, str);
}

/* A line without its end (\n, \r or \r\n); "" at the end of the file (FEOF says which), and a line longer than 1000 characters comes in pieces. */
FUNC(fread_line)
{
    bf_file_t *f;
    OPEN();
    CK(arg_file(s, l, "FREAD_LINE(file)", &f));
    CLOSE();
    size_t got = 0;
    while (got < BF_STRING_MAX) {
        const int b = next_byte(f);
        if (b < 0)
            break;
        if (b == '\n')
            break;
        if (b == '\r') {
            if (next_byte(f) != '\n' && f->rpos > 0)
                f->rpos--;   /* (a byte that was read ahead, or none was read: the buffer is not empty) */
            break;
        }
        text[got++] = (char)b;
    }
    return push_text(s, l, got);
}

/* The values a program gave, as bytes: a whole number is one byte (its low eight bits), a string is its text. */
static int write_values(struct mb_interpreter_t *s, void **l, const char *who, int newline)
{
    bf_file_t *f;
    OPEN();
    CK(arg_file(s, l, who, &f));
    unread(f);
    long total = 0;
    int bad = 0;
    while (mb_has_arg(s, l)) {
        mb_value_t v;
        mb_make_nil(v);
        CK(mb_pop_value(s, l, &v));
        uint8_t byte;
        const char *str = NULL;
        size_t len = 0;
        if (v.type == MB_DT_INT)
            byte = (uint8_t)v.value.integer, len = 1, str = (const char *)&byte;
        else if (v.type == MB_DT_REAL)
            byte = (uint8_t)(int)v.value.float_point, len = 1, str = (const char *)&byte;
        else if (v.type == MB_DT_STRING)
            str = v.value.string ? v.value.string : "", len = strlen(str);
        else
            return fail(s, l, SE_RN_UNEXPECTED_TYPE, "%s: write numbers and strings", who);
        if (len && !bad) {
            if (fs_write(f->fd, str, len) == (int)len)
                total += (long)len;
            else
                bad = 1;
        }
    }
    CLOSE();
    if (newline && !bad) {
        if (fs_write(f->fd, "\n", 1) == 1)
            total++;
        else
            bad = 1;
    }
    RET(bad ? -1 : total);
    return MB_FUNC_OK;
}

FUNC(fwrite)
{
    return write_values(s, l, "FWRITE(file, ...)", 0);
}

FUNC(fwrite_line)
{
    return write_values(s, l, "FWRITE_LINE(file, ...)", 1);
}

/* 1 for a file, 2 for a folder, 0 for neither. */
FUNC(fexists)
{
    char *name = NULL;
    OPEN();
    CK(mb_pop_string(s, l, &name));
    CLOSE();
    fs_stat_t st;
    RET(fs_stat(name ? name : "", &st) < 0 ? 0 : st.is_dir ? 2 : 1);
    return MB_FUNC_OK;
}

/* Deletes a file or an empty folder: 0, or -1. */
FUNC(fdelete)
{
    char *name = NULL;
    OPEN();
    CK(mb_pop_string(s, l, &name));
    CLOSE();
    RET(fs_unlink(name ? name : "") < 0 ? -1 : 0);
    return MB_FUNC_OK;
}

static const struct {
    const char *name;
    int (*fn)(struct mb_interpreter_t *, void **);
} functions[] = {
    {"FOPEN", bf_fopen},   {"FCLOSE", bf_fclose},           {"FPEEK", bf_fpeek},         {"FPOKE", bf_fpoke},     {"FSIZE", bf_fsize},     {"FEOF", bf_feof},
    {"FREAD", bf_fread},   {"FREAD_LINE", bf_fread_line},   {"FWRITE", bf_fwrite},       {"FWRITE_LINE", bf_fwrite_line},
    {"FEXISTS", bf_fexists}, {"FDELETE", bf_fdelete},
};

void basic_file_register(struct mb_interpreter_t *s)
{
    for (size_t i = 0; i < sizeof functions / sizeof *functions; i++)
        mb_register_func(s, functions[i].name, functions[i].fn);
}
