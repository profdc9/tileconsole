#include "usblog_board.h"

#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"

/* No initial value and not cleared at start-up: after a watchdog reset the ring is as it was left. */
static usblog_t __uninitialized_ram(ring);
static usblog_t *saved;

void usblog_boot(void)
{
    if (usblog_valid(&ring) && ring.closed != USBLOG_CLOSED_NONE) {
        saved = malloc(sizeof ring);
        if (saved)
            memcpy(saved, &ring, sizeof ring);
    }
    usblog_init(&ring);
}

const usblog_t *usblog_saved(void)
{
    return saved;
}

const usblog_t *usblog_live(void)
{
    return &ring;
}

void usblog_saved_free(void)
{
    free(saved);
    saved = NULL;
}

void __not_in_flash_func(usblog)(uint8_t type, uint16_t x, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    usblog_add(&ring, time_us_32(), (uint8_t)get_core_num(), type, x, a, b, c, d);
}

void __not_in_flash_func(usblog_close_run)(uint32_t reason)
{
    usblog_usb_flush();
    usblog_close(&ring, reason);
}
