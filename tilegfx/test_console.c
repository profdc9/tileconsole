/* SPDX-License-Identifier: MIT */
/* Tests of the console: random output against an independent model of a terminal, and the individual rules. Exits non-zero on the first failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tg_console.h"

static int checks;
#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static uint32_t rng = 11;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

/* The model: one position in the 2000 cells and a flag for a wrap that is waiting, instead of a column that may be 80. */
typedef struct {
    uint16_t cells[2000];
    int pos, pending;
    uint8_t attr;
    int nowrap;
} model_t;

static void m_scroll(model_t *m)
{
    memmove(m->cells, m->cells + 80, 1920 * sizeof m->cells[0]);
    for (int i = 1920; i < 2000; i++)
        m->cells[i] = (uint16_t)(' ' | m->attr << 8);
}

static void m_newline(model_t *m)
{
    if (m->pos / 80 == 24)
        m_scroll(m);
    else
        m->pos = (m->pos / 80 + 1) * 80;
    m->pos = m->pos / 80 * 80;
    m->pending = 0;
}

static void m_put(model_t *m, uint8_t ch)
{
    switch (ch) {
    case '\n': m_newline(m); return;
    case '\r': m->pos = m->pos / 80 * 80; m->pending = 0; return;
    case '\b':
        if (m->pending)
            m->pending = 0;
        else if (m->pos % 80 > 0)
            m->pos--;
        return;
    case '\t': {
        const int col = m->pos % 80, next = (col / 8 + 1) * 8;
        m->pos = m->pos - col + (next >= 80 ? 79 : next);
        m->pending = 0;
        return;
    }
    case '\a': return;
    }
    if (m->pending) {
        if (m->nowrap)
            return;
        m_newline(m);
    }
    m->cells[m->pos] = (uint16_t)(ch | m->attr << 8);
    if (m->pos % 80 == 79)
        m->pending = 1;
    else
        m->pos++;
}

static tg_text_t screen;
static tg_console_t con;
static model_t model;

static void same(void)
{
    CHECK(memcmp(screen.cells, model.cells, sizeof model.cells) == 0);
    CHECK(con.row == model.pos / 80);
    CHECK(con.col == (model.pending ? 80 : model.pos % 80));
    CHECK(screen.cursor_col == model.pos % 80 && screen.cursor_row == model.pos / 80);   /* the cursor is at the last column while a wrap waits */
}

static void reset(uint8_t attr, int flags)
{
    memset(&screen, 0, sizeof screen);
    tg_text_init(&screen);
    tg_console_init(&con, &screen, attr);
    con.flags = (uint8_t)flags;
    tg_console_clear(&con);
    memset(&model, 0, sizeof model);
    model.attr = attr;
    model.nowrap = flags & TG_CONSOLE_NOWRAP;
    for (int i = 0; i < 2000; i++)
        model.cells[i] = (uint16_t)(' ' | attr << 8);
}

static uint8_t random_byte(void)
{
    static const uint8_t special[] = {'\n', '\r', '\b', '\t', '\a'};
    const uint32_t r = rnd() % 100;
    if (r < 12)
        return special[rnd() % sizeof special];
    if (r < 20)
        return (uint8_t)rnd();                 /* anything: faces, blocks, accents, NUL excluded below */
    return (uint8_t)(' ' + rnd() % 95);
}

static void test_random(void)
{
    for (int round = 0; round < 400; round++) {
        const int flags = round % 4 == 3 ? TG_CONSOLE_NOWRAP : 0;
        reset((uint8_t)rnd(), flags);
        same();
        const int ops = 1 + (int)(rnd() % 400);
        for (int i = 0; i < ops; i++) {
            const uint32_t what = rnd() % 10;
            if (what == 0) {                    /* a change of colours */
                const uint8_t a = (uint8_t)rnd();
                tg_console_set_attr(&con, a);
                model.attr = a;
            } else if (what < 4) {              /* a string written whole */
                char s[200];
                const int n = (int)(rnd() % 150);
                for (int k = 0; k < n; k++) {
                    uint8_t b = random_byte();
                    s[k] = (char)(b ? b : 'x');   /* a string cannot hold a NUL */
                    m_put(&model, (uint8_t)s[k]);
                }
                s[n] = 0;
                tg_console_puts(&con, s);
            } else {                            /* single characters, taken as the low 8 bits of an int */
                const uint8_t b = random_byte();
                tg_console_putc(&con, (int)b | (int)((rnd() % 3) * 256));   /* only the low 8 bits count */
                m_put(&model, b);
            }
            same();
        }
    }
}

static void test_rules(void)
{
    reset(0x07, 0);
    /* The first row, and what the controls do. */
    tg_console_puts(&con, "abc");
    CHECK(con.col == 3 && con.row == 0 && screen.cells[0] == 0x0761 && screen.cells[2] == 0x0763);
    tg_console_putc(&con, '\b');
    CHECK(con.col == 2 && screen.cells[2] == 0x0763);               /* it does not erase */
    tg_console_putc(&con, '\r');
    CHECK(con.col == 0 && con.row == 0);
    tg_console_putc(&con, '\b');
    CHECK(con.col == 0);                                             /* and does not go off the left edge */
    tg_console_putc(&con, '\t');
    CHECK(con.col == 8 && screen.cells[8] == 0x0720);                /* a tab does not write */
    tg_console_putc(&con, '\t');
    CHECK(con.col == 16);
    tg_console_gotoxy(&con, 77, 0);
    tg_console_putc(&con, '\t');
    CHECK(con.col == 79);                                            /* it stops at the last column */
    tg_console_putc(&con, '\t');
    CHECK(con.col == 79);
    tg_console_putc(&con, '\a');
    CHECK(con.col == 79 && screen.cells[79] == 0x0720);              /* the bell is nothing */
    tg_console_putc(&con, '\n');
    CHECK(con.col == 0 && con.row == 1);

    /* A line of 80 characters, then a newline: no blank line, and the wait for the wrap is invisible to the cursor. */
    tg_console_clear(&con);
    for (int i = 0; i < 80; i++)
        tg_console_putc(&con, 'a' + i % 26);
    CHECK(con.row == 0 && con.col == 80 && screen.cursor_col == 79 && screen.cursor_row == 0);
    tg_console_putc(&con, '\n');
    CHECK(con.row == 1 && con.col == 0);
    tg_console_putc(&con, 'z');
    CHECK(screen.cells[80] == 0x077A);
    tg_console_clear(&con);
    for (int i = 0; i < 81; i++)
        tg_console_putc(&con, '0' + i % 10);                         /* the 81st goes to the next row */
    CHECK(con.row == 1 && con.col == 1 && screen.cells[80] == 0x0700 + '0' && screen.cells[79] == 0x0700 + '0' + 79 % 10);
    tg_console_putc(&con, '\r');
    tg_console_putc(&con, '\b');
    CHECK(con.col == 0);
    /* Backspace and tab while a wrap waits. */
    tg_console_clear(&con);
    for (int i = 0; i < 80; i++)
        tg_console_putc(&con, 'x');
    tg_console_putc(&con, '\b');
    CHECK(con.col == 79 && con.row == 0);
    tg_console_putc(&con, 'y');
    CHECK(screen.cells[79] == 0x0779 && con.col == 80);

    /* The last row scrolls the screen up; the rows that come in take the current colours. */
    tg_console_clear(&con);
    for (int r = 0; r < 25; r++) {
        tg_console_printf(&con, "row %d", r);
        if (r < 24)
            tg_console_putc(&con, '\n');
    }
    CHECK(con.row == 24 && screen.cells[0] == 0x0700 + 'r' && screen.cells[24 * 80 + 4] == 0x0700 + '2');
    tg_console_set_attr(&con, 0x4F);
    tg_console_putc(&con, '\n');
    CHECK(con.row == 24 && con.col == 0);
    CHECK(screen.cells[0] == 0x0700 + 'r' && screen.cells[4] == 0x0700 + '1');                     /* row 0 is now what row 1 was: "row 1" */
    CHECK(screen.cells[23 * 80 + 4] == 0x0700 + '2' && screen.cells[24 * 80] == 0x4F20 && screen.cells[24 * 80 + 79] == 0x4F20);

    /* Colours apply to what is written next, not to what is there. */
    tg_console_clear(&con);
    tg_console_set_attr(&con, TG_TEXT_ATTR(TG_TEXT_YELLOW, TG_TEXT_RED));
    tg_console_puts(&con, "hi");
    tg_console_set_attr(&con, TG_TEXT_ATTR(TG_TEXT_WHITE, TG_TEXT_BLUE));
    tg_console_puts(&con, "!");
    CHECK(screen.cells[0] == (0x4E00 | 'h') && screen.cells[2] == (0x1F00 | '!'));
    CHECK(screen.cells[3] == 0x4F20);                                                              /* cleared in the colours at that time (0x4F) */

    /* gotoxy clamps, and moves the cursor. */
    tg_console_gotoxy(&con, -5, -5);
    CHECK(con.col == 0 && con.row == 0 && screen.cursor_col == 0 && screen.cursor_row == 0);
    tg_console_gotoxy(&con, 500, 500);
    CHECK(con.col == 79 && con.row == 24 && screen.cursor_col == 79 && screen.cursor_row == 24);
    tg_console_gotoxy(&con, 12, 7);
    tg_console_putc(&con, 'q');
    CHECK(screen.cells[7 * 80 + 12] == (0x1F00 | 'q') && screen.cursor_col == 13 && screen.cursor_row == 7);

    /* Attaching a console again starts it afresh. */
    con.flags = TG_CONSOLE_NOWRAP;
    con.col = 30;
    con.row = 10;
    tg_console_init(&con, &screen, 0x2A);
    CHECK(con.flags == 0 && con.col == 0 && con.row == 0 && con.attr == 0x2A && con.text == &screen);
    tg_console_set_attr(&con, 0x1F);

    /* The console does not change whether the cursor shows. */
    tg_text_show_cursor(&screen, true);
    tg_console_puts(&con, "a");
    CHECK(screen.cursor_flags & TG_CURSOR_VISIBLE);
    tg_text_show_cursor(&screen, false);
    tg_console_puts(&con, "a");
    CHECK(!(screen.cursor_flags & TG_CURSOR_VISIBLE));

    /* NOWRAP: the characters past the edge are dropped, and the controls still work. */
    reset(0x07, TG_CONSOLE_NOWRAP);
    tg_console_puts(&con, "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789");
    CHECK(con.row == 0 && con.col == 80 && screen.cells[79] == 0x0700 + '0' + 79 % 10 && screen.cells[80] == 0x0720);   /* nothing went to row 1 */
    tg_console_putc(&con, '\n');
    tg_console_putc(&con, 'k');
    CHECK(con.row == 1 && screen.cells[80] == 0x076B);          /* and a newline moves on as usual */
}

static void test_printf(void)
{
    reset(0x07, 0);
    CHECK(tg_console_printf(&con, "%d+%d=%d", 2, 3, 5) == 5);
    CHECK(screen.cells[0] == 0x0732 && screen.cells[4] == 0x0735 && con.col == 5);
    CHECK(tg_console_printf(&con, "%s", "") == 0 && con.col == 5);
    CHECK(tg_console_printf(&con, "%s|%5s|%-5s|%c|%x|%05d|%%", "ab", "cd", "ef", 'g', 255, 42) == 2 + 1 + 5 + 1 + 5 + 1 + 1 + 1 + 2 + 1 + 5 + 1 + 1);
    CHECK(screen.cells[5] == 0x0761 && screen.cells[7] == 0x077C && screen.cells[8] == 0x0720 && screen.cells[11] == 0x0763);   /* a | b, then "   cd" */
    tg_console_clear(&con);
    tg_console_printf(&con, "one\ntwo\r\n\tthree\tx");                  /* controls inside a formatted string */
    CHECK(con.row == 2 && screen.cells[0] == 0x076F && screen.cells[80] == 0x0774 && screen.cells[160 + 8] == 0x0774 && screen.cells[160 + 8 + 5 + 1] == 0x0720);
    CHECK(con.col == 17 && con.row == 2 && screen.cells[160 + 16] == 0x0778);
    tg_console_clear(&con);
    tg_console_printf(&con, "%c%c%c", 0x01, 0xC9, 0xFF);                /* bytes are character codes */
    CHECK(screen.cells[0] == 0x0701 && screen.cells[1] == 0x07C9 && screen.cells[2] == 0x07FF);

    /* A long string is cut at TG_CONSOLE_PRINTF_MAX - 1 characters, and the return value is what was written. */
    static char big[1000];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    tg_console_clear(&con);
    CHECK(tg_console_printf(&con, "%s", big) == TG_CONSOLE_PRINTF_MAX - 1);
    int xs = 0;
    for (int i = 0; i < TG_TEXT_CELLS; i++)
        xs += (screen.cells[i] & 255) == 'x';
    CHECK(xs == TG_CONSOLE_PRINTF_MAX - 1);
    CHECK(tg_console_printf(&con, "%s", "ab") == 2);                    /* and the next call is whole again */
    /* Exactly as long as the work space: still one character short, and nothing but the characters is written (not the end of the string). */
    memset(big, 'x', TG_CONSOLE_PRINTF_MAX);
    big[TG_CONSOLE_PRINTF_MAX] = 0;
    tg_console_clear(&con);
    CHECK(tg_console_printf(&con, "%s", big) == TG_CONSOLE_PRINTF_MAX - 1);
    for (int i = 0; i < TG_CONSOLE_PRINTF_MAX - 1; i++)
        CHECK((screen.cells[i] & 255) == 'x');
    CHECK(screen.cells[TG_CONSOLE_PRINTF_MAX - 1] == 0x0720);
    CHECK(con.row == (TG_CONSOLE_PRINTF_MAX - 1) / 80 && con.col == (TG_CONSOLE_PRINTF_MAX - 1) % 80);
    tg_console_clear(&con);
    CHECK(tg_console_printf(&con, "abc") == 3);                         /* the cursor follows printf too */
    CHECK(screen.cursor_col == 3 && screen.cursor_row == 0);
    tg_console_printf(&con, "\n\n%d", 12);
    CHECK(screen.cursor_col == 2 && screen.cursor_row == 2);
    /* The same text through puts is not cut. */
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    tg_console_clear(&con);
    tg_console_puts(&con, big);
    xs = 0;
    for (int i = 0; i < TG_TEXT_CELLS; i++)
        xs += (screen.cells[i] & 255) == 'x';
    CHECK(xs == 999);                                                  /* puts is not cut: 999 characters, fewer than the screen holds */
}

int main(void)
{
    test_random();
    test_rules();
    test_printf();
    printf("all %d checks passed\n", checks);
    return 0;
}
