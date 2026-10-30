/* SPDX-License-Identifier: MIT */
/* TinyUSB configuration for the keyboard build: the native USB port is a host (so there is no USB serial console). */
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_MCU OPT_MCU_RP2040   /* the RP2350 uses the same controller driver */

#define BOARD_TUH_RHPORT 0            /* the native USB port */
#define BOARD_TUH_MAX_SPEED OPT_MODE_FULL_SPEED
#define CFG_TUH_MAX_SPEED BOARD_TUH_MAX_SPEED

#define CFG_TUH_ENABLED 1
#define CFG_TUD_ENABLED 0

#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_HUB 1                 /* many keyboards are a hub with a keyboard behind it */
#define CFG_TUH_DEVICE_MAX 5          /* a hub and four things behind it */
#define CFG_TUH_HID 8                 /* keyboards, mice and pads often have several interfaces each */
#define CFG_TUH_CDC 0
#define CFG_TUH_MSC 0
#define CFG_TUH_VENDOR 0

#define CFG_TUH_HID_EPIN_BUFSIZE 64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64

#ifdef __cplusplus
}
#endif

#endif
