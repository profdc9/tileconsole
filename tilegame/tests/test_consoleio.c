/* SPDX-License-Identifier: MIT */
/* Tests of the consoleio shim: the keys as bytes, waiting for keys and frames, the output functions, and the helpers. Exits non-zero on the first failure. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "consoleio.h"
#include "platform.h"

static int checks;
#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

/* Keys come from a script (a -1 in it is "no key this time") and frames are counted. */
static int script[4096];
static int script_len, script_pos, frames;

static int get_key(void) { return script_pos < script_len ? script[script_pos++] : -1; }
static void frame(void) { frames++; if (script_pos >= script_len && frames > 100000) { printf("FAIL: waiting forever\n"); exit(1); } }

static void keys(const int *k, int n)
{
    memcpy(script, k, (size_t)n * sizeof *k);
    script_len = n;
    script_pos = 0;
}

static void typed(const char *text)   /* a string of characters; \n is the Enter key */
{
    script_len = 0;
    script_pos = 0;
    for (; *text; text++)
        script[script_len++] = (unsigned char)*text;
}

static void fresh(void)
{
    const console_hooks_t h = {get_key, frame};
    console_set_hooks(&h);
    console_init();
    script_len = script_pos = frames = 0;
}

static const tg_text_t *scr(void) { return console_screen(); }
static uint16_t cell(int col, int row) { return scr()->cells[row * 80 + col]; }

static const char *row(int r)
{
    static char text[2][81];
    static int which;
    char *out = text[which ^= 1];
    int n = 0;
    for (int c = 0; c < 80; c++)
        out[n++] = (char)(cell(c, r) & 255);
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = 0;
    return out;
}

static void test_expand(void)
{
    char out[4];
    static const struct { int key; const char *bytes; } table[] = {
        {PLAT_KEY_UP, "\033[A"}, {PLAT_KEY_DOWN, "\033[B"}, {PLAT_KEY_RIGHT, "\033[C"}, {PLAT_KEY_LEFT, "\033[D"},
        {PLAT_KEY_HOME, "\033Op"}, {PLAT_KEY_END, "\033Or"}, {127, "\033Ol"}, {PLAT_KEY_PAGEDOWN, "\026"}, {PLAT_KEY_PAGEUP, "\020\026"},
        {PLAT_KEY_INSERT, "\016"}, {PLAT_KEY_F(1), "\034"}, {PLAT_KEY_F(2), "\020\n"}, {10, "\r"}, {8, "\010"}, {9, "\t"}, {27, "\033"}, {'a', "a"}, {'~', "~"}, {0, "\0"},
    };
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) {
        memset(out, 'X', sizeof out);
        const int n = console_expand_key(table[i].key, out);
        const int want = table[i].key == 0 ? 1 : (int)strlen(table[i].bytes);
        CHECK(n == want && memcmp(out, table[i].bytes, (size_t)want) == 0);
    }
    for (int k = 0; k < 256; k++) {   /* every other character is itself */
        if (k == 10 || k == 127)
            continue;
        CHECK(console_expand_key(k, out) == 1 && (unsigned char)out[0] == k);
    }
    for (int k = PLAT_KEY_F(3); k <= PLAT_KEY_F(12); k++)   /* the keys that mean nothing here */
        CHECK(console_expand_key(k, out) == 0);
    CHECK(console_expand_key(PLAT_KEY_INSERT + 1, out) == 0 && console_expand_key(0x1000, out) == 0 && console_expand_key(-1, out) == 0);
    CHECK(console_expand_key(256, out) == 3 && console_expand_key(0x10F, out) == 0 && console_expand_key(0x120, out) == 0);
    /* The editor reads an arrow as ESC, anything, then A, B, C or D: check that the bytes are what its table wants. */
    CHECK(console_expand_key(PLAT_KEY_UP, out) == 3 && out[0] == 27 && out[2] == 'A');
    CHECK(console_expand_key(PLAT_KEY_HOME, out) == 3 && out[0] == 27 && out[2] == 'p');
    CHECK(console_expand_key(PLAT_KEY_END, out) == 3 && out[0] == 27 && out[2] == 'r');
}

static void test_input(void)
{
    fresh();
    CHECK(console_inchar() == -1 && frames == 0);                        /* nothing: -1, and no frame is waited for */
    CHECK(console_waiting() == 0 && frames == 0);
    static const int a[] = {PLAT_KEY_UP, 'a', PLAT_KEY_PAGEUP, 10, PLAT_KEY_F(5), PLAT_KEY_HOME, 'z'};
    keys(a, 7);
    static const int want[] = {27, '[', 'A', 'a', 16, 22, '\r', 27, 'O', 'p', 'z'};   /* (the F5 is dropped) */
    for (size_t i = 0; i < sizeof want / sizeof *want; i++)
        CHECK(console_inchar() == want[i]);
    CHECK(console_inchar() == -1);

    /* console_waiting: is a byte already there? It does not use it up, and a key that is dropped does not count. */
    fresh();
    static const int b[] = {PLAT_KEY_UP};
    keys(b, 1);
    CHECK(console_waiting() == 1 && console_waiting() == 1);              /* asking twice does not take anything */
    CHECK(console_inchar() == 27 && console_waiting() == 1);              /* the rest of the sequence is waiting */
    CHECK(console_inchar() == '[' && console_inchar() == 'A' && console_waiting() == 0 && console_inchar() == -1);
    static const int c[] = {PLAT_KEY_F(7), PLAT_KEY_F(8)};
    keys(c, 2);
    CHECK(console_waiting() == 0 && script_pos == 2);                     /* both dropped */
    static const int d[] = {PLAT_KEY_F(7), 'q'};
    keys(d, 2);
    CHECK(console_waiting() == 1 && console_inchar() == 'q');
    static const int e[] = {27};                                          /* a lone Escape: one byte, and nothing behind it */
    keys(e, 1);
    CHECK(console_inchar() == 27 && console_waiting() == 0);
    static const int f[] = {27, 'x'};                                     /* an Escape with a key right behind it: the editor takes it as a sequence */
    keys(f, 2);
    CHECK(console_inchar() == 27 && console_waiting() == 1 && console_inchar() == 'x');
    /* A hook that has no function at all. */
    console_hooks_t none = {0, 0};
    console_set_hooks(&none);
    CHECK(console_inchar() == -1 && console_waiting() == 0);
    console_delayframes(3);                                               /* (no frame function: nothing to wait on) */
    fresh();

    /* console_init drops the rest of a key that was only partly read. */
    fresh();
    static const int h0[] = {PLAT_KEY_UP};
    keys(h0, 1);
    CHECK(console_inchar() == 27);
    console_init();
    CHECK(console_inchar() == -1 && console_waiting() == 0);
    fresh();

    /* getch waits, a frame at a time, for a key. */
    static const int g[] = {-1, -1, -1, 'x'};
    keys(g, 4);
    CHECK(console_getch() == 'x' && frames == 3);
    static const int h[] = {PLAT_KEY_LEFT};
    keys(h, 1);
    frames = 0;
    CHECK(console_getch() == 27 && console_getch() == '[' && console_getch() == 'D' && frames == 0);   /* a sequence waits for no frame */
    static const int i2[] = {PLAT_KEY_F(3), -1, PLAT_KEY_F(4), -1, 'k'};
    keys(i2, 5);
    frames = 0;
    CHECK(console_getch() == 'k' && frames == 2);                        /* the ignored keys do not end the wait */
    frames = 0;
    console_delayframes(5);
    CHECK(frames == 5);
    console_delayframes(0);
    CHECK(frames == 5);
    console_delayframes(65535);
    CHECK(frames == 5 + 65535);
}

static void test_output(void)
{
    fresh();
    const tg_console_t *con = console_console();
    CHECK(con->flags == (TG_CONSOLE_ANSI | TG_CONSOLE_LF_ONLY | TG_CONSOLE_NOWRAP) && con->col == 0 && con->row == 0);
    CHECK((scr()->cursor_flags & TG_CURSOR_VISIBLE) && cell(0, 0) == 0x0720);
    /* The PC character set is loaded: the glyph of a capital A is not blank, and a box character is there. */
    int set = 0;
    for (int r = 0; r < 16; r++)
        set += tg_text_font(scr())['A' * 16 + r] != 0;
    CHECK(set > 4 && tg_text_font(scr())[0xDB * 16] == 0xFF);
    console_puts("Hi");
    console_putch('!');
    CHECK(cell(0, 0) == 0x0748 && cell(1, 0) == 0x0769 && cell(2, 0) == 0x0721 && con->col == 3);
    console_printcrlf();
    CHECK(con->col == 0 && con->row == 1);
    console_puts("a\nb");                                                  /* a line feed alone does not return the carriage */
    CHECK(cell(0, 1) == 0x0761 && cell(1, 2) == 0x0762 && con->col == 2 && con->row == 2);
    console_gotoxy(10, 5);                                                 /* column 10, row 5, from 1 */
    CHECK(con->col == 9 && con->row == 4 && scr()->cursor_col == 9 && scr()->cursor_row == 4);
    console_gotoxy(1, 1);
    CHECK(con->col == 0 && con->row == 0);
    console_gotoxy(80, 25);
    CHECK(con->col == 79 && con->row == 24);
    console_gotoxy(200, 99);
    CHECK(con->col == 79 && con->row == 24);
    console_gotoxy(0, 0);
    CHECK(con->col == 0 && con->row == 0);
    console_gotoxy(7, 7);
    console_gotoxy(-5, -3);                                                /* below 1 is 1, and no sequence that a terminal cannot read is sent */
    CHECK(con->col == 0 && con->row == 0);
    console_gotoxy(9, 4);
    console_gotoxy(-1, 6);
    CHECK(con->col == 0 && con->row == 5);
    console_gotoxy(9, 4);
    console_gotoxy(3, -100);
    CHECK(con->col == 2 && con->row == 0);
    console_putch('Q');
    CHECK(cell(2, 0) == 0x0751);
    console_gotoxy(5, 1);
    console_clreol();
    CHECK(cell(0, 0) == 0x0748 && cell(3, 0) == 0x0720 && cell(4, 0) == 0x0720);   /* from column 5 on: "Hi!" is at 1..3, so it is intact */
    CHECK(cell(1, 0) == 0x0769);
    console_gotoxy(2, 1);
    console_clreol();
    CHECK(cell(0, 0) == 0x0748 && cell(1, 0) == 0x0720 && cell(2, 0) == 0x0720);
    console_clrscr();
    CHECK(con->col == 0 && con->row == 0);
    for (int i = 0; i < 2000; i++)
        CHECK(scr()->cells[i] == 0x0720);
    console_highvideo();
    console_putch('B');
    console_lowvideo();
    console_putch('n');
    CHECK(cell(0, 0) == 0x0F42 && cell(1, 0) == 0x076E);
    console_printcrlf();
    console_printint(0);
    console_putch(' ');
    console_printint(-12345);
    console_putch(' ');
    console_printint(INT_MIN);
    console_putch(' ');
    console_printuint(0);
    console_putch(' ');
    console_printuint(UINT_MAX);
    CHECK(strcmp(row(1), "0 -12345 -2147483648 0 4294967295") == 0);
    /* Output is ANSI: a sequence in the text does its work. */
    console_clrscr();
    console_puts("\033[3;4Hx");
    CHECK(cell(3, 2) == 0x0778);
    /* Re-initialising starts afresh. */
    console_puts("\033[1m");
    console_init();
    CHECK(con->attr == 0x07 && cell(3, 2) == 0x0720 && con->col == 0 && con->row == 0);
}

static void test_getstring(void)
{
    fresh();
    char buf[16];
    memset(buf, 'X', sizeof buf);
    typed("abc\b\bxy\r");
    CHECK(console_getstring(buf, 8, 3, 5, 20) == 3 && strcmp(buf, "axy") == 0);
    CHECK(strcmp(row(2), "    axy") == 0);                               /* echoed from column 5 of row 3 */
    CHECK(console_console()->row == 2 && console_console()->col == 7);   /* the cursor was put at the next place (column 8) before it read the Enter */
    /* The Enter key (10 as plat_getchar gives it) ends it, as \r. */
    fresh();
    typed("ok\n");
    CHECK(console_getstring(buf, 8, 1, 1, 20) == 2 && strcmp(buf, "ok") == 0);
    /* A backspace with nothing typed does nothing, nor do control characters. */
    fresh();
    static const int a[] = {8, 8, 1, 2, 'a', 'b', 10};
    keys(a, 7);
    CHECK(console_getstring(buf, 8, 1, 1, 20) == 2 && strcmp(buf, "ab") == 0);
    /* Keys that arrive as escape sequences are not typing. */
    fresh();
    static const int k[] = {PLAT_KEY_UP, 'a', 'b', PLAT_KEY_HOME, PLAT_KEY_DOWN, PLAT_KEY_F(5), PLAT_KEY_LEFT, PLAT_KEY_END, 127, PLAT_KEY_PAGEUP, 'c', 10};
    keys(k, 12);
    CHECK(console_getstring(buf, 8, 1, 1, 20) == 3 && strcmp(buf, "abc") == 0 && strcmp(row(0), "abc") == 0);
    /* A lone Escape is dropped, and the key after it is typed. */
    fresh();
    static const int e[] = {27, 'x', 10};
    keys(e, 3);
    CHECK(console_getstring(buf, 8, 1, 1, 20) == 1 && strcmp(buf, "x") == 0);
    /* The length limit: len characters and no more. */
    fresh();
    typed("abcdef\r");
    CHECK(console_getstring(buf, 3, 1, 1, 20) == 3 && strcmp(buf, "abc") == 0 && strcmp(row(0), "abc") == 0);
    fresh();
    typed("abcdef\r");
    CHECK(console_getstring(buf, 0, 1, 1, 20) == 0 && buf[0] == 0);
    /* The printable range: space to ~, not DEL (the Delete key is a sequence) or the characters over 127. */
    fresh();
    static const int p[] = {' ', '~', 127, 0x80, 0xFF, 'z', 10};
    keys(p, 7);
    CHECK(console_getstring(buf, 8, 1, 1, 20) == 3 && strcmp(buf, " ~z") == 0);
    /* wrap: the characters go on in the rows below, wrap columns wide, and a backspace goes back up. */
    fresh();
    typed("0123456789\b\b\b\bXY\r");
    CHECK(console_getstring(buf, 20, 2, 10, 4) == 8 && strcmp(buf, "012345XY") == 0);
    CHECK(strcmp(row(1), "         0123") == 0 && strcmp(row(2), "         45XY") == 0 && strcmp(row(3), "") == 0);
    /* A backspace over a character shows as a blank there. */
    fresh();
    typed("abc\b\r");
    console_getstring(buf, 8, 1, 1, 20);
    CHECK(cell(2, 0) == 0x0720 && cell(1, 0) == 0x0762);
}

static void test_menu(void)
{
    fresh();
    typed("xz?b");
    CHECK(console_selectmenu("Pick: ", "ABC") == 'B' && strcmp(row(0), "Pick: B") == 0);
    CHECK(frames == 0);
    fresh();
    typed("c");
    CHECK(console_selectmenu("", "AC") == 'C');                            /* what is compared is the upper case of the key */
    fresh();
    typed("C");
    CHECK(console_selectmenu("", "AC") == 'C');
    fresh();
    typed("1");
    CHECK(console_selectmenu("", "123") == '1');
    /* The cursor keys are not the letters A to D that they arrive as. */
    fresh();
    static const int k[] = {PLAT_KEY_UP, PLAT_KEY_DOWN, PLAT_KEY_RIGHT, PLAT_KEY_LEFT, PLAT_KEY_HOME, PLAT_KEY_END, 127, 'c'};
    keys(k, 8);
    CHECK(console_selectmenu("", "ABCDPRL") == 'C');
    /* Waiting for a key waits for frames. */
    fresh();
    static const int w[] = {-1, -1, 'a'};
    keys(w, 3);
    CHECK(console_selectmenu("", "A") == 'A' && frames == 2);

    fresh();
    typed("ab cd");
    console_press_space();
    CHECK(script_pos == 3 && strcmp(row(0), "") == 0 && strcmp(row(2), "Press SPACE to continue") == 0);
    CHECK(console_console()->row == 2 && console_console()->col == 23);
}

static void test_yes_and_array(void)
{
    fresh();
    typed("YES\n");
    CHECK(console_yes(10) == 0 && strcmp(row(9), "Type YES to continue: YES") == 0);
    fresh();
    typed("yes\n");
    CHECK(console_yes(10) != 0);                                           /* it wants capitals */
    fresh();
    typed("Y\n");
    CHECK(console_yes(10) != 0);
    fresh();
    typed("YESX\n");
    CHECK(console_yes(10) == 0);                                           /* three characters at most: the X is not taken */
    fresh();
    typed("XYES\n");
    CHECK(console_yes(10) != 0);
    fresh();
    typed("\n");
    CHECK(console_yes(10) != 0);

    fresh();
    unsigned char a[10] = {0, 1, 2, 3, 4, 5, 6, 7, 200, 255};
    console_print_array("Data", a, 10);
    CHECK(strcmp(row(0), "") == 0 && strcmp(row(1), "Data") == 0 && strcmp(row(2), "0 1 2 3 4 5 6 7") == 0 && strcmp(row(3), "200 255") == 0);
    CHECK(console_console()->row == 4 && console_console()->col == 0);
    fresh();
    console_print_array("None", a, 0);
    CHECK(strcmp(row(1), "None") == 0 && console_console()->row == 2 && console_console()->col == 0);
}

static void test_helpers(void)
{
    char s[MY_ITOASIZE], want[MY_ITOASIZE];
    CHECK(strcmp(myltoa(s, 0), "0") == 0 && strcmp(myltoa(s, 1234567), "1234567") == 0 && strcmp(myltoa(s, -42), "-42") == 0);
    snprintf(want, sizeof want, "%ld", LONG_MAX);
    CHECK(strcmp(myltoa(s, LONG_MAX), want) == 0);
    snprintf(want, sizeof want, "%ld", LONG_MIN);
    CHECK(strcmp(myltoa(s, LONG_MIN), want) == 0);
    CHECK(myltoa(s, 5) == s);
    const char *end;
    CHECK(mystrtol("123", &end) == 123 && *end == 0);
    CHECK(mystrtol("  -45abc", &end) == -45 && strcmp(end, "abc") == 0);
    CHECK(mystrtol("7 8", &end) == 7 && strcmp(end, " 8") == 0);
    CHECK(mystrtol("abc", &end) == 0 && strcmp(end, "abc") == 0);
    CHECK(mystrtol("-", &end) == 0 && *end == 0);
    CHECK(mystrtol("", &end) == 0 && *end == 0);
    CHECK(mystrtol("  ", &end) == 0 && *end == 0);
    CHECK(mystrtol("+5", &end) == 0 && strcmp(end, "+5") == 0);           /* no plus sign */
    CHECK(mystrtol("42", NULL) == 42);
    CHECK(mystrtol("-0", NULL) == 0);
    CHECK(mystrtol("0012", NULL) == 12);
    char d[8];
    memset(d, 'x', sizeof d);
    CHECK(strcpy_n(d, "abcdefghij", 4) == d && strcmp(d, "abcd") == 0 && d[5] == 'x');
    strcpy_n(d, "ab", 4);
    CHECK(strcmp(d, "ab") == 0);
    strcpy_n(d, "zzz", 0);
    CHECK(d[0] == 0);
    /* strcat_n: len is the room in all of dest (what is there counts), the same as PicoParanoia's; with len 0 dest is cut to nothing. */
    strcpy_n(d, "ab", 4);
    CHECK(strcat_n(d, "cdefg", 4) == d && strcmp(d, "abcd") == 0);
    strcpy_n(d, "ab", 4);
    CHECK(strcat_n(d, "c", 4) == d && strcmp(d, "abc") == 0);
    strcpy_n(d, "abcd", 4);
    strcat_n(d, "ef", 2);
    CHECK(strcmp(d, "ab") == 0);
    strcpy_n(d, "ab", 4);
    strcat_n(d, "x", 0);
    CHECK(d[0] == 0);
    d[0] = 0;
    strcat_n(d, "hi", 7);
    CHECK(strcmp(d, "hi") == 0);
    CHECK(strlen_n("") == 0 && strlen_n("hello") == 5);
}

int main(void)
{
    test_expand();
    test_input();
    test_output();
    test_getstring();
    test_menu();
    test_yes_and_array();
    test_helpers();
    printf("all %d checks passed\n", checks);
    return 0;
}
