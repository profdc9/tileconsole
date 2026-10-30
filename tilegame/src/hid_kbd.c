/* SPDX-License-Identifier: MIT */
#include "hid_kbd.h"

#define HID_MOD_RIGHT_SHIFT 0x20
#define HID_ERROR_ROLLOVER 0x01

plat_buttons_t hid_kbd_button_for_usage(uint8_t usage)
{
    switch (usage) {
    case 0x52: return PLAT_BTN_UP;      /* up arrow */
    case 0x51: return PLAT_BTN_DOWN;
    case 0x50: return PLAT_BTN_LEFT;
    case 0x4F: return PLAT_BTN_RIGHT;
    case 0x1D: return PLAT_BTN_A;       /* Z */
    case 0x1B: return PLAT_BTN_B;       /* X */
    case 0x04: return PLAT_BTN_X;       /* A */
    case 0x16: return PLAT_BTN_Y;       /* S */
    case 0x28:                          /* Return */
    case 0x58: return PLAT_BTN_START;   /* keypad Enter */
    default:   return 0;
    }
}

bool hid_kbd_parse(const uint8_t *report, int len, plat_buttons_t *out)
{
    if (!report || len < 8)
        return false;
    for (int i = 2; i < 8; i++)
        if (report[i] == HID_ERROR_ROLLOVER)
            return false;
    plat_buttons_t b = (report[0] & HID_MOD_RIGHT_SHIFT) ? PLAT_BTN_SELECT : 0;
    for (int i = 2; i < 8; i++)
        b |= hid_kbd_button_for_usage(report[i]);
    *out = b;
    return true;
}
