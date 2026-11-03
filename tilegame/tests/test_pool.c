/* Tests of the memory pools (src/pool.c): alignment, exhaustion, reuse, independence, and a long random run checked by contents. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define REGION (256 * 1024)

static uint64_t rng = 12345;
static uint32_t rnd(void)
{
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(rng >> 33);
}

static void test_basic(void)
{
    uint64_t *mem = malloc(REGION);
    pool_t *p = pool_create(mem, REGION);
    CHECK(p);
    const size_t fresh = pool_free_bytes(p);
    CHECK(fresh == REGION - 944);   /* (the allocator's own state takes 944 bytes of the region) */
    for (size_t n = 1; n < 300; n += 7) {
        void *b = pool_malloc(p, n);
        CHECK(b && ((uintptr_t)b & 7) == 0);
        memset(b, 0xAB, n);
        pool_free(p, b);
    }
    CHECK(pool_free_bytes(p) == fresh);

    /* calloc zeroes, and an overflowing count * size fails */
    unsigned char *z = pool_calloc(p, 100, 4);
    CHECK(z);
    int zero = 1;
    for (int i = 0; i < 400; i++)
        zero &= z[i] == 0;
    CHECK(zero);
    CHECK(pool_calloc(p, SIZE_MAX / 2, 4) == NULL);
    pool_free(p, z);

    /* realloc keeps the contents, both growing and shrinking */
    unsigned char *r = pool_malloc(p, 50);
    for (int i = 0; i < 50; i++)
        r[i] = (unsigned char)i;
    r = pool_realloc(p, r, 5000);
    CHECK(r);
    int same = 1;
    for (int i = 0; i < 50; i++)
        same &= r[i] == i;
    CHECK(same);
    r = pool_realloc(p, r, 20);
    same = 1;
    for (int i = 0; i < 20; i++)
        same &= r[i] == i;
    CHECK(same);
    pool_free(p, r);
    CHECK(pool_realloc(p, NULL, 10) != NULL);   /* (a realloc of NULL is a malloc) */

    /* a request larger than the pool fails and changes nothing */
    const size_t before = pool_free_bytes(p);
    CHECK(pool_malloc(p, REGION) == NULL);
    CHECK(pool_free_bytes(p) == before);
    pool_free(p, NULL);   /* (harmless) */

    /* all the memory can be used: allocate until it fails, then free it all and get it back */
    void **blocks = malloc(sizeof(void *) * (REGION / 32));
    size_t n = 0;
    pool_destroy(p);
    p = pool_create(mem, REGION);
    for (;;) {
        void *b = pool_malloc(p, 32);
        if (!b)
            break;
        blocks[n++] = b;
    }
    CHECK(n > REGION / 48);
    CHECK(pool_free_bytes(p) < 128);   /* (what is left is the allocator's own reserve at the end of the pool) */
    for (size_t i = 0; i < n; i++)
        pool_free(p, blocks[i]);
    CHECK(pool_free_bytes(p) == fresh);
    CHECK(pool_malloc(p, REGION - 8192) != NULL);
    free(blocks);
    pool_destroy(p);
    free(mem);
}

static void test_tiny_and_destroy(void)
{
    char small[512] __attribute__((aligned(8)));
    CHECK(pool_create(small, sizeof small) == NULL);
    pool_destroy(NULL);   /* (harmless) */

    /* a destroyed pool's memory is a fresh pool again: leaked blocks are not a problem */
    uint64_t *mem = malloc(64 * 1024);
    pool_t *p = pool_create(mem, 64 * 1024);
    const size_t fresh = pool_free_bytes(p);
    for (int i = 0; i < 100; i++)
        CHECK(pool_malloc(p, 100));
    CHECK(pool_free_bytes(p) < fresh - 10000);
    pool_destroy(p);
    p = pool_create(mem, 64 * 1024);
    CHECK(pool_free_bytes(p) == fresh);
    pool_destroy(p);
    free(mem);
}

/* two pools side by side do not touch each other's memory or each other's free counts */
static void test_independent(void)
{
    uint64_t *mem = malloc(128 * 1024);
    pool_t *a = pool_create(mem, 64 * 1024);
    pool_t *b = pool_create((char *)mem + 64 * 1024, 64 * 1024);
    CHECK(a && b && a != b);
    const size_t fa = pool_free_bytes(a), fb = pool_free_bytes(b);
    char *x = pool_malloc(a, 1000), *y = pool_malloc(b, 1000);
    memset(x, 1, 1000);
    memset(y, 2, 1000);
    CHECK(x >= (char *)mem && x < (char *)mem + 64 * 1024);
    CHECK(y >= (char *)mem + 64 * 1024 && y < (char *)mem + 128 * 1024);
    CHECK(pool_free_bytes(a) < fa && pool_free_bytes(b) < fb);
    pool_destroy(a);   /* (a's blocks are gone; b's are not) */
    CHECK(pool_free_bytes(b) < fb);
    int ok = 1;
    for (int i = 0; i < 1000; i++)
        ok &= y[i] == 2;
    CHECK(ok);
    pool_free(b, y);
    CHECK(pool_free_bytes(b) == fb);
    pool_destroy(b);
    free(mem);
}

/* thousands of random allocations, reallocations and frees; every block holds a pattern of its own, checked before it goes */
#define SLOTS 400
static void test_random(void)
{
    uint64_t *mem = malloc(REGION);
    pool_t *p = pool_create(mem, REGION);
    const size_t fresh = pool_free_bytes(p);
    struct { unsigned char *p; size_t n; unsigned char tag; } s[SLOTS];
    memset(s, 0, sizeof s);
    int bad = 0, aligned = 1;
    for (int iter = 0; iter < 60000; iter++) {
        int i = (int)(rnd() % SLOTS);
        if (s[i].p) {
            for (size_t k = 0; k < s[i].n; k++)
                if (s[i].p[k] != (unsigned char)(s[i].tag + k))
                    bad++;
        }
        const uint32_t op = rnd() % 4;
        const size_t n = 1 + rnd() % (op == 3 ? 3000 : 600);
        if (op == 0 && s[i].p) {
            pool_free(p, s[i].p);
            s[i].p = NULL;
        } else if (op == 3 && s[i].p) {
            unsigned char *q = pool_realloc(p, s[i].p, n);
            if (q) {
                const size_t keep = n < s[i].n ? n : s[i].n;
                for (size_t k = 0; k < keep; k++)
                    if (q[k] != (unsigned char)(s[i].tag + k))
                        bad++;
                s[i].p = q;
                for (size_t k = 0; k < n; k++)
                    q[k] = (unsigned char)(s[i].tag + k);
                s[i].n = n;
            }
        } else if (!s[i].p) {
            unsigned char *q = pool_malloc(p, n);
            if (q) {
                aligned &= ((uintptr_t)q & 7) == 0;
                s[i].p = q;
                s[i].n = n;
                s[i].tag = (unsigned char)rnd();
                for (size_t k = 0; k < n; k++)
                    q[k] = (unsigned char)(s[i].tag + k);
            }
        }
    }
    CHECK(bad == 0);
    CHECK(aligned);
    for (int i = 0; i < SLOTS; i++)
        if (s[i].p)
            pool_free(p, s[i].p);
    CHECK(pool_free_bytes(p) == fresh);   /* nothing lost, and the free chunks merged back into one */
    pool_destroy(p);
    free(mem);
}

int main(void)
{
    test_basic();
    test_tiny_and_destroy();
    test_independent();
    test_random();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
