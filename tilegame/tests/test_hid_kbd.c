/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hid_kbd.h"

static int checks;
#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static plat_buttons_t parse(const uint8_t *r, int len, plat_buttons_t before, bool *ok)
{
    plat_buttons_t b = before;
    *ok = hid_kbd_parse(r, len, &b);
    return b;
}

int main(void)
{
    bool ok;
    uint8_t r[8] = {0};

    /* No keys: no buttons. */
    CHECK(parse(r, 8, 0xFFFF, &ok) == 0 && ok);

    /* Each mapped key alone. */
    static const struct { uint8_t usage; plat_buttons_t b; } keys[] = {
        {0x52, PLAT_BTN_UP}, {0x51, PLAT_BTN_DOWN}, {0x50, PLAT_BTN_LEFT}, {0x4F, PLAT_BTN_RIGHT},
        {0x1D, PLAT_BTN_B},  {0x1B, PLAT_BTN_A},    {0x04, PLAT_BTN_X},    {0x16, PLAT_BTN_Y},
        {0x28, PLAT_BTN_START}, {0x58, PLAT_BTN_START},
    };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        memset(r, 0, 8);
        r[2] = keys[i].usage;
        CHECK(parse(r, 8, 0, &ok) == keys[i].b && ok);
        CHECK(hid_kbd_button_for_usage(keys[i].usage) == keys[i].b);
        /* In any of the six key slots. */
        for (int slot = 2; slot < 8; slot++) {
            memset(r, 0, 8);
            r[slot] = keys[i].usage;
            CHECK(parse(r, 8, 0, &ok) == keys[i].b && ok);
        }
    }

    /* The modifier byte: only Right Shift means anything (Select). */
    for (int m = 0; m < 256; m++) {
        memset(r, 0, 8);
        r[0] = (uint8_t)m;
        CHECK(parse(r, 8, 0, &ok) == ((m & 0x20) ? PLAT_BTN_SELECT : 0) && ok);
    }

    /* Several keys at once, with a modifier and keys that mean nothing. */
    const uint8_t chord[8] = {0x22, 0, 0x4F, 0x52, 0x1D, 0x05 /* B: not mapped */, 0x28, 0};
    CHECK(parse(chord, 8, 0, &ok) == (PLAT_BTN_RIGHT | PLAT_BTN_UP | PLAT_BTN_B | PLAT_BTN_START | PLAT_BTN_SELECT) && ok);

    /* Releasing keys shows up as a smaller set in the next report. */
    const uint8_t later[8] = {0, 0, 0x4F, 0, 0, 0, 0, 0};
    CHECK(parse(later, 8, 0xFFFF, &ok) == PLAT_BTN_RIGHT && ok);

    /* Unmapped usages give nothing. */
    for (int u = 0; u < 256; u++) {
        int mapped = 0;
        for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
            mapped |= keys[i].usage == u;
        if (!mapped)
            CHECK(hid_kbd_button_for_usage((uint8_t)u) == 0);
    }

    /* A rollover error says nothing about which keys are down: refuse it, leave the caller's state alone. */
    const uint8_t rollover[8] = {0, 0, 1, 1, 1, 1, 1, 1};
    CHECK(parse(rollover, 8, 0x1234, &ok) == 0x1234 && !ok);
    const uint8_t rollover_one[8] = {0, 0, 0x4F, 0, 0, 0, 0, 1};
    CHECK(parse(rollover_one, 8, 0x1234, &ok) == 0x1234 && !ok);

    /* A short report, or none at all, is refused too; a longer one is fine. */
    CHECK(parse(chord, 7, 0x1234, &ok) == 0x1234 && !ok);
    CHECK(parse(chord, 0, 0x1234, &ok) == 0x1234 && !ok);
    plat_buttons_t keep = 0x77;
    CHECK(!hid_kbd_parse(NULL, 8, &keep) && keep == 0x77);
    uint8_t longer[12] = {0, 0, 0x52, 0, 0, 0, 0, 0, 0x4F, 0x4F, 0x4F, 0x4F};
    CHECK(parse(longer, 12, 0, &ok) == PLAT_BTN_UP && ok);   /* only the first 8 bytes count */

    printf("all %d checks passed\n", checks);
    return 0;
}
