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

/* ---- the ANSI mode ---- */

static void ansi_reset(int extra_flags)
{
    memset(&screen, 0, sizeof screen);
    tg_text_init(&screen);
    tg_console_init(&con, &screen, 0x07);
    con.flags = (uint8_t)(TG_CONSOLE_ANSI | extra_flags);
    tg_console_clear(&con);
}

static uint16_t at(int col, int row) { return screen.cells[row * 80 + col]; }

static void fill_x(void)   /* every cell an 'x' in a recognisable colour, and the cursor where it was */
{
    for (int i = 0; i < 2000; i++)
        screen.cells[i] = 0x1E00 | 'x';
}

static void test_ansi_cursor(void)
{
    ansi_reset(0);
    tg_console_puts(&con, "\033[5;10H");
    CHECK(con.col == 9 && con.row == 4 && screen.cursor_col == 9 && screen.cursor_row == 4);
    tg_console_puts(&con, "\033[H");
    CHECK(con.col == 0 && con.row == 0);
    tg_console_puts(&con, "\033[;5H");
    CHECK(con.col == 4 && con.row == 0);
    tg_console_puts(&con, "\033[3H");
    CHECK(con.col == 0 && con.row == 2);
    tg_console_puts(&con, "\033[3;H");
    CHECK(con.col == 0 && con.row == 2);
    tg_console_puts(&con, "\033[0;0H");                     /* zero means one */
    CHECK(con.col == 0 && con.row == 0);
    tg_console_puts(&con, "\033[99;99H");                   /* clamped to the screen */
    CHECK(con.col == 79 && con.row == 24);
    tg_console_puts(&con, "\033[7;12f");                    /* f is the same as H */
    CHECK(con.col == 11 && con.row == 6);
    tg_console_puts(&con, "\033[3A");
    CHECK(con.col == 11 && con.row == 3);
    tg_console_puts(&con, "\033[A");                        /* no number: one */
    CHECK(con.row == 2);
    tg_console_puts(&con, "\033[0A");                       /* a zero is one here too */
    CHECK(con.row == 1);
    tg_console_puts(&con, "\033[100A");
    CHECK(con.row == 0 && con.col == 11);
    tg_console_puts(&con, "\033[2B");
    CHECK(con.row == 2);
    tg_console_puts(&con, "\033[2e");                       /* e is B again */
    CHECK(con.row == 4);
    tg_console_puts(&con, "\033[100B");
    CHECK(con.row == 24);
    tg_console_puts(&con, "\033[5C");
    CHECK(con.col == 16);
    tg_console_puts(&con, "\033[a");
    CHECK(con.col == 17);
    tg_console_puts(&con, "\033[100C");
    CHECK(con.col == 79);
    tg_console_puts(&con, "\033[4D");
    CHECK(con.col == 75);
    tg_console_puts(&con, "\033[D");
    CHECK(con.col == 74);
    tg_console_puts(&con, "\033[200D");
    CHECK(con.col == 0);
    tg_console_puts(&con, "\033[10;10H\033[3E");            /* down, to column 1 */
    CHECK(con.col == 0 && con.row == 12);
    tg_console_puts(&con, "\033[5;7H\033[2F");              /* up, to column 1 */
    CHECK(con.col == 0 && con.row == 2);
    tg_console_puts(&con, "\033[20G");
    CHECK(con.col == 19 && con.row == 2);
    tg_console_puts(&con, "\033[5`");
    CHECK(con.col == 4);
    tg_console_puts(&con, "\033[0G");
    CHECK(con.col == 0);
    tg_console_puts(&con, "\033[200G");
    CHECK(con.col == 79);
    tg_console_puts(&con, "\033[9d");
    CHECK(con.row == 8 && con.col == 79);
    tg_console_puts(&con, "\033[d");
    CHECK(con.row == 0);
    tg_console_puts(&con, "\033[200d");
    CHECK(con.row == 24);
    /* The cursor moves do not scroll. */
    ansi_reset(0);
    tg_console_puts(&con, "\033[25;1Hz\033[B\033[B");
    CHECK(con.row == 24 && at(0, 24) == (0x0700 | 'z'));
    /* A wrap that is waiting: the cursor is on the last column and moves from there. */
    ansi_reset(0);
    for (int i = 0; i < 80; i++)
        tg_console_putc(&con, 'a');
    CHECK(con.col == 80);
    tg_console_puts(&con, "\033[C");
    CHECK(con.col == 79 && con.row == 0);
    for (int i = 0; i < 80; i++)
        tg_console_putc(&con, 'b');                         /* (wraps once, to the next row) */
    tg_console_puts(&con, "\033[D");
    CHECK(con.col == 78 && con.row == 1);
    ansi_reset(0);
    tg_console_puts(&con, "\033[1;80Hq");
    CHECK(con.col == 80);
    tg_console_puts(&con, "\033[A");                       /* up from the pending state: column 79, no row to go up to */
    CHECK(con.col == 79 && con.row == 0);
}

static void test_ansi_erase(void)
{
    /* J: from the cursor, to the cursor, everything (and home). Cleared cells take the current colours. */
    ansi_reset(0);
    fill_x();
    tg_console_set_attr(&con, 0x2F);
    tg_console_puts(&con, "\033[10;20H\033[J");
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 80; c++)
            CHECK(at(c, r) == ((r > 9 || (r == 9 && c >= 19)) ? 0x2F20 : 0x1E00 + 'x'));
    CHECK(con.col == 19 && con.row == 9);
    fill_x();
    tg_console_puts(&con, "\033[1J");
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 80; c++)
            CHECK(at(c, r) == ((r < 9 || (r == 9 && c <= 19)) ? 0x2F20 : 0x1E00 + 'x'));
    fill_x();
    tg_console_puts(&con, "\033[0J");
    CHECK(at(18, 9) == 0x1E00 + 'x' && at(19, 9) == 0x2F20 && at(79, 24) == 0x2F20);
    fill_x();
    tg_console_puts(&con, "\033[2J");
    for (int i = 0; i < 2000; i++)
        CHECK(screen.cells[i] == 0x2F20);
    CHECK(con.col == 0 && con.row == 0);                    /* and the cursor goes to the top left */
    fill_x();
    tg_console_puts(&con, "\033[5;5H\033[3J");              /* 3 and the like: the same as 2 */
    CHECK(at(0, 0) == 0x2F20 && con.col == 0 && con.row == 0);
    /* K */
    ansi_reset(0);
    fill_x();
    tg_console_set_attr(&con, 0x4E);
    tg_console_puts(&con, "\033[3;10H\033[K");
    for (int c = 0; c < 80; c++)
        CHECK(at(c, 2) == (c >= 9 ? 0x4E20 : 0x1E00 + 'x') && at(c, 1) == 0x1E00 + 'x' && at(c, 3) == 0x1E00 + 'x');
    fill_x();
    tg_console_puts(&con, "\033[1K");
    for (int c = 0; c < 80; c++)
        CHECK(at(c, 2) == (c <= 9 ? 0x4E20 : 0x1E00 + 'x'));
    fill_x();
    tg_console_puts(&con, "\033[2K");
    for (int c = 0; c < 80; c++)
        CHECK(at(c, 2) == 0x4E20 && at(c, 1) == 0x1E00 + 'x');
    CHECK(con.col == 9 && con.row == 2);                    /* K does not move the cursor */
    fill_x();
    tg_console_puts(&con, "\033[1;80H\033[K");              /* from a waiting wrap: the last cell goes */
    CHECK(at(79, 0) == 0x4E20 && at(78, 0) == 0x1E00 + 'x');
    /* The erased cells are in the colours as they are, reversed or not. */
    ansi_reset(0);
    fill_x();
    tg_console_puts(&con, "\033[7m\033[2K");
    CHECK(at(0, 0) == 0x7020);
}

static void test_ansi_lines_and_chars(void)
{
    ansi_reset(0);
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 80; c++)
            screen.cells[r * 80 + c] = (uint16_t)(0x0700 | ('A' + r));
    tg_console_set_attr(&con, 0x3F);
    tg_console_puts(&con, "\033[5;7H\033[2L");               /* insert two lines at row 5 (index 4) */
    for (int r = 0; r < 25; r++) {
        const uint16_t want = r < 4 ? 0x0700 + 'A' + r : r < 6 ? 0x3F20 : 0x0700 + 'A' + r - 2;
        for (int c = 0; c < 80; c += 13)
            CHECK(at(c, r) == want);
    }
    CHECK(con.col == 6 && con.row == 4);                    /* the cursor stays */
    tg_console_puts(&con, "\033[M");                        /* delete one line: what was pushed down comes back up */
    for (int r = 4; r < 25; r++)                            /* row 4 was a blank one, the rows below it come up */
        CHECK(at(0, r) == (r == 4 || r == 24 ? 0x3F20 : 0x0700 + 'A' + r - 1));
    tg_console_puts(&con, "\033[0M");                       /* zero is one */
    for (int r = 4; r < 25; r++)
        CHECK(at(0, r) == (r >= 23 ? 0x3F20 : 0x0700 + 'A' + r));
    tg_console_puts(&con, "\033[100M");                     /* more than there is: all the rows from the cursor row down */
    for (int r = 4; r < 25; r++)
        CHECK(at(0, r) == 0x3F20);
    CHECK(at(0, 3) == 0x0700 + 'D');
    tg_console_puts(&con, "\033[100L");
    CHECK(at(0, 3) == 0x0700 + 'D' && at(0, 4) == 0x3F20);

    /* P: delete characters, pulling the rest of the line left. */
    ansi_reset(0);
    tg_console_puts(&con, "\033[1;1H0123456789\033[1;4H\033[2P");
    CHECK(at(2, 0) == (0x0700 | '2') && at(3, 0) == (0x0700 | '5') && at(7, 0) == (0x0700 | '9') && at(8, 0) == 0x0720 && at(79, 0) == 0x0720);
    tg_console_set_attr(&con, 0x1F);
    tg_console_puts(&con, "\033[P");
    CHECK(at(3, 0) == (0x0700 | '6') && at(78, 0) == 0x0720 && at(79, 0) == 0x1F20);        /* the cells that come in are in the current colours */
    tg_console_puts(&con, "\033[1;1H\033[500P");           /* more than the line: all of it from the cursor */
    for (int c = 0; c < 80; c++)
        CHECK(at(c, 0) == 0x1F20);
    ansi_reset(0);
    tg_console_puts(&con, "\033[2;75Hwxyz\033[2;78H\033[5P");    /* near the end of the line */
    CHECK(at(74, 1) == (0x0700 | 'w') && at(76, 1) == (0x0700 | 'y'));   /* before the cursor: as it was */
    CHECK(at(77, 1) == 0x0720 && at(78, 1) == 0x0720 && at(79, 1) == 0x0720);   /* the z and what was after it: gone, and no more than that deleted */
}

static void rows_a(void)   /* row r filled with the letter 'A' + r in colour 0x07 */
{
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 80; c++)
            screen.cells[r * 80 + c] = (uint16_t)(0x0700 | ('A' + r));
}

static void test_ansi_region(void)
{
    ansi_reset(0);
    rows_a();
    tg_console_puts(&con, "\033[5;10r");                         /* rows 5 to 10: indexes 4 to 9; the cursor goes home */
    CHECK(con.top == 4 && con.bottom == 9 && con.col == 0 && con.row == 0);
    tg_console_set_attr(&con, 0x2F);
    tg_console_puts(&con, "\033[10;3H\n");                       /* a line feed at the bottom of the region scrolls it, and only it */
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 80; c += 17)
            CHECK(at(c, r) == (r < 4 || r > 9 ? 0x0700 + 'A' + r : r == 9 ? 0x2F20 : 0x0700 + 'A' + r + 1));
    CHECK(con.row == 9 && con.col == 0);                         /* \n is a new line: column 1 */
    tg_console_puts(&con, "\033D");                              /* index: the same without the column */
    CHECK(at(0, 8) == 0x2F20 && at(0, 4) == 0x0700 + 'G' && con.row == 9);
    tg_console_puts(&con, "\033[5;1H\033M");                      /* reverse index at the top of the region: scrolls it down */
    CHECK(at(0, 4) == 0x2F20 && at(0, 5) == 0x0700 + 'G' && at(0, 3) == 0x0700 + 'D' && con.row == 4);
    tg_console_puts(&con, "\033M");                              /* ... and not above the region when the cursor is in it */
    CHECK(at(0, 4) == 0x2F20 && at(0, 5) == 0x2F20 && con.row == 4);
    /* A line feed inside the region just moves down; outside it, it moves down and stops at the bottom of the screen. */
    tg_console_puts(&con, "\033[6;1H\n");
    CHECK(con.row == 6 && at(0, 3) == 0x0700 + 'D');
    tg_console_puts(&con, "\033[15;1H\n");
    CHECK(con.row == 15);
    tg_console_puts(&con, "\033[25;1H\n\n");
    CHECK(con.row == 24 && at(0, 14) == 0x0700 + 'O');           /* nothing scrolled: the screen outside the region is not the region */
    tg_console_puts(&con, "\033[3;1H\033M");                     /* above the region: up a row, no scrolling */
    CHECK(con.row == 1 && at(0, 4) == 0x2F20);
    tg_console_puts(&con, "\033[1;1H\033M");
    CHECK(con.row == 0);
    tg_console_puts(&con, "\033[8;1H\033[D\033[E");               /* (cursor moves do not scroll either) */
    /* Insert and delete line act only with the cursor in the region, and only on the rows from it to the bottom of the region. */
    ansi_reset(0);
    rows_a();
    tg_console_puts(&con, "\033[5;10r\033[7;1H\033[L");          /* at row index 6: rows 6..9 go down, 9 falls off the region */
    for (int r = 0; r < 25; r++)
        CHECK(at(0, r) == (r == 6 ? 0x0720 : r >= 7 && r <= 9 ? 0x0700 + 'A' + r - 1 : 0x0700 + 'A' + r));
    tg_console_puts(&con, "\033[M");                             /* and back up: the blank goes to the bottom of the region */
    for (int r = 0; r < 25; r++)
        CHECK(at(0, r) == (r == 9 ? 0x0720 : r == 6 ? 0x0700 + 'G' : r >= 7 && r <= 8 ? 0x0700 + 'A' + r : 0x0700 + 'A' + r));
    rows_a();
    tg_console_puts(&con, "\033[3;1H\033[L\033[M");              /* above the region: ignored */
    tg_console_puts(&con, "\033[12;1H\033[L\033[M");             /* below it: ignored */
    for (int r = 0; r < 25; r++)
        CHECK(at(0, r) == 0x0700 + 'A' + r);
    /* Regions that are not regions are ignored, and the cursor does not move; ESC[r and a good one home it. */
    ansi_reset(0);
    tg_console_puts(&con, "\033[5;10r\033[3;3H");
    tg_console_puts(&con, "\033[10;5r");
    CHECK(con.top == 4 && con.bottom == 9 && con.col == 2 && con.row == 2);
    tg_console_puts(&con, "\033[5;5r");
    tg_console_puts(&con, "\033[5;99r");
    tg_console_puts(&con, "\033[3;1r");
    CHECK(con.top == 4 && con.bottom == 9 && con.col == 2 && con.row == 2);
    tg_console_puts(&con, "\033[2;4r");
    CHECK(con.top == 1 && con.bottom == 3 && con.col == 0 && con.row == 0);
    tg_console_puts(&con, "\033[7;9H\033[r");                    /* back to the whole screen */
    CHECK(con.top == 0 && con.bottom == 24 && con.col == 0 && con.row == 0);
    tg_console_puts(&con, "\033[0;0r");                          /* zeros are the defaults: 1 to 25 */
    CHECK(con.top == 0 && con.bottom == 24);
    tg_console_puts(&con, "\033[3r");                            /* one number: from there to the bottom */
    CHECK(con.top == 2 && con.bottom == 24);
    tg_console_puts(&con, "\033[1;25r");
    CHECK(con.top == 0 && con.bottom == 24);
    /* The function. */
    tg_console_set_scroll_region(&con, 3, 7);
    CHECK(con.top == 3 && con.bottom == 7);
    tg_console_set_scroll_region(&con, 7, 3);
    CHECK(con.top == 0 && con.bottom == 24);
    tg_console_set_scroll_region(&con, 3, 7);
    tg_console_set_scroll_region(&con, -1, 5);
    CHECK(con.top == 0 && con.bottom == 24);
    tg_console_set_scroll_region(&con, 3, 7);
    tg_console_set_scroll_region(&con, 0, 25);
    CHECK(con.top == 0 && con.bottom == 24);
    tg_console_set_scroll_region(&con, 5, 5);
    CHECK(con.top == 0 && con.bottom == 24);
    /* The region is what text scrolls in, without the ANSI mode as well. */
    reset(0x07, 0);
    rows_a();
    tg_console_set_scroll_region(&con, 2, 4);
    tg_console_gotoxy(&con, 0, 4);
    tg_console_puts(&con, "z\n");
    CHECK(con.row == 4 && at(0, 2) == 0x0700 + 'D' && at(0, 3) == (0x0700 | 'z') && at(0, 4) == 0x0720 && at(0, 1) == 0x0700 + 'B' && at(0, 5) == 0x0700 + 'F');
    tg_console_gotoxy(&con, 79, 4);
    tg_console_puts(&con, "ab");                                  /* a wrap at the bottom of the region scrolls it too */
    CHECK(con.row == 4 && con.col == 1 && at(0, 4) == (0x0700 | 'b') && at(79, 3) == (0x0700 | 'a') && at(0, 5) == 0x0700 + 'F');
    tg_console_gotoxy(&con, 0, 24);                              /* the last row of the screen is outside the region: no scroll there */
    tg_console_puts(&con, "\n\n");
    CHECK(con.row == 24 && at(0, 24) == 0x0700 + 'Y');
}

static void test_ansi_modes(void)
{
    /* \n: a new line (column 1) unless TG_CONSOLE_LF_ONLY. */
    ansi_reset(0);
    tg_console_puts(&con, "ab\ncd");
    CHECK(at(0, 1) == (0x0700 | 'c') && at(1, 1) == (0x0700 | 'd') && con.col == 2 && con.row == 1);
    ansi_reset(TG_CONSOLE_LF_ONLY);
    tg_console_puts(&con, "ab\ncd");
    CHECK(at(2, 1) == (0x0700 | 'c') && at(3, 1) == (0x0700 | 'd') && at(0, 1) == 0x0720 && con.col == 4 && con.row == 1);
    tg_console_puts(&con, "\r\n\r\n");
    CHECK(con.col == 0 && con.row == 3);
    tg_console_puts(&con, "\033[25;5H\n");                       /* at the bottom, a line feed scrolls and keeps the column */
    CHECK(con.row == 24 && con.col == 4);
    ansi_reset(TG_CONSOLE_LF_ONLY);
    for (int i = 0; i < 80; i++)
        tg_console_putc(&con, 'a');
    tg_console_putc(&con, '\n');                                 /* from a waiting wrap: the last column, and down */
    CHECK(con.col == 79 && con.row == 1);
    /* Without the ANSI mode LF_ONLY does nothing: \n is the plain console's. */
    reset(0x07, TG_CONSOLE_LF_ONLY);
    tg_console_puts(&con, "ab\ncd");
    CHECK(at(0, 1) == (0x0700 | 'c') && con.col == 2);
    /* Form feed clears the screen and goes home; NUL is nothing. */
    ansi_reset(0);
    fill_x();
    tg_console_set_attr(&con, 0x4E);
    tg_console_gotoxy(&con, 30, 10);
    tg_console_putc(&con, '\f');
    CHECK(con.col == 0 && con.row == 0);
    for (int i = 0; i < 2000; i++)
        CHECK(screen.cells[i] == 0x4E20);
    tg_console_set_attr(&con, 0x07);
    tg_console_puts(&con, "a");
    tg_console_putc(&con, 0);
    tg_console_puts(&con, "b");
    CHECK(at(0, 0) == (0x0700 | 'a') && at(1, 0) == (0x0700 | 'b') && con.col == 2);
    reset(0x07, 0);                                              /* the plain console draws both, as before */
    tg_console_putc(&con, '\f');
    CHECK(con.col == 1 && screen.cells[0] == 0x070C);
    tg_console_putc(&con, 0);
    CHECK(con.col == 2 && screen.cells[1] == 0x0700);
    tg_console_putc(&con, 0x1B);                                 /* and ESC is a character */
    tg_console_puts(&con, "[H");
    CHECK(con.col == 5 && screen.cells[2] == 0x071B && screen.cells[3] == (0x0700 | '['));
    /* Saving and restoring. */
    ansi_reset(0);
    tg_console_puts(&con, "\033[5;6H\033[1;31m\0337");
    tg_console_puts(&con, "\033[20;30H\033[0;44m");
    CHECK(con.attr == 0x17);
    tg_console_puts(&con, "\0338x");
    CHECK(at(5, 4) == 0x0C00 + 'x' && con.col == 6 && con.row == 4 && con.attr == 0x0C);
    tg_console_puts(&con, "\033[10;10H\033[s\033[1;1H\033[42m\033[u");     /* the position only */
    CHECK(con.col == 9 && con.row == 9 && con.attr == 0x2C);
    tg_console_puts(&con, "\033[7m\0337\033[0m\0338");
    CHECK(con.reverse == 1 && con.attr == (uint8_t)(0x2C << 4 | 0x2C >> 4));   /* the reversed state is saved too */
    tg_console_puts(&con, "\033[0m\033[24;75H\033[s\033[H\033[u");
    CHECK(con.col == 74 && con.row == 23);
    /* Nothing saved: the top left, in the colours the console started with. */
    ansi_reset(0);
    tg_console_puts(&con, "\033[1;31m\033[10;10H\033[u");
    CHECK(con.col == 0 && con.row == 0 && con.attr == 0x0C);
    tg_console_puts(&con, "\0338");
    CHECK(con.col == 0 && con.row == 0 && con.attr == 0x07);
    /* The index, reverse index and next line from a waiting wrap start on the last column; the next line goes to the first. */
    ansi_reset(0);
    for (int i = 0; i < 80; i++)
        tg_console_putc(&con, 'a');
    tg_console_puts(&con, "\033D");
    CHECK(con.col == 79 && con.row == 1);
    ansi_reset(0);
    tg_console_puts(&con, "\033[3;80Hq\033M");
    CHECK(con.col == 79 && con.row == 1);
    ansi_reset(0);
    tg_console_puts(&con, "\033[3;80Hq\033E");
    CHECK(con.col == 0 && con.row == 3);
    ansi_reset(0);
    tg_console_puts(&con, "\033[3;5H\033E");
    CHECK(con.col == 0 && con.row == 3);
    tg_console_puts(&con, "\033[3;5H\033D");
    CHECK(con.col == 4 && con.row == 3);
    tg_console_puts(&con, "\033[3;5H\033M");
    CHECK(con.col == 4 && con.row == 1);
    /* A save while a wrap waits is of the last column. */
    ansi_reset(0);
    tg_console_puts(&con, "\033[1;80Hq\0337\033[10;10H\0338");
    CHECK(con.col == 79 && con.row == 0);
    /* ESC c resets all of it. */
    ansi_reset(0);
    fill_x();
    tg_console_puts(&con, "\033[3;8r\033[1;37;44m\033[7m\033[12;12H\0337\033c");
    CHECK(con.top == 0 && con.bottom == 24 && con.attr == 0x07 && con.reverse == 0 && con.col == 0 && con.row == 0);
    for (int i = 0; i < 2000; i++)
        CHECK(screen.cells[i] == 0x0720);
    tg_console_puts(&con, "\0338");
    CHECK(con.col == 0 && con.row == 0 && con.attr == 0x07 && con.reverse == 0);
}

static uint8_t attr_after(const char *sequence, uint8_t start)
{
    ansi_reset(0);
    tg_console_init(&con, &screen, start);
    con.flags = TG_CONSOLE_ANSI;
    tg_console_puts(&con, sequence);
    tg_console_putc(&con, 'x');
    return (uint8_t)(screen.cells[0] >> 8);
}

static void test_ansi_colours(void)
{
    static const struct { const char *sequence; uint8_t start, want; } t[] = {
        {"", 0x07, 0x07},
        {"\033[1m", 0x07, 0x0F}, {"\033[22m", 0x0F, 0x07}, {"\033[1m\033[22m", 0x07, 0x07}, {"\033[1;22m", 0x07, 0x07},
        {"\033[30m", 0x07, 0x00}, {"\033[31m", 0x07, 0x04}, {"\033[32m", 0x07, 0x02}, {"\033[33m", 0x07, 0x06},
        {"\033[34m", 0x07, 0x01}, {"\033[35m", 0x07, 0x05}, {"\033[36m", 0x07, 0x03}, {"\033[37m", 0x00, 0x07},
        {"\033[40m", 0x4F, 0x0F}, {"\033[41m", 0x07, 0x47}, {"\033[42m", 0x07, 0x27}, {"\033[43m", 0x07, 0x67},
        {"\033[44m", 0x07, 0x17}, {"\033[45m", 0x07, 0x57}, {"\033[46m", 0x07, 0x37}, {"\033[47m", 0x07, 0x77},
        {"\033[1;31m", 0x07, 0x0C}, {"\033[31;1m", 0x07, 0x0C}, {"\033[1;33m", 0x07, 0x0E}, {"\033[1;31;44m", 0x07, 0x1C},
        {"\033[1;31m\033[32m", 0x07, 0x0A},                      /* a new colour keeps the bold */
        {"\033[91m", 0x07, 0x0C}, {"\033[97m", 0x07, 0x0F}, {"\033[90m", 0x07, 0x08},
        {"\033[101m", 0x07, 0xC7}, {"\033[107m", 0x07, 0xF7}, {"\033[100m", 0x07, 0x87},
        {"\033[5m", 0x07, 0x87}, {"\033[5;25m", 0x07, 0x07}, {"\033[41;5m", 0x07, 0xC7},      /* blink is the bright background here */
        {"\033[7m", 0x07, 0x70}, {"\033[7;27m", 0x07, 0x07}, {"\033[31;7m", 0x07, 0x40}, {"\033[7;31m", 0x07, 0x40},
        {"\033[1;44;7m", 0x07, 0xF1}, {"\033[7m\033[44m", 0x07, 0x71}, {"\033[7m\033[0m", 0x07, 0x07},
        {"\033[m", 0x1E, 0x1E}, {"\033[0m", 0x1E, 0x1E}, {"\033[31m", 0x1E, 0x1C}, {"\033[31m\033[39m", 0x1E, 0x1E},
        {"\033[42m", 0x1E, 0x2E}, {"\033[42;49m", 0x1E, 0x1E}, {"\033[1;5;7;41;32m", 0x1E, 0xAC},
        {"\033[1;7;0m", 0x1E, 0x1E}, {"\033[;m", 0x1E, 0x1E}, {"\033[;;31m", 0x1E, 0x1C},
        {"\033[31m\033[m", 0x1E, 0x1E}, {"\033[1;7m\033[m", 0x07, 0x07}, {"\033[65537m", 0x07, 0x07}, {"\033[1;70000m", 0x07, 0x0F},
        {"\033[38;2;5;31;7m", 0x07, 0x07}, {"\033[38;2;5;31;7;1m", 0x07, 0x0F}, {"\033[48;5;31m", 0x07, 0x07}, {"\033[48;5;31;1m", 0x07, 0x0F}, {"\033[48;5;31;7m", 0x07, 0x70},
        {"\033[38;5;196m", 0x07, 0x07}, {"\033[38;5;196;1m", 0x07, 0x0F}, {"\033[48;5;7;31m", 0x07, 0x04},
        {"\033[38;2;1;2;3;1m", 0x07, 0x0F}, {"\033[48;2;1;2;3m", 0x07, 0x07}, {"\033[38m", 0x07, 0x07}, {"\033[38;5m", 0x07, 0x07},
        {"\033[38;2;9m", 0x07, 0x07}, {"\033[38;6;1m", 0x07, 0x0F},                          /* 38;6 is not a form: only the 38 is skipped, the 6 does nothing and the 1 is bold */
        {"\033[99m", 0x07, 0x07}, {"\033[9999m", 0x07, 0x07}, {"\033[99999999999m", 0x07, 0x07}, {"\033[2m", 0x07, 0x07},
        {"\033[0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;1m", 0x07, 0x07},      /* only 16 numbers are kept: the 17th, the 1, is lost */
        {"\033[0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;1m", 0x07, 0x0F},        /* ... and the 16th is not */
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) {
        const uint8_t got = attr_after(t[i].sequence, t[i].start);
        if (got != t[i].want)
            printf("sequence %zu: wanted %02X, got %02X\n", i, t[i].want, got);
        CHECK(got == t[i].want);
    }
    /* The colours apply from then on, not to what is there. */
    ansi_reset(0);
    tg_console_puts(&con, "a\033[1;31mb\033[0mc");
    CHECK(at(0, 0) == 0x0761 && at(1, 0) == 0x0C62 && at(2, 0) == 0x0763);
    /* tg_console_set_attr is the colours asked for: not reversed, whatever the sequences said. */
    ansi_reset(0);
    tg_console_puts(&con, "\033[7m");
    CHECK(con.reverse == 1);
    tg_console_set_attr(&con, 0x1F);
    CHECK(con.reverse == 0 && con.attr == 0x1F && con.base_attr == 0x1F);
    tg_console_puts(&con, "\033[27m");
    CHECK(con.attr == 0x1F);
    tg_console_puts(&con, "\033[0m");                            /* a reset goes to the colours of tg_console_init, not of set_attr */
    CHECK(con.attr == 0x07 && con.default_attr == 0x07);
}

static void test_ansi_malformed(void)
{
    ansi_reset(0);
    tg_console_puts(&con, "\033[?25lX");                         /* private: nothing happens, and what follows is text */
    CHECK(at(0, 0) == (0x0700 | 'X') && at(1, 0) == 0x0720 && con.col == 1);
    ansi_reset(0);
    tg_console_puts(&con, "\033[?3;4HX");                        /* a private sequence is not the one with the same final: the cursor did not move */
    CHECK(at(0, 0) == (0x0700 | 'X') && con.col == 1 && con.row == 0);
    tg_console_puts(&con, "\033[<5;5HY\033[=2;2HZ\033[>3;3HW");
    CHECK(at(1, 0) == (0x0700 | 'Y') && at(2, 0) == (0x0700 | 'Z') && at(3, 0) == (0x0700 | 'W') && con.row == 0);
    tg_console_puts(&con, "\033[2;3 HV\033[!pU\033[\"mT");     /* nor is a sequence with an intermediate byte */
    CHECK(at(4, 0) == (0x0700 | 'V') && at(5, 0) == (0x0700 | 'U') && at(6, 0) == (0x0700 | 'T') && con.row == 0);
    ansi_reset(0);
    tg_console_puts(&con, "\033[1;2;3zX\033[ qY\033[>cZ");       /* unknown finals, an intermediate byte, a private one */
    CHECK(at(0, 0) == (0x0700 | 'X') && at(1, 0) == (0x0700 | 'Y') && at(2, 0) == (0x0700 | 'Z') && con.col == 3);
    ansi_reset(0);
    tg_console_puts(&con, "\033[31\030mX");                      /* CAN cancels it: the m is text */
    CHECK(at(0, 0) == (0x0700 | 'm') && at(1, 0) == (0x0700 | 'X') && con.attr == 0x07);
    ansi_reset(0);
    tg_console_puts(&con, "\033[31\032mX");                      /* so does SUB */
    CHECK(at(0, 0) == (0x0700 | 'm') && at(1, 0) == (0x0700 | 'X'));
    ansi_reset(0);
    tg_console_puts(&con, "\033[3\033[5;6HX");                   /* ESC starts again */
    CHECK(at(5, 4) == (0x0700 | 'X') && con.col == 6 && con.row == 4);
    ansi_reset(0);
    tg_console_puts(&con, "\033\033[2;2HX\033\033\033[3;3HY");
    CHECK(at(1, 1) == (0x0700 | 'X') && at(2, 2) == (0x0700 | 'Y'));
    ansi_reset(0);
    tg_console_puts(&con, "\033[3\xC9X");                        /* a byte that cannot be in a sequence: it is dropped, and the byte with it */
    CHECK(at(0, 0) == (0x0700 | 'X') && con.col == 1);
    ansi_reset(0);
    tg_console_puts(&con, "\033\xC9X\033ZY\033[\x7FW");           /* the same after ESC; an ESC Z that nothing knows; DEL in a sequence */
    CHECK(at(0, 0) == (0x0700 | 'X') && at(1, 0) == (0x0700 | 'Y') && at(2, 0) == (0x0700 | 'W') && con.col == 3);
    ansi_reset(0);
    tg_console_puts(&con, "\033(BX\033)0Y\033#8Z\033 FW");        /* the byte after ESC ( ESC ) ESC # ESC SP is not text */
    CHECK(at(0, 0) == (0x0700 | 'X') && at(1, 0) == (0x0700 | 'Y') && at(2, 0) == (0x0700 | 'Z') && at(3, 0) == (0x0700 | 'W') && con.col == 4);
    ansi_reset(0);
    tg_console_puts(&con, "\033[3;4\rH");                         /* a control inside a sequence acts, and the sequence goes on */
    CHECK(con.col == 3 && con.row == 2);
    ansi_reset(0);
    tg_console_puts(&con, "ab\033[1;2\bm");                      /* a backspace inside: the cursor moves, and the colour is set */
    CHECK(con.col == 1 && con.attr == 0x0F && con.base_attr == 0x0F);
    ansi_reset(0);
    tg_console_puts(&con, "\033[99999999999A\033[99999999999;99999999999H");   /* long numbers do not overflow */
    CHECK(con.col == 79 && con.row == 24);
    ansi_reset(0);
    tg_console_puts(&con, "\033[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20;3;7H");   /* more numbers than are kept: the 1st and 2nd count */
    CHECK(con.col == 1 && con.row == 0);
    /* A sequence can arrive in pieces, between calls and in printf. */
    ansi_reset(0);
    tg_console_puts(&con, "\033[1");
    tg_console_puts(&con, ";31");
    tg_console_putc(&con, 'm');
    tg_console_putc(&con, 'x');
    CHECK(at(0, 0) == 0x0C78);
    CHECK(tg_console_printf(&con, "\033[%d;%dH%s", 3, 4, "hi") == 8 && at(3, 2) == (0x0C00 | 'h') && at(4, 2) == (0x0C00 | 'i'));
    tg_console_printf(&con, "\033[%dm", 44);
    tg_console_printf(&con, "[%d;%dH", 5, 6);                    /* (the ESC was in the last call: this is text now) */
    CHECK(con.attr == 0x1C);                                     /* bold red, then a blue background: 0x1C */
}

static void test_ansi_fuzz(void)
{
    ansi_reset(0);
    static const char alphabet[] = "\033\033[[;;;;0123456789ABCDEFGHJKLMPXZabcdefmrsu?><=!\" #()789cDEM\030\032\n\r\b\t\f\a";
    for (int i = 0; i < 1500000; i++) {
        const uint32_t r = rnd() % 100;
        if (r < 3) {
            con.flags = (uint8_t)(TG_CONSOLE_ANSI | (rnd() % 2 ? TG_CONSOLE_LF_ONLY : 0) | (rnd() % 4 == 0 ? TG_CONSOLE_NOWRAP : 0));
        } else if (r < 4) {
            tg_console_set_attr(&con, (uint8_t)rnd());
        } else if (r < 5) {
            tg_console_gotoxy(&con, (int)(rnd() % 100) - 10, (int)(rnd() % 40) - 5);
        } else if (r < 6) {
            tg_console_set_scroll_region(&con, (int)(rnd() % 30) - 3, (int)(rnd() % 30) - 3);
        } else if (r < 8) {
            tg_console_printf(&con, "\033[%d;%dH\033[%dm", (int)(rnd() % 40), (int)(rnd() % 100), (int)(rnd() % 110));
        } else if (r < 70) {
            tg_console_putc(&con, alphabet[rnd() % (sizeof alphabet - 1)]);
        } else if (r < 85) {
            tg_console_putc(&con, (int)rnd());
        } else {
            tg_console_putc(&con, 'a' + (int)(rnd() % 26));
        }
        CHECK(con.col >= 0 && con.col <= 80 && con.row >= 0 && con.row <= 24);
        CHECK(con.top >= 0 && con.top < con.bottom && con.bottom <= 24);
        CHECK(con.ansi_state <= 3 && con.ansi_count <= TG_CONSOLE_ANSI_PARAMS && con.ansi_number >= 0 && con.ansi_number <= 9999);
        for (int k = 0; k < con.ansi_count; k++)
            CHECK(con.ansi_param[k] <= 9999);
        CHECK(con.attr == (con.reverse ? (uint8_t)(con.base_attr << 4 | con.base_attr >> 4) : con.base_attr));
        CHECK(con.saved_col >= 0 && con.saved_col < 80 && con.saved_row >= 0 && con.saved_row < 25);
        CHECK(screen.cursor_col >= 0 && screen.cursor_col < 80 && screen.cursor_row >= 0 && screen.cursor_row < 25);
    }
}

int main(void)
{
    test_random();
    test_rules();
    test_printf();
    test_ansi_cursor();
    test_ansi_erase();
    test_ansi_lines_and_chars();
    test_ansi_region();
    test_ansi_modes();
    test_ansi_colours();
    test_ansi_malformed();
    test_ansi_fuzz();
    printf("all %d checks passed\n", checks);
    return 0;
}
