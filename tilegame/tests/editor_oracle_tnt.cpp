// SPDX-License-Identifier: MIT
// The C side's view of TNTSCAnsi (Daniel Marks' terminal emulator of PicoParanoia, zlib licence), for editor_oracle.c.
#include "TNTSCAnsi.h"

static unsigned char buffer[25 * 80];

extern "C" {
void tnt_begin(void) { TNTSCAnsi.begin(buffer, 25, 80); }
void tnt_put(unsigned char c) { TNTSCAnsi.output_character(c); }
int tnt_cell(int row, int col) { return buffer[row * 80 + col]; }
int tnt_row(void) { return TNTSCAnsi.cur.ypos; }
int tnt_col(void) { return TNTSCAnsi.cur.xpos; }
}
