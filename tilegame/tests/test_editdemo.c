/* SPDX-License-Identifier: MIT */
/* Tests of the editor demo: the menu and the editor are driven with scripted keys, and the file in memory and the screen are checked. Exits non-zero on the first failure. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "consoleio.h"
#include "editdemo_logic.h"
#include "platform.h"

const char *__asan_default_options(void) { return "detect_leaks=0"; }

static int checks;
#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static int script[4096], script_len, script_pos, idle_frames;
static jmp_buf cut;
static int cut_armed;

static int get_key(void)
{
    if (script_pos >= script_len)
        return -1;
    idle_frames = 0;
    return script[script_pos++];
}

static void frame(void)   /* waiting for a key with none left: the run is over (a long time with no key, longer than the editor's own delays) */
{
    if (++idle_frames > 1000 && script_pos >= script_len && cut_armed)
        longjmp(cut, 1);
}

/* Keys: a character is itself, \n is Enter, ^A..^Z are control keys, {NAME} is a key that is not a character. */
static void parse_keys(const char *spec)
{
    static const struct { const char *name; int key; } names[] = {
        {"UP", PLAT_KEY_UP}, {"DOWN", PLAT_KEY_DOWN}, {"LEFT", PLAT_KEY_LEFT}, {"RIGHT", PLAT_KEY_RIGHT}, {"HOME", PLAT_KEY_HOME}, {"END", PLAT_KEY_END},
        {"ESC", 27}, {"F1", PLAT_KEY_F(1)}, {"F2", PLAT_KEY_F(2)}, {"F3", PLAT_KEY_F(3)}, {"NONE", -1},
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
        } else {
            key = (unsigned char)*spec++;
        }
        script[script_len++] = key;
    }
}

/* Run the demo's menu on a fresh screen with these keys: 1 if it came back by itself (Q), 0 if the keys ran out. */
static int run(const char *keys)
{
    const console_hooks_t h = {get_key, frame};
    console_set_hooks(&h);
    console_init();
    parse_keys(keys);
    idle_frames = 0;
    cut_armed = 1;
    if (setjmp(cut)) {
        cut_armed = 0;
        return 0;
    }
    editdemo_menu();
    cut_armed = 0;
    return 1;
}

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

static int file_is(const char *want)
{
    return editdemo_length() == strlen(want) && memcmp(editdemo_file(), want, strlen(want)) == 0;
}

static void test_menu(void)
{
    editdemo_reset();
    CHECK(run("") == 0);                                       /* the menu waits for a choice */
    CHECK(strncmp(row(0), "EDITDEMO  the editor of PicoParanoia", 35) == 0);
    CHECK(strcmp(row(2), "  E  edit the file") == 0 && strcmp(row(3), "  V  view the file") == 0 && strcmp(row(4), "  N  new, empty file") == 0);
    CHECK(strcmp(row(5), "  I  information") == 0 && strcmp(row(6), "  Q  quit") == 0 && strcmp(row(8), "Choice:") == 0);
    CHECK((console_screen()->cells[0] >> 8) == 0x0F && (console_screen()->cells[20] >> 8) == 0x07);   /* the name is in bold */
    CHECK(run("xyz?{UP}{DOWN}{F3}q") == 1);                    /* other keys are ignored: it waits for one of E V N I Q */
    CHECK(run("Q") == 1);
    CHECK(run("q") == 1);                                      /* upper or lower case */
    CHECK(run("xQ") == 1 && strcmp(row(8), "Choice: Q") == 0);
}

static void test_view_and_information(void)
{
    editdemo_reset();
    CHECK(run("V") == 0);                                      /* waiting at the end of the text: Press SPACE */
    CHECK(strcmp(row(0), "This is the editor of PicoParanoia, running on the 80 x 25 text mode.") == 0);
    CHECK(strcmp(row(2), "Keys:") == 0 && strncmp(row(3), "  arrows, Home, End", 19) == 0);
    int last = 0;
    for (int r = 0; r < 25; r++)
        if (*row(r))
            last = r;
    CHECK(strcmp(row(last), "Press SPACE to continue") == 0);
    CHECK(run("V ") == 0);                                     /* a space: back to the menu */
    CHECK(strcmp(row(0), "EDITDEMO  the editor of PicoParanoia on an 80 x 25 text screen") == 0);
    CHECK(run("V Q") == 1);
    CHECK(run("V{UP}x Q") == 1);                               /* only a space goes on */
    editdemo_set("", 0);
    CHECK(run("V") == 0);                                      /* an empty file: nothing but the prompt */
    CHECK(strcmp(row(0), "") == 0 && strcmp(row(2), "Press SPACE to continue") == 0);

    CHECK(run("NI") == 0);
    CHECK(strcmp(row(0), "The file in memory") == 0 && strcmp(row(2), "  bytes: 0") == 0 && strcmp(row(3), "  lines: 0") == 0);
    CHECK(strcmp(row(4), "  saved: 0 times") == 0 && strcmp(row(5), "  room:  24576 bytes more") == 0);
    editdemo_set("ab\ncd\nef", 8);
    CHECK(run("I") == 0 && strcmp(row(2), "  bytes: 8") == 0 && strcmp(row(3), "  lines: 2") == 0 && strcmp(row(5), "  room:  24568 bytes more") == 0);
    CHECK(run("I Q") == 1);

    /* More than a screenful: it pauses after 22 lines. */
    char big[2048];
    big[0] = 0;
    for (int i = 0; i < 60; i++) {
        char line[16];
        snprintf(line, sizeof line, "line %d\n", i);
        strcat(big, line);
    }
    editdemo_set(big, strlen(big));
    CHECK(run("V") == 0);
    CHECK(strcmp(row(0), "line 0") == 0 && strcmp(row(21), "line 21") == 0 && strcmp(row(22), "-- space for more --") == 0);
    CHECK(run("V ") == 0);
    CHECK(strcmp(row(0), "line 22") == 0 && strcmp(row(21), "line 43") == 0 && strcmp(row(22), "-- space for more --") == 0);
    CHECK(run("V  ") == 0);
    CHECK(strcmp(row(0), "line 44") == 0 && strcmp(row(15), "line 59") == 0 && strcmp(row(18), "Press SPACE to continue") == 0);   /* (the last line, a newline, a blank line, the prompt) */
    CHECK(run("V   Q") == 1);
    /* Characters that cannot be shown are left out. */
    editdemo_set("a\001b\tc\xC9z\n", 8);
    CHECK(run("V") == 0 && strcmp(row(0), "abcz") == 0);
}

static void test_editing(void)
{
    /* Edit and leave without a change: it saves what is there (as PicoParanoia's editor does) and the file is the same. */
    editdemo_reset();
    const size_t before = editdemo_length();
    CHECK(run("E^WQ") == 1);
    CHECK(editdemo_saves() == 1 && editdemo_length() == before && strncmp(editdemo_file(), "This is the editor", 18) == 0);
    /* A change in the middle. */
    editdemo_reset();
    CHECK(run("E{DOWN}{DOWN}X^WYQ") == 1);
    CHECK(editdemo_length() == before + 1 && strncmp(editdemo_file(), "This is the editor of PicoParanoia, running on the 80 x 25 text mode.\n\nXKeys:\n", 77) == 0);
    CHECK(editdemo_saves() == 1);
    /* A new file typed from nothing. */
    CHECK(run("NEhello\nworld^WYQ") == 1);
    CHECK(file_is("hello\nworld\n") && editdemo_saves() == 2);                      /* (the counter goes on from the save above) */
    /* ... edited again, and the first line is the new text. */
    CHECK(run("E{END}!^WYQ") == 1);
    CHECK(file_is("hello!\nworld\n") && editdemo_saves() == 3);
    /* Not saving: N answers the question and the file stays as it was. */
    CHECK(run("EQQ^WNQ") == 1);
    CHECK(file_is("hello!\nworld\n") && editdemo_saves() == 3);
    /* View what was saved. */
    CHECK(run("V") == 0 && strcmp(row(0), "hello!") == 0 && strcmp(row(1), "world") == 0 && strcmp(row(4), "Press SPACE to continue") == 0);
    /* A new file after a file: empty, and an empty file edited is one empty line. */
    CHECK(run("NIQ") == 0);
    CHECK(run("NE^WQ") == 1 && file_is("\n"));
    /* Searching, replacing and macros work through the menu as in the editor. */
    editdemo_set("one two three\n", 14);
    CHECK(run("E^Ftwo\nX^WYQ") == 1 && file_is("one Xtwo three\n"));
    CHECK(run("E{F2}ab^Z^C^C^WYQ") == 1 && file_is("abab" "one Xtwo three\n"));
    /* The editor can be left from the help screens and the like and the menu goes on. */
    CHECK(run("E{F1}ab^WQ") == 1);
    CHECK(run("E{F1}ab") == 0);
    /* A file with a line longer than the window (but not longer than the editor's 255 columns). */
    char longfile[400];
    memset(longfile, 'x', 200);
    longfile[200] = '\n';
    editdemo_set(longfile, 201);
    CHECK(run("E{END}!^WYQ") == 1 && editdemo_length() == 202 && editdemo_file()[200] == '!' && editdemo_file()[201] == '\n');
}

static void test_capacity(void)
{
    static char huge[EDITDEMO_FILE_MAX + 1000];
    memset(huge, 'a', sizeof huge);
    editdemo_set(huge, sizeof huge);
    CHECK(editdemo_length() == EDITDEMO_FILE_MAX);                          /* cut to the room there is */
    editdemo_set("small\n", 6);
    CHECK(file_is("small\n"));
    /* A file of lines of 79 characters filling the room: the editor reads what fits and a save writes at most the room. */
    static char lines[EDITDEMO_FILE_MAX];
    size_t n = 0;
    while (n + 80 <= sizeof lines) {
        memset(lines + n, 'z', 79);
        lines[n + 79] = '\n';
        n += 80;
    }
    editdemo_set(lines, n);
    CHECK(run("E^WQ") == 1);
    CHECK(editdemo_length() <= EDITDEMO_FILE_MAX && editdemo_length() >= n - 200);
    /* ... and typing more than the room left (16 bytes) into it: the editor has room for it, and the save is cut at the room the file has. */
    editdemo_set(lines, n);
    CHECK(run("E{END}ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ^WYQ") == 1);
    CHECK(editdemo_length() == EDITDEMO_FILE_MAX);
}

int main(void)
{
    test_menu();
    test_view_and_information();
    test_editing();
    test_capacity();
    printf("all %d checks passed\n", checks);
    return 0;
}
