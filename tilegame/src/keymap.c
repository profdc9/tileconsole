/* SPDX-License-Identifier: MIT */
#include "keymap.h"

#include <string.h>

#define MOD_CTRL 0x11    /* left or right */
#define MOD_SHIFT 0x22
#define USAGE_CAPS_LOCK 0x39
#define USAGE_NUM_LOCK 0x53
#define ROLLOVER_ERROR 1

static int16_t queue[KEYMAP_QUEUE];
static unsigned queue_head, queue_count;
static plat_key_event_t events[KEYMAP_QUEUE];   /* the key presses as events: a second stream beside the characters */
static unsigned event_head, event_count;

static void push(int ch)
{
    if (queue_count < KEYMAP_QUEUE) {
        queue[(queue_head + queue_count) % KEYMAP_QUEUE] = (int16_t)ch;
        queue_count++;
    }
}

int keymap_getchar(void)
{
    if (!queue_count)
        return -1;
    const int ch = queue[queue_head];
    queue_head = (queue_head + 1) % KEYMAP_QUEUE;
    queue_count--;
    return ch;
}

void keymap_flush(void)
{
    queue_head = queue_count = 0;
    event_head = event_count = 0;
}

static void push_event(uint8_t usage, uint8_t modifiers, const keymap_t *k)
{
    if (event_count < KEYMAP_QUEUE) {
        plat_key_event_t *e = &events[(event_head + event_count) % KEYMAP_QUEUE];
        e->usage = usage;
        e->modifiers = modifiers;
        e->caps_lock = k->caps_lock;
        e->num_lock = k->num_lock;
        event_count++;
    }
}

int keymap_getevent(plat_key_event_t *event)
{
    if (!event_count)
        return 0;
    *event = events[event_head];
    event_head = (event_head + 1) % KEYMAP_QUEUE;
    event_count--;
    return 1;
}

/* The keys from 1 (0x1E) to the slash (0x38), with what each types: the digits, Enter, Escape, Backspace, Tab, space and the punctuation. */
static const uint8_t plain[27] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '\n', 27, 8, 9, ' ', '-', '=', '[', ']', '\\', '#', ';', '\'', '`', ',', '.', '/'};
static const uint8_t shifted[27] = {'!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '\n', 27, 8, 9, ' ', '_', '+', '{', '}', '|', '~', ':', '"', '~', '<', '>', '?'};

int keymap_translate(uint8_t usage, uint8_t modifiers, int caps, int num)
{
    const int shift = (modifiers & MOD_SHIFT) != 0, ctrl = (modifiers & MOD_CTRL) != 0;
    int ch;
    if (usage >= 0x04 && usage <= 0x1D) {   /* the letters */
        ch = 'a' + (usage - 0x04);
        if (ctrl)
            return ch & 0x1F;
        return shift != (caps != 0) ? ch - 'a' + 'A' : ch;
    }
    if (usage >= 0x1E && usage <= 0x38) {
        ch = shift ? shifted[usage - 0x1E] : plain[usage - 0x1E];
    } else if (usage == 0x64) {   /* the extra key next to the left shift on some keyboards */
        ch = shift ? '|' : '\\';
    } else if (usage >= 0x3A && usage <= 0x45) {
        return PLAT_KEY_F(usage - 0x39);
    } else if (usage >= 0x54 && usage <= 0x63) {   /* the keypad: digits with num lock, the cursor keys without */
        static const int16_t keypad_symbols[5] = {'/', '*', '-', '+', '\n'};
        static const int16_t navigation[9] = {PLAT_KEY_END, PLAT_KEY_DOWN, PLAT_KEY_PAGEDOWN, PLAT_KEY_LEFT, -1, PLAT_KEY_RIGHT, PLAT_KEY_HOME, PLAT_KEY_UP, PLAT_KEY_PAGEUP};
        if (usage <= 0x58)
            return keypad_symbols[usage - 0x54];
        if (usage <= 0x61)
            return num ? '1' + (usage - 0x59) : navigation[usage - 0x59];
        if (usage == 0x62)
            return num ? '0' : PLAT_KEY_INSERT;
        return num ? '.' : 127;
    } else {
        switch (usage) {
        case 0x49: return PLAT_KEY_INSERT;
        case 0x4A: return PLAT_KEY_HOME;
        case 0x4B: return PLAT_KEY_PAGEUP;
        case 0x4C: return 127;
        case 0x4D: return PLAT_KEY_END;
        case 0x4E: return PLAT_KEY_PAGEDOWN;
        case 0x4F: return PLAT_KEY_RIGHT;
        case 0x50: return PLAT_KEY_LEFT;
        case 0x51: return PLAT_KEY_DOWN;
        case 0x52: return PLAT_KEY_UP;
        default: return -1;
        }
    }
    if (ctrl) {   /* Ctrl with @ [ \ ] ^ _ (and space) gives the control codes, the others are unchanged */
        if (ch == ' ')
            return 0;
        if (ch >= 0x40 && ch <= 0x5F)
            return ch & 0x1F;
    }
    return ch;
}

void keymap_init(keymap_t *k)
{
    memset(k, 0, sizeof *k);
    k->num_lock = 1;
}

static int was_down(const keymap_t *k, uint8_t usage)
{
    for (int i = 0; i < 6; i++)
        if (k->keys[i] == usage)
            return 1;
    return 0;
}

static void press(keymap_t *k, uint8_t usage, uint8_t modifiers, uint32_t now_ms)
{
    k->repeat_key = 0;   /* the newest key is the one that repeats */
    if (usage == USAGE_CAPS_LOCK) {
        k->caps_lock ^= 1;
        push_event(usage, modifiers, k);
    } else if (usage == USAGE_NUM_LOCK) {
        k->num_lock ^= 1;
        push_event(usage, modifiers, k);
    } else {
        push_event(usage, modifiers, k);
        const int ch = keymap_translate(usage, modifiers, k->caps_lock, k->num_lock);
        if (ch >= 0) {
            push(ch);
            k->repeat_key = usage;
            k->repeat_char = (int16_t)ch;
            k->next_repeat_ms = now_ms + PLAT_KEY_REPEAT_DELAY_MS;
        }
    }
}

void keymap_report(keymap_t *k, const uint8_t *report, int len, uint32_t now_ms)
{
    if (len < 8)
        return;
    for (int i = 2; i < 8; i++)
        if (report[i] == ROLLOVER_ERROR)   /* too many keys down for the keyboard to say which: believe nothing of it */
            return;
    for (int i = 2; i < 8; i++)
        if (report[i] >= 4 && !was_down(k, report[i]))
            press(k, report[i], report[0], now_ms);
    k->modifiers = report[0];
    memcpy(k->keys, report + 2, sizeof k->keys);
    if (k->repeat_key && !was_down(k, k->repeat_key))
        k->repeat_key = 0;
}

void keymap_tick(keymap_t *k, uint32_t now_ms)
{
    if (!k->repeat_key || (int32_t)(now_ms - k->next_repeat_ms) < 0)
        return;
    push(k->repeat_char);
    push_event(k->repeat_key, k->modifiers, k);
    k->next_repeat_ms += PLAT_KEY_REPEAT_MS;
    if ((int32_t)(now_ms - k->next_repeat_ms) >= 0)   /* it has been a long while since the last tick: do not make up for it */
        k->next_repeat_ms = now_ms + PLAT_KEY_REPEAT_MS;
}

void keymap_release_all(keymap_t *k)
{
    k->modifiers = 0;
    memset(k->keys, 0, sizeof k->keys);
    k->repeat_key = 0;
}
