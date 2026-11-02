/* SPDX-License-Identifier: MIT */
/* A disk image in a host file as a tilefs block device, for debugging the FAT file system on a PC. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tilefs.h"

int fs_blk_file_open(fs_blk_file_t *b, const char *path, uint32_t create_sectors)
{
    struct stat st;
    b->fd = open(path, O_RDWR | O_CLOEXEC);
    if (b->fd < 0) {
        if (errno != ENOENT || !create_sectors)
            return errno == ENOENT ? FS_ENOENT : FS_EIO;
        b->fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (b->fd < 0 || ftruncate(b->fd, (off_t)create_sectors * 512) < 0) {
            if (b->fd >= 0)
                close(b->fd);
            b->fd = -1;
            return FS_EIO;
        }
    }
    if (fstat(b->fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(b->fd);
        b->fd = -1;
        return FS_EIO;
    }
    const uint64_t n = (uint64_t)st.st_size / 512;
    b->sectors = n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
    return FS_OK;
}

void fs_blk_file_close(fs_blk_file_t *b)
{
    if (b->fd >= 0)
        close(b->fd);
    b->fd = -1;
}

static int file_init(void *ctx)
{
    return ((fs_blk_file_t *)ctx)->fd >= 0 ? FS_OK : FS_ENOMEDIA;
}

static int file_read(void *ctx, void *buf, uint32_t sector, uint32_t count)
{
    const fs_blk_file_t *b = ctx;
    if (sector > b->sectors || count > b->sectors - sector)
        return FS_EINVAL;
    size_t done = 0;
    const size_t total = (size_t)count * 512;
    while (done < total) {
        const ssize_t r = pread(b->fd, (char *)buf + done, total - done, (off_t)sector * 512 + (off_t)done);
        if (r <= 0)
            return FS_EIO;
        done += (size_t)r;
    }
    return FS_OK;
}

static int file_write(void *ctx, const void *buf, uint32_t sector, uint32_t count)
{
    const fs_blk_file_t *b = ctx;
    if (sector > b->sectors || count > b->sectors - sector)
        return FS_EINVAL;
    size_t done = 0;
    const size_t total = (size_t)count * 512;
    while (done < total) {
        const ssize_t r = pwrite(b->fd, (const char *)buf + done, total - done, (off_t)sector * 512 + (off_t)done);
        if (r <= 0)
            return FS_EIO;
        done += (size_t)r;
    }
    return FS_OK;
}

static int file_sync(void *ctx)
{
    return fsync(((fs_blk_file_t *)ctx)->fd) < 0 ? FS_EIO : FS_OK;
}

static uint32_t file_sectors(void *ctx)
{
    return ((fs_blk_file_t *)ctx)->sectors;
}

const fs_blockdev_t fs_blk_file_dev = {file_init, file_read, file_write, file_sync, file_sectors};
