/* SPDX-License-Identifier: MIT */
/* Tests of the keyboard to character map. Exits non-zero on the first failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keymap.h"

static int checks;
#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static uint32_t rng = 5;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

/* ---- the layout, written down key by key (not the tables the code uses) ---- */

static const struct { uint8_t usage; int plain, shifted; } pairs[] = {
    {0x1E, '1', '!'}, {0x1F, '2', '@'}, {0x20, '3', '#'}, {0x21, '4', '$'}, {0x22, '5', '%'}, {0x23, '6', '^'}, {0x24, '7', '&'},
    {0x25, '8', '*'}, {0x26, '9', '('}, {0x27, '0', ')'}, {0x28, 10, 10},   {0x29, 27, 27},   {0x2A, 8, 8},     {0x2B, 9, 9},
    {0x2C, ' ', ' '}, {0x2D, '-', '_'}, {0x2E, '=', '+'}, {0x2F, '[', '{'}, {0x30, ']', '}'}, {0x31, '\\', '|'}, {0x32, '#', '~'},
    {0x33, ';', ':'}, {0x34, '\'', '"'}, {0x35, '`', '~'}, {0x36, ',', '<'}, {0x37, '.', '>'}, {0x38, '/', '?'}, {0x64, '\\', '|'},
};
static const struct { uint8_t usage; int code; } nav[] = {
    {0x49, PLAT_KEY_INSERT}, {0x4A, PLAT_KEY_HOME}, {0x4B, PLAT_KEY_PAGEUP}, {0x4C, 127},           {0x4D, PLAT_KEY_END},
    {0x4E, PLAT_KEY_PAGEDOWN}, {0x4F, PLAT_KEY_RIGHT}, {0x50, PLAT_KEY_LEFT}, {0x51, PLAT_KEY_DOWN}, {0x52, PLAT_KEY_UP},
};
static const struct { uint8_t usage; int with_num, without_num; } keypad[] = {
    {0x54, '/', '/'}, {0x55, '*', '*'}, {0x56, '-', '-'}, {0x57, '+', '+'}, {0x58, 10, 10},
    {0x59, '1', PLAT_KEY_END}, {0x5A, '2', PLAT_KEY_DOWN}, {0x5B, '3', PLAT_KEY_PAGEDOWN}, {0x5C, '4', PLAT_KEY_LEFT},
    {0x5D, '5', -1}, {0x5E, '6', PLAT_KEY_RIGHT}, {0x5F, '7', PLAT_KEY_HOME}, {0x60, '8', PLAT_KEY_UP}, {0x61, '9', PLAT_KEY_PAGEUP},
    {0x62, '0', PLAT_KEY_INSERT}, {0x63, '.', 127},
};

static int ref(int usage, int mod, int caps, int num)
{
    const int shift = (mod & 0x02) || (mod & 0x20), ctrl = (mod & 0x01) || (mod & 0x10);
    if (usage >= 0x04 && usage <= 0x1D) {
        const int lower = 'a' + usage - 4;
        if (ctrl)
            return lower - 'a' + 1;
        return (shift ? !caps : caps) ? lower - 32 : lower;
    }
    if (usage >= 0x3A && usage <= 0x45)
        return PLAT_KEY_F1 + usage - 0x3A;
    for (size_t i = 0; i < sizeof nav / sizeof *nav; i++)
        if (nav[i].usage == usage)
            return nav[i].code;
    for (size_t i = 0; i < sizeof keypad / sizeof *keypad; i++)
        if (keypad[i].usage == usage)
            return num ? keypad[i].with_num : keypad[i].without_num;
    for (size_t i = 0; i < sizeof pairs / sizeof *pairs; i++)
        if (pairs[i].usage == usage) {
            const int ch = shift ? pairs[i].shifted : pairs[i].plain;
            if (ctrl && ch == ' ')
                return 0;
            if (ctrl && ch >= '@' && ch <= '_')
                return ch - '@';
            return ch;
        }
    return -1;
}

static void test_layout(void)
{
    for (int usage = 0; usage < 256; usage++)
        for (int mod = 0; mod < 256; mod++)
            for (int lock = 0; lock < 4; lock++)
                CHECK(keymap_translate((uint8_t)usage, (uint8_t)mod, lock & 1, lock >> 1) == ref(usage, mod, lock & 1, lock >> 1));
    /* A few keys spelled out, so that the reference itself is held to something. */
    CHECK(keymap_translate(0x04, 0, 0, 1) == 'a' && keymap_translate(0x04, 0x02, 0, 1) == 'A' && keymap_translate(0x04, 0x20, 0, 1) == 'A');
    CHECK(keymap_translate(0x04, 0, 1, 1) == 'A' && keymap_translate(0x04, 0x02, 1, 1) == 'a');
    CHECK(keymap_translate(0x1D, 0, 0, 1) == 'z' && keymap_translate(0x04, 0x01, 0, 1) == 1 && keymap_translate(0x1D, 0x10, 0, 1) == 26);
    CHECK(keymap_translate(0x1E, 0, 1, 1) == '1' && keymap_translate(0x1E, 0x02, 0, 1) == '!' && keymap_translate(0x27, 0x02, 0, 1) == ')');
    CHECK(keymap_translate(0x28, 0, 0, 1) == 10 && keymap_translate(0x29, 0, 0, 1) == 27 && keymap_translate(0x2A, 0, 0, 1) == 8);
    CHECK(keymap_translate(0x2B, 0, 0, 1) == 9 && keymap_translate(0x2C, 0, 0, 1) == ' ' && keymap_translate(0x4C, 0, 0, 1) == 127);
    CHECK(keymap_translate(0x2F, 0x01, 0, 1) == 27 && keymap_translate(0x30, 0x01, 0, 1) == 29 && keymap_translate(0x31, 0x01, 0, 1) == 28);
    CHECK(keymap_translate(0x23, 0x03, 0, 1) == 30 && keymap_translate(0x2D, 0x03, 0, 1) == 31 && keymap_translate(0x1F, 0x03, 0, 1) == 0);
    CHECK(keymap_translate(0x2C, 0x01, 0, 1) == 0 && keymap_translate(0x27, 0x01, 0, 1) == '0');       /* ctrl+space; ctrl+0 is just 0 */
    CHECK(keymap_translate(0x3A, 0, 0, 1) == PLAT_KEY_F(1) && keymap_translate(0x45, 0, 0, 1) == PLAT_KEY_F(12) && PLAT_KEY_F(12) == 0x11B);
    CHECK(keymap_translate(0x52, 0, 0, 1) == PLAT_KEY_UP && keymap_translate(0x5D, 0, 0, 1) == '5' && keymap_translate(0x5D, 0, 0, 0) == -1);
    CHECK(keymap_translate(0x04, 0x04, 0, 1) == 'a' && keymap_translate(0x04, 0x08, 0, 1) == 'a');    /* alt and the windows key do nothing */
    CHECK(keymap_translate(0x39, 0, 0, 1) == -1 && keymap_translate(0x53, 0, 0, 1) == -1 && keymap_translate(0x46, 0, 0, 1) == -1);
}

/* ---- reports ---- */

static void report(keymap_t *k, uint8_t mod, uint8_t k0, uint8_t k1, uint8_t k2, uint32_t now)
{
    const uint8_t r[8] = {mod, 0, k0, k1, k2, 0, 0, 0};
    keymap_report(k, r, 8, now);
}

static void expect(const int *want, int n)
{
    for (int i = 0; i < n; i++)
        CHECK(keymap_getchar() == want[i]);
    CHECK(keymap_getchar() == -1);
}

static void test_reports(void)
{
    keymap_t k;
    keymap_init(&k);
    keymap_flush();
    CHECK(keymap_getchar() == -1);
    report(&k, 0, 0x04, 0, 0, 0);                     /* a */
    report(&k, 0, 0x04, 0, 0, 10);                    /* the same report again: still down, typed once */
    report(&k, 0, 0, 0, 0, 20);                       /* up */
    report(&k, 0, 0x04, 0, 0, 30);                    /* and again */
    report(&k, 0, 0x04, 0x05, 0, 40);                 /* b joins */
    report(&k, 0, 0x05, 0x04, 0, 50);                 /* the same two, in the other order: nothing new */
    report(&k, 0, 0, 0, 0, 60);
    int a[] = {'a', 'a', 'b'};
    expect(a, 3);
    report(&k, 0, 0x06, 0x07, 0x08, 100);             /* three keys at once, in the order of the report */
    report(&k, 0, 0, 0, 0, 110);
    int b[] = {'c', 'd', 'e'};
    expect(b, 3);

    /* Modifiers: they count at the moment the key goes down, and alone they type nothing. */
    report(&k, 0x02, 0, 0, 0, 200);                   /* shift alone */
    report(&k, 0x02, 0x04, 0, 0, 210);                /* shift + a */
    report(&k, 0, 0x04, 0, 0, 220);                   /* shift let go with a still down: nothing new */
    report(&k, 0, 0, 0, 0, 230);
    report(&k, 0, 0x04, 0, 0, 240);
    report(&k, 0x02, 0x04, 0, 0, 250);                /* shift pressed after a: a is not typed again */
    report(&k, 0x01, 0x06, 0, 0, 260);                /* ctrl+c ... a let go, c new */
    report(&k, 0, 0, 0, 0, 270);
    int c[] = {'A', 'a', 3};
    expect(c, 3);

    /* Reports that must be ignored. */
    uint8_t rollover[8] = {0, 0, 1, 1, 1, 1, 1, 1};
    keymap_report(&k, rollover, 8, 300);
    uint8_t rollover2[8] = {0, 0, 0x04, 0x05, 1, 0, 0, 0};
    keymap_report(&k, rollover2, 8, 301);             /* an error code among real keys: all of it is ignored */
    uint8_t shortr[7] = {0, 0, 0x04, 0, 0, 0, 0};
    keymap_report(&k, shortr, 7, 302);
    keymap_report(&k, shortr, 0, 303);
    keymap_report(&k, shortr, -4, 304);
    CHECK(keymap_getchar() == -1);
    report(&k, 0, 0x04, 0, 0, 310);                   /* the state was not disturbed: a is new */
    report(&k, 0, 0, 0, 0, 320);
    CHECK(keymap_getchar() == 'a' && keymap_getchar() == -1);
    report(&k, 0, 0x04, 0, 0, 330);                   /* a rollover report in the middle of a held key is not a release */
    keymap_report(&k, rollover, 8, 340);
    report(&k, 0, 0x04, 0, 0, 350);
    report(&k, 0, 0, 0, 0, 360);
    CHECK(keymap_getchar() == 'a' && keymap_getchar() == -1);

    /* Codes below 4 (none, error, post fail, undefined) are not keys. */
    report(&k, 0, 2, 3, 0, 400);
    report(&k, 0, 0, 0, 0, 410);
    CHECK(keymap_getchar() == -1);
    /* Keys that type nothing are fine too. */
    report(&k, 0, 0x46, 0x68, 0xE0, 420);
    report(&k, 0, 0, 0, 0, 430);
    CHECK(keymap_getchar() == -1);
}

/* Six keys down at once (the most a boot keyboard report holds), the error code in any slot, and the codes that are not keys. */
static void test_slots(void)
{
    keymap_t k;
    keymap_init(&k);
    keymap_flush();
    const uint8_t six[8] = {0, 0, 4, 5, 6, 7, 8, 9};
    keymap_report(&k, six, 8, 0);
    int a[] = {'a', 'b', 'c', 'd', 'e', 'f'};
    expect(a, 6);
    keymap_report(&k, six, 8, 10);                    /* the same six: nothing new */
    const uint8_t reordered[8] = {0, 0, 9, 8, 7, 6, 5, 4};
    keymap_report(&k, reordered, 8, 20);              /* in another order: still nothing new */
    CHECK(keymap_getchar() == -1);
    const uint8_t swapped[8] = {0, 0, 9, 8, 7, 6, 5, 10};   /* a goes up, g goes down in the last slot */
    keymap_report(&k, swapped, 8, 30);
    CHECK(keymap_getchar() == 'g' && keymap_getchar() == -1);
    const uint8_t again[8] = {0, 0, 4, 8, 7, 6, 5, 10};     /* a down again, in the first slot: new; the others held */
    keymap_report(&k, again, 8, 40);
    CHECK(keymap_getchar() == 'a' && keymap_getchar() == -1);
    keymap_report(&k, again, 8, 50);
    CHECK(keymap_getchar() == -1);
    /* Whatever is in the last slot of the last report counts as held (so it is not typed a second time). */
    const uint8_t last_only[8] = {0, 0, 0, 0, 0, 0, 0, 11};
    keymap_report(&k, last_only, 8, 60);
    CHECK(keymap_getchar() == 'h');
    keymap_report(&k, last_only, 8, 70);
    CHECK(keymap_getchar() == -1);
    keymap_report(&k, six, 8, 80);                    /* h is up and none of the six was down in the last report: all six are new */
    expect(a, 6);
    /* The error code in the last slot (and in the others) makes the whole report worthless. */
    keymap_init(&k);
    for (int slot = 2; slot < 8; slot++) {
        uint8_t r[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        r[slot == 2 ? 3 : 2] = 4;                     /* a real key... */
        r[slot] = 1;                                  /* ...and the error code */
        keymap_report(&k, r, 8, 100);
        CHECK(keymap_getchar() == -1);
    }
    /* The codes 2 and 3 are not keys: they type nothing and do not take over the repeat from the key that is held. */
    keymap_init(&k);
    keymap_flush();
    report(&k, 0, 0x04, 0, 0, 0);
    CHECK(keymap_getchar() == 'a');
    report(&k, 0, 0x04, 2, 3, 100);
    for (uint32_t t = 100; t < 540; t++)
        keymap_tick(&k, t);
    CHECK(keymap_getchar() == 'a' && keymap_getchar() == 'a' && keymap_getchar() == -1);   /* repeats at 500 and 533 */
    report(&k, 0, 0, 0, 0, 600);
    keymap_flush();
}

static void test_locks(void)
{
    keymap_t k;
    keymap_init(&k);
    keymap_flush();
    CHECK(k.num_lock == 1 && k.caps_lock == 0);
    report(&k, 0, 0x39, 0, 0, 0);                     /* caps lock down and up: on */
    report(&k, 0, 0, 0, 0, 10);
    report(&k, 0, 0x04, 0x1E, 0, 20);                 /* a and 1 */
    report(&k, 0x02, 0x04, 0x1E, 0, 30);              /* with shift held, nothing new */
    report(&k, 0, 0, 0, 0, 40);
    report(&k, 0x02, 0x04, 0, 0, 50);                 /* shift + a with caps lock: lower case */
    report(&k, 0, 0, 0, 0, 60);
    report(&k, 0, 0x39, 0, 0, 70);                    /* off again */
    report(&k, 0, 0, 0, 0, 80);
    report(&k, 0, 0x04, 0, 0, 90);
    report(&k, 0, 0, 0, 0, 100);
    int a[] = {'A', '1', 'a', 'a'};
    expect(a, 4);
    CHECK(k.caps_lock == 0);
    report(&k, 0, 0x59, 0, 0, 200);                   /* keypad 1 with num lock on */
    report(&k, 0, 0, 0, 0, 210);
    report(&k, 0, 0x53, 0, 0, 220);                   /* num lock off */
    report(&k, 0, 0, 0, 0, 230);
    report(&k, 0, 0x59, 0x5D, 0x60, 240);             /* End, nothing, Up */
    report(&k, 0, 0, 0, 0, 250);
    report(&k, 0, 0x53, 0, 0, 260);                   /* back on */
    report(&k, 0, 0, 0, 0, 270);
    report(&k, 0, 0x5D, 0, 0, 280);
    int b[] = {'1', PLAT_KEY_END, PLAT_KEY_UP, '5'};
    expect(b, 4);
    /* Two keyboards have their own locks. */
    keymap_t k2;
    keymap_init(&k2);
    report(&k, 0, 0x39, 0, 0, 300);
    report(&k, 0, 0, 0, 0, 310);
    report(&k, 0, 0x04, 0, 0, 320);
    report(&k2, 0, 0x04, 0, 0, 330);
    int c[] = {'A', 'a'};
    expect(c, 2);
}

static int drain(int *out, int max)
{
    int n = 0, ch;
    while ((ch = keymap_getchar()) >= 0 && n < max)
        out[n++] = ch;
    return n;
}

static void test_repeat(void)
{
    keymap_t k;
    keymap_init(&k);
    keymap_flush();
    /* x goes down at 1000: typed at once, then nothing until 1500, then every 33 ms. */
    report(&k, 0, 0x1B, 0, 0, 1000);
    CHECK(keymap_getchar() == 'x' && keymap_getchar() == -1);
    for (uint32_t t = 1000; t < 2000; t++) {
        keymap_tick(&k, t);
        const int ch = keymap_getchar();
        const int should = t >= 1500 && (t - 1500) % 33 == 0;
        CHECK((ch == 'x') == should);
        CHECK(ch == -1 || ch == 'x');
    }
    report(&k, 0, 0, 0, 0, 2000);                     /* up: the repeat stops */
    for (uint32_t t = 2000; t < 2300; t++)
        keymap_tick(&k, t);
    CHECK(keymap_getchar() == -1);

    /* Held with shift: the repeat is the capital, even when shift is let go (the same key goes on repeating what it typed). */
    report(&k, 0x02, 0x1B, 0, 0, 3000);
    report(&k, 0, 0x1B, 0, 0, 3100);
    for (uint32_t t = 3000; t < 3700; t++)
        keymap_tick(&k, t);
    int out[64];
    int n = drain(out, 64);
    CHECK(n == 1 + (3700 - 1 - 3500) / 33 + 1 && out[0] == 'X');
    for (int i = 0; i < n; i++)
        CHECK(out[i] == 'X');
    report(&k, 0, 0, 0, 0, 3700);
    keymap_flush();

    /* The newest key repeats; letting go of it ends the repeat, though the older key is still down. */
    report(&k, 0, 0x04, 0, 0, 4000);                  /* a */
    report(&k, 0, 0x04, 0x05, 0, 4100);               /* b: now b repeats, at 4600 */
    for (uint32_t t = 4000; t < 4700; t++)
        keymap_tick(&k, t);
    n = drain(out, 64);
    CHECK(out[0] == 'a' && out[1] == 'b');
    for (int i = 2; i < n; i++)
        CHECK(out[i] == 'b');
    CHECK(n == 2 + 1 + (4699 - 4600) / 33);
    report(&k, 0, 0x04, 0, 0, 4700);                  /* b up, a still down: nothing repeats */
    for (uint32_t t = 4700; t < 5500; t++)
        keymap_tick(&k, t);
    CHECK(keymap_getchar() == -1);
    report(&k, 0, 0, 0, 0, 5500);

    /* Keys that type nothing, and the lock keys, do not repeat, and a key that types nothing stops the repeat of the one before. */
    report(&k, 0, 0x04, 0, 0, 6000);
    report(&k, 0, 0x04, 0x39, 0, 6100);               /* caps lock pressed while a is held: a no longer repeats */
    for (uint32_t t = 6000; t < 7000; t++)
        keymap_tick(&k, t);
    CHECK(keymap_getchar() == 'a' && keymap_getchar() == -1);
    report(&k, 0, 0, 0, 0, 7000);
    report(&k, 0x02, 0, 0, 0, 7100);                  /* a modifier alone */
    report(&k, 0, 0x46, 0, 0, 7200);                  /* print screen */
    for (uint32_t t = 7000; t < 8000; t++)
        keymap_tick(&k, t);
    CHECK(keymap_getchar() == -1);
    report(&k, 0, 0, 0, 0, 8000);
    keymap_init(&k);                                  /* caps lock was toggled above: start again */
    keymap_flush();

    /* The cursor keys repeat, with a late tick giving one repeat, not a burst. */
    report(&k, 0, 0x50, 0, 0, 10000);
    CHECK(keymap_getchar() == PLAT_KEY_LEFT);
    keymap_tick(&k, 10499);
    CHECK(keymap_getchar() == -1);
    keymap_tick(&k, 10500);
    CHECK(keymap_getchar() == PLAT_KEY_LEFT && keymap_getchar() == -1);
    keymap_tick(&k, 12000);                           /* 1.5 s without a tick: one character, not 45 */
    CHECK(keymap_getchar() == PLAT_KEY_LEFT && keymap_getchar() == -1);
    keymap_tick(&k, 12000 + 32);
    CHECK(keymap_getchar() == -1);
    keymap_tick(&k, 12000 + 33);
    CHECK(keymap_getchar() == PLAT_KEY_LEFT);
    report(&k, 0, 0, 0, 0, 12100);
    keymap_flush();

    /* Ctrl+letter repeats the control code. */
    report(&k, 0x01, 0x06, 0, 0, 20000);
    keymap_tick(&k, 20500);
    CHECK(keymap_getchar() == 3 && keymap_getchar() == 3 && keymap_getchar() == -1);
    report(&k, 0, 0, 0, 0, 20600);

    /* The millisecond clock wraps round. */
    report(&k, 0, 0x04, 0, 0, 0xFFFFFF00u);
    keymap_flush();
    keymap_tick(&k, 0xFFFFFF00u + 499);
    CHECK(keymap_getchar() == -1);
    keymap_tick(&k, 0xFFFFFF00u + 500);                /* this is 244 after the wrap */
    CHECK(keymap_getchar() == 'a');
    keymap_tick(&k, 0xFFFFFF00u + 500 + 33);
    CHECK(keymap_getchar() == 'a' && keymap_getchar() == -1);
    report(&k, 0, 0, 0, 0, 1000);
    keymap_flush();
}

static void test_queue_and_release(void)
{
    keymap_t k;
    keymap_init(&k);
    keymap_flush();
    /* The queue holds KEYMAP_QUEUE characters; more are dropped, and it works round its ring. */
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < KEYMAP_QUEUE + 10; i++) {
            report(&k, 0, (uint8_t)(0x04 + i % 26), 0, 0, (uint32_t)i);
            report(&k, 0, 0, 0, 0, (uint32_t)i);
        }
        for (int i = 0; i < KEYMAP_QUEUE; i++)
            CHECK(keymap_getchar() == 'a' + i % 26);
        CHECK(keymap_getchar() == -1);
    }
    /* A partly read queue, then more. */
    for (int i = 0; i < 40; i++) {
        report(&k, 0, 0x04, 0, 0, 0);
        report(&k, 0, 0, 0, 0, 0);
    }
    for (int i = 0; i < 30; i++)
        CHECK(keymap_getchar() == 'a');
    for (int i = 0; i < 50; i++) {
        report(&k, 0, 0x05, 0, 0, 0);
        report(&k, 0, 0, 0, 0, 0);
    }
    for (int i = 0; i < 10; i++)
        CHECK(keymap_getchar() == 'a');
    for (int i = 0; i < 50; i++)
        CHECK(keymap_getchar() == 'b');
    CHECK(keymap_getchar() == -1);
    keymap_flush();
    report(&k, 0, 0x04, 0, 0, 0);
    report(&k, 0, 0, 0, 0, 0);
    keymap_flush();
    CHECK(keymap_getchar() == -1);

    /* Unplugging: a held key stops repeating, and keys that were down count as new when the keyboard is back; the locks stay. */
    report(&k, 0, 0x39, 0, 0, 100);                    /* caps lock on */
    report(&k, 0, 0x04, 0, 0, 200);
    CHECK(keymap_getchar() == 'A');
    keymap_release_all(&k);
    for (uint32_t t = 200; t < 2000; t++)
        keymap_tick(&k, t);
    CHECK(keymap_getchar() == -1);
    CHECK(k.caps_lock == 1);
    report(&k, 0, 0x04, 0, 0, 3000);                   /* the same key, down in the first report of the "new" keyboard */
    CHECK(keymap_getchar() == 'A');
    report(&k, 0, 0, 0, 0, 3100);

    /* Two keyboards share the queue but not their state. */
    keymap_t k2;
    keymap_init(&k2);
    keymap_flush();
    keymap_init(&k);
    report(&k, 0, 0x04, 0, 0, 0);                      /* a down on the first */
    report(&k2, 0, 0x05, 0, 0, 1);                     /* b down on the second: it is not a release of a */
    report(&k, 0, 0x04, 0, 0, 2);                      /* the first goes on holding a: nothing new */
    int out[8];
    CHECK(drain(out, 8) == 2 && out[0] == 'a' && out[1] == 'b');
    for (uint32_t t = 0; t < 520; t++) {
        keymap_tick(&k, t);
        keymap_tick(&k2, t);
    }
    int n = drain(out, 8);
    CHECK(n == 2 && out[0] == 'a' && out[1] == 'b');   /* both repeat: a at 500 and b at 501 (their next ones are 533 and 534) */
}

static int next_event(plat_key_event_t *e)
{
    return keymap_getevent(e);
}

static void test_events(void)
{
    keymap_t k;
    plat_key_event_t e;
    keymap_init(&k);
    keymap_flush();
    CHECK(!next_event(&e));
    /* a key press is an event with its usage, the modifiers at that moment and the lock keys (num lock is on to start with) */
    report(&k, 0, 0x04, 0, 0, 0);
    CHECK(next_event(&e) && e.usage == 0x04 && e.modifiers == 0 && e.caps_lock == 0 && e.num_lock == 1);
    CHECK(!next_event(&e));
    CHECK(keymap_getchar() == 'a' && keymap_getchar() == -1);   /* the characters are a separate stream: reading events did not take from it */
    report(&k, 0, 0, 0, 0, 10);
    CHECK(!next_event(&e));                              /* letting go is nothing */
    /* the modifiers, each of the eight bits, and the keys that make no character */
    for (int bit = 0; bit < 8; bit++) {
        report(&k, (uint8_t)(1 << bit), 0, 0, 0, 100 + bit * 10);
        CHECK(!next_event(&e));                          /* a modifier alone is no event */
        report(&k, (uint8_t)(1 << bit), 0x52, 0, 0, 105 + bit * 10);   /* up arrow */
        CHECK(next_event(&e) && e.usage == 0x52 && e.modifiers == (1 << bit));
        report(&k, 0, 0, 0, 0, 108 + bit * 10);
    }
    keymap_flush();
    report(&k, 0x05, 0x3A, 0, 0, 300);                   /* ctrl+alt+F1 */
    CHECK(next_event(&e) && e.usage == 0x3A && e.modifiers == 0x05);
    report(&k, 0x44, 0x07, 0, 0, 310);                   /* right alt + right... both shifts' bits as sent: right alt (0x40) and right shift (0x04 is left alt) */
    CHECK(next_event(&e) && e.usage == 0x07 && e.modifiers == 0x44);
    report(&k, 0, 0, 0, 0, 320);
    /* the lock keys are events, and the state they leave shows in the next ones */
    report(&k, 0, 0x39, 0, 0, 400);                      /* caps lock */
    CHECK(next_event(&e) && e.usage == 0x39 && e.caps_lock == 1 && e.num_lock == 1);
    report(&k, 0, 0, 0, 0, 410);
    report(&k, 0, 0x53, 0, 0, 420);                      /* num lock */
    CHECK(next_event(&e) && e.usage == 0x53 && e.caps_lock == 1 && e.num_lock == 0);
    report(&k, 0, 0, 0, 0, 430);
    report(&k, 0, 0x05, 0, 0, 440);
    CHECK(next_event(&e) && e.usage == 0x05 && e.caps_lock == 1 && e.num_lock == 0);
    report(&k, 0, 0, 0, 0, 450);
    CHECK(!next_event(&e));
    /* the repeat of a held key is events too, with the modifiers held at the time, as many as characters */
    keymap_flush();
    report(&k, 0x01, 0x07, 0, 0, 1000);                  /* ctrl+d */
    CHECK(next_event(&e) && e.usage == 0x07 && e.modifiers == 0x01 && !next_event(&e));
    int n = 0;
    for (uint32_t t = 1000; t < 1700; t++) {
        keymap_tick(&k, t);
        while (next_event(&e)) {
            CHECK(e.usage == 0x07 && e.modifiers == 0x01);
            n++;
        }
    }
    CHECK(n == 1 + (1700 - 1 - 1500) / 33);
    report(&k, 0x02, 0x07, 0, 0, 1700);                  /* shift joins the held key: later repeats carry the shift too */
    report(&k, 0x02, 0x07, 0, 0, 1710);
    keymap_flush();
    for (uint32_t t = 1710; t < 2400; t++)
        keymap_tick(&k, t);
    CHECK(next_event(&e) && e.usage == 0x07 && e.modifiers == 0x02);
    report(&k, 0, 0, 0, 0, 2400);
    /* a key that types nothing is an event once and does not repeat: the keypad 5 with num lock off, print screen */
    keymap_flush();
    report(&k, 0, 0x46, 0, 0, 3000);
    CHECK(next_event(&e) && e.usage == 0x46);
    for (uint32_t t = 3000; t < 4000; t++)
        keymap_tick(&k, t);
    CHECK(!next_event(&e));
    report(&k, 0, 0, 0, 0, 4000);
    /* a rollover error report makes nothing, as before; a short report neither */
    keymap_flush();
    { const uint8_t bad[8] = {0, 0, 1, 1, 1, 1, 1, 1}; keymap_report(&k, bad, 8, 5000); }
    { const uint8_t shorty[4] = {0, 0, 4, 0}; keymap_report(&k, shorty, 4, 5010); }
    CHECK(!next_event(&e));
    /* the queue holds KEYMAP_QUEUE events and drops the rest; flush empties both streams */
    for (int i = 0; i < KEYMAP_QUEUE + 10; i++) {
        report(&k, 0, (uint8_t)(0x04 + i % 20), 0, 0, 6000 + i * 20);
        report(&k, 0, 0, 0, 0, 6010 + i * 20);
    }
    n = 0;
    while (next_event(&e)) {
        CHECK(e.usage == 0x04 + n % 20);
        n++;
    }
    CHECK(n == KEYMAP_QUEUE);
    report(&k, 0, 0x04, 0, 0, 9000);
    keymap_flush();
    CHECK(!next_event(&e) && keymap_getchar() == -1);
    /* the two streams drain independently: chars taken, events still there */
    report(&k, 0, 0x05, 0, 0, 9100);
    CHECK(keymap_getchar() == 'B');   /* (caps lock was turned on above) */
    CHECK(next_event(&e) && e.usage == 0x05);
    report(&k, 0, 0, 0, 0, 9200);
    /* two keyboards share the queue, each with its own lock keys */
    keymap_t k1, k2;
    keymap_init(&k1);
    keymap_init(&k2);
    keymap_flush();
    report(&k1, 0, 0x39, 0, 0, 9300);                    /* caps lock on the first */
    report(&k2, 0, 0x04, 0, 0, 9310);
    CHECK(next_event(&e) && e.usage == 0x39 && e.caps_lock == 1);
    CHECK(next_event(&e) && e.usage == 0x04 && e.caps_lock == 0);   /* not on the second */
    report(&k1, 0, 0, 0, 0, 9400);
    report(&k2, 0, 0, 0, 0, 9400);
}

static void test_fuzz(void)
{
    keymap_t k;
    keymap_init(&k);
    keymap_flush();
    uint32_t now = 0;
    for (int i = 0; i < 400000; i++) {
        now += rnd() % 40;
        const uint32_t what = rnd() % 8;
        if (what == 0) {
            keymap_tick(&k, now);
        } else if (what == 1) {
            keymap_release_all(&k);
        } else if (what == 2) {
            uint8_t r[16];
            for (int j = 0; j < 16; j++)
                r[j] = (uint8_t)rnd();
            keymap_report(&k, r, (int)(rnd() % 17), now);
        } else {
            uint8_t r[8] = {(uint8_t)(rnd() % 3 ? 0 : rnd()), 0, 0, 0, 0, 0, 0, 0};
            const int held = (int)(rnd() % 4);
            for (int j = 0; j < held; j++)
                r[2 + j] = (uint8_t)(rnd() % 2 ? 4 + rnd() % 0x60 : rnd());
            keymap_report(&k, r, 8, now);
        }
        for (int j = 0; j < 2; j++) {
            const int ch = keymap_getchar();
            CHECK(ch == -1 || (ch >= 0 && ch < 128) || (ch >= PLAT_KEY_UP && ch <= PLAT_KEY_F(12)));
        }
    }
}

int main(void)
{
    test_layout();
    test_reports();
    test_slots();
    test_locks();
    test_repeat();
    test_events();
    test_queue_and_release();
    test_fuzz();
    printf("all %d checks passed\n", checks);
    return 0;
}
