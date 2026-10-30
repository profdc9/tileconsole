/* SPDX-License-Identifier: MIT */
/*
 * The input report of an XInput (Xbox 360 style) wired gamepad, as it arrives on the pad's interrupt endpoint: 20 bytes,
 *
 *   0 message type (0 = input)   1 length (0x14)
 *   2 D-pad and buttons: up 01, down 02, left 04, right 08, start 10, back 20, left stick click 40, right stick click 80
 *   3 buttons: left shoulder 01, right shoulder 02, guide 04, A 10, B 20, X 40, Y 80
 *   4 left trigger   5 right trigger
 *   6-7 left stick X, 8-9 left stick Y (signed 16 bit, little endian, Y positive is up), 10-13 right stick
 *
 * Portable and free of hardware, so it is tested on the desktop. XInput is not HID: the board reads it through its own USB
 * class driver (board/xinput_host.c).
 */
#ifndef XINPUT_H
#define XINPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "platform.h"

#define XINPUT_STICK_THRESHOLD 10000   /* about 30% of the way over, as for the other pads */

/*
 * The platform's buttons for an input report: the D-pad and the left stick are the directions, A B X Y and the shoulders are
 * themselves, Start is Start and Back is Select. The triggers, the right stick and the stick clicks are not used.
 * Returns false, leaving *out alone, for a report that is not an input message or is too short.
 */
bool xinput_decode(const uint8_t *report, size_t length, plat_buttons_t *out);

#endif
