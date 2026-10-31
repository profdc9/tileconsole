/* SPDX-License-Identifier: MIT */
/*
 * USB keyboards, mice and gamepads on the native USB port, through TinyUSB's host stack. Keyboards and mice are put into
 * boot protocol (a fixed report layout); a gamepad's own report descriptor is parsed once when it is plugged in
 * (hid_pad.c). Keyboards are player 0; gamepads take the lowest free player, 0 then 1. The board LED lights while anything
 * usable is connected, which is the only sign of life when the USB port is a host and there is no serial console.
 */
#include "usb_hid.h"

#include <stdio.h>
#include <string.h>

#include "hardware/gpio.h"
#include "tusb.h"

#include "hid_kbd.h"
#include "hid_mouse.h"
#include "hid_pad.h"
#include "mouse_acc.h"
#include "xinput.h"

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
static struct {
    bool present;
    uint8_t player;
    plat_buttons_t held;
} xin[CFG_TUH_DEVICE_MAX + 1];             /* XInput pads, one a device */
static bool dev_has_pad[CFG_TUH_DEVICE_MAX + 1];   /* the device has a gamepad interface: ignore its keyboard and mouse ones */
static hid_pad_t pad_pool[PAD_POOL];
static bool pad_used[PAD_POOL];
static mouse_acc_t mouse;

/* What the last interface that appeared said about itself, and what the last pad report was: shown by usb_hid_diag. */
static struct {
    bool have_iface;
    uint8_t proto, nother;
    uint16_t desc_len, vid, pid;
    uint8_t desc[10];
    bool tried_pad, pad_ok;
    uint8_t pad_fields, pad_id;
    bool pad_has_id;
    uint32_t reports;
    uint8_t last_len, last[10];
    bool decoded;
    hid_pad_state_t state;
    uint32_t xin_reports, ignored;
    uint8_t xin_last[10];
    plat_buttons_t xin_buttons;
    uint32_t kbd_reports;          /* keyboard-protocol interfaces: the last report and every key code seen so far */
    uint8_t kbd_last[8];
    uint8_t seen[12];
    uint8_t nseen;
} diag;

static bool valid(uint8_t dev_addr, uint8_t idx) { return dev_addr <= CFG_TUH_DEVICE_MAX && idx < CFG_TUH_HID; }

static void update_led(void)
{
    bool any = false;
    for (int d = 0; d <= CFG_TUH_DEVICE_MAX; d++) {
        any |= xin[d].present;
        for (int i = 0; i < CFG_TUH_HID; i++)
            any |= ifc[d][i].kind != K_NONE;
    }
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

void usb_hid_task(void)
{
    tuh_task();
    xinput_poll();
}

plat_buttons_t usb_hid_buttons(int player)
{
    plat_buttons_t b = 0;
    for (int d = 0; d <= CFG_TUH_DEVICE_MAX; d++) {
        if (xin[d].present && xin[d].player == player)
            b |= xin[d].held;
        for (int i = 0; i < CFG_TUH_HID; i++) {
            const iface_t *f = &ifc[d][i];
            if ((f->kind == K_KEYBOARD && player == 0) || (f->kind == K_PAD && f->player == player))
                b |= f->held;
        }
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
        for (int d = 0; d <= CFG_TUH_DEVICE_MAX; d++) {
            taken |= xin[d].present && xin[d].player == p;
            for (int i = 0; i < CFG_TUH_HID; i++)
                taken |= ifc[d][i].kind == K_PAD && ifc[d][i].player == p;
        }
        if (!taken)
            return p;
    }
    return 0xFF;   /* a third pad is not used */
}

/* A device has a gamepad: whatever keyboard or mouse interfaces it already had are dropped, and later ones are ignored. */
static void device_is_a_pad(uint8_t dev_addr)
{
    dev_has_pad[dev_addr] = true;
    for (int i = 0; i < CFG_TUH_HID; i++)
        if (ifc[dev_addr][i].kind == K_KEYBOARD || ifc[dev_addr][i].kind == K_MOUSE)
            memset(&ifc[dev_addr][i], 0, sizeof ifc[dev_addr][i]);
    update_led();
}

void usb_hid_xinput_attach(uint8_t dev_addr)
{
    if (dev_addr > CFG_TUH_DEVICE_MAX)
        return;
    const uint8_t player = free_player();
    xin[dev_addr].present = player != 0xFF;
    xin[dev_addr].player = player;
    xin[dev_addr].held = 0;
    device_is_a_pad(dev_addr);
}

void usb_hid_xinput_detach(uint8_t dev_addr)
{
    if (dev_addr > CFG_TUH_DEVICE_MAX)
        return;
    memset(&xin[dev_addr], 0, sizeof xin[dev_addr]);
    dev_has_pad[dev_addr] = false;
    update_led();
}

void usb_hid_xinput_report(uint8_t dev_addr, const uint8_t *report, uint16_t length)
{
    if (dev_addr > CFG_TUH_DEVICE_MAX)
        return;
    diag.xin_reports++;
    memset(diag.xin_last, 0, sizeof diag.xin_last);
    memcpy(diag.xin_last, report, length < sizeof diag.xin_last ? length : sizeof diag.xin_last);
    plat_buttons_t b;
    if (xinput_decode(report, length, &b)) {
        xin[dev_addr].held = b;
        diag.xin_buttons = b;
    }
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report_desc, uint16_t desc_len)
{
    if (!valid(dev_addr, idx))
        return;
    iface_t *f = &ifc[dev_addr][idx];
    memset(f, 0, sizeof *f);
    f->pad = NO_PAD;
    const uint8_t proto = tuh_hid_interface_protocol(dev_addr, idx);
    diag.have_iface = true;
    diag.proto = proto;
    diag.desc_len = desc_len;
    diag.tried_pad = diag.pad_ok = false;
    memset(diag.desc, 0, sizeof diag.desc);
    memcpy(diag.desc, report_desc, desc_len < sizeof diag.desc ? desc_len : sizeof diag.desc);
    tuh_vid_pid_get(dev_addr, &diag.vid, &diag.pid);

    if ((proto == HID_ITF_PROTOCOL_KEYBOARD || proto == HID_ITF_PROTOCOL_MOUSE) && dev_has_pad[dev_addr]) {
        diag.ignored++;
        return;   /* this device is a gamepad: its keyboard and mouse interfaces are not wanted */
    }
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
        diag.tried_pad = true;
        const bool parsed = slot < PAD_POOL && player != 0xFF && hid_pad_parse_descriptor(report_desc, desc_len, &pad_pool[slot]);
        diag.pad_ok = parsed;
        if (parsed) {
            diag.pad_fields = pad_pool[slot].nfields;
            diag.pad_has_id = pad_pool[slot].has_id;
            diag.pad_id = pad_pool[slot].report_id;
        }
        if (parsed) {
            uint16_t vid = 0, pid = 0;
            tuh_vid_pid_get(dev_addr, &vid, &pid);
            pad_used[slot] = true;
            f->kind = K_PAD;
            f->pad = (uint8_t)slot;
            f->player = player;
            f->map = hid_pad_map_for(vid, pid);
            device_is_a_pad(dev_addr);
        } else {
            diag.nother++;
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
    bool still = xin[dev_addr].present;
    for (int k = 0; k < CFG_TUH_HID; k++)
        still |= ifc[dev_addr][k].kind == K_PAD;
    dev_has_pad[dev_addr] = still;
    update_led();
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report, uint16_t len)
{
    if (!valid(dev_addr, idx))
        return;
    iface_t *f = &ifc[dev_addr][idx];
    if (f->kind == K_PAD) {   /* for the diagnostic screen */
        diag.reports++;
        diag.last_len = len > 255 ? 255 : (uint8_t)len;
        memcpy(diag.last, report, len < sizeof diag.last ? len : sizeof diag.last);
    }
    switch (f->kind) {
    case K_KEYBOARD: {
        diag.kbd_reports++;
        memset(diag.kbd_last, 0, sizeof diag.kbd_last);
        memcpy(diag.kbd_last, report, len < sizeof diag.kbd_last ? len : sizeof diag.kbd_last);
        for (int k = 2; k < 8 && k < len; k++) {   /* remember each distinct key code, in the order first seen */
            const uint8_t code = report[k];
            bool known = code <= 1;
            for (unsigned s = 0; s < diag.nseen; s++)
                known |= diag.seen[s] == code;
            if (!known && diag.nseen < sizeof diag.seen)
                diag.seen[diag.nseen++] = code;
        }
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
        diag.decoded = f->pad < PAD_POOL && hid_pad_decode(&pad_pool[f->pad], report, len, &st);
        if (diag.decoded) {
            diag.state = st;
            f->held = hid_pad_to_buttons(&st, f->map);
        }
        break;
    }
    default:
        return;
    }
    tuh_hid_receive_report(dev_addr, idx);   /* ask for the next report */
}

int usb_hid_diag(char (*lines)[41], int max)
{
    int n = 0, kbd = 0, mice = 0, pads = 0;
    for (int d = 0; d <= CFG_TUH_DEVICE_MAX; d++)
        for (int i = 0; i < CFG_TUH_HID; i++) {
            kbd += ifc[d][i].kind == K_KEYBOARD;
            mice += ifc[d][i].kind == K_MOUSE;
            pads += ifc[d][i].kind == K_PAD;
        }
#define LINE(...) do { if (n < max) snprintf(lines[n++], 41, __VA_ARGS__); } while (0)
    int xpads = 0;
    for (int d = 0; d <= CFG_TUH_DEVICE_MAX; d++)
        xpads += xin[d].present;
    LINE("KBD %d MOUSE %d PAD %d XINPUT %d", kbd, mice, pads, xpads);
    LINE("OTHER %d, KEYBOARD/MOUSE IGNORED %lu", diag.nother, (unsigned long)diag.ignored);
    if (xpads) {
        LINE("XINPUT REPORTS %lu", (unsigned long)diag.xin_reports);
        LINE("X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", diag.xin_last[0], diag.xin_last[1], diag.xin_last[2],
             diag.xin_last[3], diag.xin_last[4], diag.xin_last[5], diag.xin_last[6], diag.xin_last[7], diag.xin_last[8],
             diag.xin_last[9]);
        LINE("DECODED PLATFORM BUTTONS %04X", diag.xin_buttons);
        n += xinput_debug(lines + n, max - n);
        return n;
    }
    if (!diag.have_iface) {
        LINE("NO HID INTERFACE SEEN YET");
        return n;
    }
    if (kbd) {
        LINE("KEYBOARD REPORTS %lu (MODIFIER, KEYS)", (unsigned long)diag.kbd_reports);
        LINE("NOW %02X: %02X %02X %02X %02X %02X %02X", diag.kbd_last[0], diag.kbd_last[2], diag.kbd_last[3], diag.kbd_last[4],
             diag.kbd_last[5], diag.kbd_last[6], diag.kbd_last[7]);
        char seen[41] = "SEEN:";
        for (unsigned s = 0; s < diag.nseen; s++)
            snprintf(seen + strlen(seen), sizeof seen - strlen(seen), " %02X", diag.seen[s]);
        LINE("%s", seen);
    }
    LINE("LAST: PROTO %d, DESC LEN %d", diag.proto, diag.desc_len);
    LINE("VID %04X PID %04X", diag.vid, diag.pid);
    LINE("DESC %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", diag.desc[0], diag.desc[1], diag.desc[2], diag.desc[3],
         diag.desc[4], diag.desc[5], diag.desc[6], diag.desc[7], diag.desc[8], diag.desc[9]);
    if (diag.tried_pad)
        LINE("PAD PARSE %s FIELDS %d ID %s", diag.pad_ok ? "OK" : "FAILED", diag.pad_fields,
             diag.pad_has_id ? "YES" : "NO");
    else
        LINE("(BOOT PROTOCOL DEVICE, NOT A PAD)");
    LINE("PAD REPORTS %lu, LAST LENGTH %d", (unsigned long)diag.reports, diag.last_len);
    LINE("RPT %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", diag.last[0], diag.last[1], diag.last[2], diag.last[3],
         diag.last[4], diag.last[5], diag.last[6], diag.last[7], diag.last[8], diag.last[9]);
    char bits[17];
    for (int b = 0; b < 16; b++)
        bits[b] = (diag.state.buttons >> b & 1) ? '1' : '0';
    bits[16] = 0;
    LINE("DECODE %s BUTTONS 1-16", diag.decoded ? "OK" : "NO ");
    LINE("%s", bits);
    LINE("DPAD U%d D%d L%d R%d", !!(diag.state.dpad & HID_DPAD_UP), !!(diag.state.dpad & HID_DPAD_DOWN),
         !!(diag.state.dpad & HID_DPAD_LEFT), !!(diag.state.dpad & HID_DPAD_RIGHT));
#undef LINE
    return n;
}
