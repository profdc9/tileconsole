/* SPDX-License-Identifier: MIT */
#ifndef USB_HID_H
#define USB_HID_H

#include "platform.h"

void usb_hid_init(void);                          /* start the USB host */
void usb_hid_task(void);                          /* call often (at least every few milliseconds) */
plat_buttons_t usb_hid_buttons(int player);       /* what the keyboards and gamepads for this player hold down */
plat_mouse_t usb_hid_mouse_frame(void);           /* the mouse snapshot for this frame (call once a frame) */
void usb_hid_mouse_set_position(int x, int y);
int usb_hid_diag(char (*lines)[41], int max_lines);

/* The XInput class driver (xinput_host.c) reports its pads here. A device that has a gamepad interface, XInput or HID, has its
 * keyboard and mouse interfaces ignored: they are emulations or configuration channels, not what is wanted. */
void xinput_poll(void);   /* repeats the LED command to a pad that has not sent a report yet */
int xinput_debug(char (*lines)[41], int max_lines);   /* counters from the XInput driver, for the diagnostic screen */
void usb_hid_xinput_attach(uint8_t dev_addr);
void usb_hid_xinput_detach(uint8_t dev_addr);
void usb_hid_xinput_report(uint8_t dev_addr, const uint8_t *report, uint16_t length);   /* text about what is connected and what it sent */

#endif
