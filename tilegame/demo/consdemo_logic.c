/* SPDX-License-Identifier: MIT */
#include "consdemo_logic.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#define PROMPT "C:\\> "
#define PROMPT_LEN 5
#define ATTR_TEXT TG_TEXT_ATTR(TG_TEXT_LIGHT_GREY, TG_TEXT_BLACK)
#define ATTR_PROMPT TG_TEXT_ATTR(TG_TEXT_LIGHT_GREEN, TG_TEXT_BLACK)
#define ATTR_HEAD TG_TEXT_ATTR(TG_TEXT_YELLOW, TG_TEXT_BLACK)

static int same_text(const char *a, const char *b)   /* equal, ignoring case */
{
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return 0;
    return *a == *b;
}

static void with(consdemo_t *d, uint8_t attr, const char *text)
{
    const uint8_t was = d->con.attr;
    tg_console_set_attr(&d->con, attr);
    tg_console_puts(&d->con, text);
    tg_console_set_attr(&d->con, was);
}

static void prompt(consdemo_t *d)
{
    if (d->con.col != 0)
        tg_console_putc(&d->con, '\n');
    with(d, ATTR_PROMPT, PROMPT);
    d->prompt_row = d->con.row;
    d->len = d->pos = 0;
    d->line[0] = 0;
    d->browsing = -1;
    tg_text_show_cursor(d->con.text, true);
}

/* The line as it is, padded to full width so that nothing of an older, longer one is left, and the cursor where it is being edited. */
static void redraw(consdemo_t *d)
{
    tg_console_gotoxy(&d->con, PROMPT_LEN, d->prompt_row);
    tg_console_printf(&d->con, "%-*s", CONSDEMO_LINE_MAX, d->line);
    tg_console_gotoxy(&d->con, PROMPT_LEN + d->pos, d->prompt_row);
}

static void set_line(consdemo_t *d, const char *text)
{
    snprintf(d->line, sizeof d->line, "%s", text);
    d->len = d->pos = (int)strlen(d->line);
    redraw(d);
}

static void insert(consdemo_t *d, char ch)
{
    if (d->len >= CONSDEMO_LINE_MAX)
        return;
    memmove(d->line + d->pos + 1, d->line + d->pos, (size_t)(d->len - d->pos + 1));
    d->line[d->pos++] = ch;
    d->len++;
    redraw(d);
}

static void remove_at(consdemo_t *d, int at)
{
    if (at < 0 || at >= d->len)
        return;
    memmove(d->line + at, d->line + at + 1, (size_t)(d->len - at));
    d->len--;
    if (d->pos > at)
        d->pos--;
    redraw(d);
}

static void print_chars_table(consdemo_t *d)
{
    for (int row = 0; row < 16; row++) {
        tg_console_printf(&d->con, " %X ", row);
        for (int col = 0; col < 16; col++) {
            const int c = row * 16 + col;
            tg_console_putc(&d->con, c == 7 || c == 8 || c == 9 || c == 10 || c == 13 ? ' ' : c);   /* the console acts on these five, so they are shown as blanks */
        }
        tg_console_putc(&d->con, '\n');
    }
}

static void test_printf(consdemo_t *d)
{
    tg_console_printf(&d->con, "int [%d] [%5d] [%-5d] [%05d] [%+d]\n", 42, 42, 42, 42, 42);
    tg_console_printf(&d->con, "hex [%x] [%X] [%08x] [%#x]  octal [%o]\n", 48879, 48879, 48879, 255, 8);
    tg_console_printf(&d->con, "char [%c%c%c] string [%s] [%10s] [%-10s] [%.3s]\n", 'a', 'b', 'c', "hello", "hello", "hello", "hello");
    tg_console_printf(&d->con, "unsigned [%u] long [%ld] 100%%\n", 4000000000u, -1234567890L);
}

static void run(consdemo_t *d, char *command)
{
    while (isspace((unsigned char)*command))
        command++;
    char *end = command + strlen(command);
    while (end > command && isspace((unsigned char)end[-1]))
        *--end = 0;
    if (!*command)
        return;
    char *args = command;
    while (*args && !isspace((unsigned char)*args))
        args++;
    if (*args)
        *args++ = 0;
    while (isspace((unsigned char)*args))
        args++;
    for (char *c = command; *c; c++)
        *c = (char)toupper((unsigned char)*c);

    if (!strcmp(command, "HELP")) {
        with(d, ATTR_HEAD, "Commands:\n");
        tg_console_puts(&d->con, "  HELP            this list\n  CLS             clear the screen\n  ECHO text       print the text\n");
        tg_console_puts(&d->con, "  COLOR fg bg     colours for new text, 0 to 15 each\n  CURSOR shape    UNDERLINE, BLOCK, HALF or LINE\n");
        tg_console_puts(&d->con, "  CHARS           the PC character set\n  TEST            printf formats\n  UPTIME          milliseconds since start\n");
        tg_console_puts(&d->con, "  KEYS            show the codes of the keys you press (Esc to stop)\n");
        tg_console_puts(&d->con, "Editing: arrows, Home, End, Backspace, Delete, Ctrl+A/E/K/U/L/C, up and down for history.\n");
    } else if (!strcmp(command, "CLS") || !strcmp(command, "CLEAR")) {
        tg_console_clear(&d->con);
    } else if (!strcmp(command, "ECHO")) {
        tg_console_printf(&d->con, "%s\n", args);
    } else if (!strcmp(command, "COLOR")) {
        int fg = -1, bg = -1;
        if (sscanf(args, "%d %d", &fg, &bg) == 2 && fg >= 0 && fg < 16 && bg >= 0 && bg < 16)
            tg_console_set_attr(&d->con, TG_TEXT_ATTR(fg, bg));
        else
            tg_console_puts(&d->con, "Usage: COLOR fg bg   (0 to 15 each)\n");
    } else if (!strcmp(command, "CURSOR")) {
        if (same_text(args, "UNDERLINE"))
            tg_text_set_cursor_shape(d->con.text, 14, 15);
        else if (same_text(args, "BLOCK"))
            tg_text_set_cursor_shape(d->con.text, 0, 15);
        else if (same_text(args, "HALF"))
            tg_text_set_cursor_shape(d->con.text, 8, 15);
        else if (same_text(args, "LINE"))
            tg_text_set_cursor_shape(d->con.text, 7, 8);
        else
            tg_console_puts(&d->con, "Usage: CURSOR UNDERLINE|BLOCK|HALF|LINE\n");
    } else if (!strcmp(command, "CHARS")) {
        print_chars_table(d);
    } else if (!strcmp(command, "TEST")) {
        test_printf(d);
    } else if (!strcmp(command, "UPTIME")) {
        tg_console_printf(&d->con, "%lu ms\n", (unsigned long)d->millis());
    } else if (!strcmp(command, "KEYS")) {
        tg_console_puts(&d->con, "Press keys; Esc stops.\n");
        d->keys_mode = 1;
    } else if (!strcmp(command, "VER")) {
        tg_console_puts(&d->con, "tilegfx console demo\n");
    } else {
        tg_console_puts(&d->con, "Bad command or file name.\n");
    }
}

static void remember(consdemo_t *d)
{
    if (!d->len || (d->history_count && !strcmp(d->history[0], d->line)))
        return;
    memmove(d->history[1], d->history[0], sizeof d->history[0] * (CONSDEMO_HISTORY - 1));
    memcpy(d->history[0], d->line, sizeof d->history[0]);
    if (d->history_count < CONSDEMO_HISTORY)
        d->history_count++;
}

static const char *key_name(int ch)
{
    static char buf[16];
    switch (ch) {
    case PLAT_KEY_UP: return "Up";
    case PLAT_KEY_DOWN: return "Down";
    case PLAT_KEY_LEFT: return "Left";
    case PLAT_KEY_RIGHT: return "Right";
    case PLAT_KEY_HOME: return "Home";
    case PLAT_KEY_END: return "End";
    case PLAT_KEY_PAGEUP: return "PageUp";
    case PLAT_KEY_PAGEDOWN: return "PageDown";
    case PLAT_KEY_INSERT: return "Insert";
    case 8: return "Backspace";
    case 9: return "Tab";
    case 10: return "Enter";
    case 27: return "Escape";
    case 127: return "Delete";
    case ' ': return "Space";
    default: break;
    }
    if (ch >= PLAT_KEY_F1 && ch <= PLAT_KEY_F(12)) {
        snprintf(buf, sizeof buf, "F%d", ch - PLAT_KEY_F1 + 1);
        return buf;
    }
    if (ch > 0 && ch < 27) {
        snprintf(buf, sizeof buf, "Ctrl+%c", 'A' + ch - 1);
        return buf;
    }
    if (ch > ' ' && ch < 127) {
        snprintf(buf, sizeof buf, "'%c'", ch);
        return buf;
    }
    return "";
}

void consdemo_init(consdemo_t *d, tg_text_t *screen, uint32_t (*millis)(void))
{
    memset(d, 0, sizeof *d);
    d->millis = millis;
    tg_console_init(&d->con, screen, ATTR_TEXT);
    tg_console_clear(&d->con);
    with(d, ATTR_HEAD, "TILEGFX CONSOLE\n");
    tg_console_puts(&d->con, "80 x 25 text mode, printf output and a keyboard. Type HELP.\n\n");
    prompt(d);
}

void consdemo_char(consdemo_t *d, int ch)
{
    if (d->keys_mode) {
        if (ch == 27) {
            d->keys_mode = 0;
            tg_console_puts(&d->con, "Done.\n");
            prompt(d);
        } else {
            tg_console_printf(&d->con, "code %3d (0x%03X) %s\n", ch, ch, key_name(ch));
        }
        return;
    }
    switch (ch) {
    case 10:                                         /* Enter: run what was typed */
        tg_console_putc(&d->con, '\n');
        remember(d);
        memcpy(d->command, d->line, sizeof d->command);
        d->len = d->pos = 0;                         /* the line is spent */
        d->line[0] = 0;
        run(d, d->command);
        if (!d->keys_mode)
            prompt(d);
        break;
    case 8: remove_at(d, d->pos - 1); break;          /* Backspace */
    case 127: remove_at(d, d->pos); break;            /* Delete */
    case PLAT_KEY_LEFT: if (d->pos > 0) { d->pos--; redraw(d); } break;
    case PLAT_KEY_RIGHT: if (d->pos < d->len) { d->pos++; redraw(d); } break;
    case PLAT_KEY_HOME: case 1: d->pos = 0; redraw(d); break;          /* Ctrl+A */
    case PLAT_KEY_END: case 5: d->pos = d->len; redraw(d); break;      /* Ctrl+E */
    case 11: d->line[d->pos] = 0; d->len = d->pos; redraw(d); break;   /* Ctrl+K: to the end of the line */
    case 21: d->line[0] = 0; d->len = d->pos = 0; redraw(d); break;    /* Ctrl+U: the whole line */
    case 12:                                         /* Ctrl+L: clear the screen, keep the line */
        tg_console_clear(&d->con);
        with(d, ATTR_PROMPT, PROMPT);
        d->prompt_row = d->con.row;
        redraw(d);
        break;
    case 3:                                          /* Ctrl+C: give the line up */
        tg_console_gotoxy(&d->con, PROMPT_LEN + d->len, d->prompt_row);
        tg_console_puts(&d->con, "^C\n");
        prompt(d);
        break;
    case PLAT_KEY_UP:
        if (d->browsing + 1 < d->history_count) {
            d->browsing++;
            set_line(d, d->history[d->browsing]);
        }
        break;
    case PLAT_KEY_DOWN:
        if (d->browsing > 0) {
            d->browsing--;
            set_line(d, d->history[d->browsing]);
        } else if (d->browsing == 0) {
            d->browsing = -1;
            set_line(d, "");
        }
        break;
    default:
        if (ch >= ' ' && ch < 127)
            insert(d, (char)ch);
        break;
    }
}
