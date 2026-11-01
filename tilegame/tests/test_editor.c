/* SPDX-License-Identifier: MIT */
/* Tests of the editor on the console: editing sessions are typed in through the key adapter, and the file the editor saves and the screen are checked.
 * Exits non-zero on the first failure. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "consoleio.h"
#include "editor.h"
#include "platform.h"

const char *__asan_default_options(void) { return "detect_leaks=0"; }   /* a session cut short leaves the editor's buffers allocated */

static int checks;
#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static int script[8192], script_len, script_pos;
static jmp_buf cut;
static int cut_armed;

static int idle_frames;   /* frames waited since the last key was read */

static int get_key(void)
{
    if (script_pos >= script_len)
        return -1;
    idle_frames = 0;
    return script[script_pos++];
}

/* The editor waits a frame at a time for a key, and also for its own delays (its notes stay up for two seconds: 120 frames). If the keys have run out
 * and a long time has gone by with none read, the editor is waiting for a key that will not come, and the session is cut short. */
static void frame(void)
{
    if (++idle_frames > 1000 && script_pos >= script_len && cut_armed)
        longjmp(cut, 1);
}

/* Keys: a character is itself; \n is the Enter key; ^A..^Z are the control keys; {NAME} is a key that is not a character; {NONE} is a moment
 * with no key (so that a lone Escape is seen as one). */
static void parse_keys(const char *spec)
{
    static const struct { const char *name; int key; } names[] = {
        {"UP", PLAT_KEY_UP}, {"DOWN", PLAT_KEY_DOWN}, {"LEFT", PLAT_KEY_LEFT}, {"RIGHT", PLAT_KEY_RIGHT}, {"HOME", PLAT_KEY_HOME}, {"END", PLAT_KEY_END},
        {"PGUP", PLAT_KEY_PAGEUP}, {"PGDN", PLAT_KEY_PAGEDOWN}, {"INS", PLAT_KEY_INSERT}, {"DEL", 127}, {"BS", 8}, {"ESC", 27}, {"TAB", 9}, {"F1", PLAT_KEY_F(1)},
        {"F2", PLAT_KEY_F(2)}, {"F3", PLAT_KEY_F(3)}, {"NONE", -1}, {"ENTER", 10}, {"^", '^'}, {"{", '{'},
    };
    script_len = script_pos = 0;
    while (*spec) {
        int key;
        if (*spec == '{') {
            const char *close = strchr(spec, '}');
            const size_t n = (size_t)(close - spec - 1);
            key = -2;
            for (size_t i = 0; i < sizeof names / sizeof *names; i++)
                if (strlen(names[i].name) == n && strncmp(spec + 1, names[i].name, n) == 0)
                    key = names[i].key;
            CHECK(key != -2);
            spec = close + 1;
        } else if (*spec == '^' && spec[1] >= 'A' && spec[1] <= 'Z') {
            key = spec[1] - '@';
            spec += 2;
        } else if (*spec == '\\' && spec[1] == '\\') {   /* ^\ is written \\ */
            key = 28;
            spec += 2;
        } else {
            key = (unsigned char)*spec++;
        }
        script[script_len++] = key;
    }
}

/* The file the editor reads and the file it saves. */
static const char *in_text;
static size_t in_pos;
static char out_text[65536];
static size_t out_len;
static int writes, closes;

static int reader(void *v)
{
    (void)v;
    return in_text && in_text[in_pos] ? (unsigned char)in_text[in_pos++] : 0;
}

static int writer(int c, void *v)
{
    (void)v;
    if (c == -1)
        closes++;
    else if (c == 0)
        writes++;
    else
        out_text[out_len++] = (char)c;
    return 0;
}

/* Run a session; returns 1 if the editor finished by itself, 0 if the keys ran out first. */
static int session(const char *file, const char *keys)
{
    const console_hooks_t h = {get_key, frame};
    console_set_hooks(&h);
    console_init();
    parse_keys(keys);
    in_text = file;
    in_pos = 0;
    out_len = 0;
    out_text[0] = 0;
    writes = closes = 0;
    rf = file ? reader : NULL;
    rf_ptr = NULL;
    wf = writer;
    wf_ptr = NULL;
    cut_armed = 1;
    idle_frames = 0;
    if (setjmp(cut)) {
        cut_armed = 0;
        return 0;
    }
    editor();
    cut_armed = 0;
    return 1;
}

static int saved(const char *want) { out_text[out_len] = 0; return strcmp(out_text, want) == 0; }

static const char *row(int r)
{
    static char text[2][81];
    static int which;
    char *out = text[which ^= 1];
    int n = 0;
    for (int c = 0; c < 80; c++)
        out[n++] = (char)(console_screen()->cells[r * 80 + c] & 255);
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = 0;
    return out;
}

static const char *longer_than_a_window(void)
{
    static char line[200];
    for (int i = 0; i < 150; i++)
        line[i] = (char)('a' + i % 26);
    line[150] = '\n';
    line[151] = 0;
    return line;
}

static void test_typing(void)
{
    CHECK(session(NULL, "hello\nworld^WY"));                       /* a new file: type, save, and the editor is done */
    CHECK(saved("hello\nworld\n") && writes == 1 && closes == 1);
    CHECK(console_console()->row == 23);                           /* it leaves the cursor below the window */
    /* Nothing changed: ^W leaves without asking, and (as the editor of PicoParanoia does) saves what is there. */
    CHECK(session("a\nbb\n", "^W"));
    CHECK(writes == 1 && saved("a\nbb\n"));
    /* "Save (y/n/cncl)": n leaves without saving, and c stays in the editor. */
    CHECK(session("a\n", "x^WN"));
    CHECK(writes == 0);
    CHECK(session("a\n", "x^WCy^WY"));
    CHECK(saved("xya\n"));                                         /* c: back to editing, and the y goes in after the x */
    CHECK(session("a\n", "x^Wn"));                                 /* the answer is not case sensitive */
    CHECK(writes == 0);
    CHECK(session("a\n", "x^Wy"));
    CHECK(saved("xa\n"));
}

static void test_cursor_keys(void)
{
    CHECK(session("abc\ndef\n", "{DOWN}{RIGHT}X^WY"));
    CHECK(saved("abc\ndXef\n"));
    CHECK(session("abc\ndef\n", "{DOWN}{DOWN}{DOWN}{UP}{LEFT}{RIGHT}{RIGHT}X^WY"));    /* down past the end stops at the last line */
    CHECK(saved("abc\ndeXf\n"));                                  /* (the empty line at the end is not part of the file) */
    CHECK(session("hello\n", "{END}!{HOME}{DEL}{END}{BS}^WY"));    /* End, Home, Delete (under the cursor) and Backspace */
    CHECK(saved("ello\n"));
    CHECK(session("hello\n", "{BS}{LEFT}{UP}X^WY"));                /* Backspace, Left and Up at the very start do nothing */
    CHECK(saved("Xhello\n"));
    CHECK(session("abc\n", "{END}{RIGHT}{RIGHT}X^WY"));            /* Right goes past the end of the line, and what is typed there is padded with blanks */
    CHECK(saved("abc  X\n"));
    CHECK(session("abc\n", "{INS}XY{INS}Z^WY"));                   /* Insert: overwrite, and back to inserting */
    CHECK(saved("XYZc\n"));
}

static void test_pages(void)
{
    char file[1024], expect[1024];
    file[0] = expect[0] = 0;
    for (int i = 0; i < 60; i++) {
        char line[16];
        snprintf(line, sizeof line, "L%d\n", i);
        strcat(file, line);
        snprintf(line, sizeof line, "%sL%d\n", i == 21 ? "X" : "", i);
        strcat(expect, line);
    }
    CHECK(session(file, "{PGDN}{PGDN}{PGUP}X^WY"));               /* a page is 21 lines: 21 + 21 - 21 */
    CHECK(saved(expect));
    expect[0] = 0;
    for (int i = 0; i < 60; i++) {
        char line[16];
        snprintf(line, sizeof line, "%sL%d\n", i == 42 ? "X" : "", i);
        strcat(expect, line);
    }
    CHECK(session(file, "{PGDN}{PGDN}X^WY"));
    CHECK(saved(expect));
    expect[0] = 0;
    for (int i = 0; i < 60; i++) {
        char line[16];
        snprintf(line, sizeof line, "%sL%d\n", i == 0 ? "X" : "", i);
        strcat(expect, line);
    }
    CHECK(session(file, "{PGDN}{PGUP}{PGUP}X^WY"));               /* page up stops at the top */
    CHECK(saved(expect));
    /* The same cursor movements with the editor's own keys: ^V and ^P ^V. */
    CHECK(session(file, "^V^V^P^VX^WY"));
    expect[0] = 0;
    for (int i = 0; i < 60; i++) {
        char line[16];
        snprintf(line, sizeof line, "%sL%d\n", i == 21 ? "X" : "", i);
        strcat(expect, line);
    }
    CHECK(saved(expect));
}

static void test_search_and_replace(void)
{
    CHECK(session("one two three\n", "^Ftwo\nX^WY"));              /* forward search puts the cursor at the match */
    CHECK(saved("one Xtwo three\n"));
    CHECK(session("one two three\n", "{END}^P^Ftwo\nX^WY"));       /* ^P ^F is the search backwards */
    CHECK(saved("one Xtwo three\n"));
    CHECK(session("one two two\n", "^Ftwo\n^R2\n^WY"));            /* replace the one at the cursor */
    CHECK(saved("one 2 two\n"));
    CHECK(session("a b a b a\n", "^Fa\n^P^RX\n^WY"));              /* replace all from the cursor on */
    CHECK(saved("a b X b X\n"));
    CHECK(session("abc\n", "^F{ESC}{NONE}X^WY"));                  /* a lone Escape cancels the prompt, and does not eat the keys after it */
    CHECK(saved("Xabc\n"));
    CHECK(session("abc\n", "^Fzzz\nX^WY"));                        /* not found: the cursor stays */
    CHECK(saved("Xabc\n"));
    CHECK(session("l0\nl1\nl2\nl3\nl4\n", "^G4\nX^WY"));           /* go to line 4 */
    CHECK(saved("l0\nl1\nl2\nXl3\nl4\n"));
    CHECK(session("abcdefgh\n", "^P^G5\nX^WY"));                   /* go to a column (^P ^G): the editor puts the cursor one before the number asked for */
    CHECK(saved("abcXdefgh\n"));
}

static void test_blocks_and_deletes(void)
{
    CHECK(session("abcdef\n", "^A{RIGHT}{RIGHT}{RIGHT}^K{END}^U^WY"));      /* mark, copy, paste */
    CHECK(saved("abcdefabc\n"));
    CHECK(session("l1\nl2\nl3\n", "{DOWN}^Y^WY"));                          /* delete a line */
    CHECK(saved("l1\nl3\n"));
    CHECK(session("l1\nl2\nl3\n", "{DOWN}^Y^U^WY"));                        /* ... and paste it back */
    CHECK(saved("l1\nl2\nl3\n"));
    CHECK(session("hello world\n", "^T^WY"));                               /* delete a word */
    CHECK(saved("world\n"));
    CHECK(session("hello world\n", "{RIGHT}{RIGHT}^P^T^WY"));               /* delete to the end of the line */
    CHECK(saved("he\n"));
    CHECK(session("abc\ndef\n", "{END}{DEL}^WY"));                          /* Delete at the end of a line joins the next one on */
    CHECK(saved("abcdef\n"));
    CHECK(session("abc\ndef\n", "{DOWN}{BS}^WY"));                          /* Backspace at the start of a line joins it to the one above */
    CHECK(saved("abcdef\n"));
    CHECK(session("ab\n", "{END}\ncd^WY"));                                 /* Enter splits or adds a line */
    CHECK(saved("ab\ncd\n"));
    CHECK(session("abcd\n", "{RIGHT}{RIGHT}\n^WY"));
    CHECK(saved("ab\ncd\n"));
}

static void test_tabs_and_screen(void)
{
    CHECK(session(NULL, "a{TAB}b^WY"));                                     /* the Tab key moves to the next multiple of 8, and what is typed there is padded */
    CHECK(saved("a       b\n"));
    CHECK(session(NULL, "{F3}x^WY"));                                       /* a key that means nothing is ignored */
    CHECK(saved("x\n"));
    /* The screen while the editor waits: the title row, the text from the second row on. */
    CHECK(session("first line\nsecond line\nthird\n", "") == 0);          /* no keys: the editor is waiting */
    CHECK(strncmp(row(0), "Editor", 6) == 0);
    CHECK(strcmp(row(1), "first line") == 0 && strcmp(row(2), "second line") == 0 && strcmp(row(3), "third") == 0 && strcmp(row(4), "") == 0);
    CHECK(console_screen()->cursor_row == 1 && console_screen()->cursor_col == 0);              /* the cursor is at the start of the text */
    CHECK(session("first line\nsecond line\nthird\n", "{DOWN}{RIGHT}{RIGHT}") == 0);
    CHECK(console_screen()->cursor_row == 2 && console_screen()->cursor_col == 2);
    CHECK(session("abc\n", "{INS}") == 0);                                 /* the flags on the title row show the insert/overwrite state */
    const char *with = row(0);
    CHECK(session("abc\n", "") == 0);
    CHECK(strcmp(with, row(0)) != 0);
    /* A line longer than the window: it scrolls sideways, the text stays on the screen, and what is saved is the whole line. */
    char longline[200];
    for (int i = 0; i < 150; i++)
        longline[i] = (char)('a' + i % 26);
    longline[150] = '\n';
    longline[151] = 0;
    CHECK(session(longline, "{END}") == 0);
    CHECK(strlen(row(1)) < 80 && strlen(row(1)) > 60 && console_screen()->cursor_col < 79);
    CHECK(session(longline, "{END}!^WY"));
    char expect[200];
    snprintf(expect, sizeof expect, "%.150s!\n", longline);
    CHECK(saved(expect));
}

static void test_second_run(void)
{
    /* The editor can be run again and again, as PicoParanoia's menu does: what one run did leaves nothing behind in the next. */
    CHECK(session("abcdef\n", "^A{RIGHT}{RIGHT}{RIGHT}^K^WY"));            /* a block of 3 is copied in this run */
    CHECK(session("xyz\n", "^U^WY"));                                      /* ... and in the next, with nothing copied, a paste puts in nothing */
    CHECK(saved("xyz\n"));
    CHECK(session("abc\n", "{INS}^WY"));                                   /* overwrite mode, left on in this run */
    CHECK(session("abc\n", "X^WY"));                                       /* ... is not on in the next */
    CHECK(saved("Xabc\n"));
    CHECK(session("abc\n", "^P^B30\n^WY"));                               /* a right margin set in this run ... */
    CHECK(session("abc\n", "^P^B0\n{END}!^WY"));                          /* (0 is taken as the least that works: 20 columns, and the editor goes on) */
    CHECK(saved("abc!\n"));
    CHECK(session("abc\n", "^P^B5000\n{END}!^WY"));                       /* (and the most is the width of the window) */
    CHECK(saved("abc!\n"));
    CHECK(session("abc\n", "^P^Bxyz\n{END}!^WY"));                        /* (what is not a number is 0) */
    CHECK(saved("abc!\n"));
    CHECK(session("abc\n", "{END}!^WY"));
    CHECK(saved("abc!\n"));
    CHECK(session(longer_than_a_window(), "{END}") == 0);                  /* ... is the 78 columns again in the next */
    CHECK(console_screen()->cursor_col > 40);
}

static void test_macros(void)
{
    /* F2 (^P ^J) starts recording, ^Z ends it, ^C plays it back (what is typed while recording is only recorded). */
    CHECK(session(NULL, "{F2}ab^Z^C^C^WY"));
    CHECK(saved("abab\n"));
    CHECK(session("\n", "{F2}ab^Z^C^WY"));
    CHECK(saved("ab\n"));
    CHECK(session(NULL, "{F2}ab^Z^WY"));                                    /* recorded and not played: nothing in the file */
    CHECK(saved("\n"));
    /* A macro with ^C in it, played, does what comes before the ^C once and does not play itself for ever (PicoParanoia's does, to a stack overflow). */
    CHECK(session(NULL, "{F2}x^C^Z^C^WY"));
    CHECK(saved("x\n"));
    CHECK(session(NULL, "{F2}x^C^Z^C^Cy^WY"));
    CHECK(saved("xxy\n"));                                                 /* and it can be played again */
    /* A recording longer than the macro buffer (59 keys) keeps the first 59, and the editor does not write past it. */
    char spec[400] = "{F2}";
    for (int i = 0; i < 100; i++)
        strcat(spec, i % 2 ? "b" : "a");
    strcat(spec, "^Z^C^WY");
    CHECK(session(NULL, spec));
    char expect[100];
    for (int i = 0; i < 59; i++)
        expect[i] = i % 2 ? 'b' : 'a';
    expect[59] = '\n';
    expect[60] = 0;
    CHECK(saved(expect));
    /* The editor's own key for it, ^P ^J, is Enter to the keyboard: it inserts the next key as it is (^P Enter is the meta-m). */
    CHECK(session(NULL, "^P\nab^WY"));
    CHECK(saved("ab\n"));
}

static void test_help(void)
{
    CHECK(session(NULL, "{F1}") == 0);                                      /* F1: the help page is showing and the editor waits for a key */
    CHECK(strcmp(row(1), "Help") == 0 && strcmp(row(3), "A set mark, append file") == 0);
    CHECK(session(NULL, "{F1}a") == 0);                                     /* a key: the second page */
    CHECK(strcmp(row(1), "Help") == 0 && strcmp(row(3), "R replace, replace all") == 0);
    CHECK(session(NULL, "{F1}ab^W") == 1);                                  /* another key: back to the text, and ^W leaves */
    CHECK(session(NULL, "\\\\ab^W") == 1);                                  /* ^\\ is the same key as F1 */
}

int main(void)
{
    test_typing();
    test_cursor_keys();
    test_pages();
    test_search_and_replace();
    test_blocks_and_deletes();
    test_tabs_and_screen();
    test_second_run();
    test_macros();
    test_help();
    printf("all %d checks passed\n", checks);
    return 0;
}
