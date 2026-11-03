/* SPDX-License-Identifier: MIT */
/* TinyUSB's own debug output (CFG_TUSB_DEBUG=2, cmake -DTUSB_LOG=ON) goes into a RAM ring, each line stamped with the time in microseconds;
 * the devkit's USBLOG command writes it to /sd/tusbNN.txt next to the USB log. There is no serial console on this board (the USB port is the host). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/sync.h"

#include "tusblog.h"

#define RING_SIZE (96 * 1024)

static char ring[RING_SIZE];
static uint32_t head;       /* next byte to write, counting all bytes ever written */
static bool at_line_start = true;

static void put(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        ring[head++ % RING_SIZE] = s[i];
}

int tusb_log_printf(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0)
        return n;
    if ((size_t)n >= sizeof line)
        n = sizeof line - 1;
    const uint32_t irq = save_and_disable_interrupts();
    const uint32_t us = time_us_32();
    char stamp[16];
    size_t from = 0;
    for (size_t i = 0; i < (size_t)n; i++) {
        if (at_line_start) {
            put(line + from, i - from);
            from = i;
            const int k = snprintf(stamp, sizeof stamp, "[%9lu] ", (unsigned long)us);
            put(stamp, (size_t)k);
            at_line_start = false;
        }
        if (line[i] == '\n')
            at_line_start = true;
    }
    put(line + from, (size_t)n - from);
    restore_interrupts(irq);
    return n;
}

size_t tusblog_read(size_t offset, void *dst, size_t max)
{
    const uint32_t irq = save_and_disable_interrupts();
    const uint32_t end = head;
    const uint32_t start = end > RING_SIZE ? end - RING_SIZE : 0;
    size_t n = 0;
    if (start + offset < end) {
        n = end - (start + offset);
        if (n > max)
            n = max;
        for (size_t i = 0; i < n; i++)
            ((char *)dst)[i] = ring[(start + offset + i) % RING_SIZE];
    }
    restore_interrupts(irq);
    return n;
}
