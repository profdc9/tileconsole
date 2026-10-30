/* SPDX-License-Identifier: MIT */
/*
 * USB keyboards, mice and gamepads on the native USB port, through TinyUSB's host stack. Keyboards and mice are put into
 * boot protocol (a fixed report layout); a gamepad's own report descriptor is parsed once when it is plugged in
 * (hid_pad.c). Keyboards are player 0; gamepads take the lowest free player, 0 then 1. The board LED lights while anything
 * usable is connected, which is the only sign of life when the USB port is a host and there is no serial console.
 */
#include "usb_hid.h"

#include <string.h>

#include "hardware/gpio.h"
#include "tusb.h"

#include "hid_kbd.h"
#include "hid_mouse.h"
#include "hid_pad.h"
#include "mouse_acc.h"

#ifndef PLAT_LED_PIN
#define PLAT_LED_PIN 25
#endif

#define PAD_POOL 4
#define NO_PAD 0xFF

typedef enum { K_NONE = 0, K_KEYBOARD, K_MOUSE, K_PAD } kind_t;

typedef struct {
    uint8_t kind;
    uint8_t pad;                   /* index into pad_pool */
    uint8_t player;                /* for a pad */
    const hid_pad_map_t *map;
    plat_buttons_t held;
} iface_t;

static iface_t ifc[CFG_TUH_DEVICE_MAX + 1][CFG_TUH_HID];
static hid_pad_t pad_pool[PAD_POOL];
static bool pad_used[PAD_POOL];
static mouse_acc_t mouse;

static bool valid(uint8_t dev_addr, uint8_t idx) { return dev_addr <= CFG_TUH_DEVICE_MAX && idx < CFG_TUH_HID; }

static void update_led(void)
{
    bool any = false;
    for (int d = 0; d <= CFG_TUH_DEVICE_MAX; d++)
        for (int i = 0; i < CFG_TUH_HID; i++)
            any |= ifc[d][i].kind != K_NONE;
    gpio_put(PLAT_LED_PIN, any);
}

void usb_hid_init(void)
{
    mouse_acc_init(&mouse);
    gpio_init(PLAT_LED_PIN);
    gpio_set_dir(PLAT_LED_PIN, GPIO_OUT);
    gpio_put(PLAT_LED_PIN, 0);
    tuh_init(BOARD_TUH_RHPORT);
}

void usb_hid_task(void) { tuh_task(); }

plat_buttons_t usb_hid_buttons(int player)
{
    plat_buttons_t b = 0;
    for (int d = 0; d <= CFG_TUH_DEVICE_MAX; d++)
        for (int i = 0; i < CFG_TUH_HID; i++) {
            const iface_t *f = &ifc[d][i];
            if ((f->kind == K_KEYBOARD && player == 0) || (f->kind == K_PAD && f->player == player))
                b |= f->held;
        }
    return b;
}

plat_mouse_t usb_hid_mouse_frame(void)
{
    mouse_acc_frame(&mouse);
    return mouse.frame;
}

void usb_hid_mouse_set_position(int x, int y) { mouse_acc_set_position(&mouse, x, y); }

/* The lowest player number that no pad has. */
static uint8_t free_player(void)
{
    for (uint8_t p = 0; p < PLAT_MAX_PLAYERS; p++) {
        bool taken = false;
        for (int d = 0; d <= CFG_TUH_DEVICE_MAX; d++)
            for (int i = 0; i < CFG_TUH_HID; i++)
                taken |= ifc[d][i].kind == K_PAD && ifc[d][i].player == p;
        if (!taken)
            return p;
    }
    return 0xFF;   /* a third pad is not used */
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report_desc, uint16_t desc_len)
{
    if (!valid(dev_addr, idx))
        return;
    iface_t *f = &ifc[dev_addr][idx];
    memset(f, 0, sizeof *f);
    f->pad = NO_PAD;
    const uint8_t proto = tuh_hid_interface_protocol(dev_addr, idx);

    if (proto == HID_ITF_PROTOCOL_KEYBOARD || proto == HID_ITF_PROTOCOL_MOUSE) {
        f->kind = proto == HID_ITF_PROTOCOL_KEYBOARD ? K_KEYBOARD : K_MOUSE;
        if (f->kind == K_MOUSE)
            mouse_acc_move(&mouse, 0, 0, 0);   /* a mouse is there */
        tuh_hid_set_protocol(dev_addr, idx, HID_PROTOCOL_BOOT);
    } else {
        unsigned slot = 0;
        while (slot < PAD_POOL && pad_used[slot])
            slot++;
        const uint8_t player = free_player();
        if (slot < PAD_POOL && player != 0xFF && hid_pad_parse_descriptor(report_desc, desc_len, &pad_pool[slot])) {
            uint16_t vid = 0, pid = 0;
            tuh_vid_pid_get(dev_addr, &vid, &pid);
            pad_used[slot] = true;
            f->kind = K_PAD;
            f->pad = (uint8_t)slot;
            f->player = player;
            f->map = hid_pad_map_for(vid, pid);
        } else {
            return;   /* something we do not use: leave it alone */
        }
    }
    update_led();
    tuh_hid_receive_report(dev_addr, idx);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx)
{
    if (!valid(dev_addr, idx))
        return;
    iface_t *f = &ifc[dev_addr][idx];
    if (f->kind == K_PAD && f->pad < PAD_POOL)
        pad_used[f->pad] = false;
    memset(f, 0, sizeof *f);
    update_led();
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report, uint16_t len)
{
    if (!valid(dev_addr, idx))
        return;
    iface_t *f = &ifc[dev_addr][idx];
    switch (f->kind) {
    case K_KEYBOARD: {
        plat_buttons_t b;
        if (hid_kbd_parse(report, len, &b))
            f->held = b;
        break;
    }
    case K_MOUSE: {
        usb_mouse_report_t m;
        if (hid_mouse_parse(report, len, &m)) {
            mouse_acc_move(&mouse, m.dx, m.dy, m.wheel);
            mouse_acc_buttons(&mouse, m.buttons);
        }
        break;
    }
    case K_PAD: {
        hid_pad_state_t st;
        if (f->pad < PAD_POOL && hid_pad_decode(&pad_pool[f->pad], report, len, &st))
            f->held = hid_pad_to_buttons(&st, f->map);
        break;
    }
    default:
        return;
    }
    tuh_hid_receive_report(dev_addr, idx);   /* ask for the next report */
}
