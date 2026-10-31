// SPDX-License-Identifier: MIT
// Compares the console's ANSI mode with TNTSCAnsi (Daniel Marks' VT100 interpreter from PicoParanoia, zlib licence) on the sequences they have in
// common: both are fed the same random sequences and must have the same characters and cursor after every step. TNTSCAnsi is compiled from a copy
// made by the Makefile (see `make test_ansi_oracle`); this file only uses it, and does not change it.
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "tg_console.h"
}
#include "TNTSCAnsi.h"

static int checks;
static uint32_t rng = 17;
static uint32_t rnd() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
static int between(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

static unsigned char theirs[25 * 80];
static tg_text_t screen;
static tg_console_t con;
static char history[4000];
static int history_len;

static void feed(const char *bytes, int n)
{
    for (int i = 0; i < n; i++) {
        TNTSCAnsi.output_character((unsigned char)bytes[i]);
        tg_console_putc(&con, (unsigned char)bytes[i]);
    }
    if (history_len + n < (int)sizeof history - 1) {
        memcpy(history + history_len, bytes, (size_t)n);
        history_len += n;
    }
}

static void feed(const char *s) { feed(s, (int)strlen(s)); }

static void fail(const char *what, int index)
{
    printf("MISMATCH (%s) at cell %d (row %d col %d): theirs %02X (cursor %d,%d), ours %02X (cursor %d,%d)\n", what, index, index / 80, index % 80,
           theirs[index] & 0x7F, TNTSCAnsi.cur.ypos, TNTSCAnsi.cur.xpos, screen.cells[index] & 255, con.row, con.col);
    printf("input so far (escapes shown as ^[): ");
    for (int i = 0; i < history_len; i++) {
        const unsigned char ch = (unsigned char)history[i];
        if (ch == 27) printf("^[");
        else if (ch < 32) printf("^%c", ch + 64);
        else printf("%c", ch);
    }
    printf("\n");
    exit(1);
}

static void compare(void)
{
    checks++;
    for (int i = 0; i < 25 * 80; i++)
        if ((theirs[i] & 0x7F) != (screen.cells[i] & 255))   // (their bit 7 carries the bold attribute)
            fail("cell", i);
    if (TNTSCAnsi.cur.xpos != con.col || TNTSCAnsi.cur.ypos != con.row)
        fail("cursor", TNTSCAnsi.cur.ypos * 80 + TNTSCAnsi.cur.xpos);
}

static void begin(void)
{
    TNTSCAnsi.begin(theirs, 25, 80);
    memset(&screen, 0, sizeof screen);
    tg_text_init(&screen);
    tg_console_init(&con, &screen, 0x07);
    con.flags = TG_CONSOLE_ANSI | TG_CONSOLE_LF_ONLY;   // their line feed does not return the carriage
    tg_console_clear(&con);
    history_len = 0;
    compare();
}

// Their line feed, reverse index and next line do nothing useful when the cursor is outside the scroll region (it stays where it is); ours moves
// the cursor as a VT100 does. Those are only sent with the cursor inside the region.
static bool in_region(void)
{
    return TNTSCAnsi.cur.ypos >= TNTSCAnsi.cur.top_scroll && TNTSCAnsi.cur.ypos < TNTSCAnsi.cur.bottom_scroll;
}

static void one_step(void)
{
    char s[96];
    const int kind = (int)(rnd() % 100);
    if (kind < 22) {   // text, at a column where it cannot reach the last one (they do not wrap there, we do)
        int n = snprintf(s, sizeof s, "\033[%d;%dH", between(1, 25), between(1, 60));
        for (int i = between(1, 15); i > 0; i--)
            s[n++] = (char)between(0x20, 0x7E);
        feed(s, n);
    } else if (kind < 40) {   // the relative moves and the absolute ones
        static const char finals[] = "ABCDEFGd`ea";
        const char f = finals[rnd() % (sizeof finals - 1)];
        if (rnd() % 4 == 0) snprintf(s, sizeof s, "\033[%c", f);
        else snprintf(s, sizeof s, "\033[%d%c", between(1, 30), f);
        feed(s);
    } else if (kind < 47) {
        const char f = rnd() % 2 ? 'H' : 'f';
        switch (rnd() % 4) {
        case 0: snprintf(s, sizeof s, "\033[%c", f); break;
        case 1: snprintf(s, sizeof s, "\033[%d%c", between(1, 30), f); break;
        case 2: snprintf(s, sizeof s, "\033[;%d%c", between(1, 95), f); break;
        default: snprintf(s, sizeof s, "\033[%d;%d%c", between(1, 30), between(1, 95), f); break;
        }
        feed(s);
    } else if (kind < 53) {
        snprintf(s, sizeof s, "\033[%dJ", between(0, 2));
        if (rnd() % 3 == 0) snprintf(s, sizeof s, "\033[J");
        feed(s);
    } else if (kind < 59) {
        snprintf(s, sizeof s, "\033[%dK", between(0, 2));
        if (rnd() % 3 == 0) snprintf(s, sizeof s, "\033[K");
        feed(s);
    } else if (kind < 66) {   // insert and delete lines
        const char f = rnd() % 2 ? 'L' : 'M';
        if (rnd() % 3 == 0) snprintf(s, sizeof s, "\033[%c", f);
        else snprintf(s, sizeof s, "\033[%d%c", between(1, 8), f);
        feed(s);
    } else if (kind < 71) {   // delete characters
        if (rnd() % 3 == 0) snprintf(s, sizeof s, "\033[P");
        else snprintf(s, sizeof s, "\033[%dP", between(1, 20));
        feed(s);
    } else if (kind < 74) {
        feed(rnd() % 2 ? "\033[s" : "\033[u");
    } else if (kind < 79) {   // a scroll region, then home: theirs does not home the cursor and ours does
        if (rnd() % 4 == 0) {
            feed("\033[r\033[H");
        } else {
            const int top = between(1, 24);
            snprintf(s, sizeof s, "\033[%d;%dr\033[H", top, between(top + 1, 25));
            feed(s);
        }
    } else if (kind < 92) {   // the controls
        switch (rnd() % 6) {
        case 0: feed("\r"); break;
        case 1: feed("\t"); break;
        case 2: if (TNTSCAnsi.cur.xpos > 0) feed("\b"); break;   // theirs wraps to the line above at the left edge
        case 3: feed("\f"); break;
        case 4: feed("\0", 1); break;
        default: if (in_region()) feed("\n"); break;
        }
    } else {
        if (!in_region())
            return;
        switch (rnd() % 3) {
        case 0: feed("\033D"); break;
        case 1: feed("\033M"); break;
        default: feed("\033E"); break;
        }
    }
    compare();
}

int main(int argc, char **argv)
{
    const int sequences = argc > 1 ? atoi(argv[1]) : 3000;
    for (int i = 0; i < sequences; i++) {
        begin();
        const int steps = between(10, 80);
        for (int j = 0; j < steps; j++)
            one_step();
    }
    printf("all %d comparisons with TNTSCAnsi matched (%d sequences)\n", checks, sequences);
    return 0;
}
