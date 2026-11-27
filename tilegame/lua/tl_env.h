/* SPDX-License-Identifier: MIT */
/*
 * The Lua environment: a command prompt on the text screen, with programs as text files on tilefs. The commands (LOAD, SAVE, LIST,
 * RUN, EDIT, FILES ...) are words typed at the prompt; anything else is Lua, run at once in the same Lua state, so what one line sets
 * the next can use. A program (the text in memory) runs in a fresh state; what it leaves can be looked at from the prompt afterwards.
 * The machine gives keys, a clock and a way to show the screen through the hooks; the game functions (tl_game.h) need the tile screen hooks too.
 */
#ifndef TL_ENV_H
#define TL_ENV_H

#include <stddef.h>
#include <stdint.h>

#include "platform.h"
#include "tg_console.h"
#include "tilegfx.h"

#define TL_PROGRAM_MAX 0x10000   /* the largest program, in bytes of text */
#define TL_LINE_MAX 200          /* the longest line typed at the prompt (and no more than fits on the screen line) */
#define TL_CHUNK_MAX 4000        /* the most a statement typed over several lines can hold */
#define TL_HISTORY 8
#define TL_NAME_MAX 260

struct lua_State;
struct snd;

typedef struct {
    int (*getchar)(void);       /* the next typed character as plat_getchar gives it, or -1 */
    void (*frame)(void);        /* show the screen and wait for the next frame (called while waiting for a key, and now and then by a running program) */
    uint32_t (*millis)(void);
    /* optional (NULL: not available): a line about the machine, for the commands STACK ("stack") and HEAP ("heap"); returns 0 if it has
     * nothing to say about `what` */
    int (*info)(const char *what, char *out, size_t size);
    /* optional (NULL: the game functions of tl_game.h that need them say so): the tile screen. wait_frame is plat_wait_frame, present is plat_present
     * (the tg_t is the game's own: the text screen is not touched), buttons(player, kind) is plat_buttons (kind 0), plat_buttons_pressed (1), plat_buttons_released (2). */
    uint32_t (*wait_frame)(void);
    void (*present)(const tg_t *g);
    unsigned (*buttons)(int player, int kind);
    /* optional (NULL: no mouse): plat_mouse and plat_mouse_set_position */
    plat_mouse_t (*mouse)(void);
    void (*set_mouse)(int x, int y);
    /* optional (NULL: malloc and free): memory for the game's tiles, map and actors. fast = 1 for what the display reads (tile graphics, the window of the map),
     * which belongs in the fastest RAM; 0 for the rest (a big map can be in the PSRAM). The block need not be cleared. */
    void *(*alloc)(size_t size, int fast);
    void (*release)(void *p);
    /* optional: told what the environment is doing, 1 at the prompt and 2 while a program or a line runs (the board writes it down with its video log) */
    void (*activity)(int what);
    /* optional: called now and then while a program runs with the tile screen up (and so shows no text screen frames): what the machine must keep doing,
     * on the board the hang monitor's heartbeat and the USB host */
    void (*poll)(void);
    /* optional (NULL: realloc and free): Lua's memory, as lua_Alloc */
    void *(*lua_alloc)(void *ud, void *ptr, size_t osize, size_t nsize);
    /* 1: the machine calls tl_env_tick() about every 16 ms (from a timer interrupt or another thread), which is how a running program is made to look at
     * the keys; 0: the environment counts Lua instructions for it instead, which makes Lua code about twice as slow */
    int async_ticks;
    /* optional (NULL: no sound; the sound functions of tl_sound.h do nothing): tilesnd's engine, already playing on the machine's audio thread
     * (plat_audio_start with snd_render). The environment is the only one that queues commands to it, always from the thread it runs on. */
    struct snd *snd;
} tl_hooks_t;

enum { TL_EXIT_BYE = 0, TL_EXIT_EDIT = 1 };

typedef struct tl_env {
    tg_console_t con;
    tl_hooks_t hooks;
    struct lua_State *L;                 /* the state lines run in (NULL if it could not be made) */
    char program[TL_PROGRAM_MAX + 1];    /* the program text, ended with a zero byte */
    size_t length;
    char name[TL_NAME_MAX + 1];          /* the file it was loaded from or saved to, "" if none */
    char line[TL_LINE_MAX + 1];          /* the line being typed, and the one that was run (in the environment: the board's stack is small) */
    char command[TL_LINE_MAX + 1];
    char chunk[TL_CHUNK_MAX + 1];        /* a statement typed over several lines, so far */
    size_t chunk_len;                    /* 0: none is being typed */
    char history[TL_HISTORY][TL_LINE_MAX + 1];
    int history_count;
    int exit_code;
    char edit_name[TL_NAME_MAX + 5];     /* with TL_EXIT_EDIT: the file to edit (the program's own file if none was named; .lua is added to a new name without an extension) */
    volatile int stop;                   /* a running program is to stop (Ctrl+C) */
    uint32_t last_poll;
    int typeahead[32];                   /* keys typed while a program ran */
    int typeahead_count;
    int at_prompt;                       /* the prompt is on the screen and no line has been read since (so that going on after the keys ran out does not print it again) */
    struct tl_game *game;                /* the game functions' state (tl_game.c): made by the first of them, kept until the next RUN or NEW */
    int game_active;                     /* a picture of the game is being shown: a running program does not put the text screen up while it runs */
    char cart[20];                       /* the folder of the archive the program came from (RUN game.zip: "/ar"), where RUN runs it; "" if none */
} tl_env_t;

/* Sets up the screen (glyphs loaded by the caller) and the Lua state, clears the screen and prints the banner. Archives an earlier environment
 * mounted are forgotten (their memory given back): the file system must have been reset since (fs_reset), or this be the first. */
void tl_env_init(tl_env_t *e, tg_text_t *screen, const tl_hooks_t *hooks);

/* Loads a program file into memory (0, or a negative tilefs error, or -100 if it is too big); the environment does not need to be running. */
int tl_env_load(tl_env_t *e, const char *path);

/* The prompt loop: runs until BYE or EDIT (the exit code). */
int tl_env_run(tl_env_t *e);

/* Frees the Lua state and the game's. */
void tl_env_close(tl_env_t *e);

/* With async_ticks: makes a running program look at the keys soon. Safe from an interrupt or another thread. */
void tl_env_tick(void);

/* For the functions that are added to Lua (tl_game.c, tl_io.c): the environment; an error line in red on the text screen; looking at the keys while
 * a program runs (shows the text screen now and then, or calls the poll hook, and sets stop at Ctrl+C); and reading a line typed at the console
 * (0 at Ctrl+C, which also sets stop). */
tl_env_t *tl_env_current(void);
void tl_env_problem(tl_env_t *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void tl_env_poll(tl_env_t *e);
int tl_env_read_line(tl_env_t *e, char *out, int size);
/* The next key typed while a program runs (as plat_getchar gives it), or -1 if there is none, without waiting: first those tl_env_poll kept, then the
 * keyboard's. Ctrl+C is not given: it sets stop. */
int tl_env_key(tl_env_t *e);

/* A full garbage collection, if there is a Lua state. The environment does it after an error at the prompt (what a failed program built is garbage
 * from then on), before HEAP and before the editor. Lua collects by itself when its own memory runs short, but nothing else makes it: a machine that
 * gives the game and the sound (the alloc hook) memory from the same place as Lua's calls this when that place is full, and tries again (the devkit
 * on the board, whose PSRAM pool they share). Safe from the alloc hook, which a Lua function may be calling. */
void tl_env_collect(tl_env_t *e);

#endif
