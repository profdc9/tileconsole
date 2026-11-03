/* The board's USB event ring: in RAM that a watchdog reset leaves alone (see ../src/usblog.h). */
#ifndef USBLOG_BOARD_H
#define USBLOG_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#if USBLOG
#include "usblog.h"
#else
/* Built without the USB log (cmake -DUSBLOG=OFF): the calls do nothing, so that the USB driver and the platform are as they were without it. */
enum { USBLOG_CLOSED_NONE = 0, USBLOG_CLOSED_HANG = 1, USBLOG_CLOSED_PANIC = 2, USBLOG_CLOSED_FAULT = 3 };
enum { USBLOG_EV_ISR = 1, USBLOG_EV_HCD_EVENT, USBLOG_EV_XFER, USBLOG_EV_SETUP, USBLOG_EV_EP_OPEN, USBLOG_EV_PORT, USBLOG_EV_MOUNT, USBLOG_EV_UNMOUNT,
       USBLOG_EV_SEQ_ERROR, USBLOG_EV_ISR_REPEAT, USBLOG_EV_HOST_RESTART, USBLOG_EV_PANIC = 0xF0, USBLOG_EV_HANG = 0xF1, USBLOG_EV_FAULT = 0xF2 };
#endif

#if USBLOG
/* Call first thing at start-up: if the previous run closed its ring (it hung, panicked or faulted), a copy is kept for usblog_saved;
 * the ring is then cleared for this run. */
void usblog_boot(void);

/* The ring of the previous run, if it ended badly and there was room to keep it; usblog_saved_free gives the copy back. */
const usblog_t *usblog_saved(void);
void usblog_saved_free(void);

/* The ring of this run, as it is now (for a snapshot while the program goes on). */
const usblog_t *usblog_live(void);

/* Records an event now (from either core, from an interrupt handler or not). */
void usblog(uint8_t type, uint16_t x, uint32_t a, uint32_t b, uint32_t c, uint32_t d);

/* The program is going down because of `reason` (USBLOG_CLOSED_*): the ring is kept through the reset. */
void usblog_close_run(uint32_t reason);

/* Starts the interrupt observer and the transfer logging (after tuh_init); usblog_usb_remove stops the observer (before tuh_deinit and
 * tuh_init again, so that it can be added again and run first); usblog_usb_flush writes out a pending "repeated N times". */
void usblog_usb_init(void);
void usblog_usb_remove(void);
void usblog_usb_flush(void);

#else
static inline void usblog_boot(void) {}
static inline void usblog(uint8_t type, uint16_t x, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { (void)type; (void)x; (void)a; (void)b; (void)c; (void)d; }
static inline void usblog_close_run(uint32_t reason) { (void)reason; }
static inline void usblog_usb_init(void) {}
static inline void usblog_usb_remove(void) {}
static inline void usblog_usb_flush(void) {}
static inline void usblog_usb_irq_state(void) {}
#endif
#endif
