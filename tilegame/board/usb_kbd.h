/* SPDX-License-Identifier: MIT */
#ifndef USB_KBD_H
#define USB_KBD_H

#include "platform.h"

void usb_kbd_init(void);             /* start the USB host */
void usb_kbd_task(void);             /* call often (at least every few milliseconds) */
plat_buttons_t usb_kbd_buttons(void);   /* what the connected keyboards are holding down */

#endif
