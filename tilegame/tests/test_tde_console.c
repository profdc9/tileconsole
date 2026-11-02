/* SPDX-License-Identifier: MIT */
/* Tests of the editor port's console functions (tde/os/console.c) on their own: the screen they draw on and the cursor, checked cell by cell. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tdestr.h"
#include "common.h"
#include "define.h"
#include "tdefunc.h"

#include "platform.h"
#include "tde_os.h"
#include "tg_text.h"

static int checks;
#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static tg_text_t screen;

static void nop(void) {}

#define CELL(r, c) (screen.cells[(r) * 80 + (c)])
#define CH(r, c) (CELL(r, c) & 255)
#define AT(r, c) (CELL(r, c) >> 8)

static void test_console_primitives(void)
{
    struct vcfg cfg;
    memset(&screen, 0, sizeof screen);
    CHECK(tg_text_load_cp437(&screen) == 0);
    tg_text_init(&screen);
    tde_console_attach(&screen, nop);
    console_init(&cfg);
    CHECK(cfg.color && cfg.cols == 80 && cfg.rows == 25 && g_display.adapter == VGA);
    g_display.ncols = 80;
    g_display.nlines = 25;
    g_display.color = colour[1];
    g_display.output_space = 0;
    g_display.shadow = 1;
    g_display.shadow_width = 2;
    colour[1][Text_color] = 0x1F;
    /* clear, and single cells */
    cls();
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 80; c++)
            CHECK(CELL(r, c) == (0x1F << 8 | ' '));
    c_output('A', 5, 3, 0x4E);
    CHECK(CH(3, 5) == 'A' && AT(3, 5) == 0x4E && CH(3, 4) == ' ' && CH(3, 6) == ' ');
    c_output(0xE9, 79, 24, 0x07);                      /* the whole PC character set, and the corners */
    CHECK(CH(24, 79) == 0xE9 && AT(24, 79) == 0x07);
    c_output('x', -1, 3, 1); c_output('x', 80, 3, 1); c_output('x', 5, -1, 1); c_output('x', 5, 25, 1);   /* off the screen: nothing, and no harm */
    CHECK(CH(3, 5) == 'A' && CH(0, 0) == ' ' && CH(3, 0) == ' ' && CH(24, 5) == ' ');
    /* repeats, along and down */
    c_repeat('-', 10, 20, 7, 0x2A);
    CHECK(CH(7, 19) == ' ' && CH(7, 20) == '-' && CH(7, 29) == '-' && CH(7, 30) == ' ' && AT(7, 25) == 0x2A);
    c_repeat('|', -4, 40, 10, 0x3B);
    CHECK(CH(9, 40) == ' ' && CH(10, 40) == '|' && CH(13, 40) == '|' && CH(14, 40) == ' ' && CH(10, 41) == ' ' && AT(12, 40) == 0x3B);
    c_repeat('z', 0, 1, 1, 7);
    CHECK(CH(1, 1) == ' ');
    c_repeat('e', 5, 76, 2, 7);                         /* runs off the right edge: it stops there */
    CHECK(CH(2, 75) == ' ' && CH(2, 76) == 'e' && CH(2, 79) == 'e' && CH(3, 0) == ' ');
    /* a string, clipped at the right edge, with and without the space either side */
    s_output("Hello", 12, 10, 0x71);
    CHECK(CH(12, 9) == ' ' && CH(12, 10) == 'H' && CH(12, 14) == 'o' && CH(12, 15) == ' ' && AT(12, 10) == 0x71 && AT(12, 9) == 0x1F && AT(12, 15) == 0x1F);
    g_display.output_space = 1;
    s_output("Hi", 13, 10, 0x71);
    CHECK(CH(13, 9) == ' ' && AT(13, 9) == 0x71 && CH(13, 10) == 'H' && CH(13, 12) == ' ' && AT(13, 12) == 0x71 && AT(13, 13) == 0x1F);
    s_output("edge", 14, 0, 0x71);                      /* nothing in front of column 0 */
    CHECK(CH(14, 0) == 'e' && CH(14, 4) == ' ' && AT(14, 4) == 0x71);
    g_display.output_space = 0;
    s_output("this string is cut at the right edge", 15, 70, 0x71);
    CHECK(CH(15, 70) == 't' && CH(15, 79) == 'n' && CH(16, 0) == ' ');
    /* a line of text with its attributes, clipped */
    text_t text[6] = {'a', 'b', 'c', 'd', 'e', 'f'};
    unsigned char attr[6] = {1, 2, 3, 4, 5, 6};
    display_line(text, attr, 6, 18, 10);
    CHECK(CH(18, 10) == 'a' && AT(18, 10) == 1 && CH(18, 15) == 'f' && AT(18, 15) == 6 && CH(18, 16) == ' ');
    display_line(text, attr, 6, 19, 77);                  /* three fit */
    CHECK(CH(19, 77) == 'a' && CH(19, 79) == 'c' && AT(19, 79) == 3 && CH(20, 0) == ' ');
    /* highlighting keeps the characters */
    hlight_line(10, 18, 3, 0x5E);
    CHECK(CH(18, 10) == 'a' && AT(18, 10) == 0x5E && CH(18, 12) == 'c' && AT(18, 12) == 0x5E && AT(18, 13) == 4);
    hlight_line(78, 19, 10, 0x6F);                        /* to the edge */
    CHECK(AT(19, 78) == 0x6F && AT(19, 79) == 0x6F && CH(19, 78) == 'b');
    hlight_line(5, 19, 0, 0x6F);
    CHECK(AT(19, 5) == 0x1F);
    /* a line saved and put back, from column 0 and from a column */
    static Char line[80];
    save_screen_line(0, 18, line);
    CHECK((line[10] & 255) == 'a' && (line[10] >> 8) == 0x5E && (line[0] & 255) == ' ');
    s_output("OVERWRITTEN OVERWRITTEN", 18, 0, 0x07);
    restore_screen_line(0, 18, line);
    CHECK(CH(18, 10) == 'a' && AT(18, 10) == 0x5E && CH(18, 0) == ' ' && AT(18, 0) == 0x1F);
    save_screen_line(10, 18, line);
    c_output('Q', 12, 18, 0x07);
    restore_screen_line(10, 18, line);
    CHECK(CH(18, 12) == 'c' && AT(18, 12) == 0x5E);
    /* an area, saved and restored. The editor's area includes the room for the shadow (and the spaces round text) when those are on: a width more by the
     * shadow's width, as far as the screen goes, and a row more if there is one. */
    static Char area[40 * 10];
    for (int variant = 0; variant < 4; variant++) {
        const int shadow = variant & 1, space = variant >> 1;
        g_display.shadow = shadow;
        g_display.output_space = space;
        for (int r = 0; r < 25; r++)
            for (int c = 0; c < 80; c++)
                CELL(r, c) = (uint16_t)(((r * 3 + c) & 255) << 8 | (' ' + (r + c) % 90));
        const int row = variant == 3 ? 20 : 4, col = variant == 2 ? 70 : variant == 3 ? 0 : 6;   /* a box at the left and the right edge, and the bottom, too */
        int wid = 8, len = 5, c0 = col;
        if (space) {
            if (col + wid < 80)
                wid++;
            if (col != 0) {
                c0--;
                wid++;
            }
        }
        if (shadow) {
            wid += col + wid + 2 <= 80 ? 2 : 80 - (c0 + wid);
            if (row + len < 25)
                len++;
        }
        uint16_t expect[40 * 10];
        for (int r = 0; r < len; r++)
            for (int c = 0; c < wid; c++)
                expect[r * wid + c] = CELL(row + r, c0 + c);
        save_area(area, 8, 5, row, col);
        for (int r = 0; r < len; r++)
            for (int c = 0; c < wid; c++)
                CHECK(area[r * wid + c] == expect[r * wid + c]);
        for (int r = row; r < 25 && r < row + len; r++)
            for (int c = c0; c < 80 && c < c0 + wid; c++)
                CELL(r, c) = 0x0720;
        restore_area(area, 8, 5, row, col);
        for (int r = 0; r < len; r++)
            for (int c = 0; c < wid; c++)
                CHECK(CELL(row + r, c0 + c) == expect[r * wid + c]);
    }
    g_display.shadow = 1;
    g_display.output_space = 0;
    /* the shadow of a box: the cells to the right (2 wide) and below, in dark grey, their characters kept; none when it is off */
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 80; c++)
            CELL(r, c) = (uint16_t)(0x1F00 | 'x');
    shadow_area(10, 5, 3, 20);
    CHECK(AT(3, 29) == 0x1F || AT(3, 29) == 8);
    int shadowed = 0;
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 80; c++)
            if (AT(r, c) == 8) {
                shadowed++;
                CHECK(CH(r, c) == 'x');
            }
    /* a box of 10 x 5 at row 3, column 20: its shadow is 2 columns on the right from the second row down to one row below it, and that row from 2 columns in */
    CHECK(shadowed == 5 * 2 + 8);
    CHECK(AT(4, 30) == 8 && AT(8, 31) == 8 && AT(3, 30) == 0x1F && AT(3, 31) == 0x1F && AT(5, 29) == 0x1F && AT(5, 32) == 0x1F);
    CHECK(AT(8, 22) == 8 && AT(8, 29) == 8 && AT(8, 21) == 0x1F && AT(8, 20) == 0x1F && AT(9, 25) == 0x1F && AT(7, 22) == 0x1F);
    g_display.shadow = 0;
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 80; c++)
            CELL(r, c) = (uint16_t)(0x1F00 | 'x');
    shadow_area(10, 5, 3, 20);
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 80; c++)
            CHECK(AT(r, c) == 0x1F);
    /* the cursor: a position and a shape, or none */
    mode.insert = TRUE;
    g_display.insert_cursor = 0x080f;
    g_display.overw_cursor = 0x000f;
    xygoto(12, 7);
    CHECK(screen.cursor_col == 12 && screen.cursor_row == 7 && (screen.cursor_flags & TG_CURSOR_VISIBLE) && screen.cursor_start == 8 && screen.cursor_end == 15);
    mode.insert = FALSE;
    xygoto(3, 4);
    CHECK(screen.cursor_col == 3 && screen.cursor_row == 4 && screen.cursor_start == 0 && screen.cursor_end == 15);
    xygoto(-1, -1);
    CHECK(!(screen.cursor_flags & TG_CURSOR_VISIBLE));
    set_cursor_size(0x0e0f);
    CHECK((screen.cursor_flags & TG_CURSOR_VISIBLE) && screen.cursor_start == 14 && screen.cursor_end == 15 && g_display.curses_cursor == 0x0e0f);
    console_exit();
    CHECK(!(screen.cursor_flags & TG_CURSOR_VISIBLE));
    CHECK(g_display.cursor[SMALL_CURSOR] == 0x0e0f && g_display.cursor[MEDIUM_CURSOR] == 0x080f && g_display.cursor[LARGE_CURSOR] == 0x000f);
}


int tde_test_console(void)
{
    tde_state_create();      /* (the editor's variables are in its state) */
    test_console_primitives();
    tde_state_destroy();
    return checks;
}
