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

static int get_char(void)
{
    if (script_pos >= script_len)
        return -1;
    idle_frames = 0;
    return script[script_pos++];
}

static void frame(void)
{
    now_ms += 17;
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

/* A fresh environment (the program, the screen and the files are new). */
static void fresh(void)
{
    fs_reset();
    CHECK(fs_ram_init(&disk, pool, sizeof pool) == 0 && fs_mount("sd", &fs_ram_ops, &disk) == 0);
    fs_chdir("/sd");
    memset(&screen, 0, sizeof screen);
    tg_text_load_cp437(&screen);
    tg_text_init(&screen);
    const basic_hooks_t hooks = {get_char, frame, millis, info_on ? test_info : NULL};
    basic_env_init(&env, &screen, &hooks);
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
    printf("all %d checks passed\n", checks);
    return 0;
}
