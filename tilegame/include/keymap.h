/* SPDX-License-Identifier: MIT */
/*
 * Keyboard to characters: turns the reports of a USB boot keyboard into a stream of typed characters, for the US layout.
 *
 * A printable key gives its ASCII character (shift and caps lock applied, and the keypad digits while num lock is on), Enter gives 10,
 * Backspace 8, Tab 9, Escape 27 and Delete 127, and Ctrl with a letter (or one of  @ [ \ ] ^ _  or space) gives the control code 0..31. The
 * cursor keys, Home, End, Page up and down, Insert and F1..F12 give the PLAT_KEY_* codes (256 and up, see platform.h). Alt and the
 * Windows key do nothing. A key that is held repeats, as on a PC: after PLAT_KEY_REPEAT_DELAY_MS, then every PLAT_KEY_REPEAT_MS, and the
 * last key pressed is the one that repeats.
 *
 * Each keyboard has a keymap_t (what it had down, its lock keys, its repeat); the characters of all of them go into one queue.
 * The caps lock and num lock LEDs are not driven. Num lock is on at the start, as on a PC.
 */
#ifndef KEYMAP_H
#define KEYMAP_H

#include <stdint.h>

#include "platform.h"

#define PLAT_KEY_REPEAT_DELAY_MS 500
#define PLAT_KEY_REPEAT_MS 33
#define KEYMAP_QUEUE 64   /* characters (and, separately, key events) waiting to be read; further ones are dropped */

typedef struct {
    uint8_t modifiers, keys[6];    /* the last report */
    uint8_t caps_lock, num_lock;
    uint8_t repeat_key;            /* the usage code that is repeating (0: none) and what it types */
    int16_t repeat_char;
    uint32_t next_repeat_ms;
} keymap_t;

void keymap_init(keymap_t *k);

/* An 8 byte boot keyboard report (modifiers, reserved, six key codes); anything shorter, or a rollover error report, is ignored. */
void keymap_report(keymap_t *k, const uint8_t *report, int len, uint32_t now_ms);

/* Call often (every few milliseconds will do): types the repeats of a held key. */
void keymap_tick(keymap_t *k, uint32_t now_ms);

/* The keyboard was unplugged: nothing is held any more. */
void keymap_release_all(keymap_t *k);

/* The characters typed, oldest first: -1 when there are none. keymap_flush throws away what is waiting. */
int keymap_getchar(void);
void keymap_flush(void);

/* The same key presses as events (plat_key_event_t in platform.h): which key, the modifiers and lock keys at the time. Includes the lock keys themselves
 * and the repeats of a held key; modifier keys alone do not make events. 1 and the event, or 0 when none is waiting. */
int keymap_getevent(plat_key_event_t *event);

/* What a key would type with these modifiers, or -1: the layout on its own, for tests. caps and num are the lock states (0 or 1). */
int keymap_translate(uint8_t usage, uint8_t modifiers, int caps, int num);

#endif
