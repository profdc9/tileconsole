/* SPDX-License-Identifier: MIT */
#include "textdemo_pages.h"

#include <stdio.h>
#include <string.h>

#define WHITE_ON_BLUE TG_TEXT_ATTR(TG_TEXT_WHITE, TG_TEXT_BLUE)
#define GREY_ON_BLACK TG_TEXT_ATTR(TG_TEXT_LIGHT_GREY, TG_TEXT_BLACK)

unsigned textdemo_init(tg_text_t *t)
{
    tg_text_init(t);
    return tg_text_load_cp437(t);
}

static void title(tg_text_t *t, const char *text, int page)
{
    for (int c = 0; c < TG_TEXT_COLS; c++)
        tg_text_put_cell(t, c, 0, ' ', WHITE_ON_BLUE);
    tg_text_put_string(t, 2, 0, text, WHITE_ON_BLUE);
    char tag[32];
    snprintf(tag, sizeof tag, "PAGE %d/%d", page + 1, TEXTDEMO_PAGES);
    tg_text_put_string(t, TG_TEXT_COLS - 2 - (int)strlen(tag), 0, tag, WHITE_ON_BLUE);
    for (int c = 0; c < TG_TEXT_COLS; c++)
        tg_text_put_cell(t, c, 24, ' ', TG_TEXT_ATTR(TG_TEXT_BLACK, TG_TEXT_LIGHT_GREY));
    tg_text_put_string(t, 2, 24, "LEFT/RIGHT OR A/B: CHANGE PAGE", TG_TEXT_ATTR(TG_TEXT_BLACK, TG_TEXT_LIGHT_GREY));
}

static void page_characters(tg_text_t *t)
{
    title(t, "THE PC CHARACTER SET (CODE PAGE 437)", 0);
    static const char hex[] = "0123456789ABCDEF";
    const uint8_t dim = TG_TEXT_ATTR(TG_TEXT_DARK_GREY, TG_TEXT_BLACK), bright = TG_TEXT_ATTR(TG_TEXT_WHITE, TG_TEXT_BLACK);
    for (int i = 0; i < 16; i++) {
        tg_text_put_cell(t, 6 + 2 * i, 2, (uint8_t)hex[i], dim);
        tg_text_put_cell(t, 3, 4 + i, (uint8_t)hex[i], dim);
    }
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 16; c++)
            tg_text_put_cell(t, 6 + 2 * c, 4 + r, (uint8_t)(r * 16 + c), bright);
    tg_text_put_string(t, 3, 2, "  ", dim);
    const uint8_t text = GREY_ON_BLACK;
    tg_text_put_string(t, 44, 4, "Characters are bytes: 256 of them,", text);
    tg_text_put_string(t, 44, 5, "each in 16 foreground and 16", text);
    tg_text_put_string(t, 44, 6, "background colours.", text);
    /* A frame, 36 cells wide, with a line of picture characters in it. */
    const uint8_t frame = TG_TEXT_ATTR(TG_TEXT_LIGHT_CYAN, TG_TEXT_BLACK);
    for (int x = 0; x < 36; x++) {
        tg_text_put_cell(t, 44 + x, 8, x == 0 ? 0xC9 : x == 35 ? 0xBB : 0xCD, frame);
        tg_text_put_cell(t, 44 + x, 10, x == 0 ? 0xC8 : x == 35 ? 0xBC : 0xCD, frame);
    }
    tg_text_put_cell(t, 44, 9, 0xBA, frame);
    tg_text_put_cell(t, 79, 9, 0xBA, frame);
    tg_text_put_string(t, 46, 9, "\x03 \x04 \x05 \x06   \x18 \x19 \x1A \x1B   \x0E \x0F \x01 \x02", TG_TEXT_ATTR(TG_TEXT_WHITE, TG_TEXT_BLACK));
    tg_text_put_string(t, 44, 12, "\xB0\xB0\xB0\xB0\xB1\xB1\xB1\xB1\xB2\xB2\xB2\xB2\xDB\xDB\xDB\xDB  shading, blocks", TG_TEXT_ATTR(TG_TEXT_YELLOW, TG_TEXT_BLACK));
    tg_text_put_string(t, 44, 14, "\xE0 \xE1 \xE2 \xE3 \xE4 \xE5 \xE6 \xE7 \xE8 \xE9  Greek", TG_TEXT_ATTR(TG_TEXT_LIGHT_GREEN, TG_TEXT_BLACK));
    tg_text_put_string(t, 44, 16, "\x80 \x81 \x82 \x83 \x84 \x85 \x86 \x87 \x88 \x89  accents", TG_TEXT_ATTR(TG_TEXT_LIGHT_MAGENTA, TG_TEXT_BLACK));
}

static void page_colours(tg_text_t *t)
{
    title(t, "16 FOREGROUND x 16 BACKGROUND COLOURS", 1);
    static const char hex[] = "0123456789ABCDEF";
    for (int bg = 0; bg < 16; bg++) {
        tg_text_put_cell(t, 6, 3 + bg, (uint8_t)hex[bg], TG_TEXT_ATTR(TG_TEXT_DARK_GREY, TG_TEXT_BLACK));
        for (int fg = 0; fg < 16; fg++) {
            tg_text_put_cell(t, 9 + 3 * fg, 3 + bg, (uint8_t)hex[fg], TG_TEXT_ATTR(fg, bg));
            tg_text_put_cell(t, 10 + 3 * fg, 3 + bg, (uint8_t)hex[bg], TG_TEXT_ATTR(fg, bg));
        }
    }
    for (int fg = 0; fg < 16; fg++)
        tg_text_put_cell(t, 9 + 3 * fg, 2, (uint8_t)hex[fg], TG_TEXT_ATTR(TG_TEXT_DARK_GREY, TG_TEXT_BLACK));
    tg_text_put_string(t, 2, 20, "Each cell shows its foreground and then its background number.", GREY_ON_BLACK);
    tg_text_put_string(t, 2, 21, "The palette can be changed: these are the standard 16 PC colours.", GREY_ON_BLACK);
}

/* Eight views of a bar turning round, 8 x 16 pixels each: the redefinable glyphs. */
static void spinner_glyph(uint8_t rows[16], int step)
{
    static const int8_t dx[8] = {0, 2, 3, 3, 0, -3, -3, -2}, dy[8] = {-7, -6, -3, 0, 0, 0, -3, -6};
    memset(rows, 0, 16);
    const int cx = 4, cy = 8;
    for (int i = 0; i <= 8; i++) {   /* a line from the centre outwards, and the same the other way */
        const int ex = dx[step] * i / 8, ey = dy[step] * i / 8;
        for (int s = -1; s <= 1; s += 2) {
            const int x = cx + s * ex, y = cy + s * ey;
            if (x >= 0 && x < 8 && y >= 0 && y < 16)
                rows[y] |= (uint8_t)(0x80 >> x);
        }
    }
}

static void box(tg_text_t *t, int x0, int y0, int w, int h, uint8_t attr)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t ch = ' ';
            if (y == 0 || y == h - 1)
                ch = x == 0 ? (y ? 0xC8 : 0xC9) : x == w - 1 ? (y ? 0xBC : 0xBB) : 0xCD;
            else if (x == 0 || x == w - 1)
                ch = 0xBA;
            tg_text_put_cell(t, x0 + x, y0 + y, ch, attr);
        }
    for (int i = 1; i <= h; i++)   /* a shadow, as on the PC */
        tg_text_put_cell(t, x0 + w, y0 + i, 0xDB, TG_TEXT_ATTR(TG_TEXT_DARK_GREY, TG_TEXT_BLACK)),
        tg_text_put_cell(t, x0 + w + 1, y0 + i, 0xDB, TG_TEXT_ATTR(TG_TEXT_DARK_GREY, TG_TEXT_BLACK));
}

static void page_windows(tg_text_t *t, uint32_t frame)
{
    title(t, "WINDOWS, AND A GLYPH REDEFINED ON THE FLY", 2);
    const uint8_t win = TG_TEXT_ATTR(TG_TEXT_WHITE, TG_TEXT_CYAN);
    box(t, 6, 3, 46, 11, win);
    tg_text_put_string(t, 8, 4, "Glyphs live in RAM. This glyph is", win);
    tg_text_put_string(t, 8, 5, "redefined every few frames:", win);
    uint8_t rows[16];
    spinner_glyph(rows, (int)(frame / 4 % 8));
    tg_text_set_glyph(t, 0x01, rows);   /* character 1 was the smiling face; now it is the spinner */
    for (int i = 0; i < 6; i++)
        tg_text_put_cell(t, 36 + 2 * i, 5, 0x01, TG_TEXT_ATTR(TG_TEXT_YELLOW + 0, TG_TEXT_CYAN));
    tg_text_put_string(t, 8, 7, "They are always 8 x 16 pixels.", win);
    tg_text_put_string(t, 8, 9, "Progress:", win);
    const int done = (int)(frame / 3 % 31);
    for (int i = 0; i < 30; i++)
        tg_text_put_cell(t, 18 + i, 9, i < done ? 0xDB : 0xB0, TG_TEXT_ATTR(i < done ? TG_TEXT_LIGHT_GREEN : TG_TEXT_DARK_GREY, TG_TEXT_CYAN));
    char line[48];
    snprintf(line, sizeof line, "Frame %lu", (unsigned long)frame);
    tg_text_put_string(t, 8, 11, "        ", win);
    tg_text_put_string(t, 8, 11, line, win);
    box(t, 30, 15, 28, 6, TG_TEXT_ATTR(TG_TEXT_YELLOW, TG_TEXT_RED));
    tg_text_put_string(t, 33, 17, "Another window, on top", TG_TEXT_ATTR(TG_TEXT_YELLOW, TG_TEXT_RED));
    tg_text_put_string(t, 33, 18, "\x03 \x04 \x05 \x06", TG_TEXT_ATTR(TG_TEXT_WHITE, TG_TEXT_RED));
}

static void page_scroll(tg_text_t *t, uint32_t frame, uint32_t *lines_done)
{
    title(t, "SCROLLING", 3);
    const uint32_t target = frame / 15;
    while (*lines_done < target) {
        for (int c = 0; c < TG_TEXT_COLS; c++)   /* the footer would scroll up with the log: take it away first, it is redrawn below */
            tg_text_put_cell(t, c, 24, ' ', GREY_ON_BLACK);
        tg_text_scroll_up(t, 1, GREY_ON_BLACK);
        char line[64];
        snprintf(line, sizeof line, "line %lu  %s", (unsigned long)*lines_done, (*lines_done % 3) == 0 ? "ok" : (*lines_done % 3) == 1 ? "still ok" : "fine");
        tg_text_put_string(t, 2, 23, line, TG_TEXT_ATTR(TG_TEXT_LIGHT_GREEN + (int)(*lines_done % 3), TG_TEXT_BLACK));
        (*lines_done)++;
    }
    title(t, "SCROLLING", 3);
}

/* Typing at a prompt, with the cursor following, and the cursor changing shape. Worked out from the frame number alone. */
static void page_cursor(tg_text_t *t, uint32_t frame)
{
    static const char *const script[] = {"DIR /W", "ECHO HELLO FROM THE TEXT MODE", "TYPE README.TXT", "CHKDSK C:", "EDIT NOTES.TXT"};
    enum { N = sizeof script / sizeof *script, PER_CHAR = 5, PAUSE = 45, FIRST_ROW = 3 };
    title(t, "THE CURSOR", 4);
    for (int r = FIRST_ROW; r < 23; r++)
        for (int c = 0; c < TG_TEXT_COLS; c++)
            tg_text_put_cell(t, c, r, ' ', GREY_ON_BLACK);
    uint32_t cycle = 0;
    for (int i = 0; i < N; i++)
        cycle += (uint32_t)strlen(script[i]) * PER_CHAR + PAUSE;
    uint32_t f = frame % cycle;
    int cursor_col = 5, cursor_row = FIRST_ROW;
    for (int i = 0; i < N; i++) {
        const uint32_t len = (uint32_t)strlen(script[i]), span = len * PER_CHAR + PAUSE;
        const int row = FIRST_ROW + i * 2;
        tg_text_put_string(t, 0, row, "C:\\>", TG_TEXT_ATTR(TG_TEXT_LIGHT_GREY, TG_TEXT_BLACK));
        if (f >= span) {   /* a finished line, and its reply */
            tg_text_put_string(t, 5, row, script[i], TG_TEXT_ATTR(TG_TEXT_WHITE, TG_TEXT_BLACK));
            tg_text_put_string(t, 0, row + 1, "ok", TG_TEXT_ATTR(TG_TEXT_DARK_GREY, TG_TEXT_BLACK));
            f -= span;
            continue;
        }
        const uint32_t typed = f / PER_CHAR < len ? f / PER_CHAR : len;   /* the line being typed */
        for (uint32_t k = 0; k < typed; k++)
            tg_text_put_cell(t, 5 + (int)k, row, (uint8_t)script[i][k], TG_TEXT_ATTR(TG_TEXT_WHITE, TG_TEXT_BLACK));
        cursor_col = 5 + (int)typed;
        cursor_row = row;
        break;
    }
    tg_text_set_cursor(t, cursor_col, cursor_row);
    tg_text_show_cursor(t, true);
    static const struct { int start, end; const char *name; } shapes[] = {
        {14, 15, "underline (scanlines 14 to 15)"}, {0, 15, "block (0 to 15)"}, {8, 15, "half block (8 to 15)"}, {7, 8, "thin line (7 to 8)"}};
    const int s = (int)(frame / 120 % 4);
    tg_text_set_cursor_shape(t, shapes[s].start, shapes[s].end);
    char line[64];
    snprintf(line, sizeof line, "Cursor shape: %s", shapes[s].name);
    tg_text_put_string(t, 2, 22, line, TG_TEXT_ATTR(TG_TEXT_YELLOW, TG_TEXT_BLACK));
}

void textdemo_draw(tg_text_t *t, int page, uint32_t frame)
{
    static int last_page = -1;
    static uint32_t lines_done;
    if (page != last_page) {
        tg_text_clear(t, GREY_ON_BLACK);
        tg_text_show_cursor(t, false);   /* only the cursor page shows it */
        last_page = page;
        lines_done = frame / 15;
    }
    switch (page) {
    case 0: page_characters(t); break;
    case 1: page_colours(t); break;
    case 2: page_windows(t, frame); break;
    case 3: page_scroll(t, frame, &lines_done); break;
    default: page_cursor(t, frame); break;
    }
}
