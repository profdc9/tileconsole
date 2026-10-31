/* SPDX-License-Identifier: MIT */
/* Tests of the console demo logic: type into it and look at the screen. Exits non-zero on the first failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "consdemo_logic.h"
#include "platform.h"
#include "tg_text.h"

static int checks;
#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static uint32_t rng = 3;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static tg_text_t screen;
static consdemo_t d;

static uint32_t clock_ms = 12345;
static uint32_t fake_millis(void) { return clock_ms; }

static void fresh(void)
{
    memset(&screen, 0, sizeof screen);
    tg_text_load_cp437(&screen);
    tg_text_init(&screen);
    consdemo_init(&d, &screen, fake_millis);
}

static void type(const char *text)
{
    for (; *text; text++)
        consdemo_char(&d, (unsigned char)*text == '\n' ? 10 : (unsigned char)*text);
}

/* The text of a row, without the spaces at the end. */
static const char *row(int r)
{
    static char text[2][81];
    static int which;
    char *out = text[which ^= 1];
    int n = 0;
    for (int c = 0; c < 80; c++)
        out[n++] = (char)(screen.cells[r * 80 + c] & 255);
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = 0;
    return out;
}

static int rows_with(const char *prefix)   /* how many rows start with this text */
{
    int n = 0;
    for (int r = 0; r < 25; r++)
        n += strncmp(row(r), prefix, strlen(prefix)) == 0;
    return n;
}

static int find_row(const char *prefix)
{
    for (int r = 0; r < 25; r++)
        if (strncmp(row(r), prefix, strlen(prefix)) == 0)
            return r;
    return -1;
}

static void test_start(void)
{
    fresh();
    CHECK(strcmp(row(0), "TILEGFX CONSOLE") == 0);
    CHECK(strncmp(row(1), "80 x 25 text mode", 17) == 0);
    CHECK(strcmp(row(2), "") == 0);
    CHECK(strcmp(row(3), "C:\\>") == 0);
    CHECK(d.prompt_row == 3 && d.len == 0 && d.pos == 0 && d.browsing == -1);
    CHECK((screen.cursor_flags & TG_CURSOR_VISIBLE) && screen.cursor_col == 5 && screen.cursor_row == 3);
    CHECK((screen.cells[0] >> 8) == TG_TEXT_ATTR(TG_TEXT_YELLOW, TG_TEXT_BLACK));       /* the banner is yellow */
    CHECK((screen.cells[3 * 80] >> 8) == TG_TEXT_ATTR(TG_TEXT_LIGHT_GREEN, TG_TEXT_BLACK));   /* the prompt is green */
    CHECK((screen.cells[3 * 80 + 5] >> 8) == TG_TEXT_ATTR(TG_TEXT_LIGHT_GREY, TG_TEXT_BLACK));   /* what is typed is grey */
}

static void test_editing(void)
{
    fresh();
    type("abc");
    CHECK(strcmp(row(3), "C:\\> abc") == 0 && d.len == 3 && d.pos == 3 && screen.cursor_col == 8);
    consdemo_char(&d, 8);                                  /* Backspace */
    CHECK(strcmp(row(3), "C:\\> ab") == 0 && screen.cursor_col == 7);
    consdemo_char(&d, 8);
    consdemo_char(&d, 8);
    consdemo_char(&d, 8);                                  /* more than there is: nothing */
    CHECK(strcmp(row(3), "C:\\>") == 0 && d.len == 0 && screen.cursor_col == 5);
    consdemo_char(&d, 127);                                /* Delete at the end: nothing */
    CHECK(d.len == 0);
    type("ac");
    consdemo_char(&d, PLAT_KEY_LEFT);
    CHECK(screen.cursor_col == 6);
    type("b");                                             /* inserted between */
    CHECK(strcmp(row(3), "C:\\> abc") == 0 && d.pos == 2 && screen.cursor_col == 7);
    consdemo_char(&d, 127);                                /* Delete the c */
    CHECK(strcmp(row(3), "C:\\> ab") == 0 && d.pos == 2 && screen.cursor_col == 7);
    consdemo_char(&d, PLAT_KEY_HOME);
    CHECK(d.pos == 0 && screen.cursor_col == 5);
    consdemo_char(&d, PLAT_KEY_LEFT);                      /* not left of the start */
    CHECK(d.pos == 0);
    consdemo_char(&d, 127);                                /* Delete the a */
    CHECK(strcmp(row(3), "C:\\> b") == 0 && d.pos == 0);
    consdemo_char(&d, PLAT_KEY_END);
    CHECK(d.pos == 1 && screen.cursor_col == 6);
    consdemo_char(&d, PLAT_KEY_RIGHT);                     /* not right of the end */
    CHECK(d.pos == 1);
    consdemo_char(&d, 1);                                  /* Ctrl+A */
    CHECK(d.pos == 0);
    consdemo_char(&d, 5);                                  /* Ctrl+E */
    CHECK(d.pos == 1);
    type("xyz");
    consdemo_char(&d, 1);
    consdemo_char(&d, PLAT_KEY_RIGHT);
    consdemo_char(&d, PLAT_KEY_RIGHT);
    CHECK(d.pos == 2 && strcmp(d.line, "bxyz") == 0);
    consdemo_char(&d, 11);                                 /* Ctrl+K: from here to the end */
    CHECK(strcmp(row(3), "C:\\> bx") == 0 && d.len == 2 && d.pos == 2);
    consdemo_char(&d, 21);                                 /* Ctrl+U: all of it */
    CHECK(strcmp(row(3), "C:\\>") == 0 && d.len == 0 && d.pos == 0 && screen.cursor_col == 5);
    /* Keys that are not for the line do nothing to it. */
    type("q");
    static const int others[] = {PLAT_KEY_F(1), PLAT_KEY_PAGEUP, PLAT_KEY_PAGEDOWN, PLAT_KEY_INSERT, 9, 27, 2, 4, 6, 7, 14, 0x1FF, -1, 300, 126 + 1};
    for (size_t i = 0; i < sizeof others / sizeof *others; i++)
        consdemo_char(&d, others[i]);
    CHECK(strcmp(row(3), "C:\\> q") == 0 && d.len == 1);
    /* The line is as long as it may be and no more, and a line of that length can be cleared away completely. */
    consdemo_char(&d, 21);
    for (int i = 0; i < 100; i++)
        consdemo_char(&d, 'a' + i % 26);
    CHECK(d.len == CONSDEMO_LINE_MAX && (int)strlen(row(3)) == 5 + CONSDEMO_LINE_MAX && d.line[CONSDEMO_LINE_MAX] == 0);
    CHECK(screen.cursor_col == 5 + CONSDEMO_LINE_MAX && screen.cursor_row == 3);
    consdemo_char(&d, PLAT_KEY_HOME);
    consdemo_char(&d, 'Z');                                /* the line is full: nothing is inserted */
    CHECK(d.line[0] == 'a' && d.len == CONSDEMO_LINE_MAX);
    consdemo_char(&d, 21);
    CHECK(strcmp(row(3), "C:\\>") == 0);
}

static void test_commands(void)
{
    fresh();
    type("echo hello world\n");
    CHECK(strcmp(row(3), "C:\\> echo hello world") == 0 && strcmp(row(4), "hello world") == 0 && strcmp(row(5), "C:\\>") == 0);
    CHECK(d.prompt_row == 5 && screen.cursor_row == 5 && screen.cursor_col == 5 && d.len == 0);
    type("   ECHO   spaced   out   \n");                   /* the blanks around the command and the end of the text are dropped */
    CHECK(strcmp(row(5), "C:\\>    ECHO   spaced   out") == 0 && strcmp(row(6), "spaced   out") == 0);
    type("\n");                                            /* an empty line: just a new prompt */
    CHECK(strcmp(row(7), "C:\\>") == 0 && strcmp(row(8), "C:\\>") == 0 && d.history_count == 2);
    type("nonsense\n");
    CHECK(strcmp(row(9), "Bad command or file name.") == 0 && strcmp(row(10), "C:\\>") == 0);
    type("echo\n");                                        /* no text: an empty line */
    CHECK(strcmp(row(11), "") == 0 && strcmp(row(12), "C:\\>") == 0);
    type("ver\n");
    CHECK(find_row("tilegfx console demo") >= 0);
    type("uptime\n");
    CHECK(find_row("12345 ms") >= 0);
    clock_ms = 4000000000u;
    type("uptime\n");
    CHECK(find_row("4000000000 ms") >= 0);

    fresh();
    type("help\n");
    CHECK(find_row("Commands:") >= 0 && find_row("  HELP") >= 0 && find_row("  KEYS") >= 0 && find_row("Editing:") >= 0);
    CHECK((screen.cells[find_row("Commands:") * 80] >> 8) == TG_TEXT_ATTR(TG_TEXT_YELLOW, TG_TEXT_BLACK));

    fresh();
    type("test\n");
    CHECK(strcmp(row(4), "int [42] [   42] [42   ] [00042] [+42]") == 0);
    CHECK(strcmp(row(5), "hex [beef] [BEEF] [0000beef] [0xff]  octal [10]") == 0);
    CHECK(strcmp(row(6), "char [abc] string [hello] [     hello] [hello     ] [hel]") == 0);
    CHECK(strcmp(row(7), "unsigned [4000000000] long [-1234567890] 100%") == 0);

    fresh();
    type("chars\n");
    for (int r = 0; r < 16; r++) {
        char want[8];
        snprintf(want, sizeof want, " %X ", r);
        const int top = find_row(" 0 ");
        CHECK(top >= 0 && strncmp(row(top + r), want, 3) == 0);
        for (int c = 0; c < 16; c++) {
            const int code = r * 16 + c;
            const uint16_t cell = screen.cells[(top + r) * 80 + 3 + c];
            const int blank = code == 7 || code == 8 || code == 9 || code == 10 || code == 13;
            CHECK((cell & 255) == (blank ? ' ' : code));
        }
    }

    fresh();
    type("color 14 1\n");
    type("echo yellow on blue\n");
    CHECK(strcmp(row(5), "yellow on blue") == 0 && (screen.cells[5 * 80] >> 8) == TG_TEXT_ATTR(14, 1));
    CHECK((screen.cells[3 * 80 + 10] >> 8) == TG_TEXT_ATTR(TG_TEXT_LIGHT_GREY, TG_TEXT_BLACK));   /* the line that was typed before keeps its colours */
    CHECK((screen.cells[6 * 80] >> 8) == TG_TEXT_ATTR(TG_TEXT_LIGHT_GREEN, TG_TEXT_BLACK));       /* and the prompt is always green on black */
    type("color 3\n");
    type("color 16 0\n");
    type("color 0 -1\n");
    type("color x y\n");
    CHECK(rows_with("Usage: COLOR") == 4);
    type("color 0 15\necho z\n");
    CHECK((screen.cells[find_row("z") * 80] >> 8) == TG_TEXT_ATTR(0, 15));

    fresh();
    type("cursor block\n");
    CHECK(screen.cursor_start == 0 && screen.cursor_end == 15);
    type("cursor half\n");
    CHECK(screen.cursor_start == 8 && screen.cursor_end == 15);
    type("CURSOR Line\n");
    CHECK(screen.cursor_start == 7 && screen.cursor_end == 8);
    type("cursor underline\n");
    CHECK(screen.cursor_start == 14 && screen.cursor_end == 15);
    type("cursor block   \n");                            /* blanks after the last word do not matter */
    CHECK(screen.cursor_start == 0 && screen.cursor_end == 15 && rows_with("Usage: CURSOR") == 0);
    type("  cursor   half\t \n");                         /* nor do blanks before it (a tab is not a blank here: it is not typed at all) */
    CHECK(screen.cursor_start == 8);
    type("cursor\n");
    type("cursor wide\n");
    CHECK(rows_with("Usage: CURSOR") == 2 && screen.cursor_start == 8);   /* the shape from before is kept */

    fresh();
    type("cls\n");                                         /* clears, and the next prompt is at the top */
    CHECK(strcmp(row(0), "C:\\>") == 0 && strcmp(row(1), "") == 0 && d.prompt_row == 0 && screen.cursor_row == 0);
    type("echo x\n");
    type("clear\n");
    CHECK(strcmp(row(0), "C:\\>") == 0 && rows_with("x") == 0);
}

static void test_history(void)
{
    fresh();
    consdemo_char(&d, PLAT_KEY_UP);                        /* no history yet */
    CHECK(d.len == 0);
    type("echo one\n");
    type("echo two\n");
    type("echo two\n");                                    /* the same again is not remembered twice */
    CHECK(d.history_count == 2);
    consdemo_char(&d, PLAT_KEY_UP);
    CHECK(strcmp(d.line, "echo two") == 0 && d.pos == 8);
    CHECK(strcmp(row(d.prompt_row), "C:\\> echo two") == 0 && screen.cursor_col == 5 + 8);
    consdemo_char(&d, PLAT_KEY_UP);
    CHECK(strcmp(d.line, "echo one") == 0);
    consdemo_char(&d, PLAT_KEY_UP);                        /* the oldest: stays */
    CHECK(strcmp(d.line, "echo one") == 0);
    consdemo_char(&d, PLAT_KEY_DOWN);
    CHECK(strcmp(d.line, "echo two") == 0);
    consdemo_char(&d, PLAT_KEY_DOWN);
    CHECK(d.len == 0 && strcmp(row(d.prompt_row), "C:\\>") == 0 && d.browsing == -1);
    consdemo_char(&d, PLAT_KEY_DOWN);
    CHECK(d.len == 0);
    /* A recalled line is edited and run like any other, and is remembered in its new form. */
    consdemo_char(&d, PLAT_KEY_UP);
    consdemo_char(&d, 8);
    consdemo_char(&d, 8);
    consdemo_char(&d, 8);
    type("one hundred\n");
    CHECK(find_row("C:\\> echo one hundred") >= 0 && find_row("one hundred") >= 0);
    CHECK(d.history_count == 3 && strcmp(d.history[0], "echo one hundred") == 0 && strcmp(d.history[1], "echo two") == 0);
    /* A recalled line shorter than the one before leaves nothing of it behind. */
    type("echo a very very long line indeed\n");
    consdemo_char(&d, PLAT_KEY_UP);
    consdemo_char(&d, PLAT_KEY_UP);
    consdemo_char(&d, PLAT_KEY_UP);
    CHECK(strcmp(d.line, "echo two") == 0 && strcmp(row(d.prompt_row), "C:\\> echo two") == 0);
    /* The history holds CONSDEMO_HISTORY lines, the newest first. */
    fresh();
    for (int i = 0; i < 12; i++) {
        char c[24];
        snprintf(c, sizeof c, "echo %d\n", i);
        type(c);
    }
    CHECK(d.history_count == CONSDEMO_HISTORY);
    for (int i = 0; i < CONSDEMO_HISTORY + 3; i++)
        consdemo_char(&d, PLAT_KEY_UP);
    CHECK(strcmp(d.line, "echo 4") == 0);
    for (int i = 0; i < CONSDEMO_HISTORY - 1; i++)
        consdemo_char(&d, PLAT_KEY_DOWN);
    CHECK(strcmp(d.line, "echo 11") == 0);
}

static void test_control_keys(void)
{
    fresh();
    type("half a line");
    consdemo_char(&d, 3);                                  /* Ctrl+C */
    CHECK(strcmp(row(3), "C:\\> half a line^C") == 0 && strcmp(row(4), "C:\\>") == 0 && d.len == 0 && d.prompt_row == 4);
    type("echo kept");
    type("");
    for (int i = 0; i < 6; i++)
        consdemo_char(&d, 'x');
    consdemo_char(&d, PLAT_KEY_LEFT);
    consdemo_char(&d, 12);                                 /* Ctrl+L: the screen is cleared and the line stays, with its cursor */
    CHECK(strcmp(row(0), "C:\\> echo keptxxxxxx") == 0 && strcmp(row(1), "") == 0 && d.prompt_row == 0);
    CHECK(screen.cursor_row == 0 && screen.cursor_col == 5 + 14);   /* after the 14th character: one left of the end of the 15 */
    consdemo_char(&d, 10);
    CHECK(strcmp(row(1), "kept" "xxxxxx") == 0);
}

static void test_keys_mode(void)
{
    fresh();
    type("keys\n");
    CHECK(d.keys_mode == 1 && find_row("Press keys; Esc stops.") >= 0);
    const int top = find_row("Press keys");
    consdemo_char(&d, 'a');
    consdemo_char(&d, PLAT_KEY_UP);
    consdemo_char(&d, 3);
    consdemo_char(&d, PLAT_KEY_F(5));
    consdemo_char(&d, 10);
    consdemo_char(&d, ' ');
    consdemo_char(&d, 127);
    CHECK(strcmp(row(top + 1), "code  97 (0x061) 'a'") == 0);
    CHECK(strcmp(row(top + 2), "code 256 (0x100) Up") == 0);
    CHECK(strcmp(row(top + 3), "code   3 (0x003) Ctrl+C") == 0);
    CHECK(strcmp(row(top + 4), "code 276 (0x114) F5") == 0);
    CHECK(strcmp(row(top + 5), "code  10 (0x00A) Enter") == 0);
    CHECK(strcmp(row(top + 6), "code  32 (0x020) Space") == 0);
    CHECK(strcmp(row(top + 7), "code 127 (0x07F) Delete") == 0);
    CHECK(d.len == 0);                                     /* nothing was typed into a line */
    consdemo_char(&d, 27);
    CHECK(d.keys_mode == 0 && strcmp(row(top + 8), "Done.") == 0 && strcmp(row(top + 9), "C:\\>") == 0);
    type("ok");
    CHECK(strcmp(row(top + 9), "C:\\> ok") == 0);
}

static void test_scrolling(void)
{
    fresh();
    for (int i = 0; i < 40; i++) {
        char c[32];
        snprintf(c, sizeof c, "echo line %d\n", i);
        type(c);
    }
    CHECK(d.prompt_row == 24);                             /* the prompt is on the last row now, and stays there */
    type("xyz");
    CHECK(strcmp(row(24), "C:\\> xyz") == 0 && strcmp(row(23), "line 39") == 0 && screen.cursor_row == 24);
    consdemo_char(&d, 8);
    consdemo_char(&d, PLAT_KEY_LEFT);
    type("Q");
    CHECK(strcmp(row(24), "C:\\> xQy") == 0);   /* xyz, Backspace: xy, Left, insert Q before the y */
    consdemo_char(&d, 21);
    CHECK(strcmp(row(24), "C:\\>") == 0 && strcmp(row(23), "line 39") == 0);
    type("help\n");                                        /* a command that prints more than is left: the screen scrolls, the prompt is still the last row */
    CHECK(d.prompt_row == 24 && strcmp(row(24), "C:\\>") == 0 && find_row("Editing:") >= 0);
}

static void test_random(void)
{
    fresh();
    for (int i = 0; i < 300000; i++) {
        int ch;
        switch (rnd() % 6) {
        case 0: ch = (int)(rnd() % 300) - 10; break;
        case 1: ch = PLAT_KEY_UP + (int)(rnd() % 9); break;
        case 2: ch = (int)(rnd() % 32); break;
        default: ch = ' ' + (int)(rnd() % 95); break;
        }
        if (i % 97 == 0) {                                 /* now and then, something that makes output */
            static const char *const commands[] = {"help\n", "test\n", "chars\n", "echo hi\n", "keys\n", "color 1 2\n", "cls\n", "uptime\n", "\x1B"};
            type(commands[rnd() % (sizeof commands / sizeof *commands)]);
        }
        consdemo_char(&d, ch);
        CHECK(d.len >= 0 && d.len <= CONSDEMO_LINE_MAX && d.pos >= 0 && d.pos <= d.len);
        CHECK((int)strlen(d.line) == d.len);
        CHECK(d.history_count >= 0 && d.history_count <= CONSDEMO_HISTORY && d.browsing >= -1 && d.browsing < d.history_count);
        CHECK(d.prompt_row >= 0 && d.prompt_row < 25 && screen.cursor_row >= 0 && screen.cursor_row < 25);
        CHECK(screen.cursor_col >= 0 && screen.cursor_col < 80);
    }
}

int main(void)
{
    test_start();
    test_editing();
    test_commands();
    test_history();
    test_control_keys();
    test_keys_mode();
    test_scrolling();
    test_random();
    printf("all %d checks passed\n", checks);
    return 0;
}
