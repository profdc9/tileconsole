/* SPDX-License-Identifier: MIT */
/*
 * The file functions of the BASIC environment, on tilefs (on the board: the SD card): FOPEN, FCLOSE, FREAD, FREAD_LINE, FWRITE, FWRITE_LINE, FPEEK, FPOKE,
 * FSIZE, FEOF, FEXISTS and FDELETE. They follow MY-BASIC's file module (its wiki) in names and in what they do, but a file is a small number (1 to BASIC_FILES),
 * not a C pointer, so that a program cannot hand the C library a handle that is not one, and the files a run left open are closed when it ends.
 * See basic/README.md for the functions.
 */
#ifndef BASIC_FILE_H
#define BASIC_FILE_H

#define BASIC_FILES 4               /* files a program can have open at once */

struct mb_interpreter_t;

/* Registers the functions (basic_env_init does). */
void basic_file_register(struct mb_interpreter_t *s);

/* Closes every file a program left open (before and after a run, and with the environment). */
void basic_file_close_all(void);

#endif
