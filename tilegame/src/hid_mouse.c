/* SPDX-License-Identifier: MIT */
#include "hid_mouse.h"

bool hid_mouse_parse(const uint8_t *report, size_t length, hid_mouse_report_t *out)
{
    if (!report || !out || length < 3)
        return false;
    out->buttons = report[0] & 7;
    out->dx = (int8_t)report[1];
    out->dy = (int8_t)report[2];
    out->wheel = length >= 4 ? (int8_t)report[3] : 0;
    return true;
}
