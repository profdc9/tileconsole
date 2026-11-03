/* SPDX-License-Identifier: MIT */
#ifndef USB_HID_H
#define USB_HID_H

#include "platform.h"

void usb_hid_init(void);                          /* start the USB host */
void usb_hid_task(void);                          /* call often (at least every few milliseconds) */
/* What the USB event wrappers (usblog_usb.c) tell the host recovery in usb_hid_task: a device was attached or removed (hub 0, port 0 is the
 * root port) and the enumeration of a new device began (a setup packet to address 0). The first is called from the interrupt handler. */
void usb_hid_host_event(uint8_t event_id, uint8_t hub_addr, uint8_t hub_port);
void usb_hid_host_setup0(void);
void usb_hid_host_storm(void);   /* the same receive timeout has repeated for a second (from the interrupt handler) */
int usb_hid_getkey(plat_key_event_t *event);                  /* the next key press with its modifiers (plat_getkey), or 0 */
int usb_hid_getchar(void);                        /* the next typed character from the keyboards, or -1 (see keymap.h) */
plat_buttons_t usb_hid_buttons(int player);       /* what the keyboards and gamepads for this player hold down */
plat_mouse_t usb_hid_mouse_frame(void);           /* the mouse snapshot for this frame (call once a frame) */
void usb_hid_mouse_set_position(int x, int y);
int usb_hid_diag(char (*lines)[41], int max_lines);

/* The XInput class driver (xinput_host.c) reports its pads here. A device that has a gamepad interface, XInput or HID, has its
 * keyboard and mouse interfaces ignored: they are emulations or configuration channels, not what is wanted. */
void usb_hid_task_stats(uint32_t *max_us, uint32_t *total_us);   /* the longest call and the total time in usb_hid_task since the last call */
void xinput_poll(void);   /* repeats the LED command to a pad that has not sent a report yet */
int xinput_debug(char (*lines)[41], int max_lines);   /* counters from the XInput driver, for the diagnostic screen */
void usb_hid_xinput_attach(uint8_t dev_addr);
void usb_hid_xinput_detach(uint8_t dev_addr);
void usb_hid_xinput_report(uint8_t dev_addr, const uint8_t *report, uint16_t length);   /* text about what is connected and what it sent */

#endif
