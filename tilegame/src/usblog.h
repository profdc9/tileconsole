/* A ring of the last USB events, for finding out what happened before a panic, a hang or a fault. The ring lives in RAM that a
 * watchdog reset leaves alone; after the reset the program finds it, sees it was closed by a fault, and keeps it (see board/usblog_board.c).
 * This part knows nothing of the hardware: the caller gives the time and the core. */
#ifndef USBLOG_H
#define USBLOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USBLOG_ENTRIES 4096   /* a power of two */

typedef struct {
    uint32_t t;               /* microseconds */
    uint8_t type;             /* USBLOG_EV_* */
    uint8_t core;
    uint16_t x;
    uint32_t a, b, c, d;      /* what they hold depends on the type (tools/usblog_decode.py has the list) */
} usblog_entry_t;

typedef struct {
    uint32_t magic, magic_inv;   /* both right: the ring was initialised (RAM at power on is random) */
    uint32_t head;               /* events added since the ring was initialised; the newest is entry (head - 1) % USBLOG_ENTRIES */
    uint32_t closed;             /* 0 while running; USBLOG_CLOSED_* once the program is going down */
    usblog_entry_t e[USBLOG_ENTRIES];
} usblog_t;

enum { USBLOG_CLOSED_NONE = 0, USBLOG_CLOSED_HANG = 1, USBLOG_CLOSED_PANIC = 2, USBLOG_CLOSED_FAULT = 3 };

/* The event types the board records. */
enum {
    USBLOG_EV_ISR = 1,        /* the USB interrupt: a = INTS, b = SIE_STATUS, c = BUFF_STATUS, d = EPX buffer control, x = low half of SIE_CTRL */
    USBLOG_EV_HCD_EVENT = 2,  /* TinyUSB's host event: x = event id << 8 | device; attach/remove: a = hub | port << 8 | speed << 16; transfer: a = endpoint | result << 8, b = length */
    USBLOG_EV_XFER = 3,       /* a transfer started: x = device << 8 | endpoint, a = length, b = 1 if accepted */
    USBLOG_EV_SETUP = 4,      /* a setup packet sent: x = device, a and b = its 8 bytes, c = 1 if accepted */
    USBLOG_EV_EP_OPEN = 5,    /* x = device << 8 | endpoint, a = max packet size, b = type | interval << 8, c = 1 if accepted */
    USBLOG_EV_PORT = 6,       /* a = 1 reset, 2 reset end, 3 device close (x = device), 4 abort (x = dev << 8 | ep), 5 clear stall (same) */
    USBLOG_EV_MOUNT = 7,      /* a HID interface mounted: x = device << 8 | index, a = report descriptor length */
    USBLOG_EV_UNMOUNT = 8,
    USBLOG_EV_SEQ_ERROR = 9,  /* usb_hid_task found the data sequence error flag */
    USBLOG_EV_ISR_REPEAT = 10, /* the interrupt record above repeated: a = how many more times, b = microseconds from the first to the last */
    USBLOG_EV_HOST_RESTART = 11, /* the USB host was restarted: a = why (1 the root port was unplugged, 2 an attach went nowhere, 3 a receive timeout storm), b = the count so far */
    USBLOG_EV_IRQ_STATE = 12, /* TinyUSB's mask on the USB interrupt: x = 1 if it is off now; a = when it was last turned off, b = the caller (return address), c = when it was last turned on */
    USBLOG_EV_PANIC = 0xF0,   /* a and b = the start of the message, c = its first argument */
    USBLOG_EV_HANG = 0xF1,    /* the hang monitor: a = SIE_STATUS, b = INTS, c = BUFF_STATUS */
    USBLOG_EV_FAULT = 0xF2    /* a = the instruction, b = the caller, c = CFSR */
};

void usblog_init(usblog_t *r);
bool usblog_valid(const usblog_t *r);   /* initialised, and has something in it */
void usblog_add(usblog_t *r, uint32_t t, uint8_t core, uint8_t type, uint16_t x, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
void usblog_close(usblog_t *r, uint32_t reason);
uint32_t usblog_count(const usblog_t *r);   /* how many events the ring holds */

/* The file: a 32-byte header ("USBL", version 1, the entry size, the count, the total ever added, the reason it was closed, 2 zero words),
 * then the entries, oldest first. Written through `out` in a few pieces. */
size_t usblog_serialize(const usblog_t *r, void (*out)(void *ctx, const void *data, size_t size), void *ctx);

#endif
