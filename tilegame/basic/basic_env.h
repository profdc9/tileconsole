/* SPDX-License-Identifier: MIT */
/*
 * The BASIC environment: a command prompt on the 80 x 25 text screen around the MY-BASIC interpreter (basic/my_basic, MIT, unmodified), with the program held as
 * free-form text in memory and files on tilefs. It is a self-contained development environment in the manner of the old home computers:
 *
 *   HELP  CLS  NEW  LIST [first [last]]  RUN  LOAD file  SAVE [file]  FILES [folder]  KILL file  CD folder  EDIT [file]  VER  BYE
 *
 * Any other line is BASIC and runs at once (PRINT 2 + 2, or A = 5 : PRINT A * A: a colon puts statements on one line); each line stands alone, its variables are gone
 * with it. RUN runs the program in memory, from a clean state. Ctrl+C stops a program that
 * is running. EDIT leaves the prompt with BASIC_EXIT_EDIT and the name of the file, for the program around this to start the editor (TDE) on the program, and start the
 * environment again afterwards with the file loaded (basic_env_load).
 *
 * The environment needs a keyboard (hooks.getchar, as plat_getchar), a way to show the screen while it waits (hooks.frame) and a clock. It does not know the platform otherwise, so a
 * test can type at it. MY-BASIC's callbacks carry no pointer of their own, so there is one environment at a time.
 */
#ifndef BASIC_ENV_H
#define BASIC_ENV_H

#include <stddef.h>
#include <stdint.h>

#include "tg_console.h"
#include "tilegfx.h"

#define BASIC_PROGRAM_MAX 0x10000   /* the largest program, in bytes of text */
#define BASIC_LINE_MAX 200          /* the longest line typed at the prompt (and no more than fits on the screen line) */
#define BASIC_HISTORY 8
#define BASIC_NAME_MAX 260

typedef struct {
    int (*getchar)(void);       /* the next typed character as plat_getchar gives it, or -1 */
    void (*frame)(void);        /* show the screen and wait for the next frame (called while waiting for a key, and now and then by a running program) */
    uint32_t (*millis)(void);
    /* optional (NULL: not available): a line about the machine, for the commands STACK ("stack") and HEAP ("heap"); returns 0 if it has
     * nothing to say about `what` */
    int (*info)(const char *what, char *out, size_t size);
    /* optional (NULL: the game functions of basic_game.h that need them say so): the tile screen. wait_frame is plat_wait_frame, present is plat_present
     * (the tg_t is the game's own: the text screen is not touched), buttons(player, kind) is plat_buttons (kind 0), plat_buttons_pressed (1), plat_buttons_released (2). */
    uint32_t (*wait_frame)(void);
    void (*present)(const tg_t *g);
    unsigned (*buttons)(int player, int kind);
    /* optional (NULL: malloc and free): memory for the game's tiles, map and actors. fast = 1 for what the display reads (tile graphics, the window of the map),
     * which belongs in the fastest RAM; 0 for the rest (a big map can be in the PSRAM). The block need not be cleared. */
    void *(*alloc)(size_t size, int fast);
    void (*release)(void *p);
    /* optional: told what the environment is doing, 1 at the prompt and 2 while a program or a line runs (the board writes it down with its video log) */
    void (*activity)(int what);
} basic_hooks_t;

enum { BASIC_EXIT_BYE = 0, BASIC_EXIT_EDIT = 1 };

typedef struct basic_env {
    tg_console_t con;
    basic_hooks_t hooks;
    struct mb_interpreter_t *bas;
    char program[BASIC_PROGRAM_MAX + 1];   /* the program text, ended with a zero byte */
    size_t length;
    char name[BASIC_NAME_MAX + 1];         /* the file it was loaded from or saved to, "" if none */
    char line[BASIC_LINE_MAX + 1];         /* the line being typed, and the one that was run (in the environment: the board's stack is small) */
    char command[BASIC_LINE_MAX + 1];
    char history[BASIC_HISTORY][BASIC_LINE_MAX + 1];
    int history_count;
    int exit_code;
    char edit_name[BASIC_NAME_MAX + 5];    /* with BASIC_EXIT_EDIT: the file to edit (the program's own file if none was named; .bas is added to a new name without an extension) */
    int stop;                              /* a running program is to stop (Ctrl+C) */
    uint32_t last_poll;
    int typeahead[32];                     /* keys typed while a program ran */
    int typeahead_count;
    int at_prompt;                         /* the prompt is on the screen and no line has been read since (so that going on after the keys ran out does not print it again) */
    struct basic_game *game;               /* the game functions' state, while a run uses them (basic_game.c) */
    int game_active;                       /* a picture of the game is being shown: a running program does not put the text screen up while it runs */
} basic_env_t;

/* Sets up the screen (glyphs loaded by the caller) and the interpreter, clears the screen and prints the banner. */
void basic_env_init(basic_env_t *e, tg_text_t *screen, const basic_hooks_t *hooks);

/* Loads a program file into memory (0, or a negative tilefs error, or -100 if it is too big); the environment does not need to be running. */
int basic_env_load(basic_env_t *e, const char *path);

/* The prompt loop: runs until BYE or EDIT (the exit code). */
int basic_env_run(basic_env_t *e);

/* Frees the interpreter. */
void basic_env_close(basic_env_t *e);

/* For the functions that are added to the interpreter (basic_game.c): the environment, and an error line in red on the text screen. */
basic_env_t *basic_env_current(void);
void basic_env_problem(basic_env_t *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
