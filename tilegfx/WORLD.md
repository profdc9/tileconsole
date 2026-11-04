# World and actors (design)

Two layers on top of `tilegfx.h`, in plain C with no dependencies (like the rest of `tilegfx`): `tg_world` keeps a large tile map and the
small SRAM map the compositor reads in step with a camera; `tg_actor` is a pool of game objects in world coordinates that are given
sprites only while they are on screen. Status: `tg_world.[ch]`, `tg_actor.[ch]` and their tests (`test_world`, `test_actor`) are written and pass; the BASIC bindings are not.

## What the compositor already does

A `tg_layer_t` has a power-of-two tile map that wraps and a scroll position in pixels (`scroll_x`, `scroll_y`). So the SRAM map is a
torus: global tile (tx, ty) lives in slot `(tx & (W-1), ty & (H-1))`, and `scroll = global pixel position` works unchanged, negative
positions included (the map's pixel width is a power of two, so it divides 2^32). The world library therefore needs no pan offset. It
keeps the slots filled with the tiles of the part of the big map around the camera.

## Frame protocol and why there is no tearing

`plat_present` copies the `tg_t` (so scroll values and sprites are consistent for a frame) but only *points* to tile maps, which the
display core reads while the game runs. Each world therefore has two SRAM maps and swaps the pointer, as `plat_present` recommends:

    for (;;) {
        plat_wait_frame();
        tg_world_begin_frame(&w);      /* hidden map := shown map, for the slots that changed last frame */
        ... read input, game logic ...
        tg_world_set(&w, tx, ty, e);   /* edits go to the big map and the hidden map */
        tg_world_set_camera(&w, x, y); /* streams entering columns and rows into the hidden map */
        tg_world_end_frame(&w);        /* layer->map = the hidden map, scroll = camera; hidden and shown swap */
        tg_actors_draw(&actors, &g, cam_x, cam_y);
        plat_present(&g);
    }

- The map the display is reading is never written: after `end_frame` it is the shown map until the next `plat_wait_frame`, and
  `begin_frame` (the only place that writes to it) runs after that.
- Every write to the hidden map sets a bit in the **dirty bitmap** (one bit a slot). `begin_frame` copies each dirty slot from the
  shown map to the hidden one and clears the bit, so the two maps are equal again and unchanged tiles are never copied.
- Cost (estimates, not measured): about 1 us for the scan of an unchanged 64 x 32 map, about 20 ns a dirty slot, a streamed column of 31
  tiles 15 to 30 us (PSRAM reads, once: the replay copies from SRAM). A jump of more than a window refills the whole map once.
- One `end_frame` per `plat_present`, and `begin_frame` after every `plat_wait_frame`. `begin_frame` does nothing if the frame is already begun,
  and `end_frame` does nothing if it is already ended. Anything that writes begins the frame by itself if the program has not (right only after the wait).
  `tg_world_init` leaves the frame begun, so edits can follow at once.
- The map that was presented stays unchanged for the whole of the next frame, not only until the next `begin_frame`: it is the one the display reads.
- A world's layer must not be written to by `tg_layer_put_*` (text); text has its own layer.

## Window

The window is the layer's own map size (`map_w_log2`, `map_h_log2`), chosen by the program before `tg_world_init`: at least the visible tiles
plus one (41 x 31 tiles of 8 x 8 pixels, so 64 x 32), or 64 x 64 for fast vertical scrolling. Because the hidden map is
never displayed, no spare border is needed for tearing; the window is only as large as the longest camera move that is streamed
column by column (`>= W` or `>= H` tiles in a frame is a full refill).

The window is centred on the camera: its top left is the global tile `(cam_tx - (W - visible_w) / 2, cam_ty - (H - visible_h) / 2)`, so
crossing a tile boundary loads the one column or row that entered and nothing else. Raster effects with `line_scroll_x` show tiles
further left or right than the camera: keep `W` larger by the amplitude (in tiles) of the effect.

## Data structures

    #define TG_TILES_MAX 1024                 /* tile numbers are 10 bits */

    /* The flag byte of a tile number (not of a map entry: the flips and palette do not matter). */
    #define TG_TILE_SOLID  0x01
    #define TG_TILE_HAZARD 0x02
    #define TG_TILE_ONEWAY 0x04               /* solid from above only */
    #define TG_TILE_LIQUID 0x08
    /* 0x10 .. 0x80: the program's */

    typedef enum { TG_BORDER_TILE, TG_BORDER_CLAMP, TG_BORDER_WRAP } tg_border_t;   /* outside the big map */

    typedef struct {
        tg_layer_t *layer;                    /* the layer it drives; its tile_log2 and map size are set before init */
        uint16_t *win[2];                     /* the two SRAM maps, (1 << map_w_log2) * (1 << map_h_log2) entries each; caller's storage */
        uint32_t *dirty;                      /* one bit a slot; caller's storage */
        uint32_t dirty_words, dirty_count;    /* count: how many bits are set (0: begin_frame has nothing to scan) */
        uint8_t shown, w_log2, h_log2, tile_log2;

        uint16_t *map;                        /* the big map: row-major, map_w * map_h entries, anywhere (PSRAM); the world edits it */
        int32_t map_w, map_h;                 /* in tiles; any size */
        tg_border_t border;
        uint16_t border_entry;
        const uint8_t *flags;                 /* TG_TILES_MAX bytes, or NULL (all zero); caller's, may be const */

        int32_t cam_x, cam_y;                 /* global pixels */
        int32_t win_tx, win_ty;               /* the global tile in the window's top left */
    } tg_world_t;

Storage is the caller's (no `malloc`): `TG_WORLD_WIN_ENTRIES(w_log2, h_log2)` and `TG_WORLD_DIRTY_WORDS(w_log2, h_log2)` give the sizes.
The two windows must be in SRAM; the big map normally is not.

## World API

    bool tg_world_init(tg_world_t *w, tg_layer_t *layer, uint16_t *win_a, uint16_t *win_b, uint32_t *dirty,
                       uint16_t *map, int32_t map_w, int32_t map_h);        /* fills both windows from the big map at camera 0, 0 */
    void tg_world_set_border(tg_world_t *w, tg_border_t border, uint16_t entry);
    void tg_world_set_flags(tg_world_t *w, const uint8_t *flags);           /* one byte per tile number */
    void tg_world_reload(tg_world_t *w);                                    /* after the big map was changed behind its back: refill everything */

    void tg_world_begin_frame(tg_world_t *w);
    void tg_world_set_camera(tg_world_t *w, int32_t x, int32_t y);          /* global pixels, signed */
    void tg_world_end_frame(tg_world_t *w);                                 /* sets layer->map and the scroll of the world's layer, swaps */

    uint16_t tg_world_get(const tg_world_t *w, int32_t tx, int32_t ty);     /* the big map: the newest value, whatever is on screen (border rule outside) */
    void tg_world_set(tg_world_t *w, int32_t tx, int32_t ty, uint16_t entry);          /* outside the big map: ignored (wraps with TG_BORDER_WRAP) */
    void tg_world_fill(tg_world_t *w, int32_t tx, int32_t ty, int32_t cols, int32_t rows, uint16_t entry);
    void tg_world_blit(tg_world_t *w, int32_t tx, int32_t ty, const uint16_t *src, int32_t cols, int32_t rows, int32_t src_stride);

    uint8_t tg_world_flags_tile(const tg_world_t *w, int32_t tx, int32_t ty);
    uint8_t tg_world_flags_px(const tg_world_t *w, int32_t x, int32_t y);
    uint8_t tg_world_flags_box(const tg_world_t *w, int32_t x, int32_t y, int32_t width, int32_t height);   /* the OR of every tile the box touches */

`set`, `fill` and `blit` write the big map, and the hidden map where the tile is in the window (marking it dirty). Everything in one frame
appears together at the next `present`, so a door of four tiles is never half open. `tg_world_flags_box` is the collision query: one call
for what BASIC would loop over.

Several worlds (parallax) are independent: each has its own layer, windows, and camera, and a parallax layer is given `cam * factor`.

## Actors

An actor is a rectangle of sprites (a metasprite) at a global position. The pool is the caller's array (`tg_actor.h`).

    typedef struct {
        int32_t x, y;           /* top left, global, 24.8 fixed point (1/256 pixel) */
        int32_t vx, vy;         /* per frame, 24.8; added by tg_actors_step */
        uint16_t tile;          /* the first sprite tile; the others follow row by row: tile + row * cols + col */
        uint8_t cols, rows;     /* 1..4 sprite tiles each way (a sprite tile is 8 or 16 pixels: g->sprite_tile_log2) */
        uint8_t flags;          /* TG_ACTOR_ACTIVE | HIDDEN | HFLIP | VFLIP (the flips mirror the whole grid, and each tile) */
        uint8_t pal, above;     /* as tg_sprite_t */
        uint8_t prio;           /* 0 in front; the same prio: in front in order of the actor's number */
        uint8_t hit_x, hit_y, hit_w, hit_h;   /* the collision box in pixels from the top left; hit_w or hit_h 0: the whole grid */
        uint16_t user;          /* the program's: a type, a state, an animation counter */
    } tg_actor_t;

    void tg_actors_init(tg_actors_t *p, tg_actor_t *storage, uint16_t n, uint16_t *order);   /* order: n entries of scratch for the draw */
    int  tg_actor_alloc(tg_actors_t *p);              /* the lowest free number, cleared, ACTIVE, 1 x 1; -1 if the pool is full */
    void tg_actor_free(tg_actors_t *p, int id);
    tg_actor_t *tg_actor(tg_actors_t *p, int id);     /* NULL for a number that is not an active actor */

    void tg_actors_step(tg_actors_t *p, int frames);  /* x += vx * frames, y += vy * frames (saturating) for every active actor */
    uint8_t tg_actor_move(tg_actors_t *p, int id, const tg_world_t *w, int32_t dx, int32_t dy);   /* x then y; returns TG_HIT_* */
    bool tg_actors_overlap(const tg_actors_t *p, int a, int b);                                   /* the collision boxes */
    int  tg_actors_draw(tg_actors_t *p, tg_t *g, int32_t cam_x, int32_t cam_y);                   /* the number of actors drawn */

`tg_actor_move` moves in steps of a tile or less, so nothing is jumped over, and stops flush against a `TG_TILE_SOLID` tile (the actor's box is
looked up with `tg_world_flags_box`; with a solid border the map's edge stops it too). A `TG_TILE_ONEWAY` tile stops it only when moving down
from above. A stopped actor loses the fraction of a pixel and never moves against its direction; one that starts inside a solid tile stays
where it is. The result is the sides that stopped it (`TG_HIT_LEFT|RIGHT|UP|DOWN`).

`tg_actors_draw` gives back the sprite slots it took at the last call (other sprites in `g` are left alone), orders the actors that are
active, not hidden and touch the screen by priority (a counting sort, stable) and allocates one sprite for each tile of the grid that is on
the screen, with `tg_sprite_alloc`. If the 128 slots run out, the back-most actors go without; an actor whose tiles do not all fit is left out
(a smaller one behind it can still be drawn). Each part gets the actor's `above`, `pal` and flips, the mirrored tile of the grid, and the
actor's `prio` as its sprite priority. `TG_ACTORS_YSORT` (in `tg_actors_t.flags`) makes the priority the bottom edge on the screen
(`255 - bottom`, so lower on the screen is nearer) and ignores `prio`. The compositor's own limit of 32 sprites a line still applies. Sprites
are in `tg_t`, which `plat_present` copies, so they need no double buffer.

## BASIC

BASIC never calls `begin_frame`, `end_frame` or `plat_present`. The runtime owns the frame, and `WAITFRAME()` is the one point where BASIC meets it.
All other commands edit the state of the next frame, and nothing is visible until `WAITFRAME()`:

1. `end_frame` for each world, `tg_actors_draw`, `plat_present`;
2. `plat_wait_frame` (which also latches the inputs for the frame);
3. the frames that went by since the last call: `n = frame_now - frame_at_last_call`, at least 1, capped at 4;
4. `tg_actors_step` and the animations `n` times, the camera follow once; `begin_frame`;
5. returns `n`.

Everything runs on the BASIC thread, so the only concurrency is the display core, which the double buffering already covers.

**Late frames.** BASIC's run time is not predictable. If it takes two frames, the display shows the last presented state for another frame:
the frame rate drops, but nothing tears and nothing is half changed. Velocities and animations are per elapsed frame (decided), so
a slow program keeps its real-time speed and only looks choppier; the returned `n` lets BASIC scale its own logic. The cap stops a long
stall (a file load) from throwing actors across the map.

**Work done in C, so that a BASIC frame is a few calls** (the cost of a call has not been measured; at about 16000 statements a frame, 100 calls are affordable):

| BASIC | does |
|---|---|
| `TILES(file)`, `LOADMAP(file)`, `FILL(...)`, `BLIT(...)` | load graphics and levels, bulk edits; `TILE(tx, ty, e)` and `GETTILE(tx, ty)` for single tiles |
| `CAMERA(x, y)`, `FOLLOW(a, margin)` | set the camera; keep an actor inside a margin of the screen |
| `ACTOR(tile, x, y)`, `FREE(a)`, `ACTORX(a)`, `ACTORY(a)`, `SETPOS(a, x, y)` | pool; positions are floating point in BASIC (24.8 in C) |
| `SETVEL(a, vx, vy)` | the runtime moves it every frame |
| `MOVE(a, dx, dy)` | moves, stopping at solid tiles; returns which sides were hit |
| `TILEFLAG(x, y)`, `BOXFLAG(x, y, w, h)`, `HIT(a, b)` | collision: one call for what a loop would do |
| `ANIM(a, table)` | frame sequences run in C |
| `BTN(p, b)`, `PRESSED(p, b)` | the inputs latched at the frame wait |

**Synchronisation:** `WAITFRAME()` (returns the frames elapsed), `WAITFRAMES(n)`, `FRAME()`, `MILLIS()`, and optionally `FRAMEMS()`, the
part of the last frame BASIC used, so that a beginner can see when a program is over budget.

**Edges.** At the prompt in tile mode there is no loop, so every typed line ends with an implicit present. Ctrl+C or the end of a program
returns to text mode and frees the worlds and the pool. A bad actor number or position is a MY-BASIC error with its line, not a crash.
Sound commands must not block and are timed by the frame counter (the `tilesnd` interface has not been looked at for this).
MY-BASIC needs parentheses on every function call: `n = waitframe()`.

    tiles("level1.til")
    loadmap("level1.map")
    p = actor(4, 100, 100)
    follow(p, 40)
    while 1
      n = waitframe()
      if btn(0, 1) then setvel(p, -1.5, 0)
      if tileflag(actorx(p), actory(p) + 8) = 0 then setvel(p, 0, 1)
    wend

A second way, later and on the same core: the runtime calls a BASIC routine once a frame (like TIC-80's `TIC()`), so that a beginner has no loop to
write. MY-BASIC has `mb_eval_routine`, which should do it (not checked).

## Files and tests

- `tg_world.[ch]`, `tg_actor.[ch]` in `tilegfx/`; `make check` runs `test_world`.
- `test_world`: random camera walks (single steps, large jumps, negative coordinates, all four tile sizes and depths) with random
  `set`/`fill`/`blit`; after each frame every line from `tg_render_line_indices` is compared with a reference that looks each pixel up
  in the big map. A second check watches the presented map: it is byte-identical, and still draws the presented picture, until the next `end_frame`.
  Also: after `begin_frame` the two windows are equal; the border rules; `flags_box` against a brute-force count.
- Mutation checks done by hand (each broken on purpose and caught): world: no replay, writing the shown map, no row streaming, skipping the clamp/wrap
  aliases; actors: slots not given back, no priority order, no grid mirroring, partial actors, steps over a tile, ignored one-way, moving backwards, velocity once.
- `test_actor`: the pool and the velocity step; drawing compared through the compositor with sprites built by hand (random actors, cameras,\n  flips, priorities, hidden ones, both sprite sizes, with and without `YSORT`, and a program's own sprite that must stay); exhaustion; moves\n  against a pixel-by-pixel reference on a model of the map (whole pixels) and by properties (fractions); one-way tiles; overlap.

## Defaults chosen (change them if wrong)

- 24.8 fixed-point actor positions and velocities.
- Metasprites up to 4 x 4, with a regular grid of consecutive tile numbers.
- Slots are freed and allocated again each frame (simple; allocation keeps the order list sorted and is cheap for 100 or so actors).
- The window is centred on the camera.
- Velocity and animation step per elapsed frame (capped at 4), not per `WAITFRAME` call.
- Later: the BASIC callback model; SD loading of the big map; actor-to-actor collision; animation tables; BASIC bindings on top (`WORLD`, `CAMERA`, `TILE`, `TILEFLAG`,
  `ACTOR`, `MOVE`), which a program would call a few times a frame.
