/* SPDX-License-Identifier: MIT */
/* Files held in the program's flash (see board/embed.cmake), the last entry has a NULL name. */
#ifndef EMBEDDED_H
#define EMBEDDED_H

#include <stddef.h>

typedef struct {
    const char *name;
    const unsigned char *data;
    size_t size;
} embedded_file_t;

extern const embedded_file_t devkit_config_files[];   /* the editor's configuration: tde.cfg, tde.hlp, tde.shl */

#endif
