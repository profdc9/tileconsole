/* SPDX-License-Identifier: MIT */
/* Tests of the HID gamepad descriptor parser and report decoder, and of the mouse report parser. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hid_mouse.h"
#include "hid_pad.h"

static int checks;
#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* A: a plain pad. 16 buttons, an 8-way hat (0..7, 8 and up means nothing), X and Y as unsigned bytes. 5 bytes a report. */
static const uint8_t desc_simple[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,                   /* Game Pad, application */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02,   /* buttons 1..16 */
    0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,               /* hat, with a null state */
    0x75, 0x04, 0x95, 0x01, 0x81, 0x03,                                                               /* 4 bits of padding */
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02,   /* X, Y */
    0xC0,
};

/* B: report ID 1 holds the pad (12 buttons, hat 1..8, X Y as signed 16 bit); report ID 2 is something else with one button. */
static const uint8_t desc_ids[] = {
    0x05, 0x01, 0x09, 0x04, 0xA1, 0x01,                   /* Joystick, application */
    0x85, 0x01,                                           /* report ID 1 */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0C, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0C, 0x81, 0x02,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x39, 0x15, 0x01, 0x25, 0x08, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,   /* hat 1..8 */
    0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02,   /* X Y: -32768..32767 */
    0x85, 0x02,                                           /* report ID 2 */
    0x05, 0x09, 0x09, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
    0x75, 0x07, 0x95, 0x01, 0x81, 0x03,
    0xC0,
};

/* C: a D-pad as four "buttons" (usages 0x90..0x93) plus six ordinary buttons. */
static const uint8_t desc_dpad[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
    0x05, 0x01, 0x19, 0x90, 0x29, 0x93, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02,   /* up down right left */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x06, 0x75, 0x01, 0x95, 0x06, 0x81, 0x02,
    0x75, 0x06, 0x95, 0x01, 0x81, 0x03,
    0xC0,
};

/* D: a mouse and a keyboard: not pads. */
static const uint8_t desc_mouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81,
    0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0,
};
static const uint8_t desc_keyboard[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
    0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x03, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00,
    0x29, 0x65, 0x81, 0x00, 0xC0,
};

/* E: a keyboard on report ID 1 and a pad on report ID 2 (as a composite device has them). */
static const uint8_t desc_composite[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
    0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x03, 0xC0,
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x02, 0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
    0x95, 0x08, 0x81, 0x02, 0xC0,
};

/* F: global state pushed and popped; usages given one by one; a 4-byte (extended) usage; a 1-byte logical maximum of 0xFF
 * that has to be read as 255; a 4-way hat 1..4; a long item that is skipped. */
static const uint8_t desc_features[] = {
    0x05, 0x01, 0x09, 0x04, 0xA1, 0x01,
    0xA4, 0x05, 0x09, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x09, 0x01, 0x09, 0x02, 0x09, 0x03, 0x09, 0x04, 0x95, 0x04, 0x81, 0x02,   /* push; buttons 1-4 by usage */
    0xB4,                                                                                                                           /* pop: back to usage page 1 */
    0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
    0xFE, 0x03, 0x10, 0x81, 0x02, 0xC0,                                                                                             /* a long item */
    0x0B, 0x30, 0x00, 0x01, 0x00, 0x15, 0x00, 0x25, 0xFF, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,                                       /* X as a 4-byte usage, max 0xFF in one byte */
    0x09, 0x31, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,                                                   /* Y: relies on the pop to bring back the Generic Desktop page */
    0x09, 0x39, 0x15, 0x01, 0x25, 0x04, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,                                                         /* a 4-way hat */
    0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
    0xC0,
};

static hid_pad_state_t decode_ok(const hid_pad_t *p, const uint8_t *r, size_t n)
{
    hid_pad_state_t s = {0xDEAD, 0xFF};
    CHECK(hid_pad_decode(p, r, n, &s));
    return s;
}

static void test_simple_pad(void)
{
    hid_pad_t p;
    CHECK(hid_pad_parse_descriptor(desc_simple, sizeof desc_simple, &p));
    CHECK(!p.has_id && p.nfields == 16 + 1 + 2);
    CHECK(p.f[0].kind == HF_BUTTON && p.f[0].index == 1 && p.f[0].bit_offset == 0 && p.f[0].bit_size == 1);
    CHECK(p.f[15].kind == HF_BUTTON && p.f[15].index == 16 && p.f[15].bit_offset == 15);
    CHECK(p.f[16].kind == HF_HAT && p.f[16].bit_offset == 16 && p.f[16].bit_size == 4 && p.f[16].min == 0 && p.f[16].max == 7);
    CHECK(p.f[17].kind == HF_AXIS_X && p.f[17].bit_offset == 24 && p.f[17].bit_size == 8 && p.f[17].max == 255);
    CHECK(p.f[18].kind == HF_AXIS_Y && p.f[18].bit_offset == 32);

    /* Nothing pressed: sticks centred, hat at its null value. */
    uint8_t r[5] = {0, 0, 0x08, 127, 128};
    hid_pad_state_t s = decode_ok(&p, r, 5);
    CHECK(s.buttons == 0 && s.dpad == 0);
    /* Buttons 1, 3 and 16. */
    r[0] = 0x05;
    r[1] = 0x80;
    s = decode_ok(&p, r, 5);
    CHECK(s.buttons == (1u | 4u | 0x8000u) && s.dpad == 0);
    /* The hat, in all eight directions. */
    static const uint8_t want[8] = {HID_DPAD_UP, HID_DPAD_UP | HID_DPAD_RIGHT, HID_DPAD_RIGHT, HID_DPAD_DOWN | HID_DPAD_RIGHT,
                                    HID_DPAD_DOWN, HID_DPAD_DOWN | HID_DPAD_LEFT, HID_DPAD_LEFT, HID_DPAD_UP | HID_DPAD_LEFT};
    r[0] = r[1] = 0;
    for (int h = 0; h < 8; h++) {
        r[2] = (uint8_t)h;
        CHECK(decode_ok(&p, r, 5).dpad == want[h]);
    }
    for (int h = 8; h < 16; h++) {                      /* every value past the range is "nothing" */
        r[2] = (uint8_t)h;
        CHECK(decode_ok(&p, r, 5).dpad == 0);
    }
    /* The stick: well off centre counts, a little does not. */
    r[2] = 0x08;
    r[3] = 0;   r[4] = 127;
    CHECK(decode_ok(&p, r, 5).dpad == HID_DPAD_LEFT);
    r[3] = 255; r[4] = 127;
    CHECK(decode_ok(&p, r, 5).dpad == HID_DPAD_RIGHT);
    r[3] = 127; r[4] = 0;
    CHECK(decode_ok(&p, r, 5).dpad == HID_DPAD_UP);
    r[3] = 127; r[4] = 255;
    CHECK(decode_ok(&p, r, 5).dpad == HID_DPAD_DOWN);
    r[3] = 255; r[4] = 255;
    CHECK(decode_ok(&p, r, 5).dpad == (HID_DPAD_RIGHT | HID_DPAD_DOWN));
    r[3] = 100; r[4] = 150;                             /* a gentle push: dead zone */
    CHECK(decode_ok(&p, r, 5).dpad == 0);
    r[3] = 76; r[4] = 178;                              /* just inside the dead zone */
    CHECK(decode_ok(&p, r, 5).dpad == (HID_DPAD_LEFT));  /* 76 is at the 30% mark: counts as left; 178 is not yet down */
    /* The hat and the stick together add up. */
    r[2] = 0x02;
    r[3] = 127; r[4] = 255;
    CHECK(decode_ok(&p, r, 5).dpad == (HID_DPAD_RIGHT | HID_DPAD_DOWN));

    /* A short report: the fields it does not reach are left out, but the ones it does are used. */
    r[0] = 0x01; r[1] = 0; r[2] = 0x00;
    hid_pad_state_t st = decode_ok(&p, r, 3);
    CHECK(st.buttons == 1 && st.dpad == HID_DPAD_UP);
    hid_pad_state_t untouched = {0x1234, 0x56};
    CHECK(!hid_pad_decode(&p, r, 0, &untouched) && untouched.buttons == 0x1234);
    CHECK(!hid_pad_decode(&p, NULL, 5, &untouched) && !hid_pad_decode(NULL, r, 5, &untouched) && !hid_pad_decode(&p, r, 5, NULL));
}

static void test_report_ids_and_signed_axes(void)
{
    hid_pad_t p;
    CHECK(hid_pad_parse_descriptor(desc_ids, sizeof desc_ids, &p));
    CHECK(p.has_id && p.report_id == 1);                /* the report with the most pad fields, not the one with a single button */
    CHECK(p.nfields == 12 + 1 + 2);
    CHECK(p.f[12].kind == HF_HAT && p.f[12].bit_offset == 16 && p.f[12].min == 1 && p.f[12].max == 8);
    CHECK(p.f[13].kind == HF_AXIS_X && p.f[13].bit_offset == 24 && p.f[13].bit_size == 16 && p.f[13].min == -32768 && p.f[13].max == 32767);
    CHECK(p.f[14].kind == HF_AXIS_Y && p.f[14].bit_offset == 40);

    /* ID, 12 buttons + 4 padding bits, hat + 4 padding bits, X, Y (little endian): 1 + 2 + 1 + 4 = 8 bytes. */
    uint8_t r[8] = {1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    hid_pad_state_t s = decode_ok(&p, r, 8);
    CHECK(s.buttons == 0 && s.dpad == 0);               /* hat 0 is outside 1..8: nothing; X = Y = 0 is dead centre for a signed axis */
    r[1] = 0x02; r[2] = 0x08;                           /* buttons 2 and 12 */
    r[3] = 0x03;                                        /* hat 3: east */
    s = decode_ok(&p, r, 8);
    CHECK(s.buttons == (2u | 0x800u) && s.dpad == HID_DPAD_RIGHT);
    r[3] = 0x01;
    CHECK(decode_ok(&p, r, 8).dpad == HID_DPAD_UP);     /* 1 is the first direction */
    r[3] = 0x08;
    CHECK(decode_ok(&p, r, 8).dpad == (HID_DPAD_UP | HID_DPAD_LEFT));
    /* X at -32768 is left, 32767 right; -1000 and +1000 are inside the dead zone. */
    r[3] = 0;
    r[4] = 0x00; r[5] = 0x80;
    CHECK(decode_ok(&p, r, 8).dpad == HID_DPAD_LEFT);
    r[4] = 0xFF; r[5] = 0x7F;
    CHECK(decode_ok(&p, r, 8).dpad == HID_DPAD_RIGHT);
    r[4] = 0x18; r[5] = 0xFC;                           /* -1000 */
    CHECK(decode_ok(&p, r, 8).dpad == 0);
    r[4] = 0xE8; r[5] = 0x03;                           /* +1000 */
    CHECK(decode_ok(&p, r, 8).dpad == 0);
    r[4] = r[5] = 0;
    r[6] = 0x00; r[7] = 0x80;                           /* Y = -32768: up */
    CHECK(decode_ok(&p, r, 8).dpad == HID_DPAD_UP);
    /* Another report ID is not the pad's. */
    r[0] = 2;
    hid_pad_state_t keep = {7, 7};
    CHECK(!hid_pad_decode(&p, r, 8, &keep) && keep.buttons == 7);
    r[0] = 0;
    CHECK(!hid_pad_decode(&p, r, 8, &keep));
}

static void test_dpad_usages_and_others(void)
{
    hid_pad_t p;
    CHECK(hid_pad_parse_descriptor(desc_dpad, sizeof desc_dpad, &p));
    CHECK(p.nfields == 4 + 6);
    uint8_t r[1] = {0x01};                              /* up */
    CHECK(decode_ok(&p, r, 1).dpad == HID_DPAD_UP);
    r[0] = 0x02;
    CHECK(decode_ok(&p, r, 1).dpad == HID_DPAD_DOWN);
    r[0] = 0x04;                                        /* usage 0x92 is right... */
    CHECK(decode_ok(&p, r, 1).dpad == HID_DPAD_RIGHT);
    r[0] = 0x08;                                        /* ...and 0x93 is left */
    CHECK(decode_ok(&p, r, 1).dpad == HID_DPAD_LEFT);
    r[0] = 0x05;
    CHECK(decode_ok(&p, r, 1).dpad == (HID_DPAD_UP | HID_DPAD_RIGHT));
    uint8_t r2[2] = {0x00, 0x00};                       /* buttons start after the 4 D-pad bits */
    r2[0] = 0x30;                                       /* bits 4 and 5: buttons 1 and 2 */
    CHECK(decode_ok(&p, r2, 2).buttons == 3 && decode_ok(&p, r2, 2).dpad == 0);

    hid_pad_t unused = {.nfields = 99};
    CHECK(!hid_pad_parse_descriptor(desc_mouse, sizeof desc_mouse, &unused) && unused.nfields == 99);
    CHECK(!hid_pad_parse_descriptor(desc_keyboard, sizeof desc_keyboard, &unused) && unused.nfields == 99);
    CHECK(!hid_pad_parse_descriptor(NULL, 10, &unused) && !hid_pad_parse_descriptor(desc_simple, sizeof desc_simple, NULL));
    CHECK(!hid_pad_parse_descriptor(desc_simple, 0, &unused));

    /* A composite device: the pad is found among the others, and its offsets count from its own report. */
    CHECK(hid_pad_parse_descriptor(desc_composite, sizeof desc_composite, &p));
    CHECK(p.has_id && p.report_id == 2 && p.nfields == 8 && p.f[0].bit_offset == 0);
    uint8_t rc[2] = {2, 0x81};
    hid_pad_state_t s = decode_ok(&p, rc, 2);
    CHECK(s.buttons == (1u | 0x80u));
    rc[0] = 1;                                           /* the keyboard's report is not the pad's */
    CHECK(!hid_pad_decode(&p, rc, 2, &s));
}

static void test_descriptor_features(void)
{
    hid_pad_t p;
    CHECK(hid_pad_parse_descriptor(desc_features, sizeof desc_features, &p));
    /* buttons 1-4 by single usages (push/pop kept the later items on the Generic Desktop page), then 4 bits of padding,
     * X as an extended usage, Y, and the 4-way hat. */
    CHECK(p.nfields == 4 + 1 + 1 + 1);
    CHECK(p.f[0].kind == HF_BUTTON && p.f[0].index == 1 && p.f[3].kind == HF_BUTTON && p.f[3].index == 4);
    CHECK(p.f[4].kind == HF_AXIS_X && p.f[4].bit_offset == 8 && p.f[4].max == 255);       /* 0xFF in one byte is 255, not -1 */
    CHECK(p.f[5].kind == HF_AXIS_Y && p.f[5].bit_offset == 16);
    CHECK(p.f[6].kind == HF_HAT && p.f[6].min == 1 && p.f[6].max == 4 && p.f[6].bit_offset == 24);
    uint8_t r[4] = {0x0A, 0, 255, 0x02};                 /* buttons 2 and 4; X centre, Y... */
    r[1] = 127; r[2] = 127; r[3] = 0x02;                 /* 4-way hat value 2: east */
    hid_pad_state_t s = decode_ok(&p, r, 4);
    CHECK(s.buttons == 0x0A && s.dpad == HID_DPAD_RIGHT);
    r[3] = 0x01;
    CHECK(decode_ok(&p, r, 4).dpad == HID_DPAD_UP);
    r[3] = 0x03;
    CHECK(decode_ok(&p, r, 4).dpad == HID_DPAD_DOWN);
    r[3] = 0x04;
    CHECK(decode_ok(&p, r, 4).dpad == HID_DPAD_LEFT);
    r[3] = 0x00;                                         /* below the range: nothing */
    CHECK(decode_ok(&p, r, 4).dpad == 0);
    r[3] = 0x0F;
    CHECK(decode_ok(&p, r, 4).dpad == 0);
}

static void test_mapping(void)
{
    const hid_pad_map_t *def = hid_pad_map_for(0x1234, 0x5678);
    CHECK(def->a == 1 && def->b == 2 && def->x == 3 && def->y == 4 && def->l == 5 && def->r == 6 && def->select == 7 && def->start == 8);
    hid_pad_state_t s = {0xFF, 0};
    CHECK(hid_pad_to_buttons(&s, def) == (PLAT_BTN_A | PLAT_BTN_B | PLAT_BTN_X | PLAT_BTN_Y | PLAT_BTN_L | PLAT_BTN_R | PLAT_BTN_SELECT | PLAT_BTN_START));
    s = (hid_pad_state_t){0x01, HID_DPAD_UP | HID_DPAD_LEFT};
    CHECK(hid_pad_to_buttons(&s, def) == (PLAT_BTN_A | PLAT_BTN_UP | PLAT_BTN_LEFT));
    s = (hid_pad_state_t){0, HID_DPAD_DOWN | HID_DPAD_RIGHT};
    CHECK(hid_pad_to_buttons(&s, def) == (PLAT_BTN_DOWN | PLAT_BTN_RIGHT));
    s = (hid_pad_state_t){0x100, 0};                      /* button 9 is not mapped by default */
    CHECK(hid_pad_to_buttons(&s, def) == 0);

    /* Sony pads: Cross (button 2) is the bottom button, which is A here. */
    const hid_pad_map_t *ds4 = hid_pad_map_for(0x054C, 0x09CC);
    CHECK(ds4 != def && hid_pad_map_for(0x054C, 0x05C4) == ds4 && hid_pad_map_for(0x054C, 0x0CE6) == ds4);
    s = (hid_pad_state_t){0x02, 0};
    CHECK(hid_pad_to_buttons(&s, ds4) == PLAT_BTN_A);
    s = (hid_pad_state_t){0x01, 0};                       /* Square */
    CHECK(hid_pad_to_buttons(&s, ds4) == PLAT_BTN_X);
    s = (hid_pad_state_t){0x300, 0};                      /* Share and Options */
    CHECK(hid_pad_to_buttons(&s, ds4) == (PLAT_BTN_SELECT | PLAT_BTN_START));
    CHECK(hid_pad_map_for(0x054C, 0x0000) == def && hid_pad_map_for(0x0000, 0x09CC) == def);

    /* A map entry of 0 means "none", and entries past 32 never match. */
    hid_pad_map_t sparse = {0};
    s = (hid_pad_state_t){0xFFFFFFFFu, 0};
    CHECK(hid_pad_to_buttons(&s, &sparse) == 0);
    sparse.a = 33;
    CHECK(hid_pad_to_buttons(&s, &sparse) == 0);
    sparse.a = 32;
    s = (hid_pad_state_t){0x80000000u, 0};
    CHECK(hid_pad_to_buttons(&s, &sparse) == PLAT_BTN_A);
}

static uint32_t rng = 2024;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

/* Whatever the device sends, nothing breaks: every prefix of a good descriptor, and random bytes, as descriptors and reports. */
static void test_robustness(void)
{
    const struct { const uint8_t *d; size_t n; } good[] = {
        {desc_simple, sizeof desc_simple}, {desc_ids, sizeof desc_ids},           {desc_dpad, sizeof desc_dpad},
        {desc_mouse, sizeof desc_mouse},   {desc_keyboard, sizeof desc_keyboard}, {desc_composite, sizeof desc_composite},
        {desc_features, sizeof desc_features},
    };
    for (size_t g = 0; g < ARRAY_LEN(good); g++)
        for (size_t n = 0; n <= good[g].n; n++) {
            hid_pad_t p;
            memset(&p, 0xAA, sizeof p);
            if (hid_pad_parse_descriptor(good[g].d, n, &p)) {
                CHECK(p.nfields > 0 && p.nfields <= HID_PAD_MAX_FIELDS);
                for (unsigned k = 0; k < p.nfields; k++)
                    CHECK(p.f[k].bit_size >= 1 && p.f[k].bit_size <= 32 && p.f[k].kind >= HF_BUTTON && p.f[k].kind <= HF_DPAD_RIGHT);
            }
        }
    /* A cut in the middle of an item is refused, not guessed at. */
    hid_pad_t p;
    CHECK(!hid_pad_parse_descriptor(desc_simple, 11, &p));   /* ends inside "0x19, 0x01, 0x29, 0x10" */
    CHECK(!hid_pad_parse_descriptor((const uint8_t[]){0x05}, 1, &p));
    CHECK(!hid_pad_parse_descriptor((const uint8_t[]){0xFE, 0x05, 0x01}, 3, &p));   /* a long item that claims more than there is */

    /* Random descriptors: mostly well formed (a gamepad header, then blocks of plausible items with random sizes, counts, ranges and
     * usages), and then damaged: bytes flipped, or cut short. This reaches the interesting code far more often than noise does. */
    hid_pad_t ok_pad;
    CHECK(hid_pad_parse_descriptor(desc_simple, sizeof desc_simple, &ok_pad));
    int found = 0;
    for (int trial = 0; trial < 300000; trial++) {
        uint8_t d[160];
        size_t n = 0;
        static const uint8_t header[] = {0x05, 0x01, 0x09, 0x05, 0xA1, 0x01};
        memcpy(d, header, sizeof header);
        n = sizeof header;
        const int blocks = 1 + (int)(rnd() % 6);
        for (int b = 0; b < blocks && n + 24 < sizeof d; b++) {
            switch (rnd() % 6) {
            case 0:   /* buttons by usage range */
                d[n++] = 0x05; d[n++] = 0x09; d[n++] = 0x19; d[n++] = 0x01; d[n++] = 0x29; d[n++] = (uint8_t)(1 + rnd() % 20);
                d[n++] = 0x15; d[n++] = 0x00; d[n++] = 0x25; d[n++] = 0x01; d[n++] = 0x75; d[n++] = 0x01;
                d[n++] = 0x95; d[n++] = (uint8_t)(1 + rnd() % 20); d[n++] = 0x81; d[n++] = (uint8_t)(rnd() & 7);
                break;
            case 1:   /* a hat */
                d[n++] = 0x05; d[n++] = 0x01; d[n++] = 0x09; d[n++] = 0x39; d[n++] = 0x15; d[n++] = (uint8_t)(rnd() & 1);
                d[n++] = 0x25; d[n++] = (uint8_t)(3 + rnd() % 6); d[n++] = 0x75; d[n++] = (uint8_t)(1 + rnd() % 8);
                d[n++] = 0x95; d[n++] = 0x01; d[n++] = 0x81; d[n++] = (uint8_t)(rnd() & 0x47);
                break;
            case 2:   /* two axes */
                d[n++] = 0x05; d[n++] = 0x01; d[n++] = 0x09; d[n++] = 0x30; d[n++] = 0x09; d[n++] = 0x31; d[n++] = 0x15; d[n++] = 0x81;
                d[n++] = 0x25; d[n++] = 0x7F; d[n++] = 0x75; d[n++] = (uint8_t)(rnd() % 3 ? 8 : 16); d[n++] = 0x95; d[n++] = 0x02;
                d[n++] = 0x81; d[n++] = (uint8_t)(rnd() & 7);
                break;
            case 3:   /* a report ID */
                d[n++] = 0x85; d[n++] = (uint8_t)(rnd() % 4);
                break;
            case 4:   /* padding */
                d[n++] = 0x75; d[n++] = (uint8_t)(1 + rnd() % 7); d[n++] = 0x95; d[n++] = 0x01; d[n++] = 0x81; d[n++] = 0x03;
                break;
            default:  /* push or pop */
                d[n++] = (rnd() & 1) ? 0xA4 : 0xB4;
                break;
            }
        }
        d[n++] = 0xC0;
        const unsigned damage = rnd() % 4;
        if (damage == 1)
            for (int k = 0; k < 3; k++)
                d[rnd() % n] = (uint8_t)rnd();
        else if (damage == 2)
            n = rnd() % n;
        hid_pad_t q;
        if (hid_pad_parse_descriptor(d, n, &q)) {
            found++;
            CHECK(q.nfields > 0 && q.nfields <= HID_PAD_MAX_FIELDS);
            uint8_t rep[24];
            for (size_t i = 0; i < sizeof rep; i++)
                rep[i] = (uint8_t)rnd();
            hid_pad_state_t s;
            hid_pad_decode(&q, rep, rnd() % sizeof rep, &s);
            if (hid_pad_decode(&q, rep, sizeof rep, &s))
                CHECK((s.dpad & ~0x0F) == 0);
        }
        uint8_t rep[24];                                   /* random reports against a real pad */
        for (size_t i = 0; i < sizeof rep; i++)
            rep[i] = (uint8_t)rnd();
        hid_pad_state_t s;
        if (hid_pad_decode(&ok_pad, rep, rnd() % sizeof rep, &s))
            CHECK((s.dpad & ~0x0F) == 0);
    }
    CHECK(found > 50000);                                    /* the random descriptors did reach the interesting code */
    printf("  %d of 300000 random descriptors parsed as pads\n", found);

    /* An enormous report count does not hang or overflow. */
    static const uint8_t huge[] = {0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0xFF, 0x75, 0x01, 0x96, 0xFF, 0xFF, 0x81, 0x02, 0xC0};
    hid_pad_parse_descriptor(huge, sizeof huge, &p);
}

static void test_mouse(void)
{
    hid_mouse_report_t m = {9, 9, 9, 9};
    CHECK(hid_mouse_parse((const uint8_t[]){0x01, 5, 0xFB}, 3, &m));
    CHECK(m.buttons == 1 && m.dx == 5 && m.dy == -5 && m.wheel == 0);
    CHECK(hid_mouse_parse((const uint8_t[]){0x07, 0x80, 0x7F, 0xFF}, 4, &m));
    CHECK(m.buttons == 7 && m.dx == -128 && m.dy == 127 && m.wheel == -1);
    CHECK(hid_mouse_parse((const uint8_t[]){0xFF, 0, 0, 1}, 4, &m) && m.buttons == 7);   /* only three buttons are kept */
    CHECK(hid_mouse_parse((const uint8_t[]){0x02, 1, 2, 3, 4, 5, 6}, 7, &m) && m.buttons == 2 && m.wheel == 3);   /* longer reports are fine */
    hid_mouse_report_t keep = {1, 2, 3, 4};
    CHECK(!hid_mouse_parse((const uint8_t[]){1, 2}, 2, &keep) && keep.dx == 2);
    CHECK(!hid_mouse_parse(NULL, 4, &keep) && !hid_mouse_parse((const uint8_t[]){1, 2, 3}, 3, NULL) && !hid_mouse_parse((const uint8_t[]){1, 2, 3}, 0, &keep));
}

int main(void)
{
    test_simple_pad();
    test_report_ids_and_signed_axes();
    test_dpad_usages_and_others();
    test_descriptor_features();
    test_mapping();
    test_robustness();
    test_mouse();
    printf("all %d checks passed\n", checks);
    return 0;
}
