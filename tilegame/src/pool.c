#include "pool.h"

#include <string.h>

/* dlmalloc as a library of fixed-region pools: no mmap, no sbrk, no locks, no malloc/free of its own (only the mspace calls). */
#define ONLY_MSPACES 1
#define HAVE_MMAP 0
#define HAVE_MREMAP 0
#define HAVE_MORECORE 0
#define USE_LOCKS 0
#define FOOTERS 0
#define MALLOC_ALIGNMENT ((size_t)8U)
#define MALLOC_FAILURE_ACTION
#define LACKS_TIME_H 1
#define LACKS_ERRNO_H 1
#define LACKS_FCNTL_H 1
#define LACKS_UNISTD_H 1
#define LACKS_SYS_PARAM_H 1
#define LACKS_SYS_MMAN_H 1
#define LACKS_SYS_TYPES_H 1
#define LACKS_STDLIB_H 1
#define LACKS_STRING_H 1
#define LACKS_STRINGS_H 1
#define LACKS_SCHED_H 1
#define NO_MALLOC_STATS 1
#define malloc_getpagesize ((size_t)4096U)
#define DEFAULT_GRANULARITY ((size_t)4096U)
#define ABORT __builtin_trap()
#define USE_BUILTIN_FFS 0
#include <stdlib.h>
#include <stdint.h>
#include "../third_party/dlmalloc/malloc.c"


pool_t *pool_create(void *base, size_t size)
{
    mspace ms = create_mspace_with_base(base, size, 0);
    return (pool_t *)ms;
}

void pool_destroy(pool_t *pool)
{
    if (pool)
        destroy_mspace((mspace)pool);
}

void *pool_malloc(pool_t *pool, size_t size)
{
    return mspace_malloc((mspace)pool, size);
}

void *pool_calloc(pool_t *pool, size_t count, size_t size)
{
    return mspace_calloc((mspace)pool, count, size);
}

void *pool_realloc(pool_t *pool, void *p, size_t size)
{
    return mspace_realloc((mspace)pool, p, size);
}

void pool_free(pool_t *pool, void *p)
{
    mspace_free((mspace)pool, p);
}

size_t pool_free_bytes(pool_t *pool)
{
    struct mallinfo mi = mspace_mallinfo((mspace)pool);
    return mi.fordblks;
}
