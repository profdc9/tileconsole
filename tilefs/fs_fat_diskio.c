/* SPDX-License-Identifier: MIT */
/* FatFs's low level disk interface (diskio.h), over tilefs block devices: each FatFs drive is attached to an fs_blockdev_t. */
#include "fs_fat.h"   /* ff.h first: diskio.h uses its types */
#include "diskio.h"

static struct {
    const fs_blockdev_t *dev;
    void *ctx;
    uint8_t ready;
    int init_error;   /* why the device would not start: FatFs only says "not ready" */
} drive[FF_VOLUMES];

int fs_fat_attach(unsigned pdrv, const fs_blockdev_t *dev, void *ctx);
void fs_fat_detach(unsigned pdrv);
int fs_fat_init_error(unsigned pdrv);

int fs_fat_attach(unsigned pdrv, const fs_blockdev_t *dev, void *ctx)
{
    if (pdrv >= FF_VOLUMES || !dev || !dev->init || !dev->read || !dev->write || !dev->sectors)
        return FS_EINVAL;
    drive[pdrv].dev = dev;
    drive[pdrv].ctx = ctx;
    drive[pdrv].ready = 0;
    drive[pdrv].init_error = 0;
    return FS_OK;
}

int fs_fat_init_error(unsigned pdrv)
{
    return pdrv < FF_VOLUMES ? drive[pdrv].init_error : 0;
}

void fs_fat_detach(unsigned pdrv)
{
    if (pdrv < FF_VOLUMES) {
        drive[pdrv].dev = NULL;
        drive[pdrv].ready = 0;
    }
}

DSTATUS disk_status(BYTE pdrv)
{
    return pdrv < FF_VOLUMES && drive[pdrv].dev && drive[pdrv].ready ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv >= FF_VOLUMES || !drive[pdrv].dev)
        return STA_NOINIT;
    drive[pdrv].init_error = drive[pdrv].dev->init(drive[pdrv].ctx);
    drive[pdrv].ready = drive[pdrv].init_error == 0;
    return drive[pdrv].ready ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (disk_status(pdrv))
        return RES_NOTRDY;
    if (!count || sector > 0xFFFFFFFFu)
        return RES_PARERR;
    return drive[pdrv].dev->read(drive[pdrv].ctx, buff, (uint32_t)sector, count) == 0 ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (disk_status(pdrv))
        return RES_NOTRDY;
    if (!count || sector > 0xFFFFFFFFu)
        return RES_PARERR;
    return drive[pdrv].dev->write(drive[pdrv].ctx, buff, (uint32_t)sector, count) == 0 ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (disk_status(pdrv))
        return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:
        return drive[pdrv].dev->sync && drive[pdrv].dev->sync(drive[pdrv].ctx) != 0 ? RES_ERROR : RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = drive[pdrv].dev->sectors(drive[pdrv].ctx);
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = 512;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;   /* the erase block is not known: 1 sector */
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
