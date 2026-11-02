/* SPDX-License-Identifier: MIT */
/* Block devices for tilefs that need nothing from the target: a block of RAM. */
#include <string.h>

#include "tilefs.h"

int fs_blk_ram_init(fs_blk_ram_t *b, void *mem, size_t bytes)
{
    if (!mem || bytes < 512)
        return FS_EINVAL;
    b->data = mem;
    b->sectors = (uint32_t)(bytes / 512 > 0xFFFFFFFFu ? 0xFFFFFFFFu : bytes / 512);
    return FS_OK;
}

static int ram_init(void *ctx)
{
    return ((fs_blk_ram_t *)ctx)->data ? FS_OK : FS_ENOMEDIA;
}

static int ram_read(void *ctx, void *buf, uint32_t sector, uint32_t count)
{
    const fs_blk_ram_t *b = ctx;
    if (sector > b->sectors || count > b->sectors - sector)
        return FS_EINVAL;
    memcpy(buf, b->data + (size_t)sector * 512, (size_t)count * 512);
    return FS_OK;
}

static int ram_write(void *ctx, const void *buf, uint32_t sector, uint32_t count)
{
    fs_blk_ram_t *b = ctx;
    if (sector > b->sectors || count > b->sectors - sector)
        return FS_EINVAL;
    memcpy(b->data + (size_t)sector * 512, buf, (size_t)count * 512);
    return FS_OK;
}

static uint32_t ram_sectors(void *ctx)
{
    return ((fs_blk_ram_t *)ctx)->sectors;
}

const fs_blockdev_t fs_blk_ram_dev = {ram_init, ram_read, ram_write, NULL, ram_sectors};
