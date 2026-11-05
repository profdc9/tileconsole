/* SPDX-License-Identifier: MIT */
#include "basic_lib.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "basic_env.h"
#include "my_basic/my_basic.h"

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

#define FUNC(n) static int bl_##n(struct mb_interpreter_t *s, void **l)
#define CK(x) do { const int ck_ = (x); if (ck_ != MB_FUNC_OK) return ck_; } while (0)
#define OPEN() CK(mb_attempt_open_bracket(s, l))
#define CLOSE() CK(mb_attempt_close_bracket(s, l))
#define RET(v) CK(mb_push_int(s, l, (int_t)(v)))

/* ---- bit operations (on the 32 bits of a whole number) ---- */

static int arg_two(struct mb_interpreter_t *s, void **l, int_t *m, int_t *n)
{
    OPEN();
    CK(mb_pop_int(s, l, m));
    CK(mb_pop_int(s, l, n));
    CLOSE();
    return MB_FUNC_OK;
}

FUNC(bit_and)
{
    int_t m, n;
    CK(arg_two(s, l, &m, &n));
    RET((uint32_t)m & (uint32_t)n);
    return MB_FUNC_OK;
}

FUNC(bit_or)
{
    int_t m, n;
    CK(arg_two(s, l, &m, &n));
    RET((uint32_t)m | (uint32_t)n);
    return MB_FUNC_OK;
}

FUNC(bit_xor)
{
    int_t m, n;
    CK(arg_two(s, l, &m, &n));
    RET((uint32_t)m ^ (uint32_t)n);
    return MB_FUNC_OK;
}

FUNC(bit_not)
{
    int_t m;
    OPEN();
    CK(mb_pop_int(s, l, &m));
    CLOSE();
    RET(~(uint32_t)m);
    return MB_FUNC_OK;
}

/* A shift by 32 or more (or a negative count) moves every bit out: 0, or for a right shift of a negative number, -1. */
FUNC(bit_lshift)
{
    int_t m, n;
    CK(arg_two(s, l, &m, &n));
    RET(n < 0 || n > 31 ? 0 : (int32_t)((uint32_t)m << n));
    return MB_FUNC_OK;
}

FUNC(bit_rshift)
{
    int_t m, n;
    CK(arg_two(s, l, &m, &n));
    RET(n < 0 || n > 31 ? (m < 0 ? -1 : 0) : (int32_t)m >> n);
    return MB_FUNC_OK;
}

/* ---- miscellaneous ---- */

/* ASSERT(condition [, message]): stops the program, saying so, if the condition is 0. */
FUNC(assert)
{
    mb_value_t v;
    char *msg = NULL;
    OPEN();
    mb_make_nil(v);
    CK(mb_pop_value(s, l, &v));
    if (mb_has_arg(s, l))
        CK(mb_pop_string(s, l, &msg));
    CLOSE();
    const int ok = v.type == MB_DT_INT ? v.value.integer != 0 : v.type == MB_DT_REAL ? v.value.float_point != 0 : v.type != MB_DT_NIL;
    if (ok)
        return MB_FUNC_OK;
    if (msg && *msg)
        return fail(s, l, SE_EA_EXTENDED_ABORT, "Assertion failed: %s", msg);
    return fail(s, l, SE_EA_EXTENDED_ABORT, "Assertion failed");
}

/* IIF(condition, a, b): a if the condition is true (not 0, not nil), else b; both are worked out first. */
FUNC(iif)
{
    mb_value_t c, a, b;
    OPEN();
    mb_make_nil(c);
    mb_make_nil(a);
    mb_make_nil(b);
    CK(mb_pop_value(s, l, &c));
    CK(mb_pop_value(s, l, &a));
    CK(mb_pop_value(s, l, &b));
    CLOSE();
    const int yes = c.type == MB_DT_INT ? c.value.integer != 0 : c.type == MB_DT_REAL ? c.value.float_point != 0 : c.type != MB_DT_NIL;
    const mb_value_t r = yes ? a : b;
    if (r.type == MB_DT_STRING)   /* (a string is the interpreter's: the answer is a copy) */
        return mb_push_string(s, l, mb_memdup(r.value.string ? r.value.string : "", (unsigned)strlen(r.value.string ? r.value.string : "") + 1));
    return mb_push_value(s, l, r);
}

/* SWAP(a, b): the two variables change values. */
FUNC(swap)
{
    void *v0 = NULL, *v1 = NULL;
    mb_value_t x, y;
    OPEN();
    CK(mb_get_var(s, l, &v0, true));
    CK(mb_get_var(s, l, &v1, true));
    CLOSE();
    mb_get_var_value(s, v0, &x);
    mb_get_var_value(s, v1, &y);
    mb_set_var_value(s, v0, y);
    mb_set_var_value(s, v1, x);
    return MB_FUNC_OK;
}

static int extreme(struct mb_interpreter_t *s, void **l, int want_max)
{
    real_t best = 0, t = 0;
    int n = 0;
    OPEN();
    while (mb_has_arg(s, l)) {
        CK(mb_pop_real(s, l, &t));
        if (!n++ || (want_max ? t > best : t < best))
            best = t;
    }
    CLOSE();
    if (!n)
        return fail(s, l, SE_RN_INVALID_ID_USAGE, "%s needs at least one value", want_max ? "MAX" : "MIN");
    return mb_push_real(s, l, best);
}

FUNC(min)
{
    return extreme(s, l, 0);
}

FUNC(max)
{
    return extreme(s, l, 1);
}

/* ---- string matching ---- */

/* FIND(text, pattern [, start]): the position (0 is the first character) of the first match at or after start, or -1. An empty pattern is never found. */
FUNC(find)
{
    char *text = NULL, *pat = NULL;
    int_t start = 0;
    OPEN();
    CK(mb_pop_string(s, l, &text));
    CK(mb_pop_string(s, l, &pat));
    if (mb_has_arg(s, l))
        CK(mb_pop_int(s, l, &start));
    CLOSE();
    const size_t tl = text ? strlen(text) : 0, pl = pat ? strlen(pat) : 0;
    long found = -1;
    if (start < 0)
        start = 0;
    if (pl && (size_t)start < tl && pl <= tl)
        for (size_t i = (size_t)start; i + pl <= tl; i++)
            if (!memcmp(text + i, pat, pl)) {
                found = (long)i;
                break;
            }
    RET(found);
    return MB_FUNC_OK;
}

/* WILDCARD_FIND(text, pattern): 1 if the whole text matches the pattern, where ? is any one character and * any run of them (also none), else 0. */
FUNC(wildcard_find)
{
    char *text = NULL, *pat = NULL;
    OPEN();
    CK(mb_pop_string(s, l, &text));
    CK(mb_pop_string(s, l, &pat));
    CLOSE();
    const char *t = text ? text : "", *p = pat ? pat : "";
    const char *star = NULL, *back = NULL;   /* the last * and where in the text it is taken to end */
    int match;
    for (;;) {
        if (*p == '*') {
            star = ++p;
            back = t;
        } else if (*t && (*p == '?' || *p == *t)) {
            p++;
            t++;
        } else if (!*t) {
            while (*p == '*')
                p++;
            match = !*p;
            break;
        } else if (star && *back) {
            p = star;
            t = ++back;
        } else {
            match = 0;
            break;
        }
    }
    RET(match);
    return MB_FUNC_OK;
}

static const struct {
    const char *name;
    int (*fn)(struct mb_interpreter_t *, void **);
} functions[] = {
    {"BIT_AND", bl_bit_and}, {"BIT_OR", bl_bit_or}, {"BIT_XOR", bl_bit_xor}, {"BIT_NOT", bl_bit_not}, {"BIT_LSHIFT", bl_bit_lshift}, {"BIT_RSHIFT", bl_bit_rshift},
    {"ASSERT", bl_assert}, {"IIF", bl_iif}, {"SWAP", bl_swap}, {"MIN", bl_min}, {"MAX", bl_max},
    {"FIND", bl_find}, {"WILDCARD_FIND", bl_wildcard_find},
};

void basic_lib_register(struct mb_interpreter_t *s)
{
    for (size_t i = 0; i < sizeof functions / sizeof *functions; i++)
        mb_register_func(s, functions[i].name, functions[i].fn);
}
