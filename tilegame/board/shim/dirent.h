/* SPDX-License-Identifier: MIT */
/* newlib has no <dirent.h>: the declarations the editor's sources need, for the board (the functions are the file layer's, through tde_libc.c). */
#ifndef SHIM_DIRENT_H
#define SHIM_DIRENT_H

typedef struct tde_dir DIR;

struct dirent {
    unsigned long d_ino;
    unsigned char d_type;
    char d_name[260];
};

#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8

DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);

#endif
