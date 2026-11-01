/* SPDX-License-Identifier: MIT */
/* Tests of the file editor demo (editsd): the menu and the editor are driven with scripted keys on a RAM disk, and the files and the screen are checked. Exits non-zero on the first failure. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "consoleio.h"
#include "editsd_logic.h"
#include "tilefs.h"
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
    editsd_menu();
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

static fs_ram_t disk;
static uint8_t pool[256 * 1024];

static void fresh(size_t pool_bytes)
{
    fs_reset();
    CHECK(fs_ram_init(&disk, pool, pool_bytes) == 0);
    CHECK(fs_mount("sd", &fs_ram_ops, &disk) == 0);
    CHECK(fs_chdir("/sd") == 0);
    editsd_init();
}

static int put(const char *name, const void *data, size_t n)
{
    const int fd = fs_open(name, FS_WRONLY | FS_CREAT | FS_TRUNC);
    if (fd < 0)
        return fd;
    const int w = fs_write(fd, data, n);
    fs_close(fd);
    return w;
}

/* The file's content equals this text (of this length). */
static int file_is(const char *name, const char *want, size_t n)
{
    static char buf[40000];
    const int fd = fs_open(name, FS_RDONLY);
    if (fd < 0)
        return 0;
    int total = 0, r;
    while ((r = fs_read(fd, buf + total, sizeof buf - (size_t)total)) > 0)
        total += r;
    fs_close(fd);
    return (size_t)total == n && memcmp(buf, want, n) == 0;
}

static int exists(const char *name)
{
    return fs_stat(name, &(fs_stat_t){0}) == 0;
}

/* Does any row of the screen start with this text? */
static int shows(const char *prefix)
{
    for (int r = 0; r < 25; r++)
        if (strncmp(row(r), prefix, strlen(prefix)) == 0)
            return 1;
    return 0;
}

static int shows_text(const char *text)
{
    for (int r = 0; r < 25; r++)
        if (strstr(row(r), text))
            return 1;
    return 0;
}

static void test_menu(void)
{
    fresh(sizeof pool);
    CHECK(run("") == 0);
    CHECK(strncmp(row(0), "EDITSD  the editor of PicoParanoia on files", 43) == 0);
    CHECK(strcmp(row(2), "  Folder: /sd") == 0 && strcmp(row(3), "  File:   (none)") == 0);
    CHECK(strcmp(row(5), "  O  open a file, or make a new one") == 0 && strcmp(row(12), "  Q  quit") == 0 && strcmp(row(14), "Choice:") == 0);
    CHECK((console_screen()->cells[0] >> 8) == 0x0F);
    CHECK(run("xyz?{UP}{DOWN}q") == 1);
    CHECK(run("Q") == 1 && run("q") == 1);
    CHECK(run("E") == 0 && shows("No file is open"));         /* nothing to edit yet */
    CHECK(run("E ") == 0 && shows("EDITSD"));                  /* and a space goes back to the menu */
    CHECK(run("V") == 0 && shows("No file is open"));
    CHECK(run("I") == 0 && shows("File:   (none)") && shows_text("saved: 0 times"));
}

static void test_open_and_save(void)
{
    fresh(sizeof pool);
    CHECK(put("/sd/a.txt", "hello\nworld\n", 12) == 12);
    /* open it and leave without a change: the editor saves what is there */
    CHECK(run("Oa.txt\n^WQ") == 1);
    CHECK(strcmp(editsd_path(), "/sd/a.txt") == 0 && editsd_saves() == 1);
    CHECK(file_is("/sd/a.txt", "hello\nworld\n", 12) && !exists("/sd/a.txt.TMP"));
    /* a change at the start, and at the end */
    CHECK(run("Oa.txt\nX^WYQ") == 1);
    CHECK(file_is("/sd/a.txt", "Xhello\nworld\n", 13) && !exists("/sd/a.txt.TMP") && editsd_saves() == 2);
    CHECK(run("E{DOWN}{END}!!^WYQ") == 1);                      /* E: the open file again */
    CHECK(file_is("/sd/a.txt", "Xhello\nworld!!\n", 15) && editsd_saves() == 3);
    /* not saving: N leaves the file alone */
    CHECK(run("EQQ^WNQ") == 1);
    CHECK(file_is("/sd/a.txt", "Xhello\nworld!!\n", 15) && editsd_saves() == 3);
    /* the menu shows the file open; information about it */
    CHECK(run("I") == 0);
    CHECK(shows("Folder: /sd") && shows("File:   /sd/a.txt") && shows_text("bytes: 15") && shows_text("lines: 2") && shows_text("line ends: LF"));
    CHECK(shows_text("free:") && shows_text("saved: 3 times"));
    CHECK(run("I ") == 0 && shows_text("File:   /sd/a.txt"));
    /* a name with a path */
    CHECK(fs_mkdir("/sd/dir") == 0 && put("/sd/dir/b.txt", "bee\n", 4) == 4);
    CHECK(run("O/sd/dir/b.txt\nZ^WYQ") == 1 && file_is("/sd/dir/b.txt", "Zbee\n", 5) && strcmp(editsd_path(), "/sd/dir/b.txt") == 0);
    CHECK(run("Odir/../a.txt\n^WQ") == 1 && strcmp(editsd_path(), "/sd/a.txt") == 0);
    /* an empty name does nothing */
    CHECK(run("O\nQ") == 1 && strcmp(editsd_path(), "/sd/a.txt") == 0);
}

static void test_new_files(void)
{
    fresh(sizeof pool);
    CHECK(run("Onew.txt\nNQ") == 1);                           /* refuse to make it */
    CHECK(!exists("/sd/new.txt") && editsd_path()[0] == 0);
    CHECK(run("Onew.txt\nYhello\nthere^WYQ") == 1);            /* make it, type, save */
    CHECK(file_is("/sd/new.txt", "hello\nthere\n", 12) && !exists("/sd/new.txt.TMP"));
    CHECK(run("Onew2.txt\nY^WQ") == 1);                        /* make it and leave at once: an empty line is saved */
    CHECK(exists("/sd/new2.txt"));
    CHECK(run("Onew3.txt\nYQ") == 0);                          /* (the editor is waiting for a key) */
    CHECK(run("Onew3.txt\nYabc^WNQ") == 1);                    /* made, typed, but not saved: it is not there */
    CHECK(!exists("/sd/new3.txt"));
    /* the menu says it is new until it is saved */
    CHECK(run("Onew4.txt\nYa^WNQ") == 1 && run("") == 0 && shows_text("(new)"));
    CHECK(run("V") == 0 && shows("The file has not been saved yet."));
    CHECK(run("I") == 0 && shows_text("not saved yet"));
    CHECK(run("E^WY") == 0 && !shows_text("SAVE FAILED"));       /* (the editor is open, waiting) */
    CHECK(run("E^WYQ") == 1 && exists("/sd/new4.txt") && run("") == 0 && !shows_text("(new)") && shows_text("File:   /sd/new4.txt"));   /* saved: no longer new */
    /* names that cannot be made */
    fresh(sizeof pool);
    CHECK(run("Onodir/x.txt\n") == 0 && shows("The folder it should be in does not exist."));
    CHECK(run("Obad:name\n") == 0 && shows_text("Cannot use that name"));
    CHECK(fs_mkdir("/sd/folder") == 0);
    CHECK(run("Ofolder\n") == 0 && shows("That is a folder"));
    CHECK(run("Ofolder\n Q") == 1);
}

static void test_line_ends(void)
{
    fresh(sizeof pool);
    CHECK(put("/sd/dos.txt", "one\r\ntwo\r\nthree\r\n", 17) == 17);
    CHECK(run("Odos.txt\nX^WYQ") == 1);                        /* shown with LF, saved with CR LF again */
    CHECK(file_is("/sd/dos.txt", "Xone\r\ntwo\r\nthree\r\n", 18));
    CHECK(run("I") == 0 && shows_text("line ends: CR LF"));
    CHECK(run("Odos.txt\n{DOWN}{DOWN}{DOWN}end^WYQ") == 1);
    CHECK(file_is("/sd/dos.txt", "Xone\r\ntwo\r\nthree\r\nend\r\n", 23));
    CHECK(run("V") == 0 && strcmp(row(0), "Xone") == 0 && strcmp(row(1), "two") == 0 && strcmp(row(3), "end") == 0);
    /* a file without a final newline gets one (as the editor always does) */
    CHECK(put("/sd/nonl.txt", "abc", 3) == 3);
    CHECK(run("Ononl.txt\n^WQ") == 1 && file_is("/sd/nonl.txt", "abc\n", 4));
    /* a mixed file follows its first line */
    CHECK(put("/sd/mixed.txt", "a\nb\r\nc\n", 7) == 7);
    CHECK(run("Omixed.txt\n^WQ") == 1 && file_is("/sd/mixed.txt", "a\nb\nc\n", 6));
    CHECK(put("/sd/mixed2.txt", "a\r\nb\nc\n", 7) == 7);
    CHECK(run("Omixed2.txt\n^WQ") == 1 && file_is("/sd/mixed2.txt", "a\r\nb\r\nc\r\n", 9));
    /* tabs stay */
    CHECK(put("/sd/tab.txt", "a\tb\n", 4) == 4);
    CHECK(run("Otab.txt\n^WQ") == 1 && file_is("/sd/tab.txt", "a\tb\n", 4));
}

static void test_refusals(void)
{
    static char big[EDITSD_FILE_MAX + 100];
    fresh(sizeof pool);
    CHECK(put("/sd/good.txt", "fine\n", 5) == 5);
    CHECK(run("Ogood.txt\n^WQ") == 1 && strcmp(editsd_path(), "/sd/good.txt") == 0);
    const char *binary = "abc\001def\n";
    CHECK(put("/sd/bin.dat", binary, 8) == 8);
    CHECK(run("Obin.dat\n") == 0 && shows_text("The editor cannot take this file: not a text file (byte 1 at 3)"));
    CHECK(run("Obin.dat\n Q") == 1 && strcmp(editsd_path(), "/sd/good.txt") == 0);   /* the open file stays */
    CHECK(put("/sd/nul.dat", "ab\0cd\n", 6) == 6);
    CHECK(run("Onul.dat\n") == 0 && shows_text("not a text file (byte 0 at 2)"));
    CHECK(put("/sd/del.dat", "ab\177\n", 4) == 4);
    CHECK(run("Odel.dat\n") == 0 && shows_text("not a text file (byte 127 at 2)"));
    CHECK(put("/sd/cr.txt", "a\rb\n", 4) == 4);
    CHECK(run("Ocr.txt\n") == 0 && shows_text("a carriage return alone, at 1"));
    CHECK(put("/sd/cr2.txt", "ab\r", 3) == 3);
    CHECK(run("Ocr2.txt\n") == 0 && shows_text("a carriage return alone, at the end"));
    CHECK(put("/sd/cr3.txt", "a\r\r\nb\n", 6) == 6);
    CHECK(run("Ocr3.txt\n") == 0 && shows_text("a carriage return alone"));
    memset(big, 'x', 300);
    big[300] = '\n';
    CHECK(put("/sd/long.txt", big, 301) == 301);
    CHECK(run("Olong.txt\n") == 0 && shows_text("line 1 is longer than 250 characters"));
    memset(big, 'y', EDITSD_LINE_MAX);          /* exactly 250 is fine, 251 is not */
    big[EDITSD_LINE_MAX] = '\n';
    CHECK(put("/sd/l250.txt", big, EDITSD_LINE_MAX + 1) == EDITSD_LINE_MAX + 1);
    CHECK(run("Ol250.txt\n^WQ") == 1);
    memset(big, 'y', EDITSD_LINE_MAX + 1);
    big[EDITSD_LINE_MAX + 1] = '\n';
    CHECK(put("/sd/l251.txt", big, EDITSD_LINE_MAX + 2) == EDITSD_LINE_MAX + 2);
    CHECK(run("Ol251.txt\n") == 0 && shows_text("longer than 250"));
    memset(big, 'a', sizeof big);               /* a line break every 79 characters */
    for (size_t i = 78; i < sizeof big; i += 79)
        big[i] = '\n';
    big[EDITSD_FILE_MAX - 1] = '\n';
    CHECK(put("/sd/max.txt", big, EDITSD_FILE_MAX) == EDITSD_FILE_MAX);   /* the most there is */
    CHECK(run("Omax.txt\n^WQ") == 1);
    {   /* what the editor saved can be opened again: it cannot write more than it holds */
        fs_stat_t st;
        CHECK(fs_stat("/sd/max.txt", &st) == 0 && st.size == EDITSD_FILE_MAX);
        CHECK(run("Omax.txt\n^WQ") == 1);
    }
    big[EDITSD_FILE_MAX - 1] = 'a';             /* without the last line feed the editor would add one: one byte over */
    CHECK(put("/sd/nolf.txt", big, EDITSD_FILE_MAX) == EDITSD_FILE_MAX);
    CHECK(run("Onolf.txt\n") == 0 && shows_text("too big for the editor"));
    CHECK(put("/sd/nolf.txt", big, EDITSD_FILE_MAX - 1) == EDITSD_FILE_MAX - 1);   /* one less: the editor adds its line feed and it fits */
    CHECK(run("Onolf.txt\n^WQ") == 1);
    {
        fs_stat_t st;
        CHECK(fs_stat("/sd/nolf.txt", &st) == 0 && st.size == EDITSD_FILE_MAX);
    }
    CHECK(put("/sd/over.txt", big, EDITSD_FILE_MAX + 1) == EDITSD_FILE_MAX + 1);
    CHECK(run("Oover.txt\n") == 0 && shows_text("too big for the editor"));
    CHECK(run("Omax.txt\n^WQ") == 1);          /* the open file is max.txt again for what follows */
    CHECK(run("I") == 0 && shows("File:   /sd/max.txt") && shows_text("bytes: 32248"));   /* the refused ones did not replace the open file */
    /* a file that goes bad after it was opened is told of when viewed */
    CHECK(put("/sd/max.txt", "now\001bad\n", 8) == 8);
    CHECK(run("V") == 0 && shows_text("Cannot show the file: not a text file (byte 1 at 3)"));
    CHECK(run("I") == 0 && shows_text("the editor cannot take it: not a text file"));
}

static void test_list_and_folders(void)
{
    fresh(sizeof pool);
    CHECK(run("L") == 0 && shows("/sd") && shows_text("(empty)"));
    CHECK(put("/sd/alpha.txt", "12345\n", 6) == 6 && fs_mkdir("/sd/Sub") == 0 && put("/sd/Sub/inner.txt", "i\n", 2) == 2);
    CHECK(run("L") == 0);
    CHECK(shows_text("alpha.txt") && shows_text("         6") && shows_text("Sub") && shows_text("<DIR>"));
    CHECK(run("L ") == 0 && shows("EDITSD"));
    /* more than a page */
    for (int i = 0; i < 40; i++) {
        char name[40];
        snprintf(name, sizeof name, "/sd/file%02d.txt", i);
        CHECK(put(name, "x", 1) == 1);
    }
    CHECK(run("L") == 0 && shows("-- space for more, Q to stop --"));
    CHECK(run("L ") == 0 && shows_text("Press SPACE"));        /* the second page is the last: 42 entries, 21 and 21 */
    CHECK(run("L ") == 0 && shows_text("file39.txt") && !shows_text("-- space for more"));
    CHECK(run("LQ") == 0 && shows("EDITSD") == 0);             /* Q stops the listing (and then Press SPACE) */
    CHECK(run("LQ ") == 0 && shows("EDITSD"));
    /* change folder */
    CHECK(run("CSub\n") == 0);
    CHECK(strcmp(fs_getcwd(), "/sd/Sub") == 0);
    CHECK(run("") == 0 && shows("  Folder: /sd/Sub"));
    CHECK(run("Oinner.txt\n^WQ") == 1 && strcmp(editsd_path(), "/sd/Sub/inner.txt") == 0);   /* names are looked up in the folder */
    CHECK(run("C..\n") == 0 && strcmp(fs_getcwd(), "/sd") == 0);
    CHECK(run("C/sd/Sub\nC/\n") == 0 && strcmp(fs_getcwd(), "/") == 0);
    CHECK(run("L") == 0 && shows_text("sd") && shows_text("<DIR>"));   /* the top lists the mounts */
    CHECK(run("Csd\n") == 0 && strcmp(fs_getcwd(), "/sd") == 0);
    CHECK(run("Cnowhere\n") == 0 && shows_text("Cannot go there:") && strcmp(fs_getcwd(), "/sd") == 0);
    CHECK(run("Calpha.txt\n") == 0 && shows_text("Cannot go there:"));
    CHECK(run("C\nQ") == 1);                                   /* nothing typed: nothing changes */
}

static void test_view(void)
{
    fresh(sizeof pool);
    static char text[8000];
    text[0] = 0;
    for (int i = 0; i < 60; i++) {
        char line[24];
        snprintf(line, sizeof line, "line %d\n", i);
        strcat(text, line);
    }
    CHECK(put("/sd/many.txt", text, strlen(text)) == (int)strlen(text));
    CHECK(run("Omany.txt\n^WQ") == 1);
    CHECK(run("V") == 0 && strcmp(row(0), "line 0") == 0 && strcmp(row(21), "line 21") == 0 && strcmp(row(22), "-- space for more, Q to stop --") == 0);
    CHECK(run("V ") == 0 && strcmp(row(0), "line 22") == 0);
    CHECK(run("V  ") == 0 && strcmp(row(0), "line 44") == 0 && shows_text("Press SPACE"));
    CHECK(run("VQ") == 0 && shows_text("Press SPACE"));
    CHECK(run("VQ Q") == 1);
    /* a tab goes to the next multiple of 8; a long line is broken at the right edge */
    char line[200];
    memset(line, 'w', 150);
    line[150] = '\n';
    CHECK(put("/sd/wide.txt", line, 151) == 151);
    CHECK(put("/sd/tabs.txt", "a\tb\n\tc\n", 7) == 7);
    CHECK(run("Otabs.txt\n^WQ") == 1 && run("V") == 0);
    CHECK(strcmp(row(0), "a       b") == 0 && strcmp(row(1), "        c") == 0);
    CHECK(run("Owide.txt\n^WQ") == 1 && run("V") == 0);
    CHECK(strlen(row(0)) == 79 && strlen(row(1)) == 71);
    CHECK(run("V Q") == 1);
}

static void test_delete(void)
{
    fresh(sizeof pool);
    CHECK(put("/sd/gone.txt", "bye\n", 4) == 4 && put("/sd/stay.txt", "stay\n", 5) == 5 && fs_mkdir("/sd/d") == 0);
    CHECK(run("Dgone.txt\nN") == 0 && shows("EDITSD"));   /* N: back to the menu */
    CHECK(exists("/sd/gone.txt"));                             /* kept */
    CHECK(run("Dgone.txt\nYQ") == 1);
    CHECK(!exists("/sd/gone.txt") && exists("/sd/stay.txt"));
    CHECK(run("Dgone.txt\n") == 0 && shows_text("Cannot find it:"));
    CHECK(run("Dd\n") == 0 && shows_text("That is a folder"));
    CHECK(exists("/sd/d"));
    CHECK(run("Ostay.txt\n^WQ") == 1 && strcmp(editsd_path(), "/sd/stay.txt") == 0);
    CHECK(run("Dstay.txt\nYQ") == 1 && !exists("/sd/stay.txt"));
    CHECK(editsd_path()[0] == 0);                              /* deleting the open file closes it */
    CHECK(run("E") == 0 && shows("No file is open"));
}

static void test_failed_saves(void)
{
    static char text[9000];
    memset(text, 'q', sizeof text);
    for (size_t i = 59; i < sizeof text; i += 60)
        text[i] = '\n';
    /* a disk too small to hold a second copy of the file: the save fails, and the file is as it was */
    fresh(20 * 1024);
    CHECK(put("/sd/big.txt", text, 8940) == 8940);
    uint64_t free_bytes, total;
    CHECK(fs_space("/sd", &free_bytes, &total) == 0);
    char filler[16000];
    memset(filler, 'f', sizeof filler);
    size_t fill = (size_t)free_bytes - 5000;                   /* leave less room than the file needs */
    CHECK(put("/sd/filler.dat", filler, fill > sizeof filler ? sizeof filler : fill) > 0);
    CHECK(run("Obig.txt\nX^WYQ") == 0);                        /* the failure is shown after leaving the editor */
    CHECK(shows("SAVE FAILED: the card or disk failed (full?): the file is as it was before"));
    CHECK(file_is("/sd/big.txt", text, 8940));                 /* unchanged */
    CHECK(!exists("/sd/big.txt.TMP"));                         /* no half written copy left behind */
    CHECK(editsd_saves() == 0);
    /* with room again, the same edit works */
    CHECK(fs_unlink("/sd/filler.dat") == 0);
    CHECK(run("Obig.txt\nX^WYQ") == 1);
    CHECK(editsd_saves() == 1 && !exists("/sd/big.txt.TMP"));
    {   /* the edit is in: an X in front */
        static char edited[9000];
        edited[0] = 'X';
        memcpy(edited + 1, text, 8940);
        CHECK(file_is("/sd/big.txt", edited, 8941));
    }
    /* a name the disk cannot hold is refused when it is typed */
    fresh(sizeof pool);
    char longname[300];
    memset(longname, 'n', 100);
    longname[100] = 0;
    char cmd[400];
    snprintf(cmd, sizeof cmd, "O%s\n", longname);
    CHECK(run(cmd) == 0 && shows_text("Cannot use that name: name too long"));
}

int main(void)
{
    test_menu();
    test_open_and_save();
    test_new_files();
    test_line_ends();
    test_refusals();
    test_list_and_folders();
    test_view();
    test_delete();
    test_failed_saves();
    printf("all %d checks passed\n", checks);
    return 0;
}
