/* SPDX-License-Identifier: MIT */
/* USB mice in boot protocol: a report of buttons, then movement. Portable, so it is tested on the desktop. */
#ifndef HID_MOUSE_H
#define HID_MOUSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t buttons;   /* bit 0 left, bit 1 right, bit 2 middle */
    int8_t dx, dy;     /* movement since the last report; x to the right, y downwards */
    int8_t wheel;      /* positive is away from the user; 0 if the mouse has no wheel report */
} usb_mouse_report_t;

/* Parse a boot mouse report (3 bytes, or 4 with a wheel). False, leaving *out alone, if it is shorter than 3 bytes. */
bool hid_mouse_parse(const uint8_t *report, size_t length, usb_mouse_report_t *out);

#endif
