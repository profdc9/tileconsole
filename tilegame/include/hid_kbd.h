/* SPDX-License-Identifier: MIT */
/*
 * Turning a USB keyboard's boot-protocol report into the platform's buttons. Portable and free of hardware, so it can be
 * tested on the desktop; the board's TinyUSB glue (board/usb_kbd.c) just feeds it reports.
 *
 * The keys match the SDL backend: the arrows are the D-pad, Z X A S are A B X Y, Return (or keypad Enter) is Start and
 * Right Shift is Select.
 */
#ifndef HID_KBD_H
#define HID_KBD_H

#include <stdbool.h>
#include <stdint.h>

#include "platform.h"

/* The button a HID keyboard usage code (HID usage table, keyboard page) stands for, or 0. */
plat_buttons_t hid_kbd_button_for_usage(uint8_t usage);

/*
 * Parse an 8-byte boot keyboard report: modifiers, reserved, then up to six pressed keys. On success stores the buttons
 * that are down and returns true. Returns false, leaving *out alone, for a short report or one that says "too many keys
 * pressed" (rollover error), which carries no information about which keys are down.
 */
bool hid_kbd_parse(const uint8_t *report, int len, plat_buttons_t *out);

#endif
