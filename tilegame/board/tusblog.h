/* SPDX-License-Identifier: MIT */
/* TinyUSB's debug output in a RAM ring (cmake -DTUSB_LOG=ON), see tusblog.c. */
#ifndef TUSBLOG_H
#define TUSBLOG_H

#include <stddef.h>

int tusb_log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Copies up to max bytes of the kept text, starting `offset` bytes into it (the last 96 KB are kept); returns the count, 0 at the end. */
size_t tusblog_read(size_t offset, void *dst, size_t max);

#endif
