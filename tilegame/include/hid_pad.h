/* SPDX-License-Identifier: MIT */
/*
 * USB HID gamepads. A pad describes its own reports in a "report descriptor": which bits are buttons, where the hat switch
 * and the sticks are. This reads that description once (hid_pad_parse_descriptor) and then decodes reports with it
 * (hid_pad_decode), then maps the result onto the platform's buttons. Portable and free of hardware, so it is tested on the
 * desktop, including with malformed and random data, since everything here comes from the device.
 *
 * It understands the usual layouts: buttons as single bits, a hat switch (8 or 4 directions, with a "nothing" value), the
 * left stick (X and Y axes, signed or unsigned, 8 or 16 bits) and the D-pad usages (up/down/left/right as buttons), with or
 * without report IDs. It does not cover pads that are not HID at all (Xbox controllers) or need a start-up handshake
 * (Switch Pro).
 */
#ifndef HID_PAD_H
#define HID_PAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "platform.h"

#define HID_PAD_MAX_FIELDS 64

typedef enum { HF_BUTTON = 1, HF_HAT, HF_AXIS_X, HF_AXIS_Y, HF_DPAD_UP, HF_DPAD_DOWN, HF_DPAD_LEFT, HF_DPAD_RIGHT } hid_field_kind_t;

typedef struct {
    uint16_t bit_offset;   /* from the start of the report data (after the report ID byte, if there is one) */
    uint8_t bit_size;
    uint8_t kind;          /* hid_field_kind_t */
    uint8_t index;         /* for a button: its number, from 1 */
    int32_t min, max;      /* the logical range of the value */
} hid_field_t;

typedef struct {
    bool has_id;           /* reports start with a report ID byte */
    uint8_t report_id;     /* the input report that holds the pad's state */
    uint8_t nfields;
    hid_field_t f[HID_PAD_MAX_FIELDS];
} hid_pad_t;

/* What a decoded report says, before it is mapped to the platform's buttons. */
typedef struct {
    uint32_t buttons;      /* HID button n is bit n-1 */
    uint8_t dpad;          /* HID_DPAD_* bits: from the hat, the left stick and the D-pad usages together */
} hid_pad_state_t;

#define HID_DPAD_UP 1
#define HID_DPAD_DOWN 2
#define HID_DPAD_LEFT 4
#define HID_DPAD_RIGHT 8

/*
 * Read a report descriptor. Returns true if it describes a gamepad or joystick (top-level usage Joystick, Game Pad or
 * Multi-axis Controller) with at least one button, hat or stick, and fills *out with the layout of its main input report.
 * Returns false for anything else (mice, keyboards), and for a descriptor that is cut short or malformed.
 */
bool hid_pad_parse_descriptor(const uint8_t *descriptor, size_t length, hid_pad_t *out);

/*
 * Decode one input report. Returns false, leaving *out alone, if it is not the pad's report (the wrong report ID) or is too
 * short to hold any field. Fields that fall beyond the end of a short report are ignored.
 */
bool hid_pad_decode(const hid_pad_t *pad, const uint8_t *report, size_t length, hid_pad_state_t *out);

/* Which HID button numbers stand for the platform's buttons (0 = none). */
typedef struct {
    uint8_t a, b, x, y, l, r, select, start;
} hid_pad_map_t;

/* The map for a pad: a table of known pads by USB vendor and product ID, or the default (1 2 3 4 = A B X Y, 5 6 = shoulders, 7 8 = back and start). */
const hid_pad_map_t *hid_pad_map_for(uint16_t vendor_id, uint16_t product_id);

/* The platform's buttons for a decoded report. */
plat_buttons_t hid_pad_to_buttons(const hid_pad_state_t *state, const hid_pad_map_t *map);

#endif
