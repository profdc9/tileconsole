/* SPDX-License-Identifier: MIT */
/*
 * What the editor's sources call that the board's C library (newlib, no operating system) does not have: there are no processes and no descriptors to
 * redirect, so these fail, and the editor's shell command and its output redirection do nothing.
 */
#include <errno.h>

int dup(int fd)
{
    (void)fd;
    errno = ENOSYS;
    return -1;
}

int dup2(int fd, int to)
{
    (void)fd;
    (void)to;
    errno = ENOSYS;
    return -1;
}

int creat(const char *path, unsigned mode)
{
    (void)path;
    (void)mode;
    errno = ENOSYS;
    return -1;
}
