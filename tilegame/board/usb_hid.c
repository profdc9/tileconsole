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
#include "hardware/structs/usb.h"
#include "hardware/structs/usb_dpram.h"
#include "pico/time.h"
#include "tusb.h"
#include "host/hcd.h"
#include "usblog_board.h"

#include "hid_kbd.h"
#include "hid_mouse.h"
#include "hid_pad.h"
#include "keymap.h"
#include "mouse_acc.h"
#include "xinput.h"

#define usb_hw_clear ((usb_hw_t *)hw_clear_alias_untyped(usb_hw))   /* write 1s to clear bits of a register */

#ifndef PLAT_LED_PIN
#define PLAT_LED_PIN 25
#endif

#define PAD_POOL 4
#define NO_PAD 0xFF

typedef enum { K_NONE = 0, K_KEYBOARD, K_MOUSE, K_PAD } kind_t;
enum { PEND_NONE = 0, PEND_PROTOCOL, PEND_RECEIVE };   /* what an interface still needs: boot protocol, then reports polled */
#define SETTLE_MS 200   /* wait this long after the last device or interface appeared before sending it anything */

typedef struct {
    uint8_t kind;
    uint8_t pending;               /* work for usb_hid_task once enumeration has settled: PEND_* */
    uint8_t pad;                   /* index into pad_pool */
    uint8_t player;                /* for a pad */
    const hid_pad_map_t *map;
    plat_buttons_t held;
    keymap_t keys;                 /* for a keyboard: what it has down, its lock keys and its key repeat */
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

static void start_host(void)
{
    tuh_init(BOARD_TUH_RHPORT);
    /* TinyUSB's RP2040 driver panics inside its interrupt handler on a data sequence error, which a low speed device behind a hub
     * produces (a repeated packet; the hardware has already dropped it). Do not take that interrupt; usb_hid_task counts them. */
    usb_hw_clear->inte = USB_INTE_ERROR_DATA_SEQ_BITS;
    usblog_usb_init();
}

void usb_hid_init(void)
{
    mouse_acc_init(&mouse);
    gpio_init(PLAT_LED_PIN);
    gpio_set_dir(PLAT_LED_PIN, GPIO_OUT);
    gpio_put(PLAT_LED_PIN, 0);
    start_host();
}

static uint32_t host_restarts;
static volatile uint32_t last_attach_ms;   /* when a device last appeared (USBLOG builds), for usb_hid_quiet */

#if USBLOG
/* Host recovery. TinyUSB 0.18 does not forget an enumeration that was under way when its device went away (process_removing_device
 * has a TODO for it), so every later attach is deferred behind an enumeration that never ends, and two such attaches queued make
 * tuh_task loop for ever (the hang in the USB log). The host is started again (tuh_deinit, tuh_init: all devices go and are found
 * again) 300 ms after the root port is unplugged, when an attach has not led to a setup packet to address 0 within 2 seconds, and when
 * the same receive timeout has gone on for a second (a transfer the hardware retries for ever: a device behind a hub that does not answer). */
#define ROOT_REMOVED_SETTLE_US 300000u
#define ATTACH_STUCK_US 2000000u
static volatile uint32_t root_removed_us, attach_us, storm_us;   /* when, or 0 */
static volatile uint8_t attach_hub, attach_port;                    /* where the attach was */
static uint32_t last_restart_us;
#define AUTO_RESTARTS_MAX 2          /* restarts for a stuck enumeration or a storm in a row; a device that fails the same way every */
#define AUTO_RESTARTS_QUIET_US 30000000u   /* time is not helped by more, so the host is left alone until the root port is unplugged or 30 s pass */
static uint32_t auto_restarts;
#define CTRL_STUCK_US 2000000u              /* a transfer on the control endpoint that has not completed for this long (the stack has no timeout) */
static uint8_t ls_port;                     /* the hub port of the device that a storm hit, until the root port is unplugged */
static volatile uint32_t ctrl_us;           /* when the control endpoint was last started and has not completed since, or 0 */
extern volatile uint16_t tilegame_hcd_spurious;   /* the same: an endpoint address | 0x100 when a buffer status arrived for an endpoint with no transfer */
extern volatile uint8_t tilegame_hub_first_port;   /* patched TinyUSB (third_party/tinyusb/README-tilegame.md): the hub port to enumerate first, once */

void __not_in_flash_func(usb_hid_host_ctrl_start)(void) { ctrl_us = time_us_32() | 1; }
void __not_in_flash_func(usb_hid_host_ctrl_done)(void) { ctrl_us = 0; }

void __not_in_flash_func(usb_hid_host_event)(uint8_t event_id, uint8_t hub_addr, uint8_t hub_port)
{
    const uint32_t now = time_us_32() | 1;
    if (event_id == HCD_EVENT_DEVICE_REMOVE)
        ctrl_us = 0;   /* (a control transfer that was under way to a device that has gone is never completed) */
    if (event_id == HCD_EVENT_DEVICE_ATTACH)
        last_attach_ms = to_ms_since_boot(get_absolute_time());
    if (event_id == HCD_EVENT_DEVICE_REMOVE && hub_addr == 0 && hub_port == 0)
        root_removed_us = now;
    else if (event_id == HCD_EVENT_DEVICE_ATTACH && !attach_us) {
        attach_us = now;
        attach_hub = hub_addr;
        attach_port = hub_port;
    } else if (event_id == HCD_EVENT_DEVICE_REMOVE && attach_us && hub_addr == attach_hub && hub_port == attach_port)
        attach_us = 0;   /* the device that was to be enumerated has gone again: nothing is stuck */
}

void __not_in_flash_func(usb_hid_host_storm)(void)
{
    if (!storm_us)
        storm_us = time_us_32() | 1;
}

/* TinyUSB panics ("Invalid speed") if it is asked for the port's speed once the device has been unplugged, which happens when the root
 * port is unplugged in the middle of an enumeration it is already carrying out (the USB log: the removal, then the panic 64 ms later).
 * Say "full" instead: the removal that is already queued cleans up. (The driver's own calls inside a transfer are stopped by the
 * wrappers in usblog_usb.c, which pass nothing on while the port is disconnected.) */
extern tusb_speed_t __real_hcd_port_speed_get(uint8_t rhport);
tusb_speed_t __wrap_hcd_port_speed_get(uint8_t rhport)
{
    if (!(usb_hw->sie_status & USB_SIE_STATUS_SPEED_BITS))
        return TUSB_SPEED_FULL;
    return __real_hcd_port_speed_get(rhport);
}

void usb_hid_host_setup0(void)
{
    attach_us = 0;
}

static void check_host(void)
{
    const uint32_t now = time_us_32();
    /* (each timer is read once: an interrupt can clear it between a test and the subtraction, and "now - 0" looks very old) */
    const uint32_t root_removed = root_removed_us, attach = attach_us, storm = storm_us, ctrl = ctrl_us;
    uint32_t why = 0;
    if (host_restarts && now - last_restart_us < 1000000u)
        return;   /* (at most one a second) */
    if (root_removed && (int32_t)(now - root_removed) > (int32_t)ROOT_REMOVED_SETTLE_US)
        why = 1;
    else if (attach && (int32_t)(now - attach) > (int32_t)ATTACH_STUCK_US)
        why = 2;
    else if (storm)
        why = 3;
    else if (tilegame_hcd_spurious)
        why = 5;   /* a completion that was dropped: the endpoint (the hub's status endpoint, usblog35) is dead */
    else if (ctrl && (int32_t)(now - ctrl) > (int32_t)CTRL_STUCK_US)
        why = 4;
    if (!why)
        return;
    if (why == 1 || now - last_restart_us > AUTO_RESTARTS_QUIET_US)
        auto_restarts = 0;
    if (why != 1 && auto_restarts >= AUTO_RESTARTS_MAX) {
        attach_us = storm_us = 0;   /* (given up: not tried again) */
        return;
    }
    if (why != 1)
        auto_restarts++;
    const uint16_t spurious = tilegame_hcd_spurious;
    tilegame_hcd_spurious = 0;
    root_removed_us = attach_us = storm_us = ctrl_us = 0;
    if (why == 1)
        ls_port = 0;
    else if (why == 3 && attach_hub != 0)
        ls_port = attach_port;   /* a receive timeout storm: the device being enumerated (behind a hub) is the one that suffers from coming after others */
    tilegame_hub_first_port = ls_port;   /* after a restart that port is enumerated first (once; 0: lowest first) */
    host_restarts++;
    last_restart_us = now;
    usblog(USBLOG_EV_HOST_RESTART, 0, why, host_restarts, spurious, 0);
    tuh_deinit(BOARD_TUH_RHPORT);
    usblog_usb_remove();
    start_host();
}

#else
static void check_host(void) {}
#endif

static uint32_t seq_errors, hid_mounts, last_mount_ms;

bool usb_hid_quiet(void)
{
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    return now - last_mount_ms > USB_PAD_QUIET_MS && now - last_attach_ms > USB_PAD_QUIET_MS;
}

/* TinyUSB asks the hub for its next status report with hub_edpt_status_xfer (-Wl,--wrap on it). If that ever fails, a hub stops noticing
 * devices that are plugged in, so the calls and failures are counted for the diagnostic screen. */
extern bool __real_hub_edpt_status_xfer(uint8_t dev_addr);
static struct { uint32_t calls, failed; uint8_t last_addr; } hubarm;

bool __wrap_hub_edpt_status_xfer(uint8_t dev_addr)
{
    const bool ok = __real_hub_edpt_status_xfer(dev_addr);
    hubarm.calls++;
    hubarm.last_addr = dev_addr;
    if (!ok)
        hubarm.failed++;
    return ok;
}

/* TinyUSB must not be asked for anything from inside a mount callback: a control transfer started there collides with the enumeration
 * in progress, which then never completes, and a hub's later devices are never looked at. So the callback only records what the
 * interface needs and this does it once things have been quiet for a moment, one control transfer at a time. */
static void start_pending(void)
{
    if (to_ms_since_boot(get_absolute_time()) - last_mount_ms < SETTLE_MS)
        return;
    for (int d = 1; d <= CFG_TUH_DEVICE_MAX; d++)
        for (int i = 0; i < CFG_TUH_HID; i++) {
            iface_t *f = &ifc[d][i];
            if (f->pending == PEND_PROTOCOL) {
                if (tuh_hid_set_protocol((uint8_t)d, (uint8_t)i, HID_PROTOCOL_BOOT))
                    f->pending = PEND_RECEIVE;
                return;   /* one control transfer at a time */
            }
            if (f->pending == PEND_RECEIVE && tuh_hid_receive_report((uint8_t)d, (uint8_t)i))
                f->pending = PEND_NONE;
        }
}

static uint32_t task_us_max, task_us_total;

void usb_hid_task_stats(uint32_t *max_us, uint32_t *total_us)
{
    *max_us = task_us_max;
    *total_us = task_us_total;
    task_us_max = task_us_total = 0;
}

void usb_hid_task(void)
{
    const uint32_t started = time_us_32();
    if (usb_hw->sie_status & USB_SIE_STATUS_DATA_SEQ_ERROR_BITS) {
        usb_hw_clear->sie_status = USB_SIE_STATUS_DATA_SEQ_ERROR_BITS;
        seq_errors++;
        usblog(USBLOG_EV_SEQ_ERROR, 0, seq_errors, usb_hw->sie_status, usb_hw->buf_status, usbh_dpram->epx_buf_ctrl);
    }
    tuh_task();
#if USBLOG && USB_PAUSE_POLL
    usblog_usb_release_held();
#endif
    check_host();
    xinput_poll();
    start_pending();
    static uint32_t last_tick_ms;
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (now_ms != last_tick_ms) {   /* the key repeat of the held keys, at most once a millisecond */
        last_tick_ms = now_ms;
        for (int d = 1; d <= CFG_TUH_DEVICE_MAX; d++)
            for (int i = 0; i < CFG_TUH_HID; i++)
                if (ifc[d][i].kind == K_KEYBOARD)
                    keymap_tick(&ifc[d][i].keys, now_ms);
    }
    const uint32_t took = time_us_32() - started;
    task_us_total += took;
    if (took > task_us_max)
        task_us_max = took;
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

int usb_hid_getchar(void) { return keymap_getchar(); }
int usb_hid_getkey(plat_key_event_t *event) { return keymap_getevent(event); }

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
    last_mount_ms = to_ms_since_boot(get_absolute_time());
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
    usblog(USBLOG_EV_MOUNT, (uint16_t)(dev_addr << 8 | idx), desc_len, 0, 0, 0);
    if (!valid(dev_addr, idx))
        return;
    hid_mounts++;
    last_mount_ms = to_ms_since_boot(get_absolute_time());
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
        if (f->kind == K_KEYBOARD)
            keymap_init(&f->keys);
        if (f->kind == K_MOUSE)
            mouse_acc_move(&mouse, 0, 0, 0);   /* a mouse is there */
        f->pending = PEND_PROTOCOL;
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
            f->pending = PEND_RECEIVE;
            device_is_a_pad(dev_addr);
        } else {
            diag.nother++;
            return;   /* something we do not use: leave it alone */
        }
    }
    update_led();
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx)
{
    usblog(USBLOG_EV_UNMOUNT, (uint16_t)(dev_addr << 8 | idx), 0, 0, 0, 0);
    if (!valid(dev_addr, idx))
        return;
    iface_t *f = &ifc[dev_addr][idx];
    if (f->kind == K_PAD && f->pad < PAD_POOL)
        pad_used[f->pad] = false;
    if (f->kind == K_KEYBOARD)
        keymap_release_all(&f->keys);   /* a key that was held stops repeating */
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
        keymap_report(&f->keys, report, len, to_ms_since_boot(get_absolute_time()));   /* and the typed characters (plat_getchar) */
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
    LINE("DATA SEQUENCE ERRORS SEEN %lu", (unsigned long)seq_errors);
    LINE("USB HOST RESTARTS %lu", (unsigned long)host_restarts);
    /* Every device the stack has enumerated, with its vendor and product ids (the hub, the keyboard, the pad...). */
    for (int d = 1; d <= CFG_TUH_DEVICE_MAX; d += 3) {
        char row[41] = "";
        for (int k = d; k < d + 3 && k <= CFG_TUH_DEVICE_MAX; k++) {
            uint16_t vid = 0, pid = 0;
            if (tuh_mounted((uint8_t)k))
                tuh_vid_pid_get((uint8_t)k, &vid, &pid);
            snprintf(row + strlen(row), sizeof row - strlen(row), "%sD%d %04X:%04X", k > d ? " " : "", k, vid, pid);
        }
        LINE("%s", row);
    }
    LINE("HID MOUNTED %lu ITF OPEN D1 %u D2 %u D3 %u", (unsigned long)hid_mounts, tuh_hid_itf_get_count(1), tuh_hid_itf_get_count(2),
         tuh_hid_itf_get_count(3));
    LINE("HUB STATUS ARMED %lu FAILED %lu (HUB %u)", (unsigned long)hubarm.calls, (unsigned long)hubarm.failed, hubarm.last_addr);
    if (!xpads)
        n += xinput_debug(lines + n, n < max ? 1 : 0);   /* did the XInput driver ever open a pad? (the first counter line) */
    /* The USB controller itself: what is it doing when the input stops? */
    LINE("SIE %08lX BUFST %08lX", (unsigned long)usb_hw->sie_status, (unsigned long)usb_hw->buf_status);
    LINE("EPX CTRL %08lX BUF %08lX", (unsigned long)usbh_dpram->epx_ctrl, (unsigned long)usbh_dpram->epx_buf_ctrl);
    for (int e = 0, shown = 0; !xpads && e < 15 && shown < 3; e++) {   /* (not when a pad is shown: the XInput lines need the room) */
        const uint32_t ctrl = usbh_dpram->int_ep_ctrl[e].ctrl;
        if (!(ctrl & (1u << 31)))   /* the enable bit */
            continue;   /* not in use */
        LINE("IEP%d CTRL %08lX BUF %08lX", e, (unsigned long)ctrl, (unsigned long)usbh_dpram->int_ep_buffer_ctrl[e].ctrl);
        LINE("     ADDR %08lX", (unsigned long)usb_hw->int_ep_addr_ctrl[e]);
        shown++;
    }
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
