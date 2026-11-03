/* Memory pools: an allocator (Doug Lea's dlmalloc, as one "mspace" per pool) over a block of memory that the caller owns, such as a
 * part of the PSRAM. A pool has no connection to the C library's heap, and pool_destroy gives everything in it back at once. A pool
 * is not thread-safe; use it from one core. Blocks are 8-byte aligned. */
#ifndef POOL_H
#define POOL_H

#include <stddef.h>

typedef struct pool pool_t;

/* Makes a pool in [base, base + size); base must be 8-byte aligned. Returns NULL if the block is too small (under ~4 KB). */
pool_t *pool_create(void *base, size_t size);

/* Forgets the pool: every block in it is gone, and the memory is the caller's again. */
void pool_destroy(pool_t *pool);

void *pool_malloc(pool_t *pool, size_t size);
void *pool_calloc(pool_t *pool, size_t count, size_t size);
void *pool_realloc(pool_t *pool, void *p, size_t size);
void pool_free(pool_t *pool, void *p);

/* The bytes that are not in use (in free chunks; one allocation can be smaller than this if the space is split up). */
size_t pool_free_bytes(pool_t *pool);

#endif
