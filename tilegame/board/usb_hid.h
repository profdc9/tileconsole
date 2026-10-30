/* SPDX-License-Identifier: MIT */
#ifndef USB_HID_H
#define USB_HID_H

#include "platform.h"

void usb_hid_init(void);                          /* start the USB host */
void usb_hid_task(void);                          /* call often (at least every few milliseconds) */
plat_buttons_t usb_hid_buttons(int player);       /* what the keyboards and gamepads for this player hold down */
plat_mouse_t usb_hid_mouse_frame(void);           /* the mouse snapshot for this frame (call once a frame) */
void usb_hid_mouse_set_position(int x, int y);

#endif
