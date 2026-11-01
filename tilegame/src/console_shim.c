/* SPDX-License-Identifier: MIT */
/* consoleio on tilegfx: see consoleio.h. */
#include "consoleio.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "keymap.h"
#include "platform.h"
#include "tg_text.h"

static tg_text_t screen;
static tg_console_t con;
static console_hooks_t hooks;

static char pending[4];   /* the rest of the bytes of a key that arrives as several */
static int pending_count, pending_next;

tg_text_t *console_screen(void) { return &screen; }
tg_console_t *console_console(void) { return &con; }

void console_set_hooks(const console_hooks_t *h)
{
    hooks = *h;
    pending_count = pending_next = 0;
}

/* ---- keys ---- */

int console_expand_key(int key, char out[4])
{
    static const struct { int key; const char *bytes; } special[] = {
        /* the editor's own keys: it reads ESC, ignores the next byte, and takes the third as a cursor key (A B C D), or as one of p r l, which are the
         * "meta" forms of ^S (line begin), ^D (line end) and ^H (delete under the cursor). */
        {PLAT_KEY_UP, "\033[A"}, {PLAT_KEY_DOWN, "\033[B"}, {PLAT_KEY_RIGHT, "\033[C"}, {PLAT_KEY_LEFT, "\033[D"},
        {PLAT_KEY_HOME, "\033Op"}, {PLAT_KEY_END, "\033Or"}, {127, "\033Ol"},   /* (Delete: 127 is what the editor takes for Backspace) */
        {PLAT_KEY_PAGEDOWN, "\026"},                                            /* ^V */
        {PLAT_KEY_PAGEUP, "\020\026"},                                          /* ^P, the meta prefix, and ^V: page up */
        {PLAT_KEY_INSERT, "\016"},                                              /* ^N: insert or overwrite */
        {PLAT_KEY_F(1), "\034"},                                                /* ^\ : help */
        {PLAT_KEY_F(2), "\020\n"},                                              /* ^P ^J: record a macro (Ctrl+J cannot be typed: it is the Enter key) */
    };
    if (key == 10) {   /* Enter is a carriage return to a terminal program */
        out[0] = '\r';
        return 1;
    }
    for (size_t i = 0; i < sizeof special / sizeof *special; i++)
        if (special[i].key == key) {
            const int n = (int)strlen(special[i].bytes);
            memcpy(out, special[i].bytes, (size_t)n);
            return n;
        }
    if (key >= 0 && key < 256) {
        out[0] = (char)key;
        return 1;
    }
    return 0;   /* a key nothing here knows */
}

int console_inchar(void)
{
    if (pending_next < pending_count)
        return (unsigned char)pending[pending_next++];
    for (;;) {
        const int key = hooks.getkey ? hooks.getkey() : -1;
        if (key < 0)
            return -1;
        const int n = console_expand_key(key, pending);
        if (n > 0) {
            pending_count = n;
            pending_next = 1;
            return (unsigned char)pending[0];
        }
    }
}

int console_waiting(void)
{
    if (pending_next < pending_count)
        return 1;
    for (;;) {   /* a key that is waiting, and that is not one that is ignored */
        const int key = hooks.getkey ? hooks.getkey() : -1;
        if (key < 0)
            return 0;
        const int n = console_expand_key(key, pending);
        if (n > 0) {
            pending_count = n;
            pending_next = 0;   /* nothing of it has been read yet */
            return 1;
        }
    }
}

int console_getch(void);

/* A byte for a prompt: the keys that are not characters (the cursor keys, Home and so on) arrive as escape sequences, which are read and dropped here
 * instead of being taken for the characters [ A and the rest. (PicoParanoia's own prompts took them for typing: this is a change.) */
static int getch_for_prompt(void)
{
    for (;;) {
        const int ch = console_getch();
        if (ch != 27)
            return ch;
        if (console_waiting() && (pending[pending_next] == '[' || pending[pending_next] == 'O')) {
            console_getch();                   /* the [ or O */
            if (console_waiting())
                console_getch();               /* and the letter: the sequence is three bytes */
        }
    }
}

int console_getch(void)
{
    int ch;
    while ((ch = console_inchar()) < 0)
        if (hooks.frame)
            hooks.frame();
    return ch;
}

/* ---- output ---- */

void console_init(void)
{
    tg_text_init(&screen);
    tg_text_load_cp437(&screen);
    tg_console_init(&con, &screen, TG_TEXT_ATTR(TG_TEXT_LIGHT_GREY, TG_TEXT_BLACK));
    con.flags = TG_CONSOLE_ANSI | TG_CONSOLE_LF_ONLY | TG_CONSOLE_NOWRAP;   /* a terminal without autowrap, as PicoParanoia's is */
    tg_console_clear(&con);
    tg_text_show_cursor(&screen, true);
    pending_count = pending_next = 0;
}

void console_putch(char c) { tg_console_putc(&con, (unsigned char)c); }
void console_puts(const char *c) { tg_console_puts(&con, c); }
void console_printcrlf(void) { console_puts("\r\n"); }
void console_clreol(void) { console_puts("\033[K"); }
void console_clrscr(void) { console_puts("\033[H\033[2J"); }
void console_highvideo(void) { console_puts("\033[1m"); }
void console_lowvideo(void) { console_puts("\033[0m"); }

void console_printint(int n)
{
    char s[16];
    snprintf(s, sizeof s, "%d", n);
    console_puts(s);
}

void console_printuint(unsigned int n)
{
    char s[16];
    snprintf(s, sizeof s, "%u", n);
    console_puts(s);
}

void console_gotoxy(int x, int y)
{
    if (x < 1) x = 1;   /* a position below 1 (the editor can ask for one, in some corners) is the first column or row, not a sequence a terminal cannot read */
    if (y < 1) y = 1;
    console_puts("\033[");
    console_printint(y);
    console_putch(';');
    console_printint(x);
    console_putch('H');
}

void console_delayframes(unsigned short fr)
{
    for (unsigned short i = 0; i < fr; i++)
        if (hooks.frame)
            hooks.frame();
}

int console_getstring(char *c, int len, int row, int col, int wrap)
{
    int n = 0;
    for (;;) {
        console_gotoxy((n % wrap) + col, (n / wrap) + row);
        const int ch = getch_for_prompt();
        if (ch == '\r') {
            c[n] = '\000';
            return n;
        }
        if (ch >= ' ' && ch < 127 && n < len) {
            console_putch((char)ch);
            c[n++] = (char)ch;
        }
        if (ch == '\010' && n > 0) {
            n--;
            console_gotoxy((n % wrap) + col, (n / wrap) + row);
            console_puts(" \010");
        }
    }
}

char console_selectmenu(const char *menutext, const char *options)
{
    console_puts(menutext);
    for (;;) {
        const int ch = toupper(getch_for_prompt());
        for (const char *scan = options; *scan; scan++)
            if (*scan == ch) {
                console_putch((char)ch);
                return (char)ch;
            }
    }
}

void console_press_space(void)
{
    console_puts("\r\n\r\nPress SPACE to continue");
    while (console_getch() != ' ')
        ;
}

int console_yes(int row)
{
    char response[4];
    console_gotoxy(1, row);
    console_puts("Type YES to continue: ");
    console_getstring(response, 3, row, 23, 10);
    return strcmp(response, "YES");   /* zero for "yes", as PicoParanoia's does */
}

void console_print_array(const char *c, unsigned char *a, int n)
{
    console_printcrlf();
    console_puts(c);
    for (int i = 0; i < n; i++) {
        if (i % 8 == 0)
            console_printcrlf();
        console_printint(a[i]);
        console_puts(" ");
    }
    console_printcrlf();
}

/* ---- the string helpers PicoParanoia's programs use ---- */

char *myltoa(char *p, long n)
{
    snprintf(p, MY_ITOASIZE, "%ld", n);
    return p;
}

long mystrtol(const char *str, const char **end)
{
    unsigned long n = 0;
    int neg;
    while (*str == ' ')
        str++;
    if ((neg = (*str == '-')))
        str++;
    while (*str >= '0' && *str <= '9')
        n = n * 10 + (unsigned long)(*str++ - '0');
    if (end)
        *end = str;
    return neg ? -((long)n) : (long)n;
}

char *strcpy_n(char *dest, const char *src, size_t len)
{
    char *d = dest;
    while (len > 0 && *src) {
        *d++ = *src++;
        len--;
    }
    *d = '\000';
    return dest;
}

char *strcat_n(char *dest, const char *src, size_t len)
{
    char *d = dest;
    while (len > 0 && *d) {
        d++;
        len--;
    }
    while (len > 0 && *src) {
        *d++ = *src++;
        len--;
    }
    *d = '\000';
    return dest;
}

size_t strlen_n(const char *d)
{
    const char *p = d;
    while (*p)
        p++;
    return (size_t)(p - d);
}
