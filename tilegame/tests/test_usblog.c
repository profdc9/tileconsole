/* Tests of the USB event ring (src/usblog.c): validity, wrap-around, the order and the file it makes. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usblog.h"

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

typedef struct { uint8_t data[sizeof(usblog_t) + 64]; size_t size; } sink_t;
static void collect(void *ctx, const void *data, size_t size)
{
    sink_t *s = ctx;
    memcpy(s->data + s->size, data, size);
    s->size += size;
}

static void test_valid(void)
{
    usblog_t *r = malloc(sizeof *r);
    memset(r, 0xA5, sizeof *r);                 /* RAM at power on: whatever */
    CHECK(!usblog_valid(r));
    usblog_init(r);
    CHECK(!usblog_valid(r) && usblog_count(r) == 0 && r->closed == 0);   /* initialised but empty */
    usblog_add(r, 100, 0, USBLOG_EV_ISR, 1, 2, 3, 4, 5);
    CHECK(usblog_valid(r) && usblog_count(r) == 1);
    r->magic_inv ^= 1;
    CHECK(!usblog_valid(r));                    /* each half of the magic matters */
    r->magic_inv ^= 1;
    r->magic ^= 1;
    CHECK(!usblog_valid(r));
    r->magic ^= 1;
    usblog_close(r, USBLOG_CLOSED_PANIC);
    CHECK(r->closed == USBLOG_CLOSED_PANIC && usblog_valid(r));
    usblog_init(r);
    CHECK(!usblog_valid(r) && r->closed == 0);  /* a new run starts clean */
    free(r);
}

static void test_serialize_few(void)
{
    usblog_t *r = malloc(sizeof *r);
    usblog_init(r);
    for (uint32_t i = 0; i < 5; i++)
        usblog_add(r, 1000 + i, (uint8_t)(i & 1), (uint8_t)(10 + i), (uint16_t)(20 + i), 30 + i, 40 + i, 50 + i, 60 + i);
    usblog_close(r, USBLOG_CLOSED_HANG);
    sink_t *s = calloc(1, sizeof *s);
    const size_t n = usblog_serialize(r, collect, s);
    CHECK(n == s->size && n == 32 + 5 * sizeof(usblog_entry_t));
    uint32_t h[8];
    memcpy(h, s->data, 32);
    CHECK(memcmp(&h[0], "USBL", 4) == 0 && h[1] == 1 && h[2] == sizeof(usblog_entry_t) && h[3] == 5 && h[4] == 5 && h[5] == USBLOG_CLOSED_HANG && h[6] == 0 && h[7] == 0);
    const usblog_entry_t *e = (const usblog_entry_t *)(s->data + 32);
    int order = 1;
    for (uint32_t i = 0; i < 5; i++)
        order &= e[i].t == 1000 + i && e[i].type == 10 + i && e[i].core == (i & 1) && e[i].x == 20 + i && e[i].a == 30 + i && e[i].b == 40 + i && e[i].c == 50 + i && e[i].d == 60 + i;
    CHECK(order);
    free(s);
    free(r);
}

static void test_wrap(void)
{
    usblog_t *r = malloc(sizeof *r);
    usblog_init(r);
    const uint32_t total = USBLOG_ENTRIES + 1000;
    for (uint32_t i = 0; i < total; i++)
        usblog_add(r, i, 0, 1, 0, i, 0, 0, 0);
    CHECK(usblog_count(r) == USBLOG_ENTRIES && r->head == total);
    sink_t *s = calloc(1, sizeof *s);
    const size_t n = usblog_serialize(r, collect, s);
    CHECK(n == 32 + (size_t)USBLOG_ENTRIES * sizeof(usblog_entry_t));
    uint32_t h[8];
    memcpy(h, s->data, 32);
    CHECK(h[3] == USBLOG_ENTRIES && h[4] == total);
    const usblog_entry_t *e = (const usblog_entry_t *)(s->data + 32);
    int order = 1;
    for (uint32_t i = 0; i < USBLOG_ENTRIES; i++)   /* the oldest still held is event 1000, then 1001, up to the newest */
        order &= e[i].a == 1000 + i;
    CHECK(order);
    /* exactly full, and one event short of full */
    usblog_init(r);
    for (uint32_t i = 0; i < USBLOG_ENTRIES; i++)
        usblog_add(r, i, 0, 1, 0, i, 0, 0, 0);
    memset(s, 0, sizeof *s);
    usblog_serialize(r, collect, s);
    e = (const usblog_entry_t *)(s->data + 32);
    CHECK(e[0].a == 0 && e[USBLOG_ENTRIES - 1].a == USBLOG_ENTRIES - 1);
    usblog_init(r);
    for (uint32_t i = 0; i < USBLOG_ENTRIES - 1; i++)
        usblog_add(r, i, 0, 1, 0, i, 0, 0, 0);
    CHECK(usblog_count(r) == USBLOG_ENTRIES - 1);
    free(s);
    free(r);
}

int main(void)
{
    test_valid();
    test_serialize_few();
    test_wrap();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
