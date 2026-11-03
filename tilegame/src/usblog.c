#include "usblog.h"

#include <string.h>

#ifndef USBLOG_HOT
#define USBLOG_HOT
#endif

#define MAGIC 0x55534C47u   /* "USLG" */

void usblog_init(usblog_t *r)
{
    r->magic = MAGIC;
    r->magic_inv = ~MAGIC;
    r->head = 0;
    r->closed = USBLOG_CLOSED_NONE;
}

bool usblog_valid(const usblog_t *r)
{
    return r->magic == MAGIC && r->magic_inv == ~MAGIC && r->head != 0;
}

/* Called from interrupt handlers and from the program at once: the slot is taken with one atomic add, so each caller has its own. */
USBLOG_HOT void usblog_add(usblog_t *r, uint32_t t, uint8_t core, uint8_t type, uint16_t x, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    usblog_entry_t *e = &r->e[__atomic_fetch_add(&r->head, 1, __ATOMIC_RELAXED) & (USBLOG_ENTRIES - 1)];
    e->t = t;
    e->type = type;
    e->core = core;
    e->x = x;
    e->a = a;
    e->b = b;
    e->c = c;
    e->d = d;
}

USBLOG_HOT void usblog_close(usblog_t *r, uint32_t reason)
{
    r->closed = reason;
}

uint32_t usblog_count(const usblog_t *r)
{
    return r->head < USBLOG_ENTRIES ? r->head : USBLOG_ENTRIES;
}

size_t usblog_serialize(const usblog_t *r, void (*out)(void *ctx, const void *data, size_t size), void *ctx)
{
    const uint32_t count = usblog_count(r);
    const uint32_t header[8] = {0x4C425355u /* "USBL" */, 1, sizeof(usblog_entry_t), count, r->head, r->closed, 0, 0};
    out(ctx, header, sizeof header);
    const uint32_t first = r->head - count;   /* the number of the oldest event held */
    const uint32_t start = first & (USBLOG_ENTRIES - 1);
    const uint32_t to_end = USBLOG_ENTRIES - start < count ? USBLOG_ENTRIES - start : count;
    out(ctx, &r->e[start], to_end * sizeof(usblog_entry_t));
    if (count > to_end)
        out(ctx, &r->e[0], (count - to_end) * sizeof(usblog_entry_t));
    return sizeof header + (size_t)count * sizeof(usblog_entry_t);
}
