/* SPDX-License-Identifier: MIT */
/*
 * TGF: the tile game's graphics and map files (docs/tgf-format.md). Checking, reading and writing, through I/O functions the caller gives,
 * so that it needs neither Lua nor a particular file system: the Lua functions loadgfx, loadmap, savegfx and savemap use it on tilefs, and a
 * game in C can use it on anything.
 *
 * Reading is in two steps, so that a broken file changes nothing: tgf_check reads the whole file and checks all of it, and says where each
 * part is (tgf_info_t); the tgf_read_* functions then read the parts into place. Only a failure of the I/O itself between the two (a card
 * pulled out) can leave a part read and the rest not.
 */
#ifndef TGF_H
#define TGF_H

#include <stddef.h>
#include <stdint.h>

#define TGF_SET 256          /* palette entries, tiles, sprites and flags a file can fill */
#define TGF_TABLES 32        /* animation tables, numbered from 1 */
#define TGF_FRAMES 32        /* frames in a table */
#define TGF_LAYERS 4         /* map layers, numbered from 1 */
#define TGF_MAP_MAX (1u << 20)   /* the most tiles in a map */
#define TGF_CHARS 512        /* characters a CHAR table can name */
#define TGF_NO_TILE 0xFFFF   /* a character's lower half: none (it is one tile tall) */
#define TGF_CODEPOINT_MAX 0x10FFFFu

/* Map entries: bits 0-9 the tile (TGF_SET at most here), 10 flipped across, 11 flipped up and down (as tilegfx's TG_HFLIP, TG_VFLIP). */
#define TGF_HFLIP 0x0400
#define TGF_VFLIP 0x0800
#define TGF_MAP_WRAPS 1      /* a map's flags: it repeats in every direction */

enum { TGF_PALT, TGF_TILE, TGF_SPRT, TGF_FLAG, TGF_KINDS };   /* the parts that are a range of a set */

/* Errors (the I/O functions' own negative values come back as they are). */
enum {
    TGF_ENOTTGF = -1000,     /* not a TGF file: it does not begin with TGAC TGF1 */
    TGF_EBROKEN = -1001,     /* its chunks do not add up: a length that is wrong, no END chunk, a chunk too short for its fields, one part twice */
    TGF_ERANGE = -1002,      /* something that does not fit: a range past 255, a table, layer or tile number out of range, a map too big */
    TGF_EIO = -1003,         /* the I/O failed (from the caller's functions: they may return their own negative value instead) */
};

typedef struct {
    int (*read)(void *ctx, void *buf, uint32_t n);         /* the bytes read (fewer than n only at the end of the file), or negative */
    int (*seek)(void *ctx, uint32_t pos);                  /* to pos bytes from the start: 0, or negative */
    int (*write)(void *ctx, const void *buf, uint32_t n);  /* n, or negative */
    void *ctx;
    uint32_t size;                                         /* reading: the file's size in bytes */
} tgf_io_t;

typedef struct {
    uint32_t at;             /* where the first item is in the file */
    uint16_t first, count;   /* count 0: not in the file */
} tgf_range_t;

typedef struct {
    uint32_t at;             /* where its frames are */
    uint8_t present, mode, count;   /* mode 0 loop, 1 once, 2 pingpong */
} tgf_table_t;

typedef struct {
    uint32_t at;             /* where its entries are */
    uint32_t w, h;
    uint8_t present, flags;  /* TGF_MAP_WRAPS */
} tgf_map_t;

typedef struct {
    uint32_t at;             /* where its entries are */
    uint16_t count;
    uint8_t present;
} tgf_chars_info_t;

typedef struct {
    uint8_t tile_size;               /* 8 or 16 */
    tgf_range_t part[TGF_KINDS];     /* TGF_PALT ... TGF_FLAG */
    tgf_table_t table[TGF_TABLES];   /* table n is [n - 1] */
    tgf_map_t map[TGF_LAYERS];       /* layer n is [n - 1] */
    tgf_chars_info_t chars;          /* the character table (CHAR) */
    int tables, maps;                /* how many are in the file */
} tgf_info_t;

typedef struct {
    uint16_t sprite;
    uint8_t ticks;
} tgf_frame_t;

/* An entry of the character table: which tile is the character (and, for one two tiles tall, which is its lower half). */
typedef struct {
    uint32_t codepoint;
    uint16_t tile, lower;    /* lower: TGF_NO_TILE for a character one tile tall */
} tgf_char_t;

/* Reads the whole file and checks it: 0, or a TGF_E* (or the I/O's) error, with what is wrong in `why` (n bytes; may be NULL). */
int tgf_check(const tgf_io_t *io, tgf_info_t *info, char *why, size_t n);

/* The part's items into dest, at dest + first * item: 3 bytes (red, green, blue) for TGF_PALT, tile_size * tile_size for TGF_TILE and
 * TGF_SPRT, 1 for TGF_FLAG. Nothing if the part is not in the file. 0 or an error. */
int tgf_read_part(const tgf_io_t *io, const tgf_info_t *info, int kind, void *dest);
/* Table n's frames (info->table[n - 1].count of them). */
int tgf_read_table(const tgf_io_t *io, const tgf_info_t *info, int n, tgf_frame_t *frames);
/* Layer n's map: w * h entries, row by row. */
int tgf_read_map(const tgf_io_t *io, const tgf_info_t *info, int n, uint16_t *entries);
/* The character table (info->chars.count entries, in rising order of codepoint); nothing if the file has none. */
int tgf_read_chars(const tgf_io_t *io, const tgf_info_t *info, tgf_char_t *chars);

/* Writing: the header first and tgf_write_end last, the rest in between, each at most once (a table and a map once each for its number).
 * Each returns 0 or the I/O's error. */
int tgf_write_header(const tgf_io_t *io, int tile_size);
int tgf_write_part(const tgf_io_t *io, int kind, int first, int count, int tile_size, const void *items);   /* items: those of `first` on */
int tgf_write_table(const tgf_io_t *io, int n, int mode, int count, const tgf_frame_t *frames);
int tgf_write_map(const tgf_io_t *io, int n, int flags, uint32_t w, uint32_t h, const uint16_t *entries);
/* The character table: at most TGF_CHARS entries in rising order of codepoint, each once, tiles 0 to 255 (TGF_ERANGE otherwise). */
int tgf_write_chars(const tgf_io_t *io, int count, const tgf_char_t *chars);
int tgf_write_end(const tgf_io_t *io);

#endif
