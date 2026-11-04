/* SPDX-License-Identifier: MIT */
/* Tests of the BASIC environment: it is typed at with scripted keys on a RAM disk and the screen and the files are looked at. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "basic_env.h"
#include "platform.h"
#include "tg_text.h"
#include "tilefs.h"

const char *__asan_default_options(void) { return "detect_leaks=0"; }

static int checks;
#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            show();                                                          \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static tg_text_t screen;
static basic_env_t env;
static fs_ram_t disk;
static uint8_t pool[512 * 1024];

static int script[8192], script_len, script_pos, idle_frames;
static jmp_buf cut;
static int cut_armed;
static uint32_t now_ms;

static int presents;
static const char *late_keys;   /* typed only once a program has ended with its picture still up (typed earlier, they would be typeahead of the program) */
static int late_pos, late_min_presents;   /* ... and not before this many pictures have been shown */

static int get_char(void)
{
    if (script_pos >= script_len) {
        if (late_keys && late_keys[late_pos] && !env.running && env.game_active && presents >= late_min_presents)
            return (unsigned char)late_keys[late_pos++];
        return -1;
    }
    idle_frames = 0;
    return script[script_pos++];
}

static int frame_while_game;   /* the text screen was put up while the game was on screen */

static void frame(void)
{
    now_ms += 17;
    if (env.game_active)
        frame_while_game++;
    if (++idle_frames > 100000 && script_pos >= script_len && cut_armed)   /* (a running program calls this now and then too, and may run a while) */
        longjmp(cut, 1);
}

static uint32_t millis(void) { return now_ms += 1; }

static void show(void)
{
    for (int r = 0; r < 25; r++) {
        char row[81];
        for (int c = 0; c < 80; c++)
            row[c] = (char)(screen.cells[r * 80 + c] & 255);
        row[80] = 0;
        printf("|%s\n", row);
    }
}

static const char *row(int r)
{
    static char text[4][81];
    static int which;
    char *out = text[which = (which + 1) & 3];
    int n = 0;
    for (int c = 0; c < 80; c++)
        out[n++] = (char)(screen.cells[r * 80 + c] & 255);
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = 0;
    return out;
}

static int shows(const char *text)
{
    for (int r = 0; r < 25; r++)
        if (strstr(row(r), text))
            return 1;
    return 0;
}

static int row_of(const char *prefix)
{
    for (int r = 0; r < 25; r++)
        if (!strncmp(row(r), prefix, strlen(prefix)))
            return r;
    return -1;
}

static int last_row_of(const char *prefix)
{
    int found = -1;
    for (int r = 0; r < 25; r++)
        if (!strncmp(row(r), prefix, strlen(prefix)))
            found = r;
    return found;
}

/* Keys: a character is itself, \n is Enter, ^A..^Z Ctrl, {NAME} others. */
static void type(const char *spec)
{
    static const struct { const char *name; int key; } names[] = {
        {"UP", PLAT_KEY_UP}, {"DOWN", PLAT_KEY_DOWN}, {"LEFT", PLAT_KEY_LEFT}, {"RIGHT", PLAT_KEY_RIGHT}, {"HOME", PLAT_KEY_HOME}, {"END", PLAT_KEY_END},
        {"ESC", 27}, {"BS", 8}, {"DEL", 127},
    };
    while (*spec) {
        int key = -1;
        if (*spec == '{') {
            const char *close = strchr(spec, '}');
            const size_t n = (size_t)(close - spec - 1);
            for (size_t i = 0; i < sizeof names / sizeof *names; i++)
                if (strlen(names[i].name) == n && !strncmp(spec + 1, names[i].name, n))
                    key = names[i].key;
            CHECK(key >= 0);
            spec = close + 1;
        } else if (*spec == '^' && spec[1] >= 'A' && spec[1] <= 'Z') {
            key = spec[1] - '@';
            spec += 2;
        } else {
            key = (unsigned char)*spec++;
        }
        CHECK(script_len < 8192);
        script[script_len++] = key;
    }
}

/* what the machine says about itself (STACK and HEAP); `info_on` off gives an environment without it */
static int info_on = 1;
static int test_info(const char *what, char *out, size_t size)
{
    if (!strcmp(what, "stack"))
        snprintf(out, size, "stack: peak 1234 of 4096");
    else if (!strcmp(what, "heap"))
        snprintf(out, size, "heap: 5678 free");
    else if (!strcmp(what, "usblog"))
        snprintf(out, size, "usblog: saved");
    else
        return 0;
    return 1;
}

/* the tile screen: a frame counter that moves by frame_jump at each wait, the last picture that was presented (as palette indices), and the buttons */
static int game_hooks_on = 1;
static uint32_t game_frame;
static int frame_jump = 1;
static tg_t shown_g;
static uint8_t image[TG_HEIGHT][TG_WIDTH];
static unsigned held, went_down;
static int allocs, releases, fast_allocs;
static int press_b_after;   /* B is held from this many frames on (0: never): the example programs end when it is pressed */

static uint32_t t_wait_frame(void)
{
    now_ms += 17;
    if (++idle_frames > 100000 && script_pos >= script_len && cut_armed)
        longjmp(cut, 1);
    game_frame += (uint32_t)frame_jump;
    if (press_b_after && game_frame >= 100 + (uint32_t)press_b_after)
        held |= PLAT_BTN_B;
    return game_frame;
}

static void t_present(const tg_t *g)
{
    presents++;
    shown_g = *g;
    for (int line = 0; line < TG_HEIGHT; line++)
        tg_render_line_indices(&shown_g, line, image[line]);
}

static unsigned t_buttons(int player, int kind)
{
    return player == 0 ? (kind == 0 ? held : kind == 1 ? went_down : 0) : 0;
}

static void *t_alloc(size_t n, int fast)
{
    allocs++;
    fast_allocs += fast;
    return malloc(n);
}

static void t_release(void *p)
{
    releases++;
    free(p);
}

/* A fresh environment (the program, the screen and the files are new). */
static void fresh(void)
{
    fs_reset();
    CHECK(fs_ram_init(&disk, pool, sizeof pool) == 0 && fs_mount("sd", &fs_ram_ops, &disk) == 0);
    fs_chdir("/sd");
    memset(&screen, 0, sizeof screen);
    tg_text_load_cp437(&screen);
    tg_text_init(&screen);
    basic_hooks_t hooks = {get_char, frame, millis, info_on ? test_info : NULL};
    if (game_hooks_on) {
        hooks.wait_frame = t_wait_frame;
        hooks.present = t_present;
        hooks.buttons = t_buttons;
        hooks.alloc = t_alloc;
        hooks.release = t_release;
    }
    basic_env_init(&env, &screen, &hooks);
    game_frame = 100;
    late_keys = NULL;
    late_min_presents = 0;
    press_b_after = 0;
    frame_jump = 1;
    presents = 0;
    held = went_down = 0;
    memset(image, 0xEE, sizeof image);
    script_len = script_pos = 0;
}

/* Runs the environment with what is in the script: the exit code if it left the prompt, -1 if the keys ran out. */
static int go(void)
{
    idle_frames = 0;
    cut_armed = 1;
    if (setjmp(cut)) {
        cut_armed = 0;
        return -1;
    }
    const int r = basic_env_run(&env);
    cut_armed = 0;
    return r;
}

static int put_file(const char *path, const char *text)
{
    const int fd = fs_open(path, FS_WRONLY | FS_CREAT | FS_TRUNC);
    if (fd < 0)
        return fd;
    const int w = fs_write(fd, text, strlen(text));
    fs_close(fd);
    return w;
}

static int file_is(const char *path, const char *text)
{
    char buf[4096];
    const int fd = fs_open(path, FS_RDONLY);
    if (fd < 0)
        return 0;
    const int n = fs_read(fd, buf, sizeof buf);
    fs_close(fd);
    return n == (int)strlen(text) && !memcmp(buf, text, (size_t)n);
}

static void test_basics(void)
{
    fresh();
    type("PRINT 2 + 2\n");
    CHECK(go() == -1);
    CHECK(strncmp(row(0), "TILEGAME BASIC  MY-BASIC ", 24) == 0);
    CHECK(strcmp(row(1), "Type HELP for the commands, or BASIC at once.") == 0);
    CHECK(strcmp(row(3), "] PRINT 2 + 2") == 0 && strcmp(row(4), "4") == 0 && strcmp(row(5), "]") == 0);
    /* each line stands alone ("A = 5 : PRINT A * A" gives its statements one line); strings; a variable of an earlier line is gone */
    type("A = 5 : PRINT A * A\nPRINT \"ab\" + \"cd\"\nB$ = \"x\" : PRINT B$ + B$\nPRINT 7 / 2, \" \", 7 MOD 2\n");
    CHECK(go() == -1);
    CHECK(shows("25") && shows("abcd") && shows("xx") && shows("3.5 1"));
    type("PRINT A\n");
    CHECK(go() == -1 && strcmp(row(row_of("] PRINT A") + 1), "0") == 0);   /* (an undefined number is 0) */
    /* an error shows its message and where it was, and the prompt comes back */
    type("PRINT (\n");
    CHECK(go() == -1);
    CHECK(shows("Error:") && last_row_of("]") > row_of("Error:"));   /* and the prompt is below it again */
    /* HELP and CLS */
    fresh();
    type("HELP\n");
    CHECK(go() == -1 && shows("Commands:") && shows("EDIT [file]") && shows("Anything else is BASIC"));
    type("cls\n");
    CHECK(go() == -1 && strcmp(row(0), "]") == 0 && !shows("Commands:"));   /* cleared, and the prompt is at the top */
    type("ver\n");
    CHECK(go() == -1 && shows("MY-BASIC 1."));
    type("bye\n");
    CHECK(go() == BASIC_EXIT_BYE);
}

static const char hello_bas[] =
    "print \"Hello, world\";\n"
    "for i = 1 to 3\n"
    "  print i * i;\n"
    "next\n";

static void test_programs(void)
{
    fresh();
    CHECK(put_file("/sd/hello.bas", hello_bas) > 0);
    type("RUN\n");                                  /* nothing in memory yet */
    type("LOAD hello\nLIST\nRUN\n");
    CHECK(go() == -1);
    CHECK(shows("No program"));
    CHECK(shows("Loaded hello.bas, 62 bytes.") || shows("Loaded hello.bas, ") );
    CHECK(row_of("   1  print \"Hello, world\";") >= 0);
    CHECK(row_of("   2  for i = 1 to 3") >= 0 && row_of("   3    print i * i;") >= 0 && row_of("   4  next") >= 0);
    const int hello = row_of("Hello, world");
    CHECK(hello >= 0 && strcmp(row(hello + 1), "1") == 0 && strcmp(row(hello + 2), "4") == 0 && strcmp(row(hello + 3), "9") == 0 && strcmp(row(hello + 4), "Ready.") == 0);
    /* a program starts clean, and its variables are gone after it, as those of a line are */
    type("PRINT i\n");
    CHECK(go() == -1 && strcmp(row(last_row_of("] PRINT i") + 1), "0") == 0);
    type("NEW\nRUN\n");
    CHECK(go() == -1 && last_row_of("No program") >= 0);
    /* LIST ranges */
    type("LOAD hello.bas\nCLS\nLIST 2 3\n");
    CHECK(go() == -1);
    CHECK(row_of("   1  ") == -1 && row_of("   2  for i") >= 0 && row_of("   3    print") >= 0 && row_of("   4  ") == -1);
    type("CLS\nLIST 4\n");
    CHECK(go() == -1 && row_of("   4  next") >= 0 && row_of("   3  ") == -1);
    /* a program with a function, a list and a dictionary, a while loop and string functions */
    const char *rich =
        "def fib(n)\n"
        "  if n < 2 then return n\n"
        "  return fib(n - 1) + fib(n - 2)\n"
        "enddef\n"
        "print fib(20);\n"
        "l = list(1, 2, 3)\n"
        "push(l, 4)\n"
        "t = \"\"\n"
        "for x in l\n"
        "  t = t + str(x) + \",\"\n"
        "next\n"
        "print len(l), \" \", t;\n"
        "d = dict()\n"
        "d(\"a\") = 1\n"
        "d(\"b\") = 2\n"
        "print len(d);\n"
        "s = \"\"\n"
        "i = 0\n"
        "while i < 5\n"
        "  s = s + str(i)\n"
        "  i = i + 1\n"
        "wend\n"
        "print s, \" \", left(s, 2), \" \", mid(s, 2, 2);\n";
    CHECK(put_file("/sd/rich.bas", rich) > 0);
    type("CLS\nLOAD rich\nRUN\n");
    CHECK(go() == -1);
    const int out = row_of("6765");
    CHECK(out >= 0 && strcmp(row(out + 1), "4 1,2,3,4,") == 0 && strcmp(row(out + 2), "2") == 0 && strcmp(row(out + 3), "01234 01 23") == 0 && strcmp(row(out + 4), "Ready.") == 0);
    /* a runtime error says where; the program stops and the prompt returns */
    CHECK(put_file("/sd/bad.bas", "print 1;\nprint nothing(3);\nprint 2;\n") > 0);
    type("CLS\nLOAD bad\nRUN\n");
    CHECK(go() == -1);
    CHECK(shows("Error:") && shows("in line 2") && !shows("Ready.") && last_row_of("]") > row_of("Error:"));
    CHECK(row_of("1") >= 0 && row_of("1") < row_of("Error:") && row_of("2") == -1);   /* the line before the error ran, the line after did not */
}

/* STACK and HEAP show what the machine's hook says; without one they say so */
static void test_info_commands(void)
{
    fresh();
    type("CLS\nSTACK\n");
    CHECK(go() == -1 && shows("stack: peak 1234 of 4096") && !shows("heap:"));
    type("CLS\nheap\n");
    CHECK(go() == -1 && shows("heap: 5678 free") && !shows("stack:"));
    type("CLS\nusblog\n");
    CHECK(go() == -1 && shows("usblog: saved") && !shows("heap:"));
    type("CLS\nHELP\n");
    CHECK(go() == -1 && shows("STACK") && shows("HEAP") && shows("USBLOG"));
    info_on = 0;
    fresh();
    type("CLS\nSTACK\nHEAP\n");
    CHECK(go() == -1 && shows("No stack on this machine") && shows("No heap on this machine"));
    info_on = 1;
}

static void test_files(void)
{
    fresh();
    CHECK(put_file("/sd/hello.bas", hello_bas) > 0);
    /* LOAD finds NAME.bas for NAME; errors for what is not there */
    type("LOAD nothing\nLOAD\nLOAD hello\n");
    CHECK(go() == -1);
    CHECK(shows("Cannot load nothing: no such file or directory") && shows("Usage: LOAD file") && shows("Loaded hello.bas, "));
    CHECK(strcmp(env.name, "hello.bas") == 0 && env.length == strlen(hello_bas) && strcmp(env.program, hello_bas) == 0);
    /* SAVE with no name saves in place; with a name saves there, and that is the program's file from then on */
    CHECK(put_file("/sd/hello.bas", "changed on disk\n") > 0);
    type("SAVE\n");
    CHECK(go() == -1 && shows("Saved hello.bas, "));
    CHECK(file_is("/sd/hello.bas", hello_bas) && fs_stat("/sd/hello.bas.TMP", &(fs_stat_t){0}) == FS_ENOENT);
    type("SAVE copy.bas\n");
    CHECK(go() == -1 && file_is("/sd/copy.bas", hello_bas) && strcmp(env.name, "copy.bas") == 0);
    type("NEW\nSAVE\n");
    CHECK(go() == -1 && shows("Usage: SAVE file") && env.length == 0 && env.name[0] == 0);
    type("SAVE empty.bas\n");                        /* an empty program saves as an empty file */
    CHECK(go() == -1 && file_is("/sd/empty.bas", ""));
    /* FILES, with sizes and folders */
    CHECK(fs_mkdir("/sd/games") == 0);
    type("CLS\nFILES\n");
    CHECK(go() == -1);
    CHECK(shows("copy.bas") && shows("empty.bas") && shows("hello.bas") && shows("games") && shows("<DIR>"));
    char want[40];
    snprintf(want, sizeof want, "%9lu", (unsigned long)strlen(hello_bas));
    CHECK(shows(want));
    type("CLS\nFILES games\n");
    CHECK(go() == -1 && shows("(empty)"));
    type("CLS\nFILES nothere\n");
    CHECK(go() == -1 && shows("Cannot list nothere:"));
    /* CD */
    type("CLS\nCD\nCD games\nCD\nSAVE g.bas\nFILES\nCD /sd\nCD nowhere\n");
    CHECK(go() == -1);
    CHECK(shows("/sd/games") && shows("g.bas") && shows("Cannot go to nowhere:") && strcmp(fs_getcwd(), "/sd") == 0 && file_is("/sd/games/g.bas", ""));
    /* KILL asks first */
    type("CLS\nKILL copy.bas\nn\n");
    CHECK(go() == -1 && shows("Delete copy.bas? (Y/N)") && file_is("/sd/copy.bas", hello_bas));
    type("CLS\nKILL copy.bas\ny\nKILL copy.bas\ny\nKILL\n");
    CHECK(go() == -1 && fs_stat("/sd/copy.bas", &(fs_stat_t){0}) == FS_ENOENT && shows("Cannot delete copy.bas:") && shows("Usage: KILL file"));
    /* a program that is too big is refused, and the program in memory stays */
    static char big[BASIC_PROGRAM_MAX + 100];
    memset(big, ' ', sizeof big - 1);
    big[sizeof big - 1] = 0;
    for (size_t i = 70; i < sizeof big; i += 71)
        big[i] = '\n';
    int fd = fs_open("/sd/big.bas", FS_WRONLY | FS_CREAT);
    CHECK(fd >= 0 && fs_write(fd, big, strlen(big)) == (int)strlen(big) && fs_close(fd) == 0);
    type("CLS\nLOAD hello\nLOAD big\n");
    CHECK(go() == -1 && shows("That file is too big: the most is 65536 bytes.") && strcmp(env.name, "hello.bas") == 0 && strcmp(env.program, hello_bas) == 0);
    /* exactly the most is accepted */
    fd = fs_open("/sd/max.bas", FS_WRONLY | FS_CREAT);
    CHECK(fd >= 0 && fs_write(fd, big, BASIC_PROGRAM_MAX) == BASIC_PROGRAM_MAX && fs_close(fd) == 0);
    CHECK(basic_env_load(&env, "/sd/max.bas") == 0 && env.length == BASIC_PROGRAM_MAX);
}

static void test_editing_keys(void)
{
    fresh();
    /* the keys of the line editor, each shown by what the line then does */
    type("PRINT 12{LEFT}{LEFT}34\n");                                     /* inserts in the middle: PRINT 3412 */
    CHECK(go() == -1 && strcmp(row(3), "] PRINT 3412") == 0 && strcmp(row(4), "3412") == 0);
    type("PRINT 99{BS}1\n");                                              /* Backspace: PRINT 91 */
    CHECK(go() == -1 && strcmp(row(5), "] PRINT 91") == 0 && strcmp(row(6), "91") == 0);
    type("{LEFT}{LEFT}{HOME}PRINT 5{RIGHT}{RIGHT}\n");                    /* Left and Right at the ends do nothing */
    CHECK(go() == -1 && strcmp(row(7), "] PRINT 5") == 0 && strcmp(row(8), "5") == 0);
    type("XPRINT 100{HOME}{DEL}{END}0\n");                                /* Home, Delete, End: PRINT 1000 */
    CHECK(go() == -1 && strcmp(row(9), "] PRINT 1000") == 0 && strcmp(row(10), "1000") == 0);
    type("PRINT 1;XYZ{LEFT}{LEFT}{LEFT}^K\n");                            /* Ctrl+K: to the end of the line */
    CHECK(go() == -1 && strcmp(row(11), "] PRINT 1;") == 0 && strcmp(row(12), "1") == 0);
    type("PRINT 9^U\nPRINT 7\n");                                         /* Ctrl+U: the whole line (Enter on the empty line gives a prompt again) */
    CHECK(go() == -1 && strcmp(row(13), "]") == 0 && strcmp(row(14), "] PRINT 7") == 0 && strcmp(row(15), "7") == 0);
    type("RINT 5^AP^E\n");                                                /* Ctrl+A and Ctrl+E */
    CHECK(go() == -1 && strcmp(row(16), "] PRINT 5") == 0 && strcmp(row(17), "5") == 0);
    type("PRINT 6{DEL}{DEL}\n");                                           /* Delete at the end does nothing */
    CHECK(go() == -1 && strcmp(row(18), "] PRINT 6") == 0 && strcmp(row(19), "6") == 0);
    /* history: Up goes back through the lines run (a line the same as the last one is kept once), Down forward again and to an empty line */
    fresh();
    type("PRINT 1\nPRINT 2\nPRINT 3\n{UP}\n{UP}{UP}\n{UP}{DOWN}\n");
    CHECK(go() == -1);
    CHECK(strcmp(row(3), "] PRINT 1") == 0 && strcmp(row(5), "] PRINT 2") == 0 && strcmp(row(7), "] PRINT 3") == 0);
    CHECK(strcmp(row(9), "] PRINT 3") == 0 && strcmp(row(10), "3") == 0);       /* Up: the last line */
    CHECK(strcmp(row(11), "] PRINT 2") == 0 && strcmp(row(12), "2") == 0);      /* Up, Up: the one before it */
    CHECK(strcmp(row(13), "]") == 0 && strcmp(row(14), "]") == 0);              /* Up, Down: an empty line again, which does nothing */
    type("{UP}{UP}{UP}{UP}{UP}{UP}\n");                                         /* far more than there are: the oldest */
    CHECK(go() == -1 && strcmp(row(14), "] PRINT 1") == 0 && strcmp(row(15), "1") == 0);
    type("{DOWN}{DOWN}{UP}{DOWN}x\n");                                          /* Down with nothing newer: the empty line, typed on */
    CHECK(go() == -1 && strcmp(row(16), "] x") == 0);
    /* the line is as long as the screen line allows and no more */
    fresh();
    char longline[200];
    memset(longline, 'x', sizeof longline);
    longline[199] = 0;
    type(longline);
    type("\n");
    CHECK(go() == -1);
    const int pr = row_of("] xxxx");
    CHECK(pr >= 0 && strlen(row(pr)) == 79);                             /* 2 for the prompt, 77 typed: one column is left free */
    CHECK(shows("Error:") || shows("xxx"));
    /* Ctrl+C at the prompt gives the line up */
    fresh();
    type("PRINT 1^C\nPRINT 2\n");
    CHECK(go() == -1 && shows("] PRINT 1^C") && shows("2") && shows("] PRINT 2"));
    CHECK(!shows("] PRINT 1\n1"));
}

static void test_input_and_break(void)
{
    fresh();
    CHECK(put_file("/sd/ask.bas", "input \"Name: \", n$\nprint \"Hi \" + n$;\ninput \"Age: \", a\nprint a + 1;\n") > 0);
    type("LOAD ask\nRUN\nBob\n41\nPRINT n$\n");
    CHECK(go() == -1);
    const int ask = row_of("Name: Bob");
    CHECK(ask >= 0 && strcmp(row(ask + 1), "Hi Bob") == 0 && strcmp(row(ask + 2), "Age: 41") == 0 && strcmp(row(ask + 3), "42") == 0 && strcmp(row(ask + 4), "Ready.") == 0);
    CHECK(last_row_of("] PRINT n$") > ask + 4 && strcmp(row(last_row_of("] PRINT n$") + 1), "(EMPTY)") == 0);   /* the program's string variable is gone at the prompt */
    /* Ctrl+C at an INPUT ends the program */
    type("CLS\nRUN\n^C\n");
    CHECK(go() == -1 && shows("Name: ^C") && shows("Break") && !shows("Ready."));
    /* Ctrl+C stops a program that runs on for ever, and the prompt is back: nothing waits for it to end by itself */
    CHECK(put_file("/sd/loop.bas", "i = 0\nwhile 1\n  i = i + 1\nwend\n") > 0);
    type("CLS\nLOAD loop\nRUN\n^C");
    CHECK(go() == -1 && shows("Break"));
    type("PRINT 1\n");                              /* and the prompt works */
    CHECK(go() == -1 && last_row_of("1") > row_of("Break"));
    /* keys typed while a program runs are kept for its INPUT */
    CHECK(put_file("/sd/wait.bas", "for k = 1 to 3000\nnext\ninput \"x? \", x\nprint x * 2;\n") > 0);
    type("CLS\nLOAD wait\nRUN\n21\n");
    CHECK(go() == -1 && shows("x? 21") && shows("42"));
}

static void test_edit_and_exit(void)
{
    fresh();
    CHECK(put_file("/sd/hello.bas", hello_bas) > 0);
    type("EDIT hello.bas\n");
    CHECK(go() == BASIC_EXIT_EDIT && strcmp(env.edit_name, "hello.bas") == 0);
    fresh();
    CHECK(put_file("/sd/hello.bas", hello_bas) > 0);
    type("EDIT hello\n");                           /* NAME for NAME.bas, as for LOAD */
    CHECK(go() == BASIC_EXIT_EDIT && strcmp(env.edit_name, "hello.bas") == 0);
    fresh();
    CHECK(put_file("/sd/plain", "x\n") > 0);
    type("EDIT plain\n");                           /* a file that is there under that name, as it is */
    CHECK(go() == BASIC_EXIT_EDIT && strcmp(env.edit_name, "plain") == 0);
    fresh();
    type("EDIT fresh\n");                           /* a new name without an extension is a .bas file */
    CHECK(go() == BASIC_EXIT_EDIT && strcmp(env.edit_name, "fresh.bas") == 0);
    fresh();
    type("EDIT dotted.txt\n");
    CHECK(go() == BASIC_EXIT_EDIT && strcmp(env.edit_name, "dotted.txt") == 0);
    fresh();
    type("EDIT\n");                                 /* with no program there is nothing to edit */
    CHECK(go() == -1 && shows("Usage: EDIT file"));
    CHECK(put_file("/sd/prog.bas", hello_bas) > 0);
    type("LOAD prog\nEDIT\n");                      /* the program's own file */
    CHECK(go() == BASIC_EXIT_EDIT && strcmp(env.edit_name, "prog.bas") == 0);
    /* back from the editor: the file is loaded again and the prompt continues */
    CHECK(put_file("/sd/prog.bas", "print \"edited\";\n") > 0);
    CHECK(basic_env_load(&env, env.edit_name) == 0);
    script_len = script_pos = 0;
    type("RUN\nBYE\n");
    CHECK(go() == BASIC_EXIT_BYE && shows("edited"));
    CHECK(env.exit_code == BASIC_EXIT_BYE);
    /* QUIT and EXIT are BYE too, in any case */
    fresh();
    type("quit\n");
    CHECK(go() == BASIC_EXIT_BYE);
    fresh();
    type("  Exit  \n");
    CHECK(go() == BASIC_EXIT_BYE);
}

static void test_gaps(void)
{
    /* edges of the line editor */
    fresh();
    type("XPRINT 1{HOME}{RIGHT}{BS}\n");                                 /* Backspace at position 1 removes the first character */
    CHECK(go() == -1 && strcmp(row(3), "] PRINT 1") == 0 && strcmp(row(4), "1") == 0);
    type("PRINT 12{LEFT}{DEL}\n");                                       /* Delete of the last character with the cursor before it */
    CHECK(go() == -1 && strcmp(row(5), "] PRINT 1") == 0 && strcmp(row(6), "1") == 0);
    type("PRINT 1;XYZ{LEFT}{LEFT}{LEFT}^K2\n");                          /* typing on after Ctrl+K appends at the new end */
    CHECK(go() == -1 && strcmp(row(7), "] PRINT 1;2") == 0);
    type("PRINT 9^UPRINT 8\n");                                          /* typing on after Ctrl+U starts at the left */
    CHECK(go() == -1 && strcmp(row(row_of("] PRINT 8")), "] PRINT 8") == 0 && shows("8"));
    fresh();
    type("PRINT 1\nPRINT 2\n{DOWN}{UP}\n");                              /* Down with nothing newer does not go below the empty line */
    CHECK(go() == -1 && strcmp(row(7), "] PRINT 2") == 0);
    /* LOAD: the size limit is exact and a refused file leaves the program alone */
    fresh();
    CHECK(put_file("/sd/keep.bas", "print \"kept\";\n") > 0);
    static char big[BASIC_PROGRAM_MAX + 2];
    memset(big, ' ', sizeof big);
    big[BASIC_PROGRAM_MAX + 1] = 0;
    CHECK(put_file("/sd/over.bas", big) > 0);
    big[BASIC_PROGRAM_MAX] = 0;
    CHECK(put_file("/sd/exact.bas", big) > 0);
    CHECK(basic_env_load(&env, "keep") == 0 && env.length == 14);
    CHECK(basic_env_load(&env, "over") != 0 && env.length == 14 && strcmp(env.name, "keep.bas") == 0);
    CHECK(strncmp(env.program, "print \"kept\"", 12) == 0);
    CHECK(basic_env_load(&env, "exact") == 0 && env.length == BASIC_PROGRAM_MAX);
    /* a failed SAVE leaves no temporary file */
    fresh();
    CHECK(put_file("/sd/x.bas", "print 1;\n") > 0 && fs_mkdir("/sd/dir.bas") == 0);
    CHECK(basic_env_load(&env, "x") == 0);
    type("SAVE dir.bas\n");
    CHECK(go() == -1 && shows("Cannot save"));
    fs_stat_t st;
    CHECK(fs_stat("/sd/dir.bas.TMP", &st) != 0);
    /* paging of LIST and FILES: 22 rows, then "-- more --"; q (or Esc, Ctrl+C) stops, any other key goes on */
    fresh();
    char prog[2048];
    int n = 0;
    for (int i = 1; i <= 60; i++)
        n += snprintf(prog + n, sizeof prog - (size_t)n, "REM L%d\n", i);
    CHECK(put_file("/sd/long.bas", prog) > 0);
    type("LOAD long\nCLS\nLIST\n");
    CHECK(go() == -1 && shows("-- more --") && shows("REM L22") && !shows("REM L23"));
    fresh();                                        /* (the keys run out inside "-- more --", so each case is run again from the start) */
    CHECK(put_file("/sd/long.bas", prog) > 0);
    type("LOAD long\nCLS\nLIST\n ");
    CHECK(go() == -1 && shows("REM L44") && !shows("REM L45") && shows("-- more --"));
    fresh();
    CHECK(put_file("/sd/long.bas", prog) > 0);
    type("LOAD long\nCLS\nLIST\nq");
    CHECK(go() == -1 && !shows("REM L23") && !shows("-- more --") && last_row_of("]") > row_of("  22"));
    const int keys[] = {'q', 'Q', 27, 3};
    for (int k = 0; k < 4; k++) {
        fresh();
        CHECK(put_file("/sd/long.bas", prog) > 0);
        type("LOAD long\nCLS\nLIST\n");
        script[script_len++] = keys[k];
        CHECK(go() == -1 && !shows("REM L23") && !shows("-- more --"));
    }
    fresh();
    CHECK(put_file("/sd/long.bas", prog) > 0);
    type("LOAD long\nCLS\nLIST 5 7\nLIST 9\n");                       /* a range, and a single line */
    CHECK(go() == -1 && shows("REM L5") && shows("REM L7") && !shows("REM L8") && shows("REM L9") && !shows("REM L10"));
    fresh();
    for (int i = 0; i < 30; i++) {
        char name[32];
        snprintf(name, sizeof name, "/sd/f%02d.txt", i);
        CHECK(put_file(name, "x") > 0);
    }
    type("CLS\nFILES\n");
    CHECK(go() == -1 && shows("-- more --") && !shows("f22.txt"));
    fresh();
    for (int i = 0; i < 30; i++) {
        char name[32];
        snprintf(name, sizeof name, "/sd/f%02d.txt", i);
        CHECK(put_file(name, "x") > 0);
    }
    type("CLS\nFILES\nq");
    CHECK(go() == -1 && !shows("f23.txt") && !shows("-- more --"));
}


/* ---- the game functions ---- */

#define R8(x) x x x x x x x x
#define R64(x) R8(R8(x))

/* Runs a program (with `after` typed once it has ended, which is what the picture waits for). Leaves the environment as the run left it. */
static int run_jump = 1;   /* the frames that pass at each WAITFRAME */
static unsigned run_held, run_down;   /* the buttons a run sees */

static void run_program(const char *text, const char *after)
{
    fresh();
    frame_jump = run_jump;
    held = run_held;
    went_down = run_down;
    CHECK(put_file("/sd/t.bas", text) > 0);
    type("LOAD t\nRUN\n");
    late_keys = after;
    late_pos = 0;
    CHECK(go() == -1);
    late_keys = NULL;
}

static int printed(const char *line)   /* a line of the program's output is on the screen */
{
    return row_of(line) >= 0;
}

static int pixels(int idx)            /* how many pixels of the last picture have this palette index */
{
    int n = 0;
    for (int y = 0; y < TG_HEIGHT; y++)
        for (int x = 0; x < TG_WIDTH; x++)
            n += image[y][x] == idx;
    return n;
}

static void test_game_draw(void)
{
    run_program(
        "gfx(8)\n"
        "palette(4, 255, 0, 0)\n"
        "tiledef(1, \"" R64("4") "\")\n"
        "sprdef(1, \"" R64("7") "\")\n"
        "newmap(20, 20)\n"
        "tile(0, 0, 1)\n"
        "tile(2, 1, 1)\n"
        "a = actor(1, 100, 50)\n"
        "waitframe()\n"
        "print gettile(2, 1);\n"
        "print gettile(5, 5);\n"
        "print a;\n", " ");
    CHECK(printed("1") && printed("0") && printed("Ready."));
    CHECK(env.game == NULL && !env.game_active);                    /* freed, and the text screen is back */
    CHECK(shown_g.palette[4] == 0xF800);
    CHECK(pixels(4) == 128 && pixels(7) == 64 && pixels(0) == TG_WIDTH * TG_HEIGHT - 192);
    CHECK(image[0][0] == 4 && image[7][7] == 4 && image[0][8] == 0 && image[8][16] == 4 && image[15][23] == 4 && image[16][16] == 0);
    CHECK(image[50][100] == 7 && image[57][107] == 7 && image[58][100] == 0 && image[50][108] == 0);
    CHECK(shown_g.sprites[0].above == TG_MAX_LAYERS);                  /* in front of the map (0 would put the actors behind its tiles) */
    CHECK(presents >= 2);                                            /* WAITFRAME, then the last picture until the key */
    CHECK(allocs > 0 && allocs == releases && fast_allocs > 0);

    /* the camera: the picture moves the other way; a negative camera too */
    run_program(
        "gfx(8) : palette(4, 255, 0, 0) : tiledef(1, \"" R64("4") "\") : newmap(20, 20) : tile(0, 0, 1)\n"
        "camera(4, 3)\n"
        "waitframe()\n", " ");
    CHECK(pixels(4) == 20 && image[4][3] == 4 && image[4][4] == 0 && image[5][3] == 0 && image[0][0] == 4);
    run_program(
        "gfx(8) : tiledef(1, \"" R64("4") "\") : newmap(20, 20) : tile(0, 0, 1)\n"
        "camera(-10, -10)\n"
        "waitframe()\n", " ");
    CHECK(pixels(4) == 64 && image[10][10] == 4 && image[17][17] == 4 && image[9][10] == 0 && image[10][9] == 0);

    /* flips and single pixels, any colour 0 to 255 */
    run_program(
        "gfx(8) : tiledef(2, \"9" R8(R8(".")) "\") : tilepix(3, 2, 1, 200)\n"
        "newmap(8, 8) : tile(0, 0, 2, 1) : tile(1, 0, 2, 2) : tile(2, 0, 2, 3) : tile(3, 0, 3)\n"
        "waitframe()\n", " ");
    CHECK(image[0][7] == 9 && image[0][0] == 0);                   /* flipped across */
    CHECK(image[7][8] == 9 && image[0][8] == 0);                   /* flipped down */
    CHECK(image[7][23] == 9);                                      /* both */
    CHECK(image[1][26] == 200);

    /* 16 pixel tiles, and no map at all: only the actors */
    run_program(
        "gfx(16)\n"
        "for y = 0 to 15\n"
        "  sprrow(5, y, \"3333333333333333\")\n"
        "next\n"
        "a = actor(5, 10, 20)\n"
        "waitframe()\n", " ");
    CHECK(pixels(3) == 256 && image[20][10] == 3 && image[35][25] == 3 && image[36][10] == 0);
}

static void test_game_actors(void)
{
    /* velocity is per frame that went by: 3 frames at a wait, and no more than 4 */
    run_jump = 3;
    run_program(
        "gfx(8) : a = actor(1, 10, 20) : actorvel(a, 2, 0.5)\n"
        "print waitframe();\n"                  /* the first one has no frame before it to count from: 1 */
        "n = waitframe()\n"
        "print n;\n"
        "print actorx(a);\n"
        "print actory(a);\n"
        "print frames();\n", " ");
    {
        const int r = row_of("1");
        CHECK(r >= 0 && !strcmp(row(r + 1), "3") && !strcmp(row(r + 2), "18") && !strcmp(row(r + 3), "22") && !strcmp(row(r + 4), "106"));
    }
    run_jump = 9;
    run_program("gfx(8) : a = actor(1, 10, 20) : actorvel(a, 2, 0)\nwaitframe()\nn = waitframe()\nprint n;\nprint actorx(a);\n", " ");
    CHECK(printed("4") && printed("20"));                                                 /* 9 frames went by: no more than 4 are counted */
    run_jump = 1;

    /* several tiles, flips: the grid is read row by row and mirrored */
    run_program(
        "gfx(8) : sprdef(1, \"" R64("1") "\") : sprdef(2, \"" R64("2") "\") : sprdef(3, \"" R64("3") "\") : sprdef(4, \"" R64("4") "\")\n"
        "a = actor(1, 20, 30) : actorsize(a, 2, 2)\n"
        "waitframe()\n", " ");
    CHECK(image[30][20] == 1 && image[30][28] == 2 && image[38][20] == 3 && image[38][28] == 4 && image[46][36] == 0);
    run_program(
        "gfx(8) : sprdef(1, \"" R64("1") "\") : sprdef(2, \"" R64("2") "\") : sprdef(3, \"" R64("3") "\") : sprdef(4, \"" R64("4") "\")\n"
        "a = actor(1, 20, 30) : actorsize(a, 2, 2) : actorflip(a, 1)\n"
        "waitframe()\n", " ");
    CHECK(image[30][20] == 2 && image[30][28] == 1 && image[38][20] == 4 && image[38][28] == 3);

    /* priority, hiding, and freeing: the front actor is the lower priority number */
    run_program(
        "gfx(8) : sprdef(1, \"" R64("1") "\") : sprdef(2, \"" R64("2") "\")\n"
        "a = actor(1, 10, 10) : b = actor(2, 14, 10) : actorprio(a, 5) : actorprio(b, 9)\n"
        "waitframe()\n", " ");
    CHECK(image[10][10] == 1 && image[10][18] == 2 && image[10][14] == 1 && image[10][16] == 1);           /* a is in front where they overlap */
    run_program(
        "gfx(8) : sprdef(1, \"" R64("1") "\") : sprdef(2, \"" R64("2") "\")\n"
        "a = actor(1, 10, 10) : b = actor(2, 14, 10) : actorprio(a, 9) : actorprio(b, 5)\n"
        "waitframe()\n", " ");
    CHECK(image[10][14] == 2);
    run_program(
        "gfx(8) : sprdef(1, \"" R64("1") "\")\n"
        "a = actor(1, 10, 10) : b = actor(1, 40, 10) : actorhide(a, 1) : actorfree(b) : c = actor(1, 70, 10)\n"
        "waitframe()\n"
        "print b;\n"
        "print c;\n", " ");
    CHECK(pixels(1) == 64 && image[10][70] == 1 && image[10][10] == 0 && image[10][40] == 0);
    CHECK(printed("1"));                                                                 /* b's number was used again by c */

    /* only 128 actors */
    run_program(
        "gfx(8)\n"
        "for i = 1 to 130\n"
        "  a = actor(1, i, 0)\n"
        "next\n"
        "print a;\n", " ");
    CHECK(printed("-1"));

    /* what the program keeps in an actor */
    run_program(
        "gfx(8) : a = actor(1, 0, 0) : print actoruser(a); : actoruser(a, 4321) : print actoruser(a);\n", " ");
    CHECK(printed("0") && printed("4321"));
}

static void test_game_world(void)
{
    /* collisions: flags, and an actor that stops flush against a wall */
    run_program(
        "gfx(8) : newmap(30, 20) : settileflags(1, 1)\n"
        "tilefill(6, 0, 1, 10, 1)\n"
        "a = actor(1, 10, 10)\n"
        "h = actormove(a, 100, 0)\n"
        "print h;\n"
        "print actorx(a);\n"
        "h = actormove(a, 0, 100)\n"
        "print h;\n"
        "print actory(a);\n"
        "print tileflag(50, 4);\n"
        "print tileflag(10, 4);\n"
        "print tilebox(0, 0, 60, 10);\n"
        "print actorhit(a, a);\n", " ");
    {
        const int r = row_of("2");
        CHECK(r >= 0 && !strcmp(row(r + 1), "40") && !strcmp(row(r + 2), "0") && !strcmp(row(r + 3), "110"));
        CHECK(!strcmp(row(r + 4), "1") && !strcmp(row(r + 5), "0") && !strcmp(row(r + 6), "1") && !strcmp(row(r + 7), "1"));
    }
    /* a box of its own inside the picture */
    run_program(
        "gfx(8) : newmap(30, 20) : settileflags(1, 1) : tilefill(6, 0, 1, 10, 1)\n"
        "a = actor(1, 10, 10) : actorbox(a, 2, 0, 4, 8)\n"
        "h = actormove(a, 100, 0)\n"
        "print actorx(a);\n", " ");
    CHECK(printed("42"));                                                   /* the box is x + 2 to x + 6, and stops at 48 */

    /* infinite scrolling: the map repeats, the camera is kept within one map, the actors wrap */
    run_program(
        "gfx(8) : palette(4, 255, 0, 0) : tiledef(1, \"" R64("4") "\") : sprdef(1, \"" R64("7") "\") : newmap(64, 32) : tile(0, 0, 1)\n"
        "print wrap(1);\n"
        "camera(512, 256)\n"
        "a = actor(1, 600, 300)\n"
        "waitframe()\n"
        "print camx();\n"
        "print camy();\n"
        "camera(1000000, 5000)\n"
        "print camx();\n"
        "print camy();\n"
        "b = actor(1, 510, 5) : actorvel(b, 4, 0)\n"
        "waitframe()\n"
        "print actorx(b);\n", " ");
    CHECK(printed("1"));
    {
        const int r = row_of("1");
        CHECK(!strcmp(row(r + 1), "0") && !strcmp(row(r + 2), "0") && !strcmp(row(r + 3), "64") && !strcmp(row(r + 4), "136") && !strcmp(row(r + 5), "2"));
    }
    run_program(
        "gfx(8) : palette(4, 255, 0, 0) : tiledef(1, \"" R64("4") "\") : sprdef(1, \"" R64("7") "\") : newmap(64, 32) : tile(0, 0, 1) : wrap(1)\n"
        "camera(512, 256)\n"
        "a = actor(1, 600, 300)\n"
        "waitframe()\n", " ");
    CHECK(image[0][0] == 4 && image[7][7] == 4);                             /* the tile at the origin again after one map */
    CHECK(image[44][88] == 7 && image[51][95] == 7);                         /* an actor 88, 44 from the camera, whatever the lap */
    run_program(
        "gfx(8) : newmap(10, 10) : print wrap(1);\n", " ");                  /* 80 x 80 pixels: too small for the actors to wrap */
    CHECK(printed("0"));
}

static void test_game_input_and_errors(void)
{
    run_held = PLAT_BTN_LEFT;
    run_down = PLAT_BTN_START;
    run_program(
        "print btn(0, \"left\");\n"
        "print btn(0, 4);\n"
        "print btn(0, \"a\");\n"
        "print btn(0, 4 + 16);\n"
        "print btn(1, \"left\");\n"
        "print pressed(0, \"START\");\n"
        "print pressed(0, 256);\n", " ");
    {
        const int r = row_of("1");
        CHECK(r >= 0 && !strcmp(row(r + 1), "1") && !strcmp(row(r + 2), "0") && !strcmp(row(r + 3), "1") && !strcmp(row(r + 4), "0") && !strcmp(row(r + 5), "1") && !strcmp(row(r + 6), "1"));
    }
    run_held = run_down = 0;
    CHECK(env.game == NULL && presents == 0);                               /* these did not need the graphics */

    /* mistakes are reported with what to do, and the program stops with nothing left behind */
    run_program("gfx(8)\na = actor(1, 0, 0)\nprint actorx(7);\nprint 99;\n", "");
    CHECK(shows("there is no actor 7") && shows("Error:") && !printed("99"));
    CHECK(env.game == NULL && !env.game_active && allocs == releases);
    /* an error after a picture was shown: no waiting for a key, the text screen with the error is there at once */
    run_program("gfx(8)\nwaitframe()\nprint actorx(7);\n", "");
    CHECK(shows("there is no actor 7") && env.game == NULL && !env.game_active);
    run_program("tile(0, 0, 1)\n", "");
    CHECK(shows("There is no map") && env.game == NULL);
    run_program("gfx(7)\n", "");
    CHECK(shows("the size of a tile is 8 or 16"));
    run_program("gfx(8) : tiledef(300, \"1\")\n", "");
    CHECK(shows("n is 0 to 255"));
    run_program("gfx(8) : print actor(1, 0, 0) : a = actorsize(0, 5, 1)\n", "");
    CHECK(shows("1 to 4 of each"));
    run_program("gfx(8) : newmap(0, 5)\n", "");
    CHECK(shows("at most"));

    /* without a tile screen the graphics functions still work, and WAITFRAME says so */
    game_hooks_on = 0;
    run_program("gfx(8) : newmap(4, 4) : tile(1, 1, 2) : print gettile(1, 1); : waitframe()\nprint 99;\n", "");
    CHECK(printed("2") && shows("no tile screen") && !printed("99") && env.game == NULL);
    game_hooks_on = 1;

    /* Ctrl+C stops a program that loops over WAITFRAME, and the text screen is back */
    fresh();
    CHECK(put_file("/sd/t.bas", "gfx(8)\nwhile 1\n  waitframe()\nwend\n") > 0);
    type("LOAD t\nRUN\n^C");
    type("PRINT 6 * 7\n");
    CHECK(go() == -1);
    CHECK(shows("Break") && row_of("42") >= 0 && env.game == NULL && !env.game_active);
    CHECK(allocs == releases);

    /* the picture stays until a key is typed: it is presented again and again, and the key that ends it is not a command */
    fresh();
    CHECK(put_file("/sd/t.bas", "gfx(8) : waitframe()\n") > 0);
    type("LOAD t\nRUN\n");
    late_keys = "x";
    late_pos = 0;
    late_min_presents = 8;
    CHECK(go() == -1);
    CHECK(presents >= 8 && !env.game_active && env.game == NULL && shows("Ready.") && !shows("] x"));
    basic_env_close(&env);
    CHECK(allocs == releases);
}

/* The example program: a ball that walks around a room and stops at the walls. */
static void test_game_example(void)
{
    FILE *f = fopen("basic/examples/walk.bas", "r");
    CHECK(f != NULL);
    static char text[4096];
    const size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    CHECK(n > 500 && n < sizeof text - 1);

    /* left for 21 frames: 2 pixels a frame; the camera follows, so the ball stays in the middle of the screen */
    run_held = PLAT_BTN_LEFT;
    fresh();
    frame_jump = 1;
    held = run_held;
    press_b_after = 21;
    CHECK(put_file("/sd/walk.bas", text) > 0);
    type("LOAD walk\nRUN\n");
    late_keys = " ";
    late_pos = 0;
    CHECK(go() == -1);
    CHECK(printed("118") && printed("Ready."));                       /* 160 - 21 * 2: B was down after the 21st wait */
    CHECK(pixels(4) == 52 && image[123][160] == 4 && image[120][160] == 2 && image[120][162] == 4);   /* the ball: 8 x 8 without its corners (the floor shows there), in the middle */
    CHECK(image[100][10] == 0 && image[100][100] == 2 && image[100][44] == 1);   /* left of the map is the backdrop (the camera is at -42), then its wall, then the floor */

    /* left for ever: it stops at the wall (the floor starts at x = 8) */
    fresh();
    frame_jump = 1;
    held = PLAT_BTN_LEFT | PLAT_BTN_UP;
    press_b_after = 150;
    CHECK(put_file("/sd/walk.bas", text) > 0);
    type("LOAD walk\nRUN\n");
    late_keys = " ";
    late_pos = 0;
    CHECK(go() == -1);
    {
        const int r = row_of("8");
        CHECK(r >= 0 && !strcmp(row(r + 1), "8"));
    }
    CHECK(env.game == NULL && allocs == releases);
    CHECK(frame_while_game == 0);                                     /* the running program never put the text screen up over the game */
    run_held = 0;
}

int main(void)
{
    test_basics();
    test_programs();
    test_files();
    test_info_commands();
    test_editing_keys();
    test_input_and_break();
    test_edit_and_exit();
    test_gaps();
    test_game_draw();
    test_game_actors();
    test_game_world();
    test_game_input_and_errors();
    test_game_example();
    printf("all %d checks passed\n", checks);
    return 0;
}
