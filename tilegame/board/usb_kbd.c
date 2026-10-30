/* SPDX-License-Identifier: MIT */
/*
 * USB keyboards on the native USB port, through TinyUSB's host stack. Each keyboard is put into boot protocol, which
 * makes its reports a fixed 8-byte layout that hid_kbd.c understands. The board LED lights while a keyboard is connected,
 * which is the only sign of life when the USB port is a host and there is no serial console.
 */
#include "usb_kbd.h"

#include "hardware/gpio.h"
#include "tusb.h"

#include "hid_kbd.h"

#ifndef PLAT_LED_PIN
#define PLAT_LED_PIN 25
#endif

static plat_buttons_t held[CFG_TUH_DEVICE_MAX + 1][CFG_TUH_HID];
static bool is_keyboard[CFG_TUH_DEVICE_MAX + 1][CFG_TUH_HID];

static bool valid(uint8_t dev_addr, uint8_t idx) { return dev_addr <= CFG_TUH_DEVICE_MAX && idx < CFG_TUH_HID; }

static void update_led(void)
{
    bool any = false;
    for (int d = 0; d <= CFG_TUH_DEVICE_MAX; d++)
        for (int i = 0; i < CFG_TUH_HID; i++)
            any |= is_keyboard[d][i];
    gpio_put(PLAT_LED_PIN, any);
}

void usb_kbd_init(void)
{
    gpio_init(PLAT_LED_PIN);
    gpio_set_dir(PLAT_LED_PIN, GPIO_OUT);
    gpio_put(PLAT_LED_PIN, 0);
    tuh_init(BOARD_TUH_RHPORT);
}

void usb_kbd_task(void) { tuh_task(); }

plat_buttons_t usb_kbd_buttons(void)
{
    plat_buttons_t b = 0;
    for (int d = 0; d <= CFG_TUH_DEVICE_MAX; d++)
        for (int i = 0; i < CFG_TUH_HID; i++)
            b |= held[d][i];
    return b;
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report_desc, uint16_t desc_len)
{
    (void)report_desc;
    (void)desc_len;
    if (!valid(dev_addr, idx) || tuh_hid_interface_protocol(dev_addr, idx) != HID_ITF_PROTOCOL_KEYBOARD)
        return;
    is_keyboard[dev_addr][idx] = true;
    held[dev_addr][idx] = 0;
    update_led();
    tuh_hid_set_protocol(dev_addr, idx, HID_PROTOCOL_BOOT);
    tuh_hid_receive_report(dev_addr, idx);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx)
{
    if (!valid(dev_addr, idx))
        return;
    is_keyboard[dev_addr][idx] = false;
    held[dev_addr][idx] = 0;
    update_led();
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report, uint16_t len)
{
    if (!valid(dev_addr, idx) || !is_keyboard[dev_addr][idx])
        return;
    plat_buttons_t b;
    if (hid_kbd_parse(report, len, &b))
        held[dev_addr][idx] = b;
    tuh_hid_receive_report(dev_addr, idx);   /* ask for the next report */
}
