/* SPDX-License-Identifier: MIT */
/*
 * Random editing sessions, run twice and compared: THEIRS is the unmodified editor.c of PicoParanoia on its own TNTSCAnsi terminal emulator, OURS is the
 * editor of tilegame on the console shim. Both are given the same keys, and for every session one line is printed: a hash of the whole screen and the
 * cursor, the number of bytes the editor saved and a hash of them, and whether the editor finished or was waiting for a key when the keys ran out. The
 * two outputs must be the same, line for line (see `make test_editor_oracle`).
 */
#include <setjmp.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"   /* (only for the PLAT_KEY_* numbers) */

#ifdef THEIRS
#include "editor.c"     /* PicoParanoia's, from its own directory: its statics can be reset from here */
void tnt_begin(void);
void tnt_put(unsigned char c);
int tnt_cell(int row, int col);
int tnt_row(void);
int tnt_col(void);
#else
#include "consoleio.h"
#include "editor.h"
#endif

const char *__asan_default_options(void) { return "detect_leaks=0"; }

static int cell_char(int row, int col);
static int cursor_row(void);
static int cursor_col(void);
static int trace_steps;   /* STEPS=1: before each key, a line with a hash of the screen, to find the first key after which two builds differ */
static void trace_step(int step, int key)
{
    uint32_t h = 2166136261u;
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 80; c++)
            h = (h ^ (unsigned)cell_char(r, c)) * 16777619u;
    h = (h ^ (unsigned)cursor_row()) * 16777619u;
    h = (h ^ (unsigned)cursor_col()) * 16777619u;
    fprintf(stderr, "step %d (key %d) before: %08x cursor %d,%d\n", step, key, h, cursor_row(), cursor_col());
    if (getenv("STEPS") && atoi(getenv("STEPS")) == 2) {   /* STEPS=2: and the whole screen */
        for (int r = 0; r < 25; r++) {
            char line[81];
            for (int c = 0; c < 80; c++) { const int ch = cell_char(r, c); line[c] = ch >= 32 && ch < 127 ? (char)ch : '?'; }
            line[80] = 0;
            for (int c = 79; c >= 0 && line[c] == ' '; c--) line[c] = 0;
            fprintf(stderr, "      %2d|%s\n", r, line);
        }
    }
}

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
static int between(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

/* ---- the session: keys, and the file in and out ---- */

#define MAX_KEYS 600
static int keys[MAX_KEYS], nkeys, kpos;   /* plat_getchar style keys */
static jmp_buf cut;
static int cut_armed;

static const char *in_text;
static size_t in_pos;
static unsigned char out_bytes[1 << 16];
static size_t out_len;
static int writes, closes;

static int reader(void *v)
{
    (void)v;
    return in_text[in_pos] ? (unsigned char)in_text[in_pos++] : 0;
}

static int writer(int c, void *v)
{
    (void)v;
    if (c == -1)
        closes++;
    else if (c == 0)
        writes++;
    else if (out_len < sizeof out_bytes)
        out_bytes[out_len++] = (unsigned char)c;
    return 0;
}

#ifdef THEIRS
/* ---- THEIRS: the console API over TNTSCAnsi, the way consoleio.cpp has it; keys arrive as the bytes a keyboard driver of theirs would produce ---- */

static char sent[20000];   /* what the editor has sent to the terminal since the last key (STEPS=3) */
static int sent_n;
static unsigned char bytes[MAX_KEYS * 4];
static int nbytes, bpos;
static int starts[MAX_KEYS * 4], start_key[MAX_KEYS * 4];   /* does byte i begin a key, and which */

static int expand(int key, unsigned char *out)   /* what the keys are as bytes (the table of the shim, written out again here) */
{
    switch (key) {
    case PLAT_KEY_UP: memcpy(out, "\033[A", 3); return 3;
    case PLAT_KEY_DOWN: memcpy(out, "\033[B", 3); return 3;
    case PLAT_KEY_RIGHT: memcpy(out, "\033[C", 3); return 3;
    case PLAT_KEY_LEFT: memcpy(out, "\033[D", 3); return 3;
    case PLAT_KEY_HOME: memcpy(out, "\033Op", 3); return 3;
    case PLAT_KEY_END: memcpy(out, "\033Or", 3); return 3;
    case 127: memcpy(out, "\033Ol", 3); return 3;
    case PLAT_KEY_PAGEDOWN: out[0] = 22; return 1;
    case PLAT_KEY_PAGEUP: out[0] = 16; out[1] = 22; return 2;
    case PLAT_KEY_INSERT: out[0] = 14; return 1;
    case PLAT_KEY_F(1): out[0] = 28; return 1;
    case PLAT_KEY_F(2): out[0] = 16; out[1] = 10; return 2;
    case 10: out[0] = '\r'; return 1;
    default: out[0] = (unsigned char)key; return 1;
    }
}

int console_getch(void)
{
    if (bpos >= nbytes) {
        if (cut_armed)
            longjmp(cut, 1);
        return -1;
    }
    if (trace_steps && starts[bpos]) { static int step; if (getenv("STEPS") && atoi(getenv("STEPS")) == 3) { sent[sent_n] = 0; fprintf(stderr, "BYTES %s\n", sent); sent_n = 0; } trace_step(step++, start_key[bpos]); }
    return bytes[bpos++];
}

static unsigned char tail[200];
static int tail_n;
static void trace_byte(unsigned char c)   /* TRACE=1: show the escape sequences that have a minus sign in them; TRACE=2 the last bytes sent at the end */
{
    tail[tail_n++ % 200] = c;
    { static FILE *raw; if (!raw && getenv("BYTESFILE")) raw = fopen(getenv("BYTESFILE"), "wb"); if (raw) { fputc(c, raw); fflush(raw); } }   /* BYTESFILE=name: all that is sent to the terminal, as it is */
    if (sent_n < (int)sizeof sent - 8) { if (c == 27) { sent[sent_n++] = '^'; sent[sent_n++] = '['; } else if (c < 32) { sent[sent_n++] = '^'; sent[sent_n++] = (char)(c + 64); } else sent[sent_n++] = (char)c; }
    static char seq[40];
    static int n;
    if (!getenv("TRACE")) return;
    if (c == 27) n = 0;
    if (n < (int)sizeof seq - 1) seq[n++] = (char)c;
    if (n > 1 && c >= 0x40 && c <= 0x7E && seq[1] == '[') {
        seq[n] = 0;
        if (strchr(seq, '-')) fprintf(stderr, "bad sequence: ESC%s\n", seq + 1);
        n = 0;
    }
}
/* TNTSCAnsi draws a bell as a glyph and moves a backspace at the left edge to the end of the line above: those two, which a terminal does not do (ours and
 * the others do not), are left out here, so that a raw bell or backspace in the text being edited does not make the two screens differ for that reason. */
static void send(unsigned char c)
{
    trace_byte(c);
    if (c == 7 || (c == 8 && tnt_col() == 0))
        return;
    tnt_put(c);
}
void console_putch(char c) { send((unsigned char)c); }
void console_puts(const char *c) { while (*c) send((unsigned char)*c++); }
void console_clreol(void) { console_puts("\033[K"); }
void console_clrscr(void) { console_puts("\033[H\033[2J"); }
void console_highvideo(void) { console_puts("\033[1m"); }
void console_lowvideo(void) { console_puts("\033[0m"); }
void console_gotoxy(int x, int y) { char s[32]; if (x < 1) x = 1; if (y < 1) y = 1; snprintf(s, sizeof s, "\033[%d;%dH", y, x); console_puts(s); }   /* (as the shim of tilegame: PicoParanoia's prints a minus sign, which its terminal does not understand) */
void console_delayframes(unsigned short fr) { (void)fr; }

char *myltoa(char *p, long n) { snprintf(p, MY_ITOASIZE, "%ld", n); return p; }

long mystrtol(const char *str, const char **end)
{
    unsigned long n = 0;
    int neg;
    while (*str == ' ') str++;
    if ((neg = (*str == '-'))) str++;
    if (*str >= '0' && *str <= '9')
        for (;;) {
            if (*str >= '0' && *str <= '9') n = (n * 10) + (unsigned long)(*str++ - '0');
            else break;
        }
    if (end) *end = str;
    return neg ? -((long)n) : (long)n;
}

static void begin_session(void)
{
    tnt_begin();
    nbytes = bpos = 0;
    memset(starts, 0, sizeof starts);
    for (int i = 0; i < nkeys; i++) {
        if (keys[i] < 0)
            continue;   /* (a moment with no key: nothing to feed a byte stream) */
        starts[nbytes] = 1;
        start_key[nbytes] = keys[i];
        nbytes += expand(keys[i], bytes + nbytes);
    }
    memset(flag, 0, sizeof flag);   /* the editor of PicoParanoia keeps its flags and its margin between runs: not wanted here */
    sww = 78;
    tabsize = 8;
    blen = 0;   /* (it keeps the length of the last block, though the block buffer is new each time: a paste would use what is in it) */
}

static int cell_char(int row, int col) { return tnt_cell(row, col) & 0x7F; }
static int cursor_row(void) { return tnt_row(); }
static int cursor_col(void) { return tnt_col(); }
static void run_editor(void) { editor(); }
#else
/* ---- OURS ---- */

static int idle;

static int get_key(void)
{
    while (kpos < nkeys && keys[kpos] < 0)
        kpos++;
    if (kpos >= nkeys)
        return -1;
    idle = 0;
    if (trace_steps) { static int step; trace_step(step++, keys[kpos]); }
    return keys[kpos++];
}

static void frame(void)
{
    if (++idle > 1000 && kpos >= nkeys && cut_armed)
        longjmp(cut, 1);
}

static void begin_session(void)
{
    const console_hooks_t h = {get_key, frame};
    console_set_hooks(&h);
    console_init();
    kpos = 0;
    idle = 0;
}

static int cell_char(int row, int col) { return console_screen()->cells[row * 80 + col] & 0x7F; }
static int cursor_row(void) { return console_console()->row; }
static int cursor_col(void) { return console_console()->col; }
static void run_editor(void) { editor(); }
#endif

/* ---- the scenarios ---- */

static void make_file(char *file, size_t size)
{
    static const char *const words[] = {"the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog", "alpha", "beta", "gamma", "x", "ab", "end", "foo", "bar"};
    size_t n = 0;
    const int lines = between(0, 40);
    for (int l = 0; l < lines; l++) {
        const int len = rnd() % 8 == 0 ? between(60, 130) : between(0, 40);   /* (some lines are longer than the window) */
        int col = 0;
        while (col < len && n + 200 < size) {
            const char *w = words[rnd() % (sizeof words / sizeof *words)];
            if (rnd() % 25 == 0) { file[n++] = '\t'; col += 4; continue; }
            while (*w && n + 200 < size) { file[n++] = *w++; col++; }
            file[n++] = ' ';
            col++;
        }
        while (n > 0 && file[n - 1] == ' ' && rnd() % 2) n--;
        file[n++] = '\n';
    }
    file[n] = 0;
}

static void make_keys(void)
{
    static const int specials[] = {PLAT_KEY_UP, PLAT_KEY_DOWN, PLAT_KEY_LEFT, PLAT_KEY_RIGHT, PLAT_KEY_HOME, PLAT_KEY_END, 127,
                                   PLAT_KEY_PAGEUP, PLAT_KEY_PAGEDOWN, PLAT_KEY_INSERT, PLAT_KEY_UP, PLAT_KEY_DOWN, PLAT_KEY_LEFT, PLAT_KEY_RIGHT};
    /* The editor's commands after ^P or ^Q (the "meta" prefix) are chosen by key | 0x60. Two of them are kept away from: j (it records a macro, and a
     * recording of more than 59 keys overruns PicoParanoia's buffer) and b (the right margin: any number is taken, and 0 breaks its screen). And m n o p
     * put the next key into the text as it is: it is made a printable one, because a raw control character (a bell, a backspace at the left edge) is
     * drawn differently by the two terminal emulators, which is not what is being compared. */
    enum { NORMAL, META, LITERAL } state = NORMAL;
    nkeys = 0;
    const int n = between(10, 150);
    for (int i = 0; i < n && nkeys < MAX_KEYS - 4; i++) {
        int key;
        const int kind = (int)(rnd() % 100);
        if (kind < 45) key = between(' ', '~');
        else if (kind < 52) key = 10;                                     /* Enter */
        else if (kind < 56) key = 8;                                      /* Backspace */
        else if (kind < 76) key = specials[rnd() % (sizeof specials / sizeof *specials)];
        else if (kind < 96) {                                             /* a control key: ^A..^Z, but ^W (save and leave) is rarer */
            key = between(1, 26);
            if (key == 3) key = 'c';                                      /* ^C plays a macro, and a macro with ^C in it plays itself until the stack is gone (PicoParanoia's does) */
            if (key == 23 && rnd() % 3) key = 'y';
            if (key == 10 || key == 13) key = 'n';
        } else key = 28;                                                  /* help */
        if (state == META && key < 256) {
            while ((key | 0x60) == 'b' || (key | 0x60) == 'j')
                key = 'k';
            state = ((key | 0x60) >= 'm' && (key | 0x60) <= 'p') ? LITERAL : NORMAL;
        } else if (state == LITERAL) {
            if (key < ' ' || key > '~')
                key = between(' ', '~');
            state = NORMAL;
        } else if (state == META) {
            state = NORMAL;                                               /* (a key that is a sequence: it is not one of those) */
        } else if (key == 16 || key == 17) {
            state = META;
        }
        keys[nkeys++] = key;
    }
    if (rnd() % 2) { keys[nkeys++] = 23; keys[nkeys++] = 'Y'; }          /* often, end by saving */
}

static uint32_t fnv(uint32_t h, unsigned v) { return (h ^ v) * 16777619u; }

/* Run one session and print its line. */
static void one_session(int s, const char *file)
{
    in_text = file;
    in_pos = 0;
    out_len = 0;
    writes = closes = 0;
    begin_session();
    rf = reader;
    rf_ptr = NULL;
    wf = writer;
    wf_ptr = NULL;
    int finished = 0;
    cut_armed = 1;
    if (setjmp(cut) == 0) {
        run_editor();
        finished = 1;
    }
    cut_armed = 0;
#ifdef THEIRS
    if (getenv("TRACE") && atoi(getenv("TRACE")) == 2) {
        fprintf(stderr, "last bytes sent:");
        for (int i = tail_n > 120 ? tail_n - 120 : 0; i < tail_n; i++) { const int ch = tail[i % 200]; if (ch == 27) fprintf(stderr, " ESC"); else if (ch < 32) fprintf(stderr, " ^%c", ch + 64); else fprintf(stderr, "%c", ch); }
        fprintf(stderr, "\n");
    }
#endif
    if (getenv("DUMP")) {   /* the screen, to look at */
        for (int r = 0; r < 25; r++) {
            char line[81];
            for (int c = 0; c < 80; c++) { const int ch = cell_char(r, c); line[c] = ch >= 32 && ch < 127 ? (char)ch : '?'; }
            line[80] = 0;
            for (int c = 79; c >= 0 && line[c] == ' '; c--) line[c] = 0;
            fprintf(stderr, "%2d|%s\n", r, line);
        }
        fprintf(stderr, "cursor row %d col %d\n", cursor_row(), cursor_col());
    }
    uint32_t h = 2166136261u;
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 80; c++)
            h = fnv(h, (unsigned)cell_char(r, c));
    h = fnv(h, (unsigned)cursor_row());
    h = fnv(h, (unsigned)cursor_col());
    uint32_t o = 2166136261u;
    for (size_t i = 0; i < out_len; i++)
        o = fnv(o, out_bytes[i]);
    printf("%d screen %08x saved %zu %08x writes %d closes %d finished %d\n", s, h, out_len, o, writes, closes, finished);
}

int main(int argc, char **argv)
{
    const int sessions = argc > 1 ? atoi(argv[1]) : 2000;
    const int only = argc > 2 ? atoi(argv[2]) : -1;   /* a single session, with its file and keys shown */
    trace_steps = getenv("STEPS") != NULL;
    static char file[8192];
    for (int s = 0; s < sessions; s++) {
        make_file(file, sizeof file);
        make_keys();
        if (only >= 0 && s != only)
            continue;
        if (only >= 0) {
            fprintf(stderr, "file (%zu bytes):\n%s\nkeys:", strlen(file), file);
            for (int i = 0; i < nkeys; i++)
                fprintf(stderr, " %d", keys[i]);
            fprintf(stderr, "\n");
            one_session(s, file);
            continue;
        }
        fflush(stdout);
        const pid_t pid = fork();   /* each session in a process of its own: nothing is left over from one to the next, and a crash is that session's result */
        if (pid == 0) {
            alarm(20);   /* a session that does not end (an editor stuck in a loop) is stopped, and counts as a crash */
            freopen("/dev/null", "w", stderr);   /* (the report of a crash is not wanted for each of the thousands) */
            one_session(s, file);
            fflush(stdout);
            _exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            printf("%d CRASH\n", s);
    }
    return 0;
}
