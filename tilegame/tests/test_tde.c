/* SPDX-License-Identifier: MIT */
/*
 * Tests of the editor port: TDE (tde/src, tde/os) is run in a child process of its own on a RAM disk with scripted keys, and what is on its screen and in
 * its files afterwards is checked. (The editor keeps its state in globals, so each run is a process.) The platform layer is replaced by a few functions
 * that feed keys and end the run when they are used up.
 */
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "platform.h"
#include "tde_os.h"
#include "tg_text.h"
#include "tilefs.h"

FILE *__real_fopen(const char *name, const char *mode);
static int checks;
static void show_screen(void);
#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            show_screen();                                                   \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

/* ---- what the child sees as the platform ---- */

static plat_key_event_t script[4096];
static int script_len, script_pos;
static int out_fd;                       /* the pipe to the parent: the screen when the keys are used up */
static tg_text_t screen;
static fs_ram_t disk;
static uint8_t pool[512 * 1024];

int plat_getkey(plat_key_event_t *e)
{
    if (script_pos >= script_len)
        return 0;
    *e = script[script_pos++];
    return 1;
}

bool plat_quit_requested(void) { return false; }

static void finish(int code);

static void idle(void)
{
    finish(0);
}

static char dump_names[8][64];            /* files whose content the child sends back after the screen */
static int dump_count;

/* The screen as 25 lines of 80 characters, and then the files: the child writes it all to the pipe and ends. */
static void finish(int code)
{
    char line[82];
    for (int r = 0; r < 25; r++) {
        for (int c = 0; c < 80; c++)
            line[c] = (char)(screen.cells[r * 80 + c] & 255);
        line[80] = '\n';
        if (write(out_fd, line, 81) != 81)
            _exit(3);
    }
    {   /* the attribute of every cell, as a byte */
        unsigned char attrs[25 * 80];
        for (int i = 0; i < 25 * 80; i++)
            attrs[i] = (unsigned char)(screen.cells[i] >> 8);
        if (write(out_fd, attrs, sizeof attrs) != (ssize_t)sizeof attrs)
            _exit(3);
    }
 {
        char head[100];
        const int n = snprintf(head, sizeof head, "cursor %d %d %d %d %d\n", screen.cursor_col, screen.cursor_row, screen.cursor_start, screen.cursor_end,
                               (screen.cursor_flags & TG_CURSOR_VISIBLE) != 0);
        if (write(out_fd, head, (size_t)n) != n)
            _exit(3);
    }
    for (int i = 0; i < dump_count; i++) {   /* "name length\n" and the bytes; length -1 for a file that is not there */
        static char data[200000];
        fs_stat_t st;
        char head[100];
        int len = -1;
        const int fd = fs_open(dump_names[i], FS_RDONLY);
        if (fd >= 0) {
            len = 0;
            int r;
            while ((r = fs_read(fd, data + len, sizeof data - (size_t)len)) > 0)
                len += r;
            fs_close(fd);
        } else if (fs_stat(dump_names[i], &st) == 0) {
            len = -2;                          /* a directory */
        }
        const int n = snprintf(head, sizeof head, "%s %d\n", dump_names[i], len);
        if (write(out_fd, head, (size_t)n) != n || (len > 0 && write(out_fd, data, (size_t)len) != len))
            _exit(3);
    }
    _exit(code);
}

static void nop(void) {}

static const char *const config_files[] = {"tde.cfg", "tde.hlp", "tde.shl"};

/* Keys, as for the other tests: a character is itself, ^A..^Z Ctrl letters, {NAME} other keys, {C-x} Ctrl+x, {A-x} Alt+x, {S-NAME} Shift+NAME. */
static const struct { const char *name; uint8_t usage; } named[] = {
    {"ENTER", 0x28}, {"ESC", 0x29}, {"BS", 0x2A}, {"TAB", 0x2B}, {"SPACE", 0x2C}, {"F1", 0x3A}, {"F2", 0x3B}, {"F3", 0x3C}, {"F4", 0x3D},
    {"F5", 0x3E}, {"F6", 0x3F}, {"F7", 0x40}, {"F8", 0x41}, {"F9", 0x42}, {"F10", 0x43}, {"F11", 0x44}, {"F12", 0x45}, {"INS", 0x49},
    {"KPENTER", 0x58}, {"KP/", 0x54}, {"KP*", 0x55}, {"KP-", 0x56}, {"KP+", 0x57}, {"KP1", 0x59}, {"KP2", 0x5A}, {"KP3", 0x5B}, {"KP4", 0x5C},
    {"KP5", 0x5D}, {"KP6", 0x5E}, {"KP7", 0x5F}, {"KP8", 0x60}, {"KP9", 0x61}, {"KP0", 0x62}, {"KP.", 0x63},
    {"HOME", 0x4A}, {"PGUP", 0x4B}, {"DEL", 0x4C}, {"END", 0x4D}, {"PGDN", 0x4E}, {"RIGHT", 0x4F}, {"LEFT", 0x50}, {"DOWN", 0x51}, {"UP", 0x52},
};

static int usage_of_char(int c, uint8_t *mod)
{
    *mod = 0;
    if (c >= 'a' && c <= 'z')
        return 0x04 + c - 'a';
    if (c >= 'A' && c <= 'Z') {
        *mod = PLAT_MOD_LSHIFT;
        return 0x04 + c - 'A';
    }
    if (c >= '1' && c <= '9')
        return 0x1E + c - '1';
    if (c == '0')
        return 0x27;
    static const char *plain = " -=[]\\#;'`,./";
    static const uint8_t plain_usage[] = {0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};
    const char *p = strchr(plain, c);
    if (p && c)
        return plain_usage[p - plain];
    static const char *shifted = "!@#$%^&*()_+{}|:\"~<>?";
    static const uint8_t shifted_usage[] = {0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};
    p = strchr(shifted, c);
    if (p && c) {
        *mod = PLAT_MOD_LSHIFT;
        return shifted_usage[p - shifted];
    }
    return -1;
}

static int cur_caps, cur_num = 1;      /* the lock keys' state in the events that follow ({CAPSON}, {NUMOFF}, ...) */

static void add(int usage, int mod)
{
    CHECK(usage > 0 && script_len < 4096);
    script[script_len].usage = (uint8_t)usage;
    script[script_len].modifiers = (uint8_t)mod;
    script[script_len].caps_lock = (uint8_t)cur_caps;
    script[script_len].num_lock = (uint8_t)cur_num;
    script_len++;
}

static void parse_trace(const char *path)
{
    FILE *fp = __real_fopen(path, "r");
    char line[300];
    CHECK(fp != NULL);
    script_len = script_pos = 0;
    while (fgets(line, sizeof line, fp)) {
        unsigned usage, mods;
        int caps, num;
        if (sscanf(line, "key: usage 0x%x modifiers 0x%x caps %d num %d", &usage, &mods, &caps, &num) == 4) {
            cur_caps = caps;
            cur_num = num;
            add((int)usage, (int)mods);
        }
    }
    fclose(fp);
}

static void parse_keys(const char *spec)
{
    if (spec[0] == '@') {
        parse_trace(spec + 1);
        return;
    }
    script_len = script_pos = 0;
    cur_caps = 0;
    cur_num = 1;
    while (*spec) {
        uint8_t mod;
        if (*spec == '{') {
            const char *close = strchr(spec, '}');
            char name[16];
            CHECK(close && close - spec - 1 < 15);
            memcpy(name, spec + 1, (size_t)(close - spec - 1));
            name[close - spec - 1] = 0;
            spec = close + 1;
            int m = 0;
            char *n = name;
            while (n[1] == '-' && (n[0] == 'C' || n[0] == 'A' || n[0] == 'S')) {
                m |= n[0] == 'C' ? PLAT_MOD_LCTRL : n[0] == 'A' ? PLAT_MOD_LALT : PLAT_MOD_LSHIFT;
                n += 2;
            }
            if (!strcmp(n, "CAPSON") || !strcmp(n, "CAPSOFF") || !strcmp(n, "NUMON") || !strcmp(n, "NUMOFF")) {
                if (n[0] == 'C')
                    cur_caps = n[4] == 'O' && n[5] == 'N';
                else
                    cur_num = n[3] == 'O' && n[4] == 'N';
                continue;
            }
            int usage = -1;
            for (size_t i = 0; i < sizeof named / sizeof *named; i++)
                if (!strcmp(named[i].name, n))
                    usage = named[i].usage;
            if (usage < 0 && n[0] && !n[1]) {
                int u = usage_of_char((unsigned char)n[0], &mod);
                usage = u;
                m |= mod;
            }
            CHECK(usage > 0);
            add(usage, m);
        } else if (*spec == '^' && spec[1] >= 'A' && spec[1] <= 'Z') {
            add(0x04 + spec[1] - 'A', PLAT_MOD_LCTRL);
            spec += 2;
        } else if (*spec == '\n') {
            add(0x28, 0);
            spec++;
        } else {
            const int u = usage_of_char((unsigned char)*spec, &mod);
            CHECK(u > 0);
            add(u, mod);
            spec++;
        }
    }
}

/* ---- running it ---- */

static char screen_text[25][81];

/* A file in the RAM disk of the next run. */
static struct { char name[64]; char *data; size_t len; } files[8];
static int file_count;

static void put_file(const char *name, const char *data, size_t len)
{
    snprintf(files[file_count].name, sizeof files[file_count].name, "%s", name);
    files[file_count].data = malloc(len + 1);
    memcpy(files[file_count].data, data, len);
    files[file_count].len = len;
    file_count++;
}

/* The files as they are after the run: the child lists them to a file of the parent's choosing. */
static char dump_dir[64];

static void child(const char *keys, const char *cmdline)
{
    char *args[8];
    int n = 0;
    static char program[] = "/sd/tde/tde";
    static char buf[256];
    parse_keys(keys);
    fs_reset();
    if (fs_ram_init(&disk, pool, sizeof pool) != 0 || fs_mount("sd", &fs_ram_ops, &disk) != 0)
        _exit(4);
    fs_mkdir("/sd/tde");
    for (int i = 0; i < file_count; i++) {
        char path[128];
        snprintf(path, sizeof path, "/sd/%s", files[i].name);
        const int fd = fs_open(path, FS_WRONLY | FS_CREAT | FS_TRUNC);
        if (fd < 0 || fs_write(fd, files[i].data, files[i].len) != (int)files[i].len)
            _exit(5);
        fs_close(fd);
    }
    for (size_t i = 0; i < sizeof config_files / sizeof *config_files; i++) {   /* the editor's configuration, from the source tree */
        char host[128], path[128];
        snprintf(host, sizeof host, "tde/config/%s", config_files[i]);
        snprintf(path, sizeof path, "/sd/tde/%s", config_files[i]);
        FILE *fp = __real_fopen(host, "rb");   /* (fopen itself is the editor's, answered from tilefs) */
        if (!fp)
            _exit(7);
        static char data[100000];
        const size_t len = fread(data, 1, sizeof data, fp);
        fclose(fp);
        const int fd = fs_open(path, FS_WRONLY | FS_CREAT | FS_TRUNC);
        if (fd < 0 || fs_write(fd, data, len) != (int)len)
            _exit(8);
        fs_close(fd);
    }
    fs_chdir("/sd");
    if (tg_text_load_cp437(&screen) != 0)
        _exit(6);
    tg_text_init(&screen);
    tde_console_attach(&screen, nop);
    tde_console_set_idle(idle);
    tde_console_set_flush(0);   /* the script's keys are not type-ahead */
    setenv("TDEHOME", "/sd/tde", 1);   /* the editor's configuration is looked for here, not in $HOME */
    args[n++] = program;
    snprintf(buf, sizeof buf, "%s", cmdline ? cmdline : "");
    for (char *p = strtok(buf, " "); p && n < 7; p = strtok(NULL, " "))
        args[n++] = p;
    args[n] = NULL;
    tde_main(n, args);
    finish(10);   /* the editor ended by itself: this is its screen, and the exit code says so */
}

/* The files the child sent back. */
static unsigned char attr_rows[25 * 80];   /* the attribute of every cell of the screen */
static int cursor[5];   /* column, row, first and last scan line, visible */
static struct { char name[64]; int len; char *data; } got_files[8];
static int got_count;

static void parse_dump(const char *raw, size_t got)
{
    for (int i = 0; i < got_count; i++)
        free(got_files[i].data);
    got_count = 0;
    memcpy(attr_rows, raw + 25 * 81, got >= 25 * 81 + 25 * 80 ? 25 * 80 : 0);
    size_t pos = 25 * 81 + 25 * 80;
    const char *cl = memchr(raw + pos, '\n', got > pos ? got - pos : 0);
    if (cl) {
        if (sscanf(raw + pos, "cursor %d %d %d %d %d", &cursor[0], &cursor[1], &cursor[2], &cursor[3], &cursor[4]) != 5)
            cursor[0] = cursor[1] = -1;
        pos = (size_t)(cl - raw) + 1;
    }
    while (pos < got && got_count < 8) {
        const char *nl = memchr(raw + pos, '\n', got - pos);
        if (!nl)
            break;
        char name[64];
        int len;
        char head[100];
        snprintf(head, sizeof head, "%.*s", (int)(nl - raw - (long)pos), raw + pos);
        if (sscanf(head, "%63s %d", name, &len) != 2)
            break;
        pos = (size_t)(nl - raw) + 1;
        snprintf(got_files[got_count].name, sizeof got_files[got_count].name, "%s", name);
        got_files[got_count].len = len;
        got_files[got_count].data = NULL;
        if (len > 0 && pos + (size_t)len <= got) {
            got_files[got_count].data = malloc((size_t)len + 1);
            memcpy(got_files[got_count].data, raw + pos, (size_t)len);
            got_files[got_count].data[len] = 0;
            pos += (size_t)len;
        }
        got_count++;
    }
}

/* The content of a file after the run (NULL if there was none, with *len -1; a directory is -2). */
static const char *file_after(const char *name, int *len)
{
    for (int i = 0; i < got_count; i++)
        if (!strcmp(got_files[i].name, name)) {
            *len = got_files[i].len;
            return got_files[i].data ? got_files[i].data : "";
        }
    *len = -9;
    return NULL;
}

/* Runs the editor with these keys. Returns 1 if it used all the keys and was waiting for more, 0 if it ended on its own (or died). */
static int run(const char *keys, const char *cmdline)
{
    if (getenv("TDE_NOFORK")) {
        out_fd = 1;
        alarm(5);
        child(keys, cmdline);
        return 0;
    }
    int fds[2];
    CHECK(pipe(fds) == 0);
    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        out_fd = fds[1];
        signal(SIGALRM, SIG_DFL);
        alarm(20);
        child(keys, cmdline);
        _exit(0);
    }
    close(fds[1]);
    static char raw[25 * 81 + 400000];
    size_t got = 0;
    ssize_t r;
    while (got < sizeof raw - 1 && (r = read(fds[0], raw + got, sizeof raw - 1 - got)) > 0)
        got += (size_t)r;
    close(fds[0]);
    parse_dump(raw, got);
    int status;
    waitpid(pid, &status, 0);
    memset(screen_text, 0, sizeof screen_text);
    if (got >= 25 * 81 + 25 * 80) {
        for (int i = 0; i < 25; i++) {
            memcpy(screen_text[i], raw + i * 81, 80);
            int n = 80;
            while (n > 0 && screen_text[i][n - 1] == ' ')
                n--;
            screen_text[i][n] = 0;
        }
    }
    if (getenv("TDE_DEBUG"))
        printf("[child: got %zu bytes, %s %d]\n", got, WIFSIGNALED(status) ? "signal" : "exit", WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
    return got >= 25 * 81 + 25 * 80 && WIFEXITED(status) && WEXITSTATUS(status) == 0;   /* (10: it ended) */
}

static int screen_has(const char *text)
{
    for (int i = 0; i < 25; i++)
        if (strstr(screen_text[i], text))
            return 1;
    return 0;
}

static void show_screen(void)
{
    for (int i = 0; i < 25; i++)
        printf("|%s\n", screen_text[i]);
}

static void want(const char *name)
{
    snprintf(dump_names[dump_count++], sizeof dump_names[0], "%s", name);
}

static int file_is(const char *name, const char *want_text)
{
    int len;
    const char *d = file_after(name, &len);
    return d && len == (int)strlen(want_text) && !memcmp(d, want_text, (size_t)len);
}

/* A row of the screen without the spaces at the end (the editor's own rows are 0 for the title line, 1 for the ruler). */
#define ROW(r) (screen_text[r])
#define ROW_IS(r, text) (strcmp(ROW(r), text) == 0)
#define ROW_HAS(r, text) (strstr(ROW(r), text) != NULL)

static void fresh_files(void)
{
    for (int i = 0; i < file_count; i++)
        free(files[i].data);
    file_count = 0;
    dump_count = 0;
}

static void test_start_and_text(void)
{
    fresh_files();
    /* no file given: the credits and the question for a file name; an empty answer starts an empty file with no name */
    CHECK(run("", NULL));
    CHECK(ROW_HAS(4, "TDE, the Thomson-Davis Editor") && ROW_HAS(6, "Version 5.1v") && ROW_HAS(23, "File name, path or pattern :"));
    CHECK(run("\n", NULL) && ROW_HAS(3, "Directory : /sd/*") && ROW_HAS(8, "../") && ROW_HAS(8, "tde/"));   /* an empty answer lists the folder */
    /* a name that is not there is a new, empty file */
    CHECK(run("", "empty.txt"));
    CHECK(ROW_HAS(0, "1a ") && ROW_HAS(0, "empty.txt") && ROW_HAS(0, "lf   1") && ROW_HAS(1, "+....1....+....2....") && ROW_IS(2, "===============================   Top of File   ================================"));
    CHECK(ROW_HAS(4, "End of File") && ROW_HAS(24, "Tabs=SD8  Indent"));
    CHECK(cursor[4] == 1 && cursor[0] == 0 && cursor[1] == 3 && cursor[2] == 14 && cursor[3] == 15);   /* an underline at the start of the text */
    /* a new file, typed and saved with F2 */
    want("/sd/new.txt");
    CHECK(run("hello\nworld{F2}", "new.txt"));
    CHECK(file_is("/sd/new.txt", "hello\nworld\n"));
    CHECK(ROW_HAS(0, "new.txt") && ROW_IS(3, "hello") && ROW_IS(4, "world") && ROW_HAS(0, "   2") && ROW_HAS(0, "2:6"));
    CHECK(!ROW_HAS(0, "*new.txt"));                      /* saved: no star in front of the name */
    /* all the printing characters of the keyboard, shifted and not */
    want("/sd/chars.txt");
    CHECK(run("abc XYZ 0123456789 -=[]\\;'`,./ !@#$%^&*()_+{S-[}{S-]}|:\"~<>?{F2}", "chars.txt"));
    CHECK(file_is("/sd/chars.txt", "abc XYZ 0123456789 -=[]\\;'`,./ !@#$%^&*()_+{}|:\"~<>?\n"));
    /* caps lock (as the event says it) */
}

static void test_open_edit_save(void)
{
    fresh_files();
    put_file("a.txt", "one\ntwo\nthree\n", 14);
    want("/sd/a.txt");
    CHECK(run("", "a.txt"));
    CHECK(ROW_HAS(0, "a.txt") && ROW_HAS(0, "rw-") && ROW_HAS(0, "lf   3") && ROW_HAS(0, "1:1"));
    CHECK(ROW_IS(3, "one") && ROW_IS(4, "two") && ROW_IS(5, "three") && ROW_HAS(6, "End of File"));
    CHECK(cursor[0] == 0 && cursor[1] == 3);
    /* the cursor keys */
    CHECK(run("{DOWN}{DOWN}", "a.txt") && cursor[0] == 0 && cursor[1] == 5 && ROW_HAS(0, "3:1"));
    CHECK(run("{END}", "a.txt") && cursor[0] == 3 && cursor[1] == 3);
    CHECK(run("{DOWN}{DOWN}{END}{HOME}", "a.txt") && cursor[0] == 0 && cursor[1] == 5);
    CHECK(run("{DOWN}{RIGHT}{RIGHT}{LEFT}{UP}", "a.txt") && cursor[0] == 1 && cursor[1] == 3);
    CHECK(run("{DOWN}{DOWN}{DOWN}{DOWN}", "a.txt") && cursor[1] == 6 && ROW_HAS(0, "4:1"));   /* the cursor can go on the empty line after the text, and no further */
    /* an insertion in the middle, saved */
    CHECK(run("{DOWN}X{F2}", "a.txt"));
    CHECK(file_is("/sd/a.txt", "one\nXtwo\nthree\n") && ROW_IS(4, "Xtwo") && !ROW_HAS(0, "*a.txt"));
    /* delete and backspace */
    CHECK(run("{DOWN}{DEL}{F2}", "a.txt") && file_is("/sd/a.txt", "one\nwo\nthree\n"));
    CHECK(run("{DOWN}{END}{BS}{F2}", "a.txt") && file_is("/sd/a.txt", "one\ntw\nthree\n"));
    CHECK(run("{DOWN}{BS}{F2}", "a.txt") && file_is("/sd/a.txt", "onetwo\nthree\n"));      /* at the start of a line it joins the lines */
    /* overwrite mode with the Insert key */
    CHECK(run("{INS}ABC{F2}", "a.txt") && file_is("/sd/a.txt", "ABC\ntwo\nthree\n"));
    /* Enter keeps the indentation of the line above */
    put_file("ind.txt", "  ab\n", 5);
    want("/sd/ind.txt");
    CHECK(run("{END}\ncd{F2}", "ind.txt") && file_is("/sd/ind.txt", "  ab\n  cd\n"));
    /* the star shows a change that is not saved */
    CHECK(run("Z", "a.txt") && ROW_HAS(0, "*a.txt") && cursor[0] == 1);
}

static void test_prompts_and_quit(void)
{
    fresh_files();
    put_file("a.txt", "one\ntwo\nthree\n", 14);
    put_file("b.txt", "bee\n", 4);
    want("/sd/a.txt");
    want("/sd/c.txt");
    want("/sd/b.txt");
    /* a change and F3 (Quit): the question; N goes back, y abandons it (and the editor ends with nothing more to do) */
    CHECK(run("X{F3}", "a.txt") && ROW_HAS(23, "Abandon changes? (y/n):"));
    CHECK(run("X{F3}n", "a.txt") && ROW_HAS(0, "*a.txt") && !ROW_HAS(23, "Abandon"));
    CHECK(!run("X{F3}y", "a.txt"));                       /* the editor ended by itself */
    CHECK(!run("{F3}", "a.txt"));                         /* nothing changed: quits at once */
    /* Save As (Shift+F2): the name is asked for, and the file is then in both places */
    CHECK(run("{S-F2}", "a.txt") && ROW_HAS(23, "New file name: /sd/a.txt"));
    CHECK(run("{S-F2}c.txt\n", "a.txt") && ROW_HAS(0, "c.txt"));
    CHECK(file_is("/sd/c.txt", "one\ntwo\nthree\n") && file_is("/sd/a.txt", "one\ntwo\nthree\n"));
    /* Edit another file (Shift+F4) and switch between them (F10) */
    CHECK(run("{S-F4}b.txt\n", "a.txt") && ROW_HAS(0, "b.txt") && ROW_IS(3, "bee") && ROW_HAS(24, "F=2"));
    CHECK(run("{S-F4}b.txt\n{F3}", "a.txt") && ROW_HAS(0, " 1a  a.txt") && ROW_IS(3, "one") && ROW_HAS(24, "F=1"));   /* closing the second file goes back to the first */
    /* a file that is not there is a new, empty one */
    CHECK(run("", "nothere.txt") && ROW_HAS(0, "nothere.txt") && ROW_HAS(0, "lf   1"));
}

static void test_search_and_dirs(void)
{
    fresh_files();
    put_file("a.txt", "one\ntwo\nthree\n", 14);
    put_file("b.txt", "bee\n", 4);
    /* Shift+F5: find forward */
    CHECK(run("{S-F5}", "a.txt") && ROW_HAS(23, "String to find:"));
    CHECK(run("{S-F5}two\n", "a.txt") && cursor[0] == 0 && cursor[1] == 4 && ROW_HAS(0, "2:1"));
    CHECK(run("{S-F5}ee\n", "a.txt") && cursor[0] == 3 && cursor[1] == 5);                 /* "three": the 3rd character... after "ee" */
    /* F5 repeats it; Shift+F6 searches backwards */
    CHECK(run("{S-F5}e\n{F5}", "a.txt") && ROW_HAS(0, "3:"));
    CHECK(run("{DOWN}{DOWN}{S-F6}o\n", "a.txt") && cursor[1] == 4);
    /* the directory list (Alt+F1): "../", the folder of the editor's own files, the files, and picking one opens it */
    CHECK(run("{A-F1}\n", "a.txt") && ROW_HAS(3, "Directory : /sd/*") && ROW_HAS(8, "../") && ROW_HAS(8, "tde/") && ROW_HAS(8, "a.txt") && ROW_HAS(8, "b.txt"));
    CHECK(run("{A-F1}*.txt\n", "a.txt") && ROW_HAS(3, "/sd/*.txt") && ROW_HAS(8, "tde/") && ROW_HAS(8, "a.txt") && ROW_HAS(8, "b.txt"));   /* (folders are always listed) */
    CHECK(run("{A-F1}\n{RIGHT}{RIGHT}{RIGHT}\n", "a.txt") && ROW_HAS(0, "b.txt") && ROW_IS(3, "bee"));
    /* a pattern that is a path elsewhere */
    CHECK(run("{A-F1}/sd/tde/*.cfg\n", "a.txt") && ROW_HAS(3, "/sd/tde/*.cfg") && ROW_HAS(8, "tde.cfg") && !ROW_HAS(8, "tde.hlp"));
}

static void test_menus_and_help(void)
{
    fresh_files();
    put_file("a.txt", "one\ntwo\nthree\n", 14);
    CHECK(run("{F1}", "a.txt") && ROW_HAS(0, "HELP") && ROW_HAS(2, "Shift") && cursor[4] == 0);
    /* the help is the one in tde.hlp, which the editor reads next to tde.cfg (the built-in one says ^B ^L ^X, where the configuration has Alt: @B @L @X) */
    CHECK(run("{F1}", "a.txt") && ROW_HAS(17, "box") && ROW_HAS(17, "@B") && ROW_HAS(18, "line") && ROW_HAS(18, "@L") && ROW_HAS(19, "stream") && ROW_HAS(19, "@X"));
    CHECK(run("{F1}{ESC}", "a.txt") && ROW_HAS(0, "1a"));                                  /* Esc leaves the help */
    /* the menu (Ctrl+\) with its entries, and a letter picks one */
    CHECK(run("{C-\\}", "a.txt") && ROW_HAS(0, "File   Search   Block   Toggles   Other") && ROW_HAS(2, "a) Directory") && ROW_HAS(2, "Alt+F1"));
    CHECK(run("{C-\\}{ESC}", "a.txt") && ROW_HAS(0, "1a"));
    CHECK(run("{C-\\}a", "a.txt") && ROW_HAS(23, "Search path or pattern :"));      /* "a) Directory" asks what to list */
    /* Right and Left move between the menus */
    CHECK(run("{C-\\}{RIGHT}", "a.txt") && ROW_HAS(2, "a) "));
}

static void test_line_endings_and_big(void)
{
    fresh_files();
    /* CR LF files are known, shown and saved as they were */
    put_file("dos.txt", "ab\r\ncd\r\n", 8);
    want("/sd/dos.txt");
    CHECK(run("", "dos.txt") && ROW_HAS(0, "crlf") && ROW_IS(3, "ab") && ROW_IS(4, "cd"));
    CHECK(run("X{F2}", "dos.txt") && file_is("/sd/dos.txt", "Xab\r\ncd\r\n"));
    /* Ctrl+F3 toggles the line ending (a changed file is saved with the other one; one that is not changed is not saved at all) */
    CHECK(run("{C-F3}X{F2}", "dos.txt") && file_is("/sd/dos.txt", "Xab\ncd\n") && ROW_HAS(0, "lf"));
    CHECK(run("{C-F3}{F2}", "dos.txt") && file_is("/sd/dos.txt", "ab\r\ncd\r\n"));
    /* a file of many lines: Page Down shows the later ones, and the file survives a save of an unchanged file */
    char *big = malloc(30000);
    size_t n = 0;
    for (int i = 0; i < 1000; i++)
        n += (size_t)sprintf(big + n, "line number %d\n", i);
    put_file("big.txt", big, n);
    want("/sd/big.txt");
    CHECK(run("", "big.txt") && ROW_IS(3, "line number 0") && ROW_HAS(0, "1000"));
    CHECK(run("{PGDN}", "big.txt") && ROW_IS(3, "line number 21") && ROW_HAS(0, "22:1"));
    CHECK(run("{PGDN}{PGUP}", "big.txt") && ROW_IS(3, "line number 0"));
    CHECK(run("{C-END}", "big.txt") && ROW_IS(3 + 20, "line number 999") == 0);   /* (shown below) */
    CHECK(run("{F2}", "big.txt") && file_is("/sd/big.txt", big));                 /* saved as it was, byte for byte */
    CHECK(run("{DOWN}{DOWN}X{F2}", "big.txt"));
    int len;
    const char *d = file_after("/sd/big.txt", &len);
    CHECK(d && len == (int)n + 1 && !strncmp(d, "line number 0\nline number 1\nXline number 2\n", 41) && !memcmp(d + 42, big + 41, n - 41));
    free(big);
    /* a long line, 300 characters */
    char *line = malloc(400);
    memset(line, 'w', 300);
    line[300] = '\n';
    put_file("wide.txt", line, 301);
    want("/sd/wide.txt");
    CHECK(run("{END}", "wide.txt") && cursor[0] == 79 && ROW_HAS(0, "1:301"));
    CHECK(run("{END}!{F2}", "wide.txt"));
    d = file_after("/sd/wide.txt", &len);
    CHECK(d && len == 302 && d[300] == '!');
    free(line);
}

/* Things that stopped the editor: a stream block that begins at the end of its first line, copied (an assertion in the copy code; the fix is in block.c), and what
 * a prompt does with the cursor keys. The editor is built with its assertion checks on for the tests. */
static void test_crashes(void)
{
    fresh_files();
    put_file("a.txt", "one\ntwo\nthree\n", 14);
    want("/sd/a.txt");
    CHECK(run("{END}{A-x}{END}{A-l}{A-k}{A-c}{A-k}", "a.txt"));
    CHECK(run("{END}{A-x}{DOWN}{END}{A-x}{A-c}{F2}", "a.txt"));      /* a block from the end of a line: its first line adds nothing, the break and the next line are copied */
    CHECK(file_is("/sd/a.txt", "one\ntwo\ntwo\nthree\n"));
    CHECK(run("{A-c}{A-m}{A-u}{A-k}{A-g}", "a.txt"));               /* block commands with no block marked */
    /* the cursor keys in a prompt, from the first word: they once looked at the character before the start of the text (found by running the editor with the
     * compiler's checks on: `make test_tde_san`) */
    CHECK(run("{S-F5}ab cd{C-LEFT}X", "a.txt") && ROW_HAS(23, "String to find: ab Xcd"));
    CHECK(run("{S-F5}ab{C-LEFT}X", "a.txt") && ROW_HAS(23, "String to find: Xab"));
    CHECK(run("{S-F5}ab cd{C-LEFT}{C-LEFT}X", "a.txt") && ROW_HAS(23, "String to find: Xab cd"));
    CHECK(run("{S-F5}ab cd{C-BS}", "a.txt") && ROW_HAS(23, "String to find: ab"));
    CHECK(run("{S-F5}ab  cd{LEFT}{LEFT}{LEFT}{LEFT}{C-BS}Z", "a.txt") && ROW_HAS(23, "String to find: Z  cd"));
    CHECK(run("{S-F5}ab{HOME}X{HOME}Y", "a.txt") && ROW_HAS(23, "String to find: YXab"));          /* typing at the start inserts */
    /* three that the compiler's checks flagged: an overlapping copy in Ctrl+Backspace, and an empty line given to memchr and to fwrite */
    CHECK(run("{ENTER}{TAB}{C-BS}", "a.txt"));
    CHECK(run("{A-l}{ENTER}{A-e}", "a.txt"));
    /* empty lines are saved */
    CHECK(run("{ENTER}{ENTER}{F2}", "a.txt") && file_is("/sd/a.txt", "\n\none\ntwo\nthree\n"));
}

static void test_keys(void)
{
    fresh_files();
    put_file("a.txt", "one\ntwo\nthree\n", 14);
    put_file("big.txt", "", 0);
    char *big = malloc(30000);
    size_t n = 0;
    for (int i = 0; i < 1000; i++)
        n += (size_t)sprintf(big + n, "line number %d\n", i);
    fresh_files();
    put_file("a.txt", "one\ntwo\nthree\n", 14);
    put_file("big.txt", big, n);
    want("/sd/a.txt");
    /* Ctrl with the cursor keys: words (to the start of the next one, over the end of a line) */
    CHECK(run("{C-RIGHT}", "a.txt") && cursor[0] == 0 && cursor[1] == 4 && ROW_HAS(0, "2:1"));
    CHECK(run("{C-RIGHT}{C-RIGHT}{C-LEFT}", "a.txt") && cursor[1] == 4 && ROW_HAS(0, "2:1"));
    /* Ctrl+Backspace deletes the word before the cursor (here the line end, so it joins the lines) */
    CHECK(run("{C-RIGHT}{C-BS}{F2}", "a.txt") && file_is("/sd/a.txt", "onetwo\nthree\n"));
    /* Ctrl+End puts the last line at the bottom of the window (nothing to do for a file that fits), Ctrl+Home goes to the top, Ctrl+PgUp/PgDn to the top and bottom line of the screen */
    CHECK(run("{C-END}", "a.txt") && cursor[1] == 3 && ROW_HAS(0, "1:1"));
    CHECK(run("{C-END}", "big.txt") && ROW_HAS(0, "981:1") && ROW_IS(2, "line number 979") && ROW_IS(22, "line number 999") && ROW_HAS(23, "End of File"));
    CHECK(run("{C-END}{C-HOME}", "big.txt") && ROW_HAS(0, "   1:1") && ROW_IS(3, "line number 0"));
    CHECK(run("{PGDN}{PGDN}{C-PGUP}", "big.txt") && ROW_HAS(0, "42:1") && ROW_IS(2, "line number 41") && cursor[1] == 2);
    CHECK(run("{C-DOWN}{C-DOWN}", "big.txt") && ROW_IS(2, "line number 1") && ROW_IS(3, "line number 2") && ROW_HAS(0, "2:1"));   /* the text scrolls, the cursor stays on the screen */
    /* the keypad types its numbers with num lock on, and moves with it off; Enter on the keypad is Enter */
    CHECK(run("{KP8}{KP0}{KP.}{KP/}{KP*}{KP-}{KP+}{F2}", "a.txt") && file_is("/sd/a.txt", "80./*-+one\ntwo\nthree\n"));
    CHECK(run("{NUMOFF}{DOWN}{KP2}{KP6}", "a.txt") && cursor[0] == 1 && cursor[1] == 5 && ROW_HAS(0, "3:2"));
    CHECK(run("{NUMOFF}{KP3}", "big.txt") && ROW_HAS(0, "22:1"));                                    /* the keypad 3 is Page Down with num lock off */
    CHECK(run("{NUMOFF}{KP3}{KP9}", "big.txt") && ROW_HAS(0, "1:1"));                                /* and 9 Page Up */
    CHECK(run("{NUMOFF}{KP+}", "big.txt") && ROW_IS(2, "line number 0") && cursor[1] == 2 && ROW_HAS(0, "1:1"));   /* keypad + is "scroll down a line" with num lock off ... */
    CHECK(run("{NUMOFF}{S-KP+}{F2}", "a.txt") && file_is("/sd/a.txt", "+one\ntwo\nthree\n"));              /* ... and a plus with Shift */
    CHECK(run("{NUMON}{KP+}{F2}", "a.txt") && file_is("/sd/a.txt", "+one\ntwo\nthree\n"));                  /* with it on, the other way round */
    CHECK(run("a{KPENTER}b{F2}", "a.txt") && file_is("/sd/a.txt", "a\nbone\ntwo\nthree\n"));
    /* the caps lock, as the keyboard says it, and shift against it */
    CHECK(run("{CAPSON}abC{F2}", "a.txt") && file_is("/sd/a.txt", "ABcone\ntwo\nthree\n"));
    CHECK(run("{CAPSON}ab{S-C}{F2}", "a.txt") && file_is("/sd/a.txt", "ABcone\ntwo\nthree\n"));
    CHECK(run("{CAPSON}1{S-1}{F2}", "a.txt") && file_is("/sd/a.txt", "1!one\ntwo\nthree\n"));    /* caps lock is for letters only */
    /* Tab inserts spaces up to the next tab stop (every 8), Shift+Tab does nothing at the start of a line */
    CHECK(run("a{TAB}b{F2}", "a.txt") && file_is("/sd/a.txt", "a       bone\ntwo\nthree\n"));
    CHECK(run("{S-TAB}{F2}", "a.txt") && ROW_HAS(0, "1:1"));
    /* Alt+letter: Alt+Q is the grep dialog, and Escape closes it */
    CHECK(run("{A-Q}", "a.txt") && ROW_HAS(8, "Pattern:") && ROW_HAS(10, "Files:"));
    CHECK(run("{A-Q}{ESC}", "a.txt") && ROW_IS(3, "one") && !ROW_HAS(8, "Pattern:"));
    /* F11 and F12 are keys of their own (Repeat Diff and Repeat Grep give a message, and the key after it goes on working) */
    CHECK(run("{F11}{F12}x{F2}", "a.txt") && file_is("/sd/a.txt", "xone\ntwo\nthree\n"));
    free(big);
}

/* The key numbers from an independent table: the PC (set 1) scan code of the key in the place of each USB usage code. TDE's numbers are 256 + scan code - 1 for most
 * keys, with its own choices for some (see define.h): the grey keypad keys are numbers of their own. */
static int pc_scan_of(int usage)
{
    static const char letters[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
    if (usage >= 0x04 && usage <= 0x1D)
        return letters[usage - 4];
    if (usage >= 0x1E && usage <= 0x27)
        return 2 + usage - 0x1E;                      /* 1 2 3 ... 9 0 */
    switch (usage) {
    case 0x28: return 0x1C;
    case 0x29: return 0x01;
    case 0x2A: return 0x0E;
    case 0x2B: return 0x0F;
    case 0x2C: return 0x39;
    case 0x2D: return 0x0C;
    case 0x2E: return 0x0D;
    case 0x2F: return 0x1A;
    case 0x30: return 0x1B;
    case 0x31: case 0x32: return 0x2B;
    case 0x33: return 0x27;
    case 0x34: return 0x28;
    case 0x35: return 0x29;
    case 0x36: return 0x33;
    case 0x37: return 0x34;
    case 0x38: return 0x35;
    case 0x39: return 0x3A;
    case 0x57: return -1;
    default: break;
    }
    if (usage >= 0x3A && usage <= 0x43)
        return 0x3B + usage - 0x3A;                   /* F1 .. F10 */
    if (usage == 0x44) return 0x57;
    if (usage == 0x45) return 0x58;
    switch (usage) {
    case 0x49: case 0x62: return 0x52;                /* Insert */
    case 0x4A: case 0x5F: return 0x47;                /* Home */
    case 0x4B: case 0x61: return 0x49;                /* Page Up */
    case 0x4C: case 0x63: return 0x53;                /* Delete */
    case 0x4D: case 0x59: return 0x4F;                /* End */
    case 0x4E: case 0x5B: return 0x51;                /* Page Down */
    case 0x4F: case 0x5E: return 0x4D;                /* Right */
    case 0x50: case 0x5C: return 0x4B;                /* Left */
    case 0x51: case 0x5A: return 0x50;                /* Down */
    case 0x52: case 0x60: return 0x48;                /* Up */
    case 0x5D: return 0x4C;                           /* the keypad 5 */
    case 0x64: return 0x56;                           /* the key left of Z on a 102-key keyboard */
    default: return 0;
    }
}

/* ---- the C library functions the editor calls, answered from tilefs ---- */

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <utime.h>

static int rewind_ok(FILE *f)
{
    rewind(f);
    return ftell(f) == 0;
}

static void test_libc(void)
{
    char buf[128];
    struct stat st;
    FILE *f;
    fs_reset();
    CHECK(fs_ram_init(&disk, pool, sizeof pool) == 0 && fs_mount("sd", &fs_ram_ops, &disk) == 0);
    CHECK(chdir("/sd") == 0 && !strcmp(getcwd(buf, sizeof buf), "/sd"));
    CHECK(getcwd(buf, 3) == NULL && errno == ERANGE);
    char *dyn = getcwd(NULL, 0);
    CHECK(dyn && !strcmp(dyn, "/sd"));
    free(dyn);
    /* fopen and the modes */
    CHECK(fopen("none.txt", "r") == NULL && errno == ENOENT);
    f = fopen("a.txt", "w");
    CHECK(f && fputs("hello\nworld\n", f) >= 0 && fprintf(f, "n=%d\n", 42) > 0 && fclose(f) == 0);
    CHECK(stat("a.txt", &st) == 0 && S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && st.st_size == 17);
    f = fopen("a.txt", "r");
    CHECK(f && fgets(buf, sizeof buf, f) && !strcmp(buf, "hello\n") && fgetc(f) == 'w' && ftell(f) == 7);
    CHECK(fseek(f, 0, SEEK_SET) == 0 && fgetc(f) == 'h' && fseek(f, -3, SEEK_END) == 0 && fgetc(f) == '4' && fgetc(f) == '2');
    CHECK(fgetc(f) == '\n' && fgetc(f) == EOF && feof(f));
    CHECK(fseek(f, 100, SEEK_SET) != 0);                      /* tilefs has no holes */
    CHECK(fclose(f) == 0);
    f = fopen("a.txt", "a");
    CHECK(f && fputs("more\n", f) >= 0 && fclose(f) == 0 && stat("a.txt", &st) == 0 && st.st_size == 22);
    f = fopen("a.txt", "r+");
    CHECK(f && fseek(f, 0, SEEK_END) == 0 && fputc('!', f) == '!' && rewind_ok(f) && fclose(f) == 0);
    f = fopen("a.txt", "w");                                   /* "w" empties it */
    CHECK(f && fclose(f) == 0 && stat("a.txt", &st) == 0 && st.st_size == 0);
    f = fopen("a.txt", "w+");
    CHECK(f && fputs("xyz", f) >= 0 && fseek(f, 0, SEEK_SET) == 0 && fgetc(f) == 'x' && fclose(f) == 0);
    CHECK(fopen("a.txt", "q") == NULL && errno == EINVAL);
    CHECK(fopen("bad:name", "w") == NULL && errno == EINVAL);
    CHECK(fopen("nodir/x", "w") == NULL && errno == ENOENT);
    /* stat, access, chmod, utime of what is and is not there */
    CHECK(stat("/sd", &st) == 0 && S_ISDIR(st.st_mode) && stat("/", &st) == 0 && S_ISDIR(st.st_mode) && stat("nothing", &st) == -1 && errno == ENOENT);
    CHECK(access("a.txt", 4) == 0 && access("nothing", 4) == -1 && errno == ENOENT);
    CHECK(chmod("a.txt", 0600) == 0 && chmod("nothing", 0600) == -1);
    struct utimbuf ut = {1, 2};
    CHECK(utime("a.txt", &ut) == 0 && utime("nothing", &ut) == -1);
    /* directories, rename, remove */
    CHECK(mkdir("d", 0777) == 0 && mkdir("d", 0777) == -1 && errno == EEXIST && stat("d", &st) == 0 && S_ISDIR(st.st_mode));
    f = fopen("d/in.txt", "w");
    CHECK(f && fclose(f) == 0);
    CHECK(rmdir("d") == -1 && errno == ENOTEMPTY);
    CHECK(rename("a.txt", "b.txt") == 0 && stat("a.txt", &st) == -1 && stat("b.txt", &st) == 0 && st.st_size == 3);
    f = fopen("c.txt", "w");
    CHECK(f && fputs("CCCCCCCC", f) >= 0 && fclose(f) == 0);
    CHECK(rename("c.txt", "b.txt") == 0 && stat("c.txt", &st) == -1 && stat("b.txt", &st) == 0 && st.st_size == 8);   /* C's rename replaces what is there */
    CHECK(rename("nothing", "x") == -1 && errno == ENOENT);
    CHECK(unlink("b.txt") == 0 && unlink("b.txt") == -1 && errno == ENOENT);
    CHECK(remove("d/in.txt") == 0 && remove("d") == 0 && remove("d") == -1 && stat("d", &st) == -1);
    /* chdir, relative names, realpath */
    CHECK(mkdir("sub", 0777) == 0 && chdir("sub") == 0 && !strcmp(getcwd(buf, sizeof buf), "/sd/sub"));
    f = fopen("here.txt", "w");
    CHECK(f && fclose(f) == 0 && stat("/sd/sub/here.txt", &st) == 0);
    CHECK(chdir("nothing") == -1 && chdir("here.txt") == -1 && !strcmp(getcwd(buf, sizeof buf), "/sd/sub"));
    CHECK(realpath("../sub/./here.txt", buf) && !strcmp(buf, "/sd/sub/here.txt"));
    CHECK(realpath("nothing", buf) == NULL && errno == ENOENT);
    char *rp = realpath("..", NULL);
    CHECK(rp && !strcmp(rp, "/sd"));
    free(rp);
    /* a directory read the way a Unix one is: "." and ".." and then the files */
    DIR *dir = opendir("/sd");
    CHECK(dir != NULL);
    int dots = 0, subs = 0, others = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            dots += (de->d_type == DT_DIR);
        else if (!strcmp(de->d_name, "sub"))
            subs += (de->d_type == DT_DIR);
        else
            others++;
    }
    CHECK(closedir(dir) == 0 && dots == 2 && subs == 1 && others == 0);
    CHECK(opendir("/sd/nothing") == NULL && errno == ENOENT && opendir("/sd/sub/here.txt") == NULL && errno == ENOTDIR);
    chdir("/");
    fs_reset();
}

/* ---- the console functions on their own: tests/test_tde_console.c (it includes the editor's headers, whose names clash with this file's) ---- */

int tde_test_console(void);

/* TDE's numbers (define.h) for the keys the table test names: 256 + the PC scan code - 1, and the grey keypad keys' own numbers. */
enum { K_1 = 257, K_ENTER = 283, K_A = 285, K_SPACE = 312, K_LBRACKET = 281, K_RIGHT = 332, K_PRTSC = 339, K_GREY_SLASH = 325, K_GREY_STAR = 310,
       K_GREY_MINUS = 329, K_GREY_PLUS = 333, K_GREY_ENTER = 324, K_SHIFT = 512, K_CTRL = 1024, K_ALT = 2048, K_FUNCTION = 4096 };

static void test_key_table(void)
{
    int shift, ch;
    plat_key_event_t e = {0, 0, 0, 1};
    for (int usage = 0; usage < 0x100; usage++) {
        e.usage = (uint8_t)usage;
        const int key = tde_event_key(&e, &shift, &ch);
        const int scan = pc_scan_of(usage);
        if (scan > 0) {
            CHECK(key == 256 + scan - 1);
        } else if (usage >= 0x54 && usage <= 0x58) {   /* the grey keys of the keypad have numbers of their own in TDE: */
            static const int grey[5] = {K_GREY_SLASH, K_GREY_STAR, K_GREY_MINUS, K_GREY_PLUS, K_GREY_ENTER};
            CHECK(key == grey[usage - 0x54]);
        } else if (usage == 0x46) {
            CHECK(key == K_PRTSC);
        } else {
            CHECK(key == 0);                           /* a key the editor has no use for */
        }
    }
    /* the shift state, for each modifier bit and the combinations; the cursor block keys are extended, the keypad ones not */
    e.usage = 0x4F;
    CHECK(tde_event_key(&e, &shift, &ch) == K_RIGHT && shift == K_FUNCTION && ch == 0);
    e.usage = 0x5E;                                   /* keypad 6 */
    CHECK(tde_event_key(&e, &shift, &ch) == K_RIGHT && shift == 0 && ch == '6');
    e.num_lock = 0;
    CHECK(tde_event_key(&e, &shift, &ch) == K_RIGHT && shift == 0 && ch == 0);
    e.num_lock = 1;
    e.usage = 0x04;
    static const struct { int mods, state; } bits[] = {
        {PLAT_MOD_LCTRL, K_CTRL}, {PLAT_MOD_RCTRL, K_CTRL}, {PLAT_MOD_LSHIFT, K_SHIFT}, {PLAT_MOD_RSHIFT, K_SHIFT}, {PLAT_MOD_LALT, K_ALT}, {PLAT_MOD_RALT, K_ALT},
        {PLAT_MOD_LGUI, 0}, {PLAT_MOD_RGUI, 0}, {PLAT_MOD_LCTRL | PLAT_MOD_LALT, K_CTRL | K_ALT}, {PLAT_MOD_RSHIFT | PLAT_MOD_RCTRL | PLAT_MOD_RALT, K_SHIFT | K_CTRL | K_ALT},
    };
    for (size_t i = 0; i < sizeof bits / sizeof *bits; i++) {
        e.modifiers = (uint8_t)bits[i].mods;
        CHECK(tde_event_key(&e, &shift, &ch) == K_A && shift == bits[i].state);
    }
    /* the character: shift and caps lock apply; Ctrl and Alt do not change it (the editor makes keys of them) */
    e.modifiers = 0;
    CHECK(tde_event_key(&e, &shift, &ch) == K_A && ch == 'a');
    e.modifiers = PLAT_MOD_LSHIFT;
    CHECK(tde_event_key(&e, &shift, &ch) == K_A && ch == 'A');
    e.modifiers = 0;
    e.caps_lock = 1;
    CHECK(tde_event_key(&e, &shift, &ch) == K_A && ch == 'A');
    e.modifiers = PLAT_MOD_RSHIFT;
    CHECK(tde_event_key(&e, &shift, &ch) == K_A && ch == 'a');
    e.caps_lock = 0;
    e.modifiers = PLAT_MOD_LCTRL;
    CHECK(tde_event_key(&e, &shift, &ch) == K_A && ch == 'a' && shift == K_CTRL);   /* (the editor makes a key of Ctrl+A and does not use the character) */
    e.usage = 0x28;
    e.modifiers = 0;
    CHECK(tde_event_key(&e, &shift, &ch) == K_ENTER && ch == 0);   /* Enter is 10 */
    e.usage = 0x2C;
    CHECK(tde_event_key(&e, &shift, &ch) == K_SPACE && ch == ' ');
    e.usage = 0x1E;                                   /* 1 and the shifted symbol; caps lock does not touch it */
    e.modifiers = PLAT_MOD_LSHIFT;
    e.caps_lock = 1;
    CHECK(tde_event_key(&e, &shift, &ch) == K_1 && ch == '!');
    e.usage = 0x2F;
    e.modifiers = 0;
    e.caps_lock = 0;
    CHECK(tde_event_key(&e, &shift, &ch) == K_LBRACKET && ch == '[');
    e.modifiers = PLAT_MOD_LSHIFT;
    CHECK(tde_event_key(&e, &shift, &ch) == K_LBRACKET && ch == '{');
}

/* ./test_tde peek 'KEYS' [FILE...]: shows the screen after these keys, with a.txt (one, two, three) and b.txt (bee) in the folder: for finding out what the editor does. */
static void peek_main(int argc, char **argv)
{
    if (getenv("TDE_LONG")) {   /* a.txt of 20 lines of words, for replaying a trace of a real session */
        static char words[2000];
        size_t n = 0;
        for (int i = 0; i < 20; i++)
            n += (size_t)sprintf(words + n, "the quick brown fox %d jumps over the lazy dog\n", i);
        put_file("a.txt", words, n);
    } else {
        put_file("a.txt", "one\ntwo\nthree\n", 14);
    }
    put_file("b.txt", "bee\n", 4);
    put_file("dos.txt", "ab\r\ncd\r\n", 8);
    {
        static char big[30000];
        size_t n = 0;
        for (int i = 0; i < 1000; i++)
            n += (size_t)sprintf(big + n, "line number %d\n", i);
        put_file("big.txt", big, n);
    }
    want("/sd/a.txt");
    want("/sd/dos.txt");
    const int r = run(argc > 2 ? argv[2] : "", argc > 3 ? argv[3] : NULL);
    printf("run=%d cursor %d,%d visible %d\n", r, cursor[0], cursor[1], cursor[4]);
    show_screen();
    if (getenv("TDE_ATTRS")) {   /* the attribute of each cell in hex, rows that are not all the same */
        for (int r = 0; r < 25; r++) {
            int same = 1;
            for (int c = 1; c < 80; c++)
                same &= attr_rows[r * 80 + c] == attr_rows[r * 80];
            if (same)
                printf("@%02d: all %02x\n", r, attr_rows[r * 80]);
            else {
                printf("@%02d:", r);
                for (int c = 0; c < 80; c++)
                    printf(" %02x", attr_rows[r * 80 + c]);
                printf("\n");
            }
        }
    }
    int len;
    const char *d = file_after("/sd/a.txt", &len);
    printf("a.txt (%d): %s\n", len, d ? d : "");
    d = file_after("/sd/dos.txt", &len);
    printf("dos.txt (%d):", len);
    for (int i = 0; d && i < len; i++)
        printf(" %02x", (unsigned char)d[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "peek")) {
        peek_main(argc, argv);
        return 0;
    }
    test_libc();
    {   /* in a process of its own: it changes the editor's globals, which the runs below start from */
        int fds[2];
        CHECK(pipe(fds) == 0);
        const pid_t pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            const int n = tde_test_console();
            if (write(fds[1], &n, sizeof n) != sizeof n)
                _exit(3);
            _exit(0);
        }
        int n = 0, status;
        CHECK(read(fds[0], &n, sizeof n) == sizeof n);
        waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        close(fds[0]);
        close(fds[1]);
        checks += n;
    }
    test_key_table();
    test_start_and_text();
    test_open_edit_save();
    test_prompts_and_quit();
    test_search_and_dirs();
    test_menus_and_help();
    test_line_endings_and_big();
    test_keys();
    test_crashes();
    printf("all %d checks passed\n", checks);
    return 0;
}
