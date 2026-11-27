/* SPDX-License-Identifier: MIT */
/* Tests of the Lua environment: it is typed at with scripted keys on a RAM disk and the screen and the files are looked at. */
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lua.h"
#include "platform.h"
#include "snd.h"
#include "tg_text.h"
#include "tilefs.h"
#include "tl_env.h"

const char *__asan_default_options(void) { return "detect_leaks=0"; }

static int checks;
#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            show();                                                          \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static tg_text_t screen;
static tl_env_t env;
static fs_ram_t disk;
static uint8_t pool[512 * 1024];

static int script[8192], script_len, script_pos, idle_frames;
static jmp_buf cut;
static int cut_armed;
static uint32_t now_ms;

static int presents;

/* The sound engine, and an audio thread played by the hooks: every frame, poll and frame wait mixes a block (the machine's thread would, now and then) */
static snd_t tsnd;
static int sound_on = 1;
static int audio_stalled;   /* the audio thread does not run (the machine's core 1 held up) */
static int16_t sbuf[2 * 4800];
static void pump(void)
{
    if (env.hooks.snd && !audio_stalled)
        snd_render(&tsnd, sbuf, 64);
}

static int get_char(void)
{
    if (script_pos >= script_len) {
        return -1;
    }
    idle_frames = 0;
    return script[script_pos++];
}

static int frame_while_game;   /* the text screen was put up while the game was on screen */
static int frame_text_kept;    /* ... and while the graphics were kept but not on screen (textmode) */
static int polls;              /* the poll hook was called (a program ran with the game's picture up) */

static void frame(void)
{
    pump();
    now_ms += 17;
    if (env.game_active)
        frame_while_game++;
    else if (env.game)
        frame_text_kept++;
    if (++idle_frames > 100000 && script_pos >= script_len && cut_armed)   /* (a running program calls this now and then too, and may run a while) */
        longjmp(cut, 1);
}

static uint32_t millis(void) { return now_ms += 1; }

static void poll_hook(void)
{
    pump();
    polls++;
    if (++idle_frames > 1000000 && script_pos >= script_len && cut_armed)
        longjmp(cut, 1);
}

static void show(void)
{
    for (int r = 0; r < 25; r++) {
        char row[81];
        for (int c = 0; c < 80; c++)
            row[c] = (char)(screen.cells[r * 80 + c] & 255);
        row[80] = 0;
        printf("|%s\n", row);
    }
}

static const char *row(int r)
{
    static char text[4][81];
    static int which;
    char *out = text[which = (which + 1) & 3];
    int n = 0;
    for (int c = 0; c < 80; c++)
        out[n++] = (char)(screen.cells[r * 80 + c] & 255);
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = 0;
    return out;
}

static int shows(const char *text)
{
    for (int r = 0; r < 25; r++)
        if (strstr(row(r), text))
            return 1;
    return 0;
}

static int row_of(const char *prefix)
{
    for (int r = 0; r < 25; r++)
        if (!strncmp(row(r), prefix, strlen(prefix)))
            return r;
    return -1;
}

static int last_row_of(const char *prefix)
{
    int found = -1;
    for (int r = 0; r < 25; r++)
        if (!strncmp(row(r), prefix, strlen(prefix)))
            found = r;
    return found;
}

/* Keys: a character is itself, \n is Enter, ^A..^Z Ctrl, {NAME} others. */
static void type(const char *spec)
{
    static const struct { const char *name; int key; } names[] = {
        {"UP", PLAT_KEY_UP}, {"DOWN", PLAT_KEY_DOWN}, {"LEFT", PLAT_KEY_LEFT}, {"RIGHT", PLAT_KEY_RIGHT}, {"HOME", PLAT_KEY_HOME}, {"END", PLAT_KEY_END},
        {"ESC", 27}, {"BS", 8}, {"DEL", 127}, {"PGUP", PLAT_KEY_PAGEUP}, {"PGDN", PLAT_KEY_PAGEDOWN}, {"INS", PLAT_KEY_INSERT}, {"F1", PLAT_KEY_F(1)},
        {"F12", PLAT_KEY_F(12)}, {"TAB", 9}, {"CR", 13}, {"NUL", 0}, {"FS", 28}, {"X128", 128}, {"X200", 0x200},
    };
    while (*spec) {
        int key = -1;
        if (*spec == '{') {
            const char *close = strchr(spec, '}');
            const size_t n = (size_t)(close - spec - 1);
            for (size_t i = 0; i < sizeof names / sizeof *names; i++)
                if (strlen(names[i].name) == n && !strncmp(spec + 1, names[i].name, n))
                    key = names[i].key;
            CHECK(key >= 0);
            spec = close + 1;
        } else if (*spec == '^' && spec[1] >= 'A' && spec[1] <= 'Z') {
            key = spec[1] - '@';
            spec += 2;
        } else {
            key = (unsigned char)*spec++;
        }
        CHECK(script_len < 8192);
        script[script_len++] = key;
    }
}

/* what the machine says about itself (STACK and HEAP); `info_on` off gives an environment without it */
static int info_on = 1;
static int test_info(const char *what, char *out, size_t size)
{
    if (!strcmp(what, "stack"))
        snprintf(out, size, "stack: peak 1234 of 4096");
    else if (!strcmp(what, "heap"))
        snprintf(out, size, "heap: 5678 free");
    else if (!strcmp(what, "usblog"))
        snprintf(out, size, "usblog: saved");
    else
        return 0;
    return 1;
}

/* the tile screen: a frame counter that moves by frame_jump at each wait, the last picture that was presented (as palette indices), and the buttons */
static int game_hooks_on = 1;
static int async_on;          /* ticks from a thread instead of the count hook */
static uint32_t game_frame;
static int frame_jump = 1;
static tg_t shown_g;
static uint8_t image[TG_HEIGHT][TG_WIDTH];
static unsigned held, went_down;
static int allocs, releases, fast_allocs;
static int press_b_after;   /* B is held from this many frames on (0: never): the example programs end when it is pressed */

static uint32_t t_wait_frame(void)
{
    pump();
    now_ms += 17;
    if (++idle_frames > 100000 && script_pos >= script_len && cut_armed)
        longjmp(cut, 1);
    game_frame += (uint32_t)frame_jump;
    if (press_b_after && game_frame >= 100 + (uint32_t)press_b_after)
        held |= PLAT_BTN_B;
    return game_frame;
}

static void t_present(const tg_t *g)
{
    presents++;
    shown_g = *g;
    for (int line = 0; line < TG_HEIGHT; line++)
        tg_render_line_indices(&shown_g, line, image[line]);
}

static unsigned t_buttons(int player, int kind)
{
    return player == 0 ? (kind == 0 ? held : kind == 1 ? went_down : 0) : 0;
}

/* the mouse: plat_mouse gives what the test puts in tmouse, and plat_mouse_set_position moves it there (kept on the screen, as the machine does); with
 * mouse_on 0 the environment has no mouse hooks */
static int mouse_on = 1;
static plat_mouse_t tmouse;
static int mouse_sets;

static plat_mouse_t t_mouse(void) { return tmouse; }

static void t_set_mouse(int x, int y)
{
    mouse_sets++;
    tmouse.x = (int16_t)(x < 0 ? 0 : x > TG_WIDTH - 1 ? TG_WIDTH - 1 : x);
    tmouse.y = (int16_t)(y < 0 ? 0 : y > TG_HEIGHT - 1 ? TG_HEIGHT - 1 : y);
}

static size_t lua_bytes;   /* what Lua has through the allocator hook */
static size_t shared_limit;   /* nonzero: the game's memory and Lua's share this much, as the board's PSRAM pool does (Lua's own does not fail) */
static int shared_collect;    /* ... and when it is full, the hook has Lua collect and tries again, as the devkit's game_alloc does */

static void *t_alloc(size_t n, int fast)
{
    if (shared_limit && lua_bytes + n > shared_limit && shared_collect)
        tl_env_collect(&env);
    if (shared_limit && lua_bytes + n > shared_limit)
        return NULL;
    allocs++;
    fast_allocs += fast;
    return malloc(n);
}

static void t_release(void *p)
{
    releases++;
    free(p);
}

static void *t_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;
    if (ptr)
        lua_bytes -= osize;
    if (nsize == 0) {
        free(ptr);
        return NULL;
    }
    void *p = realloc(ptr, nsize);
    if (p)
        lua_bytes += nsize;
    else if (ptr)
        lua_bytes += osize;
    return p;
}

/* A fresh environment (the program, the screen and the files are new). */
static void fresh(void)
{
    fs_reset();
    CHECK(fs_ram_init(&disk, pool, sizeof pool) == 0 && fs_mount("sd", &fs_ram_ops, &disk) == 0);
    fs_chdir("/sd");
    memset(&screen, 0, sizeof screen);
    tg_text_load_cp437(&screen);
    tg_text_init(&screen);
    tl_hooks_t hooks = {.getchar = get_char, .frame = frame, .millis = millis, .info = info_on ? test_info : NULL, .poll = poll_hook,
                        .lua_alloc = t_lua_alloc, .async_ticks = async_on};
    if (game_hooks_on) {
        hooks.wait_frame = t_wait_frame;
        hooks.present = t_present;
        hooks.buttons = t_buttons;
        hooks.alloc = t_alloc;
        hooks.release = t_release;
    }
    if (mouse_on) {
        hooks.mouse = t_mouse;
        hooks.set_mouse = t_set_mouse;
    }
    tmouse = (plat_mouse_t){.x = TG_WIDTH / 2, .y = TG_HEIGHT / 2};
    mouse_sets = 0;
    lua_bytes = 0;   /* (the state from before is not closed, as a cut may have left it half way: what it had does not count) */
    allocs = releases = fast_allocs = 0;
    snd_init(&tsnd, PLAT_AUDIO_RATE);
    hooks.snd = sound_on ? &tsnd : NULL;
    tl_env_init(&env, &screen, &hooks);
    game_frame = 100;
    press_b_after = 0;
    frame_jump = 1;
    presents = 0;
    polls = 0;
    held = went_down = 0;
    memset(image, 0xEE, sizeof image);
    script_len = script_pos = 0;
}

/* Runs the environment with what is in the script: the exit code if it left the prompt, -1 if the keys ran out. */
static int go(void)
{
    idle_frames = 0;
    cut_armed = 1;
    if (setjmp(cut)) {
        cut_armed = 0;
        return -1;
    }
    const int r = tl_env_run(&env);
    cut_armed = 0;
    return r;
}

static int put_data(const char *path, const void *data, size_t len)
{
    const int fd = fs_open(path, FS_WRONLY | FS_CREAT | FS_TRUNC);
    if (fd < 0)
        return fd;
    const int w = fs_write(fd, data, len);
    fs_close(fd);
    return w;
}

static int put_file(const char *path, const char *text) { return put_data(path, text, strlen(text)); }

static int file_is(const char *path, const char *text)
{
    char buf[4096];
    const int fd = fs_open(path, FS_RDONLY);
    if (fd < 0)
        return 0;
    const int n = fs_read(fd, buf, sizeof buf);
    fs_close(fd);
    return n == (int)strlen(text) && !memcmp(buf, text, (size_t)n);
}

static void test_basics(void)
{
    fresh();
    type("print(2 + 2)\n");
    CHECK(go() == -1);
    CHECK(strcmp(row(0), "TILEGAME LUA  " LUA_RELEASE) == 0);
    CHECK(strcmp(row(1), "Type HELP for the commands, or Lua at once.") == 0);
    CHECK(strcmp(row(3), "] print(2 + 2)") == 0 && strcmp(row(4), "4") == 0 && strcmp(row(5), "]") == 0);
    /* an expression is shown; variables stay from one line to the next; strings; integer and float division */
    type("x = 5\nx * x\n= \"ab\" .. \"cd\"\nb = \"x\"; print(b .. b)\nprint(7 / 2, 7 // 2, 7 % 2)\nprint(zz)\n");
    CHECK(go() == -1);
    CHECK(shows("25") && shows("abcd") && shows("xx") && shows("3.5     3       1") && shows("nil"));
    /* several values, and a function's results */
    type("CLS\n1, 2, \"three\"\nmath.max(3, 9, 4)\n");
    CHECK(go() == -1 && shows("1       2       three") && strcmp(row(row_of("] math.max") + 1), "9") == 0);
    /* a statement over several lines: the prompt is ">>" until it is finished */
    type("CLS\nfor i = 1, 3 do\nprint(i * 10)\nend\n");
    CHECK(go() == -1);
    CHECK(strcmp(row(1), ">> print(i * 10)") == 0 && strcmp(row(2), ">> end") == 0 && strcmp(row(3), "10") == 0 && strcmp(row(5), "30") == 0);
    type("CLS\nfunction sq(n)\nreturn n * n\nend\nsq(12)\n");
    CHECK(go() == -1 && shows("144"));
    type("CLS\ns = [[one\ntwo]]\nprint(#s)\n");     /* a long string that goes on */
    CHECK(go() == -1 && shows("7"));
    type("CLS\nif true then\n^Cprint(5)\n");        /* Ctrl+C gives up the unfinished statement */
    CHECK(go() == -1 && shows(">> ^C") && strcmp(row(3), "5") == 0);
    /* an error shows its message, and the prompt comes back */
    type("CLS\nprint(1 +)\n");
    CHECK(go() == -1);
    CHECK(shows("Error:") && shows("unexpected symbol") && last_row_of("]") > row_of("Error:"));
    type("CLS\nnothing()\n");
    CHECK(go() == -1 && shows("Error:") && shows("attempt to call a nil value (global 'nothing')"));
    type("CLS\nerror(true)\n");                     /* an error that is not a string */
    CHECK(go() == -1 && shows("Error: (the error is a boolean)"));
    /* a Lua name that is a command's word with something after it that makes it Lua is Lua */
    type("CLS\nlist = 7\nlist + 1\nrun = 3\nrun * 2\n");
    CHECK(go() == -1 && shows("8") && shows("6") && !shows("No program"));
    /* HELP and CLS */
    fresh();
    type("HELP\n");
    CHECK(go() == -1 && shows("Commands:") && shows("EDIT [file]") && shows("Anything else is Lua"));
    type("cls\n");
    CHECK(go() == -1 && strcmp(row(0), "]") == 0 && !shows("Commands:"));   /* cleared, and the prompt is at the top */
    type("ver\n");
    CHECK(go() == -1 && shows(LUA_RELEASE ", with 32-bit integers and floats"));
    type("math.maxinteger\n");
    CHECK(go() == -1 && shows("2147483647"));
    type("bye\n");
    CHECK(go() == TL_EXIT_BYE);
}

static const char hello_lua[] =
    "print(\"Hello, world\")\n"
    "for i = 1, 3 do\n"
    "  print(i * i)\n"
    "end\n"
    "kept = 42\n";

static void test_programs(void)
{
    fresh();
    CHECK(put_file("/sd/hello.lua", hello_lua) > 0);
    type("RUN\n");                                  /* nothing in memory yet */
    type("LOAD hello\nLIST\nRUN\n");
    CHECK(go() == -1);
    CHECK(shows("No program"));
    CHECK(shows("Loaded hello.lua, "));
    CHECK(row_of("   1  print(\"Hello, world\")") >= 0);
    CHECK(row_of("   2  for i = 1, 3 do") >= 0 && row_of("   3    print(i * i)") >= 0 && row_of("   4  end") >= 0);
    const int hello = row_of("Hello, world");
    CHECK(hello >= 0 && strcmp(row(hello + 1), "1") == 0 && strcmp(row(hello + 2), "4") == 0 && strcmp(row(hello + 3), "9") == 0 && strcmp(row(hello + 4), "Ready.") == 0);
    /* what the program left can be looked at from the prompt; RUN starts afresh, and NEW forgets it */
    type("kept\nx = 1\nRUN\nx\nNEW\nkept\n");
    CHECK(go() == -1);
    CHECK(strcmp(row(last_row_of("] kept") - 9), "42") == 0 || shows("42"));
    CHECK(strcmp(row(last_row_of("] x") + 1), "nil") == 0 && strcmp(row(last_row_of("] kept") + 1), "nil") == 0);
    type("RUN\n");
    CHECK(go() == -1 && last_row_of("No program") > last_row_of("] kept"));
    /* LIST ranges */
    type("LOAD hello.lua\nCLS\nLIST 2 3\n");
    CHECK(go() == -1);
    CHECK(row_of("   1  ") == -1 && row_of("   2  for i") >= 0 && row_of("   3    print") >= 0 && row_of("   4  ") == -1);
    type("CLS\nLIST 4\n");
    CHECK(go() == -1 && row_of("   4  end") >= 0 && row_of("   3  ") == -1);
    /* a program with a function, a list and a dictionary, a while loop and string functions */
    const char *rich =
        "local function fib(n)\n"
        "  if n < 2 then return n end\n"
        "  return fib(n - 1) + fib(n - 2)\n"
        "end\n"
        "print(fib(20))\n"
        "local l = {1, 2, 3}\n"
        "table.insert(l, 4)\n"
        "local t = \"\"\n"
        "for _, x in ipairs(l) do\n"
        "  t = t .. x .. \",\"\n"
        "end\n"
        "print(#l .. \" \" .. t)\n"
        "local d = {a = 1, b = 2}\n"
        "local n = 0\n"
        "for _ in pairs(d) do n = n + 1 end\n"
        "print(n)\n"
        "local s, i = \"\", 0\n"
        "while i < 5 do\n"
        "  s = s .. i\n"
        "  i = i + 1\n"
        "end\n"
        "print(s .. \" \" .. s:sub(1, 2) .. \" \" .. s:sub(3, 4))\n"
        "print(string.format(\"%5.2f|%d|%s\", 3.14159, 7, \"x\"))\n";
    CHECK(put_file("/sd/rich.lua", rich) > 0);
    type("CLS\nLOAD rich\nRUN\n");
    CHECK(go() == -1);
    const int out = row_of("6765");
    CHECK(out >= 0 && strcmp(row(out + 1), "4 1,2,3,4,") == 0 && strcmp(row(out + 2), "2") == 0 && strcmp(row(out + 3), "01234 01 23") == 0);
    CHECK(strcmp(row(out + 4), " 3.14|7|x") == 0 && strcmp(row(out + 5), "Ready.") == 0);
    /* a runtime error says where; the program stops and the prompt returns */
    CHECK(put_file("/sd/bad.lua", "print(1)\nprint(nothing(3))\nprint(2)\n") > 0);
    type("CLS\nLOAD bad\nRUN\n");
    CHECK(go() == -1);
    CHECK(shows("Error: bad.lua:2: attempt to call a nil value (global 'nothing')") && !shows("Ready.") && last_row_of("]") > row_of("Error:"));
    CHECK(row_of("1") >= 0 && row_of("1") < row_of("Error:") && row_of("2") == -1);   /* the line before the error ran, the line after did not */
    /* a mistake in the program text is found before anything runs */
    CHECK(put_file("/sd/syntax.lua", "print(1)\nif x then\n") > 0);
    type("CLS\nLOAD syntax\nRUN\n");
    CHECK(go() == -1 && shows("Error: syntax.lua:3:") && row_of("1") == -1);
    /* os.exit ends a program as its end would */
    CHECK(put_file("/sd/stop.lua", "print(1)\nos.exit()\nprint(2)\n") > 0);
    type("CLS\nLOAD stop\nRUN\n");
    CHECK(go() == -1 && row_of("1") >= 0 && row_of("2") == -1 && shows("Ready.") && !shows("Error"));
}

/* STACK and HEAP show what the machine's hook says; without one they say so */
static void test_info_commands(void)
{
    fresh();
    type("CLS\nSTACK\n");
    CHECK(go() == -1 && shows("stack: peak 1234 of 4096") && !shows("heap:"));
    type("CLS\nheap\n");
    CHECK(go() == -1 && shows("heap: 5678 free") && shows("Lua is using ") && !shows("stack:"));
    type("CLS\nusblog\n");
    CHECK(go() == -1 && shows("usblog: saved") && !shows("heap:"));
    type("CLS\nHELP\n");
    CHECK(go() == -1 && shows("STACK") && shows("HEAP") && shows("USBLOG"));
    info_on = 0;
    fresh();
    type("CLS\nSTACK\nHEAP\n");
    CHECK(go() == -1 && shows("No stack on this machine") && shows("Lua is using "));
    info_on = 1;
}

static void test_files(void)
{
    fresh();
    CHECK(put_file("/sd/hello.lua", hello_lua) > 0);
    /* LOAD finds NAME.lua for NAME; errors for what is not there */
    type("LOAD nothing\nLOAD\nLOAD hello\n");
    CHECK(go() == -1);
    CHECK(shows("Cannot load nothing: no such file or directory") && shows("Usage: LOAD file") && shows("Loaded hello.lua, "));
    CHECK(strcmp(env.name, "hello.lua") == 0 && env.length == strlen(hello_lua) && strcmp(env.program, hello_lua) == 0);
    /* SAVE with no name saves in place; with a name saves there, and that is the program's file from then on */
    CHECK(put_file("/sd/hello.lua", "changed on disk\n") > 0);
    type("SAVE\n");
    CHECK(go() == -1 && shows("Saved hello.lua, "));
    CHECK(file_is("/sd/hello.lua", hello_lua) && fs_stat("/sd/hello.lua.TMP", &(fs_stat_t){0}) == FS_ENOENT);
    type("SAVE copy.lua\n");
    CHECK(go() == -1 && file_is("/sd/copy.lua", hello_lua) && strcmp(env.name, "copy.lua") == 0);
    type("NEW\nSAVE\n");
    CHECK(go() == -1 && shows("Usage: SAVE file") && env.length == 0 && env.name[0] == 0);
    type("SAVE empty.lua\n");                        /* an empty program saves as an empty file */
    CHECK(go() == -1 && file_is("/sd/empty.lua", ""));
    /* FILES, with sizes and folders */
    CHECK(fs_mkdir("/sd/games") == 0);
    type("CLS\nFILES\n");
    CHECK(go() == -1);
    CHECK(shows("copy.lua") && shows("empty.lua") && shows("hello.lua") && shows("games") && shows("<DIR>"));
    char want[40];
    snprintf(want, sizeof want, "%9lu", (unsigned long)strlen(hello_lua));
    CHECK(shows(want));
    type("CLS\nFILES games\n");
    CHECK(go() == -1 && shows("(empty)"));
    type("CLS\nFILES nothere\n");
    CHECK(go() == -1 && shows("Cannot list nothere:"));
    /* CD */
    type("CLS\nCD\nCD games\nCD\nSAVE g.lua\nFILES\nCD /sd\nCD nowhere\n");
    CHECK(go() == -1);
    CHECK(shows("/sd/games") && shows("g.lua") && shows("Cannot go to nowhere:") && strcmp(fs_getcwd(), "/sd") == 0 && file_is("/sd/games/g.lua", ""));
    /* KILL asks first */
    type("CLS\nKILL copy.lua\nn\n");
    CHECK(go() == -1 && shows("Delete copy.lua? (Y/N)") && file_is("/sd/copy.lua", hello_lua));
    type("CLS\nKILL copy.lua\ny\nKILL copy.lua\ny\nKILL\n");
    CHECK(go() == -1 && fs_stat("/sd/copy.lua", &(fs_stat_t){0}) == FS_ENOENT && shows("Cannot delete copy.lua:") && shows("Usage: KILL file"));
    /* a program that is too big is refused, and the program in memory stays */
    static char big[TL_PROGRAM_MAX + 100];
    memset(big, ' ', sizeof big - 1);
    big[sizeof big - 1] = 0;
    for (size_t i = 70; i < sizeof big; i += 71)
        big[i] = '\n';
    int fd = fs_open("/sd/big.lua", FS_WRONLY | FS_CREAT);
    CHECK(fd >= 0 && fs_write(fd, big, strlen(big)) == (int)strlen(big) && fs_close(fd) == 0);
    type("CLS\nLOAD hello\nLOAD big\n");
    CHECK(go() == -1 && shows("That file is too big: the most is 65536 bytes.") && strcmp(env.name, "hello.lua") == 0 && strcmp(env.program, hello_lua) == 0);
    /* exactly the most is accepted */
    fd = fs_open("/sd/max.lua", FS_WRONLY | FS_CREAT);
    CHECK(fd >= 0 && fs_write(fd, big, TL_PROGRAM_MAX) == TL_PROGRAM_MAX && fs_close(fd) == 0);
    CHECK(tl_env_load(&env, "/sd/max.lua") == 0 && env.length == TL_PROGRAM_MAX);
}

static void test_editing_keys(void)
{
    fresh();
    /* the keys of the line editor, each shown by what the line then gives (a number on its own is shown) */
    type("12{LEFT}{LEFT}34\n");                                           /* inserts in the middle: 3412 */
    CHECK(go() == -1 && strcmp(row(3), "] 3412") == 0 && strcmp(row(4), "3412") == 0);
    type("99{BS}1\n");                                                    /* Backspace: 91 */
    CHECK(go() == -1 && strcmp(row(5), "] 91") == 0 && strcmp(row(6), "91") == 0);
    type("{LEFT}{LEFT}{HOME}5{RIGHT}{RIGHT}\n");                          /* Left and Right at the ends do nothing */
    CHECK(go() == -1 && strcmp(row(7), "] 5") == 0 && strcmp(row(8), "5") == 0);
    type("X100{HOME}{DEL}{END}0\n");                                      /* Home, Delete, End: 1000 */
    CHECK(go() == -1 && strcmp(row(9), "] 1000") == 0 && strcmp(row(10), "1000") == 0);
    type("10XYZ{LEFT}{LEFT}{LEFT}^K\n");                                  /* Ctrl+K: to the end of the line */
    CHECK(go() == -1 && strcmp(row(11), "] 10") == 0 && strcmp(row(12), "10") == 0);
    type("9^U\n7\n");                                                     /* Ctrl+U: the whole line (Enter on the empty line gives a prompt again) */
    CHECK(go() == -1 && strcmp(row(13), "]") == 0 && strcmp(row(14), "] 7") == 0 && strcmp(row(15), "7") == 0);
    type(" + 5^A1^E\n");                                                  /* Ctrl+A and Ctrl+E */
    CHECK(go() == -1 && strcmp(row(16), "] 1 + 5") == 0 && strcmp(row(17), "6") == 0);
    type("6{DEL}{DEL}\n");                                                /* Delete at the end does nothing */
    CHECK(go() == -1 && strcmp(row(18), "] 6") == 0 && strcmp(row(19), "6") == 0);
    /* history: Up goes back through the lines run (a line the same as the last one is kept once), Down forward again and to an empty line */
    fresh();
    type("1\n2\n3\n{UP}\n{UP}{UP}\n{UP}{DOWN}\n");
    CHECK(go() == -1);
    CHECK(strcmp(row(3), "] 1") == 0 && strcmp(row(5), "] 2") == 0 && strcmp(row(7), "] 3") == 0);
    CHECK(strcmp(row(9), "] 3") == 0 && strcmp(row(10), "3") == 0);       /* Up: the last line */
    CHECK(strcmp(row(11), "] 2") == 0 && strcmp(row(12), "2") == 0);      /* Up, Up: the one before it */
    CHECK(strcmp(row(13), "]") == 0 && strcmp(row(14), "]") == 0);        /* Up, Down: an empty line again, which does nothing */
    type("{UP}{UP}{UP}{UP}{UP}{UP}\n");                                   /* far more than there are: the oldest */
    CHECK(go() == -1 && strcmp(row(14), "] 1") == 0 && strcmp(row(15), "1") == 0);
    type("{DOWN}{DOWN}{UP}{DOWN}x\n");                                    /* Down with nothing newer: the empty line, typed on */
    CHECK(go() == -1 && strcmp(row(16), "] x") == 0);
    /* the line is as long as the screen line allows and no more */
    fresh();
    char longline[200];
    memset(longline, 'x', sizeof longline);
    longline[199] = 0;
    type(longline);
    type("\n");
    CHECK(go() == -1);
    const int pr = row_of("] xxxx");
    CHECK(pr >= 0 && strlen(row(pr)) == 79);                             /* 2 for the prompt, 77 typed: one column is left free */
    CHECK(strcmp(row(pr + 1), "nil") == 0);                              /* (a variable that has no value) */
    /* Ctrl+C at the prompt gives the line up */
    fresh();
    type("1^C\n2\n");
    CHECK(go() == -1 && shows("] 1^C") && shows("] 2") && strcmp(row(row_of("] 2") + 1), "2") == 0);
}

static int ticker_on;
static void *ticker(void *arg)
{
    (void)arg;
    while (ticker_on) {
        tl_env_tick();
        usleep(1000);
    }
    return NULL;
}

static void test_input_and_break(void)
{
    fresh();
    CHECK(put_file("/sd/ask.lua", "io.write(\"Name: \")\nlocal n = io.read()\nprint(\"Hi \" .. n)\nio.write(\"Age: \")\nlocal a = io.read(\"n\")\nprint(a + 1)\nname = n\n") > 0);
    type("LOAD ask\nRUN\nBob\n41\nname\n");
    CHECK(go() == -1);
    const int ask = row_of("Name: Bob");
    CHECK(ask >= 0 && strcmp(row(ask + 1), "Hi Bob") == 0 && strcmp(row(ask + 2), "Age: 41") == 0 && strcmp(row(ask + 3), "42") == 0 && strcmp(row(ask + 4), "Ready.") == 0);
    CHECK(strcmp(row(last_row_of("] name") + 1), "Bob") == 0);   /* what the program kept is there at the prompt */
    /* not a number for "n": nil */
    type("CLS\nRUN\nAnn\nold\n");
    CHECK(go() == -1 && shows("Age: old") && shows("attempt to perform arithmetic on a nil value"));
    /* Ctrl+C at io.read ends the program */
    type("CLS\nRUN\n^C\n");
    CHECK(go() == -1 && shows("Name: ^C") && shows("Break") && !shows("Ready."));
    /* Ctrl+C stops a program that runs on for ever, and the prompt is back: nothing waits for it to end by itself */
    CHECK(put_file("/sd/loop.lua", "local i = 0\nwhile true do\n  i = i + 1\nend\n") > 0);
    type("CLS\nLOAD loop\nRUN\n^C");
    CHECK(go() == -1 && shows("Break"));
    type("1\n");                                    /* and the prompt works */
    CHECK(go() == -1 && last_row_of("1") > row_of("Break"));
    /* the same with a pcall in the way: it cannot keep the program from stopping */
    CHECK(put_file("/sd/loop2.lua", "while true do\n  pcall(function() for i = 1, 100000 do end end)\nend\n") > 0);
    type("CLS\nLOAD loop2\nRUN\n^C");
    CHECK(go() == -1 && shows("Break"));
    /* and with ticks from another thread (as the board's timer interrupt and the desktop's SDL timer give them) instead of counting instructions */
    async_on = 1;
    fresh();
    CHECK(put_file("/sd/loop.lua", "local i = 0\nwhile true do\n  i = i + 1\nend\n") > 0);
    ticker_on = 1;
    pthread_t th;
    CHECK(pthread_create(&th, NULL, ticker, NULL) == 0);
    type("LOAD loop\nRUN\n^C1 + 1\n");
    CHECK(go() == -1 && shows("Break") && strcmp(row(last_row_of("] 1 + 1") + 1), "2") == 0);
    ticker_on = 0;
    pthread_join(th, NULL);
    async_on = 0;
    /* keys typed while a program runs are kept for its io.read */
    fresh();
    CHECK(put_file("/sd/wait.lua", "for k = 1, 1000000 do end\nio.write(\"x? \")\nlocal x = io.read(\"n\")\nprint(x * 2)\n") > 0);
    type("LOAD wait\nRUN\n21\n");
    CHECK(go() == -1 && shows("x? 21") && shows("42"));
}

static void test_edit_and_exit(void)
{
    fresh();
    CHECK(put_file("/sd/hello.lua", hello_lua) > 0);
    type("EDIT hello.lua\n");
    CHECK(go() == TL_EXIT_EDIT && strcmp(env.edit_name, "hello.lua") == 0);
    fresh();
    CHECK(put_file("/sd/hello.lua", hello_lua) > 0);
    type("EDIT hello\n");                           /* NAME for NAME.lua, as for LOAD */
    CHECK(go() == TL_EXIT_EDIT && strcmp(env.edit_name, "hello.lua") == 0);
    fresh();
    CHECK(put_file("/sd/plain", "x\n") > 0);
    type("EDIT plain\n");                           /* a file that is there under that name, as it is */
    CHECK(go() == TL_EXIT_EDIT && strcmp(env.edit_name, "plain") == 0);
    fresh();
    type("EDIT fresh\n");                           /* a new name without an extension is a .lua file */
    CHECK(go() == TL_EXIT_EDIT && strcmp(env.edit_name, "fresh.lua") == 0);
    fresh();
    type("EDIT dotted.txt\n");
    CHECK(go() == TL_EXIT_EDIT && strcmp(env.edit_name, "dotted.txt") == 0);
    fresh();
    type("EDIT\n");                                 /* with no program there is nothing to edit */
    CHECK(go() == -1 && shows("Usage: EDIT file"));
    CHECK(put_file("/sd/prog.lua", hello_lua) > 0);
    type("LOAD prog\nEDIT\n");                      /* the program's own file */
    CHECK(go() == TL_EXIT_EDIT && strcmp(env.edit_name, "prog.lua") == 0);
    /* back from the editor: the file is loaded again and the prompt continues */
    CHECK(put_file("/sd/prog.lua", "print(\"edited\")\n") > 0);
    CHECK(tl_env_load(&env, env.edit_name) == 0);
    script_len = script_pos = 0;
    type("RUN\nBYE\n");
    CHECK(go() == TL_EXIT_BYE && shows("edited"));
    CHECK(env.exit_code == TL_EXIT_BYE);
    /* QUIT and EXIT are BYE too, in any case */
    fresh();
    type("quit\n");
    CHECK(go() == TL_EXIT_BYE);
    fresh();
    type("  Exit  \n");
    CHECK(go() == TL_EXIT_BYE);
}

static void test_gaps(void)
{
    /* edges of the line editor */
    fresh();
    type("X1{HOME}{RIGHT}{BS}\n");                                       /* Backspace at position 1 removes the first character */
    CHECK(go() == -1 && strcmp(row(3), "] 1") == 0 && strcmp(row(4), "1") == 0);
    type("12{LEFT}{DEL}\n");                                             /* Delete of the last character with the cursor before it */
    CHECK(go() == -1 && strcmp(row(5), "] 1") == 0 && strcmp(row(6), "1") == 0);
    type("10XYZ{LEFT}{LEFT}{LEFT}^K2\n");                                /* typing on after Ctrl+K appends at the new end */
    CHECK(go() == -1 && strcmp(row(7), "] 102") == 0);
    type("9^U8\n");                                                      /* typing on after Ctrl+U starts at the left */
    CHECK(go() == -1 && row_of("] 8") >= 0 && strcmp(row(row_of("] 8") + 1), "8") == 0);
    fresh();
    type("1\n2\n{DOWN}{UP}\n");                                          /* Down with nothing newer does not go below the empty line */
    CHECK(go() == -1 && strcmp(row(7), "] 2") == 0);
    /* LOAD: the size limit is exact and a refused file leaves the program alone */
    fresh();
    CHECK(put_file("/sd/keep.lua", "print(\"kept\")\n") > 0);
    static char big[TL_PROGRAM_MAX + 2];
    memset(big, ' ', sizeof big);
    big[TL_PROGRAM_MAX + 1] = 0;
    CHECK(put_file("/sd/over.lua", big) > 0);
    big[TL_PROGRAM_MAX] = 0;
    CHECK(put_file("/sd/exact.lua", big) > 0);
    CHECK(tl_env_load(&env, "keep") == 0 && env.length == 14);
    CHECK(tl_env_load(&env, "over") != 0 && env.length == 14 && strcmp(env.name, "keep.lua") == 0);
    CHECK(strncmp(env.program, "print(\"kept\")", 13) == 0);
    CHECK(tl_env_load(&env, "exact") == 0 && env.length == TL_PROGRAM_MAX);
    /* a failed SAVE leaves no temporary file */
    fresh();
    CHECK(put_file("/sd/x.lua", "print(1)\n") > 0 && fs_mkdir("/sd/dir.lua") == 0);
    CHECK(tl_env_load(&env, "x") == 0);
    type("SAVE dir.lua\n");
    CHECK(go() == -1 && shows("Cannot save"));
    fs_stat_t st;
    CHECK(fs_stat("/sd/dir.lua.TMP", &st) != 0);
    /* paging of LIST and FILES: 22 rows, then "-- more --"; q (or Esc, Ctrl+C) stops, any other key goes on */
    fresh();
    char prog[2048];
    int n = 0;
    for (int i = 1; i <= 60; i++)
        n += snprintf(prog + n, sizeof prog - (size_t)n, "-- L%d\n", i);
    CHECK(put_file("/sd/long.lua", prog) > 0);
    type("LOAD long\nCLS\nLIST\n");
    CHECK(go() == -1 && shows("-- more --") && shows("-- L22") && !shows("-- L23"));
    fresh();                                        /* (the keys run out inside "-- more --", so each case is run again from the start) */
    CHECK(put_file("/sd/long.lua", prog) > 0);
    type("LOAD long\nCLS\nLIST\n ");
    CHECK(go() == -1 && shows("-- L44") && !shows("-- L45") && shows("-- more --"));
    fresh();
    CHECK(put_file("/sd/long.lua", prog) > 0);
    type("LOAD long\nCLS\nLIST\nq");
    CHECK(go() == -1 && !shows("-- L23") && !shows("-- more --") && last_row_of("]") > row_of("  22"));
    const int keys[] = {'q', 'Q', 27, 3};
    for (int k = 0; k < 4; k++) {
        fresh();
        CHECK(put_file("/sd/long.lua", prog) > 0);
        type("LOAD long\nCLS\nLIST\n");
        script[script_len++] = keys[k];
        CHECK(go() == -1 && !shows("-- L23") && !shows("-- more --"));
    }
    fresh();
    CHECK(put_file("/sd/long.lua", prog) > 0);
    type("LOAD long\nCLS\nLIST 5 7\nLIST 9\n");                       /* a range, and a single line */
    CHECK(go() == -1 && shows("-- L5") && shows("-- L7") && !shows("-- L8") && shows("-- L9") && !shows("-- L10"));
    fresh();
    for (int i = 0; i < 30; i++) {
        char name[32];
        snprintf(name, sizeof name, "/sd/f%02d.txt", i);
        CHECK(put_file(name, "x") > 0);
    }
    type("CLS\nFILES\n");
    CHECK(go() == -1 && shows("-- more --") && !shows("f22.txt"));
    fresh();
    for (int i = 0; i < 30; i++) {
        char name[32];
        snprintf(name, sizeof name, "/sd/f%02d.txt", i);
        CHECK(put_file(name, "x") > 0);
    }
    type("CLS\nFILES\nq");
    CHECK(go() == -1 && !shows("f23.txt") && !shows("-- more --"));
}

/* ---- the game functions ---- */

#define R8(x) x x x x x x x x
#define R64(x) R8(R8(x))

/* Runs a program. Leaves the environment as the run left it. */
static int run_jump = 1;   /* the frames that pass at each waitframe */
static unsigned run_held, run_down;   /* the buttons a run sees */

static struct { const char *path, *text; size_t len; } seeds[8];   /* files every run_program starts with (fresh empties the disk) */
static int seed_count;

static void seed_data(const char *path, const void *data, size_t len)
{
    for (int i = 0; i < seed_count; i++)
        if (!strcmp(seeds[i].path, path)) {
            seeds[i].text = data;
            seeds[i].len = len;
            return;
        }
    CHECK(seed_count < 8);
    seeds[seed_count].path = path;
    seeds[seed_count].text = data;
    seeds[seed_count++].len = len;
}

static void seed_file(const char *path, const char *text) { seed_data(path, text, strlen(text)); }

static void run_program(const char *text)
{
    fresh();
    for (int i = 0; i < seed_count; i++) {
        char dir[64];
        snprintf(dir, sizeof dir, "%s", seeds[i].path);
        char *slash = strrchr(dir, '/');
        if (slash && slash > dir + 3) {   /* (a file in a folder: the folder first) */
            *slash = 0;
            fs_mkdir(dir);
        }
        CHECK(put_data(seeds[i].path, seeds[i].text, seeds[i].len) > 0);
    }
    frame_jump = run_jump;
    held = run_held;
    went_down = run_down;
    CHECK(put_file("/sd/t.lua", text) > 0);
    type("LOAD t\nRUN\n");
    CHECK(go() == -1);
}

static int printed(const char *line)   /* a line of the program's output is on the screen */
{
    return row_of(line) >= 0;
}

static int pixels(int idx)            /* how many pixels of the last picture have this palette index */
{
    int n = 0;
    for (int y = 0; y < TG_HEIGHT; y++)
        for (int x = 0; x < TG_WIDTH; x++)
            n += image[y][x] == idx;
    return n;
}

/* the rows after the one that starts with `first`, compared with want[] */
static int rows_after(const char *first, const char *const *want, int n)
{
    const int r = row_of(first);
    if (r < 0)
        return 0;
    for (int i = 0; i < n; i++)
        if (strcmp(row(r + 1 + i), want[i]))
            return 0;
    return 1;
}

/* NEW frees the game's state: what the graphics took is all given back */
static int freed_by_new(void)
{
    type("NEW\n");
    return go() == -1 && env.game == NULL && allocs == releases;
}

static void test_game_draw(void)
{
    run_program(
        "gfx(8)\n"
        "palette(\"4\", 255, 0, 0)\n"
        "tiledef(1, \"" R64("4") "\")\n"
        "sprdef(1, \"" R64("7") "\")\n"
        "newmap(20, 20)\n"
        "tile(0, 0, 1)\n"
        "tile(2, 1, 1)\n"
        "a = actor(1, 100, 50)\n"
        "waitframe()\n"
        "print(gettile(2, 1))\n"
        "print(gettile(5, 5))\n"
        "print(a)\n");
    CHECK(printed("1") && printed("0") && printed("Ready."));
    CHECK(env.game != NULL && !env.game_active);                    /* kept for the prompt, and the text screen is back */
    CHECK(shown_g.palette['4'] == 0xF800);
    CHECK(pixels('4') == 128 && pixels('7') == 64 && pixels(0) == TG_WIDTH * TG_HEIGHT - 192);
    CHECK(image[0][0] == '4' && image[7][7] == '4' && image[0][8] == 0 && image[8][16] == '4' && image[15][23] == '4' && image[16][16] == 0);
    CHECK(image[50][100] == '7' && image[57][107] == '7' && image[58][100] == 0 && image[50][108] == 0);
    CHECK(shown_g.sprites[0].above == TG_MAX_LAYERS);                  /* in front of the map (0 would put the actors behind its tiles) */
    CHECK(presents == 1);                                            /* waitframe; nothing is shown after the program ends */
    CHECK(allocs > 0 && fast_allocs > 0 && allocs > releases);
    type("gettile(2, 1)\n");                                         /* the graphics are there to look at from the prompt */
    CHECK(go() == -1 && strcmp(row(last_row_of("] gettile") + 1), "1") == 0);
    CHECK(freed_by_new());

    /* the camera: the picture moves the other way; a negative camera too */
    run_program(
        "gfx(8); palette(\"4\", 255, 0, 0); tiledef(1, \"" R64("4") "\"); newmap(20, 20); tile(0, 0, 1)\n"
        "camera(4, 3)\n"
        "waitframe()\n");
    CHECK(pixels('4') == 20 && image[4][3] == '4' && image[4][4] == 0 && image[5][3] == 0 && image[0][0] == '4');
    run_program(
        "gfx(8); tiledef(1, \"" R64("4") "\"); newmap(20, 20); tile(0, 0, 1)\n"
        "camera(-10, -10)\n"
        "waitframe()\n");
    CHECK(pixels('4') == 64 && image[10][10] == '4' && image[17][17] == '4' && image[9][10] == 0 && image[10][9] == 0);

    /* flips and single pixels, any colour 0 to 255 */
    run_program(
        "gfx(8); tiledef(2, \"9" R8(R8(".")) "\"); tilepix(3, 2, 1, 200)\n"
        "newmap(8, 8); tile(0, 0, 2, 1); tile(1, 0, 2, 2); tile(2, 0, 2, 3); tile(3, 0, 3)\n"
        "waitframe()\n");
    CHECK(image[0][7] == '9' && image[0][0] == 0);                   /* flipped across */
    CHECK(image[7][8] == '9' && image[0][8] == 0);                   /* flipped down */
    CHECK(image[7][23] == '9');                                      /* both */
    CHECK(image[1][26] == 200);

    /* a colour is named by a character, the same one a graphics string uses; a number is the entry itself */
    run_program(
        "gfx(8); palette(\"A\", 255, 0, 0); palette(66, 0, 255, 0); palette(\"a\", 0, 0, 255)\n"
        "tiledef(1, \"" R64("A") "\"); tiledef(2, \"" R64("B") "\"); tiledef(3, \"" R64("a") "\"); newmap(8, 8); tile(0, 0, 1); tile(1, 0, 2); tile(2, 0, 3)\n"
        "waitframe()\n");
    CHECK(shown_g.palette[65] == 0xF800 && shown_g.palette[66] == 0x07E0 && shown_g.palette[97] == 0x001F);
    CHECK(image[0][0] == 65 && image[0][8] == 66 && image[0][16] == 97);                   /* the character's code is the entry */

    /* the colours at the start: the PC's text colours on the hex digits, the rest black, and entry 0 the backdrop */
    run_program(
        "gfx(8); tiledef(1, \"" R64("C") "\"); newmap(8, 8); tile(0, 0, 1)\n"
        "waitframe()\n");
    CHECK(shown_g.palette['0'] == 0 && shown_g.palette['1'] == 0x0015 && shown_g.palette['9'] == 0x52BF && shown_g.palette['C'] == 0xFAAA && shown_g.palette['F'] == 0xFFFF);
    CHECK(shown_g.palette[0] == 0 && shown_g.palette['G'] == 0 && image[0][0] == 'C');

    /* the see-through character is "." at first, and a negative colour makes another one it: the old one is then an ordinary colour */
    run_program(
        "gfx(8); palette(\"@\", -1, -1, -1); palette(\".\", 10, 20, 30)\n"
        "tiledef(1, \"@.@.@.@." R8("@") R8("@") R8("@") R8("@") R8("@") R8("@") "\"); newmap(8, 8); tile(0, 0, 1)\n"
        "waitframe()\n");
    CHECK(image[0][0] == 0 && image[0][1] == '.' && image[0][2] == 0 && shown_g.palette['.'] == (uint16_t)(((10 >> 3) << 11) | ((20 >> 2) << 5) | (30 >> 3)));
    run_program("gfx(8); palette(2, -1, 0, 0)\n");
    CHECK(shows("needs a character"));
    run_program("gfx(8); palette(\"ab\", 1, 2, 3)\n");
    CHECK(shows("one character"));
    run_program("gfx(8); palette(256, 1, 2, 3)\n");
    CHECK(shows("0 to 255"));

    /* 16 pixel tiles, and no map at all: only the actors */
    run_program(
        "gfx(16)\n"
        "for y = 0, 15 do\n"
        "  sprrow(5, y, \"3333333333333333\")\n"
        "end\n"
        "a = actor(5, 10, 20)\n"
        "waitframe()\n");
    CHECK(pixels('3') == 256 && image[20][10] == '3' && image[35][25] == '3' && image[36][10] == 0);

    /* tilerow with row 0 and a string for every pixel of the tile makes the whole tile, row by row; anything shorter, or any other row, is one row */
    run_program(
        "gfx(8); newmap(8, 8); tile(0, 0, 1); tile(1, 0, 2); tile(2, 0, 3); tile(3, 0, 4)\n"
        "tilerow(1, 0, \"" R8("1") R8("2") R8("3") R8("4") R8("5") R8("6") R8("7") R8("8") "\")\n"
        "tilerow(2, 0, \"" R8("9") R8("8") "\")\n"                         /* 16 characters: row 0, and the first 8 of them */
        "tilerow(3, 1, \"" R8("7") R8("7") R8("7") R8("7") R8("7") R8("7") R8("7") R8("7") "\")\n"   /* a whole tile of characters, but at row 1 */
        "waitframe()\n");
    CHECK(image[0][0] == '1' && image[1][7] == '2' && image[2][3] == '3' && image[7][0] == '8' && image[7][7] == '8' && image[4][5] == '5');
    CHECK(image[0][8] == '9' && image[0][15] == '9' && image[1][8] == 0 && image[7][15] == 0);
    CHECK(image[1][16] == '7' && image[1][23] == '7' && image[0][16] == 0 && image[2][16] == 0 && image[7][23] == 0);

    /* a 16 pixel tile in one call, made with string functions */
    run_program(
        "gfx(16)\n"
        "sprrow(1, 0, string.rep(string.rep(\"1\", 8) .. string.rep(\"2\", 8), 16))\n"   /* 16 rows of 16: rows alternating 1 and 2 by 8 */
        "a = actor(1, 0, 0)\n"
        "waitframe()\n");
    CHECK(image[0][0] == '1' && image[0][7] == '1' && image[0][8] == '2' && image[0][15] == '2' && image[15][0] == '1' && image[15][15] == '2' && image[16][0] == 0);
}

static void test_game_tilestr(void)
{
    /* the bytes of the string are tile numbers, width to a row */
    run_program(
        "gfx(8); newmap(8, 8)\n"
        "tilestr(1, 2, 3, \"ABCDE\")\n"
        "print(\"tiles\")\n"
        "for _, p in ipairs({{1, 2}, {3, 2}, {1, 3}, {2, 3}, {3, 3}, {0, 2}, {4, 2}, {1, 4}}) do print(gettile(p[1], p[2])) end\n");
    {
        static const char *const want[] = {"65", "67", "68", "69", "0", "0", "0", "0"};
        CHECK(rows_after("tiles", want, 8));
    }

    /* the offset: a space is tile 0 with -32, and it replaces what was there; flips are not kept */
    run_program(
        "gfx(8); newmap(8, 8); tilefill(0, 0, 8, 1, 9); tile(1, 0, 9, 3)\n"
        "tilestr(0, 0, 4, \" !]\", -32)\n"
        "print(\"tiles\")\n"
        "for x = 0, 3 do print(gettile(x, 0)) end\n");
    {
        static const char *const want[] = {"0", "1", "61", "9"};
        CHECK(rows_after("tiles", want, 4));
    }
    run_program(
        "gfx(8); newmap(8, 8)\n"
        "tilestr(0, 0, 2, \"AB\", 10)\n"
        "print(gettile(0, 0) .. \" \" .. gettile(1, 0))\n"
        "tilestr(0, 0, 2, \"\")\n"
        "print(gettile(0, 0))\n");
    CHECK(printed("75 76") && printed("75"));                  /* (an empty string changes nothing) */
    /* a Lua string can hold a zero byte: tile 0 without an offset */
    run_program("gfx(8); newmap(4, 4); tilefill(0, 0, 4, 4, 9); tilestr(0, 0, 4, \"\\0\\1\\0\")\nprint(gettile(0, 0) .. gettile(1, 0) .. gettile(2, 0) .. gettile(3, 0))\n");
    CHECK(printed("0109"));

    /* off the edge of the map is the map's business, as with tile */
    run_program("gfx(8); newmap(4, 4); tilestr(2, 2, 4, \"AAAAAAAAAA\"); print(gettile(3, 3))\n");
    CHECK(printed("65"));

    /* mistakes */
    run_program("gfx(8); newmap(4, 4); tilestr(0, 0, 0, \"A\")\n");
    CHECK(shows("the width is 1 or more"));
    run_program("gfx(8); newmap(4, 4); tilestr(0, 0, 2, \"AB\", 190)\n");
    CHECK(shows("character 2 is tile 256") && shows("tiles are 0 to 255"));
    run_program("gfx(8); newmap(4, 4); tilestr(0, 0, 2, \"AB\", -66)\n");
    CHECK(shows("character 1 is tile -1"));
    run_program("tilestr(0, 0, 2, \"AB\")\n");
    CHECK(shows("there is no map"));
}

static void test_game_textmode(void)
{
    /* textmode() puts the text screen up at once; the graphics are kept, and the next waitframe shows the picture again */
    frame_text_kept = frame_while_game = 0;
    run_program(
        "gfx(8); newmap(4, 4); tile(1, 1, 7); waitframe()\n"
        "textmode()\n"
        "print(gettile(1, 1))\n"
        "print(55)\n");
    CHECK(printed("7") && printed("55") && printed("Ready.") && frame_text_kept >= 1 && frame_while_game == 0 && presents == 1);
    frame_text_kept = 0;
    run_program(
        "gfx(8); newmap(4, 4); tile(1, 1, 7); waitframe()\n"
        "textmode()\n"
        "textmode()\n"
        "waitframe()\n"
        "print(gettile(1, 1))\n");
    CHECK(printed("7") && frame_text_kept >= 1 && presents == 2);
    CHECK(!env.game_active && freed_by_new());
    /* with no picture up it does nothing, and does not make the graphics */
    frame_text_kept = 0;
    run_program("textmode()\nprint(8)\n");
    CHECK(printed("8") && env.game == NULL && frame_text_kept == 0);
}

static void test_game_actors(void)
{
    /* velocity is per frame that went by: 3 frames at a wait, and no more than 4 */
    run_jump = 3;
    run_program(
        "gfx(8); a = actor(1, 10, 20); actorvel(a, 2, 0.5)\n"
        "print(\"v\")\n"
        "print(waitframe())\n"                  /* the first one has no frame before it to count from: 1 */
        "print(waitframe())\n"
        "print(actorx(a))\n"
        "print(actory(a))\n"
        "print(frames())\n"
        "print(actorvel(a))\n"
        "print(actorpos(a))\n");
    {
        static const char *const want[] = {"1", "3", "18", "22", "106", "2       0.5", "18      22"};
        CHECK(rows_after("v", want, 7));
    }
    run_jump = 9;
    run_program("gfx(8); a = actor(1, 10, 20); actorvel(a, 2, 0)\nwaitframe()\nn = waitframe()\nprint(n)\nprint(actorx(a))\n");
    CHECK(printed("4") && printed("20"));                                                 /* 9 frames went by: no more than 4 are counted */
    run_jump = 1;
    /* a fraction of a pixel stays a fraction */
    run_program("gfx(8); a = actor(1, 10.25, 0)\nprint(actorx(a))\nprint(math.type(actorx(a)) .. \" \" .. math.type(actory(a)))\n");
    CHECK(printed("10.25") && printed("float integer"));

    /* several tiles, flips: the grid is read row by row and mirrored */
    run_program(
        "gfx(8); sprdef(1, \"" R64("1") "\"); sprdef(2, \"" R64("2") "\"); sprdef(3, \"" R64("3") "\"); sprdef(4, \"" R64("4") "\")\n"
        "a = actor(1, 20, 30); actorsize(a, 2, 2)\n"
        "waitframe()\n");
    CHECK(image[30][20] == '1' && image[30][28] == '2' && image[38][20] == '3' && image[38][28] == '4' && image[46][36] == 0);
    run_program(
        "gfx(8); sprdef(1, \"" R64("1") "\"); sprdef(2, \"" R64("2") "\"); sprdef(3, \"" R64("3") "\"); sprdef(4, \"" R64("4") "\")\n"
        "a = actor(1, 20, 30); actorsize(a, 2, 2); actorflip(a, 1)\n"
        "waitframe()\n");
    CHECK(image[30][20] == '2' && image[30][28] == '1' && image[38][20] == '4' && image[38][28] == '3');

    /* priority, hiding, and freeing: the front actor is the lower priority number */
    run_program(
        "gfx(8); sprdef(1, \"" R64("1") "\"); sprdef(2, \"" R64("2") "\")\n"
        "a = actor(1, 10, 10); b = actor(2, 14, 10); actorprio(a, 5); actorprio(b, 9)\n"
        "waitframe()\n");
    CHECK(image[10][10] == '1' && image[10][18] == '2' && image[10][14] == '1' && image[10][16] == '1');           /* a is in front where they overlap */
    run_program(
        "gfx(8); sprdef(1, \"" R64("1") "\"); sprdef(2, \"" R64("2") "\")\n"
        "a = actor(1, 10, 10); b = actor(2, 14, 10); actorprio(a, 9); actorprio(b, 5)\n"
        "waitframe()\n");
    CHECK(image[10][14] == '2');
    run_program(
        "gfx(8); sprdef(1, \"" R64("1") "\")\n"
        "a = actor(1, 10, 10); b = actor(1, 40, 10); actorhide(a, true); actorfree(b); c = actor(1, 70, 10)\n"
        "waitframe()\n"
        "print(\"c=\" .. c)\n");
    CHECK(pixels('1') == 64 && image[10][70] == '1' && image[10][10] == 0 && image[10][40] == 0);
    CHECK(printed("c=1"));                                                               /* b's number was used again by c */

    /* only 128 actors: then actor() gives nil */
    run_program(
        "gfx(8)\n"
        "local n = 0\n"
        "for i = 1, 130 do\n"
        "  if actor(1, i, 0) then n = n + 1 end\n"
        "end\n"
        "print(\"n=\" .. n .. \" \" .. tostring(actor(1, 0, 0)))\n");
    CHECK(printed("n=128 nil"));

    /* what the program keeps in an actor */
    run_program("gfx(8); a = actor(1, 0, 0); print(actoruser(a)); actoruser(a, 4321); print(actoruser(a))\n");
    CHECK(printed("0") && printed("4321"));
}

static void test_game_anim(void)
{
    /* a looping table: 5 for 2 frames, 6 for 2, 7 for 2; waitframe() moves it on by the frames that went by */
    run_program(
        "gfx(8)\n"
        "print(\"anim\")\n"
        "print(animdef(1, \"loop\", 2, \"5 6 7\"))\n"
        "a = actor(1, 0, 0)\n"
        "print(actoranim(a))\n"
        "print(actoranim(a, 1))\n"
        "print(actortile(a))\n"
        "waitframe()\n"
        "print(actortile(a))\n"
        "waitframe()\n"
        "print(actortile(a))\n"
        "waitframe()\n"
        "print(actortile(a))\n"
        "waitframe()\n"
        "print(actortile(a))\n"
        "print(animframe(a))\n"
        "print(actoranim(a, 1))\n"                 /* already playing: not restarted */
        "print(actortile(a))\n"
        "print(actoranim(a, 1, true))\n"           /* restart */
        "print(actortile(a))\n"
        "print(actoranim(a, 0))\n"
        "waitframe()\n"
        "waitframe()\n"
        "print(actortile(a))\n");
    {
        static const char *const want[] = {"3", "0", "1", "5", "5", "6", "6", "7", "2", "1", "7", "1", "5", "0", "5"};
        CHECK(rows_after("anim", want, 15));
    }

    /* a table of one-shot frames with their own times, a ping-pong one given as a Lua table, the finished test */
    run_program(
        "gfx(8)\n"
        "print(\"once\")\n"
        "animdef(2, \"once\", 1, \"1:2 2 3:3\")\n"
        "animdef(3, 2, 1, {10, 11, 12})\n"
        "a = actor(1, 0, 0); b = actor(1, 0, 0)\n"
        "actoranim(a, 2); actoranim(b, 3)\n"
        "for i = 1, 7 do\n"
        "  waitframe()\n"
        "  print(actortile(a) .. \" \" .. tostring(animdone(a)) .. \" \" .. actortile(b) .. \" \" .. animframe(b))\n"
        "end\n");
    {
        /* after 1, 2 ... 7 frames. a: 1 for 2 frames, 2 for 1, 3 for 3 (done at 6). b: 10 11 12 11 10 ..., one frame each */
        static const char *const want[] = {"1 false 11 1", "2 false 12 2", "3 false 11 1", "3 false 10 0", "3 false 11 1", "3 true 12 2", "3 true 11 1"};
        CHECK(rows_after("once", want, 7));
    }
    /* a table with times of its own for some frames */
    run_program("gfx(8)\nprint(animdef(4, \"once\", 2, {7, {8, 5}, 9}))\na = actor(1, 0, 0); actoranim(a, 4)\nfor i = 1, 3 do waitframe() end\nprint(\"t=\" .. actortile(a))\n");
    CHECK(printed("3") && printed("t=8"));
    static const char *const bad[] = {
        "gfx(8); animdef(0, 0, 1, \"1\")\n", "gfx(8); animdef(1, \"spin\", 1, \"1\")\n", "gfx(8); animdef(1, 0, 1, \"1 x\")\n", "gfx(8); animdef(1, 0, 1, \"1:0\")\n",
        "gfx(8); animdef(1, 0, 1, \"300\")\n", "gfx(8); animdef(1, 0, 1, {1, \"two\"})\n", "gfx(8); animdef(1, 0, 1, {{1, 0}})\n",
        "gfx(8); a = actor(1, 0, 0); actoranim(a, 33)\n", "gfx(8); print(animdone(5))\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        run_program(bad[i]);
        CHECK(shows("Error: t.lua:1:"));
    }
}

static void test_game_world(void)
{
    /* collisions: flags, and an actor that stops flush against a wall */
    run_program(
        "gfx(8); newmap(30, 20); settileflags(1, 1)\n"
        "tilefill(6, 0, 1, 10, 1)\n"
        "a = actor(1, 10, 10)\n"
        "print(\"w\")\n"
        "print(actormove(a, 100, 0))\n"
        "print(actorx(a))\n"
        "print(actormove(a, 0, 100))\n"
        "print(actory(a))\n"
        "print(tileflag(50, 4))\n"
        "print(tileflag(10, 4))\n"
        "print(tilebox(0, 0, 60, 10))\n"
        "print(actorhit(a, a))\n");
    {
        static const char *const want[] = {"2", "40", "0", "110", "1", "0", "1", "true"};
        CHECK(rows_after("w", want, 8));
    }
    /* flag 2 is one-way: solid from above only, so an actor goes up through a row of it and stops on it going down; flag 3 (both) is solid from every side */
    run_program(
        "gfx(8); newmap(30, 20); settileflags(1, 2); settileflags(2, 3); tilefill(0, 10, 30, 1, 1); tile(20, 10, 2)\n"
        "a = actor(1, 10, 30)\n"
        "print(\"w\")\n"
        "print(actormove(a, 0, 100))\n"
        "print(actory(a))\n"
        "print(actormove(a, 0, -100))\n"
        "print(actory(a))\n"
        "b = actor(1, 160, 100)\n"
        "print(actormove(b, 0, -100))\n"
        "print(actory(b))\n"
        "print(tileflag(10, 82))\n"
        "print(tileflag(160, 82))\n"
        "print(tilebox(0, 82, 200, 1))\n");
    {
        static const char *const want[] = {"8", "72", "0", "-28", "4", "88", "2", "3", "3"};   /* down: stopped on it at 72; up: through it; the solid one stops it from below, flush */
        CHECK(rows_after("w", want, 9));
    }

    /* the tile that stopped an actor is recorded as it was then: where, its number and its flags, per side; a later change of the map does not touch it */
    run_program(
        "gfx(8); newmap(30, 20); settileflags(7, 1); settileflags(8, 2); tile(6, 2, 7); tile(2, 8, 8)\n"
        "a = actor(1, 10, 16); b = actor(1, 16, 30)\n"
        "actormove(a, 100, 0)\n"
        "print(\"w\")\n"
        "print(hitx(a, \"right\"))\n"
        "print(hity(a, 2))\n"
        "print(hittile(a, \"RIGHT\"))\n"
        "print(hitflags(a, 2))\n"
        "print(hitx(a, \"left\"))\n"
        "tile(6, 2, 0)\n"
        "print(hittile(a, 2))\n"
        "print(gettile(6, 2))\n"
        "actormove(a, -5, 0)\n"
        "print(hitx(a, 2))\n"
        "actormove(b, 0, 100)\n"
        "print(hitx(b, 8))\n"
        "print(hity(b, \"down\"))\n"
        "print(hittile(b, 8))\n"
        "print(hitflags(b, 8))\n");
    {
        static const char *const want[] = {"6", "2", "7", "1", "0", "7", "0", "6", "2", "8", "8", "2"};
        CHECK(rows_after("w", want, 12));
    }
    /* a trigger tile (flag 4) stops nothing: actormove says it was touched (16), the record is the tile as it was, and lastx/lasty are the position before */
    run_program(
        "gfx(8); newmap(30, 20); settileflags(9, 4); tile(6, 2, 9)\n"
        "a = actor(1, 10, 16); b = actor(1, 100, 100); actorvel(b, 2, 0)\n"
        "actormove(a, 30, 0)\n"
        "print(\"w\")\n"
        "print(actormove(a, 4, 0))\n"
        "print(hitx(a, \"touch\"))\n"
        "print(hity(a, 16))\n"
        "print(hittile(a, \"touch\"))\n"
        "print(hitflags(a, 16))\n"
        "print(lastx(a))\n"
        "print(lasty(a))\n"
        "actorpos(a, lastx(a), lasty(a))\n"
        "print(actorx(a))\n"
        "print(lastx(a))\n"
        "print(actormove(a, 40, 0))\n"
        "print(actorx(a))\n"
        "tile(6, 2, 0)\n"
        "print(hittile(a, 16))\n"
        "print(gettile(6, 2))\n"
        "waitframe()\n"
        "print(lastx(b))\n"
        "print(actorx(b))\n");
    {
        static const char *const want[] = {"16", "6", "2", "9", "4", "40", "16", "40", "44", "16", "80", "9", "0", "100", "102"};
        CHECK(rows_after("w", want, 15));
    }
    run_program("gfx(8); a = actor(1, 0, 0); print(hitx(a, 3))\n");
    CHECK(shows("the side is 1"));
    run_program("gfx(8); a = actor(1, 0, 0); print(hittile(a, \"over\"))\n");
    CHECK(shows("the side is 1"));
    run_program("gfx(8); print(hitflags(5, 2))\n");
    CHECK(shows("there is no actor 5"));

    /* a box of its own inside the picture */
    run_program(
        "gfx(8); newmap(30, 20); settileflags(1, 1); tilefill(6, 0, 1, 10, 1)\n"
        "a = actor(1, 10, 10); actorbox(a, 2, 0, 4, 8)\n"
        "actormove(a, 100, 0)\n"
        "print(actorx(a))\n");
    CHECK(printed("42"));                                                   /* the box is x + 2 to x + 6, and stops at 48 */

    /* infinite scrolling: the map repeats, the camera is kept within one map, the actors wrap */
    run_program(
        "gfx(8); palette(\"4\", 255, 0, 0); tiledef(1, \"" R64("4") "\"); sprdef(1, \"" R64("7") "\"); newmap(64, 32); tile(0, 0, 1)\n"
        "print(\"w\")\n"
        "print(wrap(true))\n"
        "camera(512, 256)\n"
        "a = actor(1, 600, 300)\n"
        "waitframe()\n"
        "print(camx())\n"
        "print(camy())\n"
        "camera(1000000, 5000)\n"
        "print(camx())\n"
        "print(camy())\n"
        "b = actor(1, 510, 5); actorvel(b, 4, 0)\n"
        "waitframe()\n"
        "print(actorx(b))\n");
    {
        static const char *const want[] = {"true", "0", "0", "64", "136", "2"};
        CHECK(rows_after("w", want, 6));
    }
    run_program(
        "gfx(8); palette(\"4\", 255, 0, 0); tiledef(1, \"" R64("4") "\"); sprdef(1, \"" R64("7") "\"); newmap(64, 32); tile(0, 0, 1); wrap(1)\n"
        "camera(512, 256)\n"
        "a = actor(1, 600, 300)\n"
        "waitframe()\n");
    CHECK(image[0][0] == '4' && image[7][7] == '4');                             /* the tile at the origin again after one map */
    CHECK(image[44][88] == '7' && image[51][95] == '7');                         /* an actor 88, 44 from the camera, whatever the lap */
    run_program("gfx(8); newmap(10, 10); print(wrap(1))\n");             /* 80 x 80 pixels: too small for the actors to wrap */
    CHECK(printed("false"));
}

static void test_game_input_and_errors(void)
{
    run_held = PLAT_BTN_LEFT;
    run_down = PLAT_BTN_START;
    run_program(
        "print(\"b\")\n"
        "print(btn(0, \"left\"))\n"
        "print(btn(0, 4))\n"
        "print(btn(0, \"a\"))\n"
        "print(btn(0, 4 + 16))\n"
        "print(btn(1, \"left\"))\n"
        "print(pressed(0, \"START\"))\n"
        "print(pressed(0, 256))\n"
        "if btn(0, \"left\") then print(\"held\") end\n");
    {
        static const char *const want[] = {"true", "true", "false", "true", "false", "true", "true", "held"};
        CHECK(rows_after("b", want, 8));
    }
    run_held = run_down = 0;
    CHECK(env.game == NULL && presents == 0);                               /* these did not need the graphics */
    run_program("print(btn(0, \"jump\"))\n");
    CHECK(shows("btn(player, button): a number, or"));
    run_program("print(btn(2, 1))\n");
    CHECK(shows("the player is 0 to 1"));

    /* mistakes are reported with what to do and where, and the program stops */
    run_program("gfx(8)\na = actor(1, 0, 0)\nprint(actorx(7))\nprint(99)\n");
    CHECK(shows("Error: t.lua:3: actorx: there is no actor 7") && !printed("99"));
    CHECK(!env.game_active && freed_by_new());
    /* an error after a picture was shown: no waiting for a key, the text screen with the error is there at once */
    run_program("gfx(8)\nwaitframe()\nprint(actorx(7))\n");
    CHECK(shows("there is no actor 7") && !env.game_active);
    run_program("tile(0, 0, 1)\n");
    CHECK(shows("there is no map"));
    run_program("gfx(7)\n");
    CHECK(shows("the size of a tile is 8 or 16"));
    run_program("gfx(8); tiledef(300, \"1\")\n");
    CHECK(shows("n is 0 to 255"));
    run_program("gfx(8); print(actor(1, 0, 0)); actorsize(0, 5, 1)\n");
    CHECK(shows("1 to 4 of each"));
    run_program("gfx(8); newmap(0, 5)\n");
    CHECK(shows("at most"));
    run_program("gfx(8); newmap(4, 4); tile(\"x\", 0, 1)\n");
    CHECK(shows("bad argument #1 to 'tile' (number expected, got string)"));
    /* a number with a fraction where a whole one is wanted is rounded down (a pixel position divided by 8 is a tile position) */
    run_program("gfx(8); newmap(8, 8); tile(17 / 8, 2.9, 5); print(gettile(2, 2))\n");
    CHECK(printed("5"));

    /* without a tile screen the graphics functions still work, and waitframe says so */
    game_hooks_on = 0;
    run_program("gfx(8); newmap(4, 4); tile(1, 1, 2); print(gettile(1, 1)); waitframe()\nprint(99)\n");
    CHECK(printed("2") && shows("no tile screen") && !printed("99"));
    game_hooks_on = 1;

    /* Ctrl+C stops a program that loops over waitframe, and the text screen is back */
    fresh();
    CHECK(put_file("/sd/t.lua", "gfx(8)\nwhile true do\n  waitframe()\nend\n") > 0);
    type("LOAD t\nRUN\n^C");
    type("6 * 7\n");
    CHECK(go() == -1);
    CHECK(shows("Break") && row_of("42") >= 0 && !env.game_active && freed_by_new());

    /* the picture goes away when the program ends, with no key: the run is over and the text screen is back (nothing is presented after the last waitframe) */
    fresh();
    CHECK(put_file("/sd/t.lua", "gfx(8); waitframe()\n") > 0);
    type("LOAD t\nRUN\n");
    CHECK(go() == -1);
    CHECK(presents == 1 && !env.game_active && shows("Ready."));
    tl_env_close(&env);
    CHECK(allocs == releases && env.game == NULL && lua_bytes == 0);   /* everything given back, Lua's memory too */
    /* a long computation with the picture up: the poll hook keeps the machine going (on the board the hang monitor would reset it otherwise) */
    run_program("gfx(8); waitframe()\nlocal s = 0\nfor i = 1, 2000000 do s = s + i end\nprint(\"s=\" .. s)\n");
    CHECK(printed("s=") && polls > 0);
}

static char *read_example(const char *name)
{
    static char text[65536];
    char path[64];
    snprintf(path, sizeof path, "lua/examples/%s", name);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    const size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    CHECK(n > 200 && n < sizeof text - 200);
    return text;
}

/* The example program: a ball that walks around a room and stops at the walls. */
static void test_game_example(void)
{
    const char *text = read_example("walk.lua");
    /* left for 21 frames: 2 pixels a frame; the camera follows, so the ball stays in the middle of the screen */
    fresh();
    frame_jump = 1;
    held = PLAT_BTN_LEFT;
    press_b_after = 21;
    frame_while_game = 0;
    CHECK(put_file("/sd/walk.lua", text) > 0);
    type("LOAD walk\nRUN\n");
    CHECK(go() == -1);
    CHECK(printed("118") && printed("Ready."));                       /* 160 - 21 * 2: B was down after the 21st wait */
    CHECK(pixels('4') == 52 && image[123][160] == '4' && image[120][160] == '2' && image[120][162] == '4');   /* the ball: 8 x 8 without its corners (the floor shows there), in the middle */
    CHECK(image[100][10] == 0 && image[100][100] == '2' && image[100][44] == '1');   /* left of the map is the backdrop (the camera is at -42), then its wall, then the floor */
    /* left for ever: it stops at the wall (the floor starts at x = 8) */
    fresh();
    frame_jump = 1;
    held = PLAT_BTN_LEFT | PLAT_BTN_UP;
    press_b_after = 150;
    CHECK(put_file("/sd/walk.lua", text) > 0);
    type("LOAD walk\nRUN\n");
    CHECK(go() == -1);
    {
        const int r = row_of("8");
        CHECK(r >= 0 && !strcmp(row(r + 1), "8"));
    }
    CHECK(frame_while_game == 0);                                     /* the running program never put the text screen up over the game */
}

/* examples/walk2.lua: walk.lua with the frame loop in run(); B ends it */
static void test_game_example2(void)
{
    static char text[8192];
    snprintf(text, sizeof text, "%s\nprint(\"cam=\" .. camx())\n", read_example("walk2.lua"));   /* the room is bigger than the screen, so the camera has to have moved */
    seed_count = 0;
    run_held = PLAT_BTN_RIGHT | PLAT_BTN_B;
    run_program(text);
    run_held = 0;
    CHECK(printed("Ready.") && !shows("Error"));
    CHECK(presents >= 1);
    CHECK(printed("cam=") && !printed("cam=0"));
}

/* examples/anim.lua: the walk cycle follows the arrow keys */
static void test_game_example_anim(void)
{
    static char text[8192];
    snprintf(text, sizeof text, "%s\nprint(\"anim=\" .. actoranim(p) .. \" x=\" .. actorx(p))\n", read_example("anim.lua"));
    seed_count = 0;
    run_held = PLAT_BTN_RIGHT | PLAT_BTN_B;
    run_program(text);
    run_held = 0;
    CHECK(printed("Ready.") && !shows("Error"));
    CHECK(printed("anim=2 x=161"));
}

/* examples/tiletest.lua: a program that checks the tile flags itself */
static void test_tile_flags_program(void)
{
    seed_count = 0;
    run_program(read_example("tiletest.lua"));
    CHECK(printed("done") && printed("Ready.") && !shows("FAIL"));
    int oks = 0;
    for (int r = 0; r < 25; r++)
        oks += !strncmp(row(r), "ok   ", 5);
    CHECK(oks >= 20);
    type("RUN\n");                                   /* and again: the second run must not find anything left by the first */
    CHECK(go() == -1);
    CHECK(printed("done") && !shows("FAIL"));
}

/* ---- onframe and run: the routine the system calls once a frame ---- */

static void test_game_callbacks(void)
{
    seed_count = 0;
    /* a function: it gets the frames that went by, the actors move between calls, and a result that is not nil or false ends run */
    run_program(
        "gfx(8)\n"
        "newmap(20, 12)\n"
        "a = actor(1, 10, 10)\n"
        "actorvel(a, 1, 0)\n"
        "function update(k)\n"
        "  actoruser(a, actoruser(a) + k)\n"
        "  if actoruser(a) >= 5 then\n"
        "    return 7\n"
        "  end\n"
        "end\n"
        "onframe(update)\n"
        "print(\"run=\" .. run())\n"
        "print(\"x=\" .. actorx(a))\n"
        "print(\"u=\" .. actoruser(a))\n");
    CHECK(printed("run=7") && printed("x=15") && printed("u=5") && printed("Ready."));
    CHECK(presents == 5);
    run_jump = 2;                                   /* the frames that went by arrive in the argument; a name in a string is the function of that name */
    run_program(
        "gfx(8)\n"
        "a = actor(1, 0, 0)\n"
        "function update(k)\n"
        "  actoruser(a, actoruser(a) + k)\n"
        "  return actoruser(a) >= 6\n"
        "end\n"
        "onframe(\"update\")\n"
        "run()\n"
        "print(\"u=\" .. actoruser(a))\n");
    run_jump = 1;
    CHECK(printed("u=7") && printed("Ready."));   /* (the first frame counts as 1, then 2 each: 1 3 5 7) */

    /* the routine changes the program's variables; its locals are its own */
    run_program("gfx(8)\nc = 10\nonframe(function(k)\n  c = c + 1\n  local d = 5\n  return c >= 13\nend)\nrun()\nprint(\"c=\" .. c .. \" d=\" .. tostring(d))\n");
    CHECK(printed("c=13 d=nil") && printed("Ready."));
    /* a closure keeps what it was made with */
    run_program("gfx(8)\nlocal function make(n)\n  return function(k) return n + k end\nend\nonframe(make(3))\nprint(\"r=\" .. run())\nonframe(make(40))\nprint(\"t=\" .. run())\n");
    CHECK(printed("r=4") && printed("t=41") && printed("Ready."));
    /* a coroutine: a game written as a straight loop, moved on one frame at a time by the routine */
    run_program(
        "gfx(8)\n"
        "local game = coroutine.wrap(function()\n"
        "  for i = 1, 3 do coroutine.yield() end\n"
        "  return \"over\"\n"
        "end)\n"
        "onframe(function(k) return game() end)\n"
        "print(\"g=\" .. run())\n");
    CHECK(printed("g=over") && presents == 4);

    /* waitframe inside the loop is an error, and so is a second run or onframe */
    run_program("gfx(8)\nonframe(function(k) waitframe() end)\nrun()\nprint(\"after\")\n");
    CHECK(shows("waitframe cannot be used while run() is looping") && !printed("after") && !printed("Ready."));
    run_program("gfx(8)\nonframe(function(k) run() end)\nrun()\n");
    CHECK(shows("run cannot be used while run is looping") && !printed("Ready."));
    run_program("gfx(8)\nonframe(function(k) onframe(nil) end)\nrun()\n");
    CHECK(shows("onframe cannot be used while run() is looping") && !printed("Ready."));
    run_program("gfx(8)\nonframe(function(k) return true end)\nrun()\nwaitframe()\nprint(\"ok\")\n");   /* after run it is allowed again */
    CHECK(printed("ok") && printed("Ready."));
    /* an error in the routine caught by the program: run can be used again */
    run_program("gfx(8)\nonframe(function(k) error(\"inside\") end)\nprint(pcall(run))\nonframe(function(k) return 1 end)\nprint(\"again=\" .. run())\n");
    CHECK(shows("inside") && printed("again=1") && printed("Ready."));

    /* mistakes: no routine, a name that is not one, a number, a routine forgotten */
    run_program("gfx(8)\nrun()\n");
    CHECK(shows("run(): onframe(routine) says what to call each frame") && !printed("Ready."));
    run_program("gfx(8)\nonframe(\"nothing\")\n");
    CHECK(shows("there is no function called \"nothing\"") && !printed("Ready."));
    run_program("gfx(8)\nonframe(5)\n");
    CHECK(shows("onframe(routine): a function, or the name of one") && !printed("Ready."));
    run_program("gfx(8)\nonframe(function(k) return 1 end)\nonframe(nil)\nrun()\n");
    CHECK(shows("onframe(routine) says what to call each frame") && !printed("Ready."));

    /* an error inside the routine stops the program, once; os.exit inside it ends the program */
    run_program("gfx(8)\nonframe(function(k)\n  actorpos(99, 1, 1)\nend)\nrun()\nprint(\"after\")\n");
    CHECK(shows("Error: t.lua:3: actorpos: there is no actor 99") && !printed("after") && !printed("Ready."));
    run_program("gfx(8)\nonframe(function(k)\n  print(\"in\")\n  os.exit()\nend)\nrun()\nprint(\"after\")\n");
    CHECK(printed("in") && !printed("after") && printed("Ready.") && !shows("Error"));

    /* Ctrl+C ends a loop whose routine never asks to stop; run() can be used again at the prompt */
    fresh();
    CHECK(put_file("/sd/t.lua", "gfx(8)\nonframe(function(k) end)\nrun()\nprint(\"after\")\n") > 0);
    type("LOAD t\nRUN\n^C");
    CHECK(go() == -1 && shows("Break") && !shows("after"));
    type("1\nonframe(function(k) return 5 end)\nrun()\n");
    CHECK(go() == -1 && last_row_of("1") > row_of("Break") && strcmp(row(last_row_of("] run()") + 1), "5") == 0);

    /* a long run that makes a string every frame: Lua's memory must not grow with the number of frames (compared after 500 frames and after 4000) */
    {
        size_t kb[2];
        const int frames[2] = {500, 4000};
        for (int i = 0; i < 2; i++) {
            char prog[600];
            snprintf(prog, sizeof prog,
                     "gfx(8)\n"
                     "a = actor(1, 0, 0)\n"
                     "onframe(function(k)\n"
                     "  local n = actoruser(a) + 1\n"
                     "  actoruser(a, n)\n"
                     "  local s = \"frame \" .. n .. \" of the long run\"\n"
                     "  return n >= %d\n"
                     "end)\n"
                     "run()\n"
                     "collectgarbage()\n"
                     "print(\"u=\" .. actoruser(a))\n",
                     frames[i]);
            run_program(prog);
            char want[24];
            snprintf(want, sizeof want, "u=%d", frames[i]);
            CHECK(printed(want) && printed("Ready."));
            kb[i] = lua_bytes;
        }
        CHECK(kb[0] > 0 && kb[1] < kb[0] + 4 * 1024);   /* 3500 more frames, less than 4 KB more */
    }
}

/* ---- follow: the camera follows an actor ---- */

static void test_game_follow(void)
{
    seed_count = 0;
    /* the middle of the actor in the middle of the screen (320 x 240): an actor of 8 pixels at 400, 300 has its middle at 404, 304 */
    run_program(
        "gfx(8)\n"
        "newmap(100, 80)\n"
        "a = actor(1, 400, 300)\n"
        "follow(a)\n"
        "waitframe()\n"
        "print(\"c1=\" .. camx() .. \",\" .. camy())\n"
        "actorpos(a, 500, 300)\n"
        "waitframe()\n"
        "print(\"c2=\" .. camx() .. \",\" .. camy())\n"
        "actorfree(a)\n"
        "actorpos(0, 0, 0)\n");
    CHECK(printed("c1=244,184") && printed("c2=344,184"));
    CHECK(shows("no actor 0") && !printed("Ready."));                       /* (the actor is gone) */
    run_program(
        "gfx(8)\n"
        "newmap(100, 80)\n"
        "a = actor(1, 400, 300)\n"
        "follow(a)\n"
        "waitframe()\n"
        "actorfree(a)\n"
        "waitframe()\n"
        "waitframe()\n"
        "print(\"c3=\" .. camx() .. \",\" .. camy())\n"
        "camera(10, 20)\n"
        "waitframe()\n"
        "print(\"c4=\" .. camx() .. \",\" .. camy())\n");
    CHECK(printed("c3=244,184") && printed("c4=10,20") && printed("Ready."));   /* freeing the actor ends it, and camera is then the program's */

    /* a dead zone: the camera moves only as far as it must to keep the middle of the actor inside it */
    run_program(
        "gfx(8)\n"
        "newmap(100, 80)\n"
        "a = actor(1, 400, 300)\n"
        "camera(244, 184)\n"
        "follow(a, 100, 60)\n"
        "local function at(x, y, name)\n"
        "  actorpos(a, x, y)\n"
        "  waitframe()\n"
        "  print(name .. \"=\" .. camx() .. \",\" .. camy())\n"
        "end\n"
        "at(400, 300, \"d1\")\n"
        "at(436, 300, \"d2\")\n"
        "at(466, 300, \"d3\")\n"
        "at(466, 360, \"d4\")\n"
        "at(100, 360, \"d5\")\n");
    CHECK(printed("d1=244,184") && printed("d2=244,184") && printed("d3=260,184") && printed("d4=260,214") && printed("d5=0,214"));

    /* the edges of a map that does not repeat; followlimit(false) lets it go past them */
    run_program(
        "gfx(8)\n"
        "newmap(100, 80)\n"
        "a = actor(1, 5, 5)\n"
        "follow(a)\n"
        "waitframe()\n"
        "print(\"e1=\" .. camx() .. \",\" .. camy())\n"
        "actorpos(a, 790, 630)\n"
        "waitframe()\n"
        "print(\"e2=\" .. camx() .. \",\" .. camy())\n"
        "followlimit(false)\n"
        "actorpos(a, 5, 5)\n"
        "waitframe()\n"
        "print(\"e3=\" .. camx() .. \",\" .. camy())\n"
        "newmap(30, 20)\n"
        "followlimit(true)\n"
        "waitframe()\n"
        "print(\"e4=\" .. camx() .. \",\" .. camy())\n");
    CHECK(printed("e1=0,0") && printed("e2=480,400") && printed("e3=-151,-111") && printed("e4=-40,-40"));   /* (a map of 240 x 160 pixels is smaller than the screen both ways: it is centred) */

    /* smooth: a fraction of the way each frame, and all of it in the end */
    run_program(
        "gfx(8)\n"
        "newmap(100, 80)\n"
        "a = actor(1, 400, 300)\n"
        "camera(0, 0)\n"
        "follow(a, 0, 0, 3)\n"
        "waitframe()\n"
        "print(\"s1=\" .. camx() .. \",\" .. camy())\n"
        "for i = 1, 80 do\n"
        "  waitframe()\n"
        "end\n"
        "print(\"s2=\" .. camx() .. \",\" .. camy())\n");
    CHECK(printed("s1=61,46") && printed("s2=244,184") && printed("Ready."));

    /* a map that repeats: the short way round the seam (512 pixels across) */
    run_program(
        "gfx(8); newmap(64, 32); wrap(true)\n"
        "a = actor(1, 500, 100)\n"
        "follow(a)\n"
        "waitframe()\n"
        "print(\"w1=\" .. camx() .. \",\" .. camy())\n"
        "actorpos(a, 8, 100)\n"
        "waitframe()\n"
        "print(\"w2=\" .. camx() .. \",\" .. camy())\n"
        "follow(a, 0, 0, 3)\n"
        "actorpos(a, 500, 100)\n"
        "waitframe()\n"
        "waitframe()\n"
        "actorpos(a, 8, 100)\n"
        "camera(344, 240)\n"
        "waitframe()\n"
        "print(\"w3=\" .. camx() .. \",\" .. camy())\n");
    CHECK(printed("w1=344,240") && printed("w2=364,240") && printed("w3=349,240") && printed("Ready."));

    /* in run() too: the routine moves the actor, the camera follows */
    run_program(
        "gfx(8)\n"
        "newmap(100, 80)\n"
        "a = actor(1, 400, 300)\n"
        "follow(a)\n"
        "onframe(function(k)\n"
        "  actorpos(a, actorx(a) + 10, 300)\n"
        "  return actorx(a) >= 450\n"
        "end)\n"
        "run()\n"
        "print(\"r=\" .. camx() .. \",\" .. camy())\n");
    CHECK(printed("r=284,184") && printed("Ready."));   /* (the camera is set at the start of each frame, before the routine moves the actor: it is where the actor was at 440) */

    /* mistakes */
    run_program("gfx(8)\nfollow(5)\n");
    CHECK(shows("there is no actor 5") && !printed("Ready."));
    run_program("gfx(8)\na = actor(1, 0, 0)\nfollow(a, -1, 0)\n");
    CHECK(shows("width 0-320, height 0-240") && !printed("Ready."));
    run_program("gfx(8)\na = actor(1, 0, 0)\nfollow(a, 10, 10, 300)\n");
    CHECK(shows("smooth 0-255") && !printed("Ready."));
    run_program("gfx(8)\nfollow(-1)\nfollow(nil)\nprint(\"ok\")\n");
    CHECK(printed("ok") && printed("Ready."));
}

/* ---- files: io and os on tilefs, require, dofile, loadfile ---- */

static void test_file_functions(void)
{
    seed_count = 0;
    run_program(
        "local f = assert(io.open(\"data.txt\", \"w\"))\n"
        "print(\"t1=\" .. io.type(f))\n"
        "assert(f:write(\"line one\\n\", \"line two\\n\", 42, \" \", 3.5, \"\\n\") == f)\n"
        "f:close()\n"
        "print(\"t2=\" .. io.type(f) .. \" \" .. tostring(io.type(42)))\n"
        "for l in io.lines(\"data.txt\") do print(\"[\" .. l .. \"]\") end\n"
        "local g = io.open(\"data.txt\")\n"
        "print(\"r1=\" .. g:read(\"l\"))\n"
        "print(\"r2=\" .. tostring(g:read(\"L\") == \"line two\\n\"))\n"
        "local n1, n2 = g:read(\"n\", \"n\")\n"
        "print(\"r3=\" .. n1 .. \" \" .. math.type(n1) .. \" \" .. n2)\n"
        "print(\"r4=\" .. tostring(g:read(\"a\") == \"\\n\") .. \" \" .. tostring(g:read(\"l\")) .. \" \" .. tostring(g:read(0)))\n"
        "print(\"s1=\" .. g:seek(\"set\", 5) .. \" \" .. g:read(3))\n"
        "print(\"s2=\" .. g:seek(\"end\") .. \" \" .. g:seek(\"cur\", -4) .. \" \" .. g:read(\"l\"))\n"
        "g:close()\n"
        "print(\"c=\" .. tostring(select(2, pcall(g.read, g))))\n"
        "local a = io.open(\"data.txt\", \"a\")\n"
        "a:write(\"more\\n\")\n"
        "a:close()\n"
        "local h = io.open(\"data.txt\", \"r+\")\n"
        "h:seek(\"set\", 0)\n"
        "h:write(\"LINE\")\n"
        "h:seek(\"set\", 0)\n"
        "local all = h:read(\"a\")\n"
        "h:close()\n"
        "print(\"len=\" .. #all .. \" \" .. all:sub(1, 8))\n"
        "local count = 0\n"
        "local lf = io.open(\"data.txt\")\n"
        "for a, b in lf:lines(1, 2) do count = count + 1 end\n"
        "lf:close()\n"                                   /* (tilefs does not rename a file that is open) */
        "print(\"pairs=\" .. count)\n"
        "print(\"no=\" .. tostring(io.open(\"nothere.txt\")) .. \" \" .. select(2, io.open(\"nothere.txt\")))\n"
        "print(\"mv=\" .. tostring(os.rename(\"data.txt\", \"d2.txt\")) .. \" \" .. tostring(os.remove(\"d2.txt\")) .. \" \" .. tostring(os.remove(\"d2.txt\")))\n"
        "print(\"bad=\" .. tostring(pcall(io.open, \"x\", \"q\")))\n");
    {
        static const char *const want[] = {"[line one]", "[line two]", "[42 3.5]", "r1=line one", "r2=true", "r3=42 integer 3.5", "r4=true nil nil",
                                           "s1=5 one", "s2=25 21 3.5", "c=attempt to use a closed file", "len=30 LINE one", "pairs=10",
                                           "no=nil nothere.txt: no such file or directory", "mv=true true nil", "bad=false"};
        CHECK(row_of("t1=file") >= 0 && row_of("t2=closed file nil") >= 0);
        CHECK(rows_after("t2=", want, 15));
    }
    /* what a folder holds; making and listing */
    seed_file("/sd/lib/deep.lua", "return {name = \"deep\"}\n");
    seed_file("/sd/mylib.lua", "loads = (loads or 0) + 1\nlocal M = {}\nfunction M.twice(x) return 2 * x end\nreturn M\n");
    seed_file("/sd/other.lua", "local a, b = ...\nreturn 5, 6, a\n");
    run_program(
        "local d = os.dir()\n"
        "local names = {}\n"
        "for _, e in ipairs(d) do names[#names + 1] = e.name .. (e.dir and \"/\" or \"\") end\n"
        "table.sort(names)\n"
        "print(\"dir=\" .. table.concat(names, \",\"))\n"
        "print(\"nodir=\" .. tostring(os.dir(\"nowhere\")))\n"
        "local m = require(\"mylib\")\n"
        "print(\"req=\" .. m.twice(21) .. \" \" .. tostring(require(\"mylib\") == m) .. \" \" .. loads .. \" \" .. tostring(package.loaded.mylib == m))\n"
        "print(\"deep=\" .. require(\"lib.deep\").name)\n"
        "print(\"miss=\" .. tostring(select(2, pcall(require, \"nope\"))))\n"
        "print(\"do=\" .. table.concat({dofile(\"other.lua\")}, \",\"))\n"
        "local f = loadfile(\"other.lua\")\n"
        "print(\"lf=\" .. table.concat({f(\"x\")}, \",\") .. \" \" .. tostring(loadfile(\"nope.lua\")))\n"
        "print(\"clock=\" .. math.type(os.clock()) .. \" \" .. math.type(os.time()))\n");
    CHECK(printed("dir=lib/,mylib.lua,other.lua,t.lua"));
    CHECK(printed("nodir=nil") && printed("req=42 true 1 true") && printed("deep=deep") && printed("do=5,6") && printed("lf=5,6,x nil") && printed("clock=float integer"));
    CHECK(shows("miss=module 'nope' not found: cannot open nope.lua"));
    seed_count = 0;
    /* a program cannot reach the host or run other programs: these are not there */
    run_program("print(\"gone=\" .. tostring(os.execute) .. tostring(io.popen) .. tostring(package.loadlib) .. tostring(debug) .. tostring(os.getenv))\n");
    CHECK(printed("gone=nilnilnilnilnil"));
    /* the libraries that are there */
    run_program("print(\"libs=\" .. type(string.format) .. type(table.sort) .. type(math.floor) .. type(utf8.char) .. type(coroutine.wrap))\nprint(utf8.char(72, 105))\n");
    CHECK(printed("libs=functionfunctionfunctionfunctionfunction") && printed("Hi"));
    /* files a program left open are closed when NEW (or the next RUN) starts afresh */
    run_program("f = io.open(\"keep.txt\", \"w\")\nf:write(\"x\")\n");
    CHECK(fs_unmount("sd") == FS_EBUSY);
    type("NEW\n");
    CHECK(go() == -1 && fs_unmount("sd") == 0 && fs_mount("sd", &fs_ram_ops, &disk) == 0);
}

/* ---- more than one tile layer ---- */

static void test_game_layers(void)
{
    seed_count = 0;
    /* two layers: 1 at the back, 2 in front; a see-through pixel of layer 2 shows layer 1; the map functions work on the layer layer() chose */
    run_program(
        "gfx(8)\n"
        "tiledef(1, string.rep(\"1\", 64))\n"
        "tiledef(2, string.rep(\"....2222\", 8))\n"
        "newmap(8, 8)\n"
        "tile(0, 0, 1)\n"
        "print(\"l\")\n"
        "print(newmap(8, 8, 2))\n"
        "print(layer(2))\n"
        "tile(0, 0, 2)\n"
        "print(layer())\n"
        "print(gettile(0, 0))\n"
        "layer(1)\n"
        "print(gettile(0, 0))\n"
        "waitframe()\n");
    {
        static const char *const want[] = {"2", "1", "2", "2", "1"};
        CHECK(rows_after("l", want, 5));
    }
    CHECK(image[0][0] == '1' && image[0][3] == '1' && image[0][4] == '2' && image[7][7] == '2' && image[0][8] == 0);
    CHECK(shown_g.layers[0].kind == TG_LAYER_TILES && shown_g.layers[1].kind == TG_LAYER_TILES && shown_g.layers[2].kind == TG_LAYER_OFF);

    /* parallax: layer 2 at half the speed of the main camera; then not moving at all, with an offset */
    run_program(
        "gfx(8); tiledef(1, string.rep(\"1\", 64)); tiledef(2, string.rep(\"2\", 64))\n"
        "newmap(64, 32); tile(10, 0, 1)\n"
        "newmap(64, 32, 2); layer(2); tile(10, 1, 2)\n"
        "parallax(2, 0.5)\n"
        "camera(40, 0)\n"
        "print(\"c=\" .. camx())\n"
        "waitframe()\n");
    CHECK(printed("c=40"));
    CHECK(image[0][40] == '1' && image[0][47] == '1' && image[0][39] == 0 && image[0][48] == 0);   /* layer 1: the tile at x 80, camera 40 */
    CHECK(image[8][60] == '2' && image[8][67] == '2' && image[8][59] == 0 && image[8][68] == 0);   /* layer 2: camera 20 */
    run_program(
        "gfx(8); tiledef(2, string.rep(\"2\", 64))\n"
        "newmap(64, 32); newmap(64, 32, 2); layer(2); tile(1, 1, 2)\n"
        "parallax(2, 0, 0, -4, -2)\n"
        "camera(300, 100)\n"
        "waitframe()\n");
    CHECK(image[10][12] == '2' && image[17][19] == '2' && image[9][12] == 0 && image[10][11] == 0);   /* the tile at 8, 8 seen from -4, -2, whatever the camera */

    /* an actor between two layers: in front of layer 1 and behind layer 2, then in front of both */
    static const char draw_between[] =
        "gfx(8); tiledef(1, string.rep(\"1\", 64)); tiledef(2, string.rep(\"2\", 32) .. string.rep(\".\", 32)); sprdef(1, string.rep(\"7\", 64))\n"
        "newmap(8, 8); tile(1, 1, 1)\n"
        "newmap(8, 8, 2); layer(2); tile(1, 1, 2)\n"
        "a = actor(1, 8, 8)\n"
        "print(\"was \" .. actorlayer(a))\n"
        "actorlayer(a, %d)\n"
        "waitframe()\n";
    char prog[600];
    snprintf(prog, sizeof prog, draw_between, 1);
    run_program(prog);
    CHECK(printed("was 4"));
    CHECK(image[8][8] == '2' && image[12][8] == '7');      /* the top half of layer 2's tile hides it; below, it is in front of layer 1 */
    snprintf(prog, sizeof prog, draw_between, 2);
    run_program(prog);
    CHECK(image[8][8] == '7' && image[12][8] == '7');
    snprintf(prog, sizeof prog, draw_between, 0);
    run_program(prog);
    CHECK(image[8][8] == '2' && image[12][8] == '1');      /* behind both */

    /* the solid layer: actormove stops at the tiles of the layer solidlayer() chose; tileflag and tilebox look at the current layer */
    run_program(
        "gfx(8); settileflags(1, 1)\n"
        "newmap(30, 20)\n"
        "newmap(30, 20, 2); layer(2); tilefill(6, 0, 1, 10, 1)\n"
        "a = actor(1, 10, 10)\n"
        "print(\"s\")\n"
        "print(actormove(a, 100, 0) .. \" \" .. actorx(a))\n"
        "actorpos(a, 10, 10)\n"
        "print(solidlayer(2))\n"
        "print(actormove(a, 100, 0) .. \" \" .. actorx(a))\n"
        "print(solidlayer())\n"
        "print(tileflag(50, 4) .. \" \" .. tilebox(0, 0, 60, 10))\n"
        "layer(1)\n"
        "print(tileflag(50, 4) .. \" \" .. tilebox(0, 0, 60, 10))\n");
    {
        static const char *const want[] = {"0 110", "1", "2 40", "2", "1 1", "0 0"};
        CHECK(rows_after("s", want, 6));
    }

    /* wrap on a layer that is not the solid one: it repeats, the actors do not wrap; on the solid one they do */
    run_program(
        "gfx(8); newmap(64, 32); newmap(64, 32, 2)\n"
        "layer(2)\n"
        "print(\"w\")\n"
        "print(wrap(true))\n"
        "a = actor(1, 600, 0)\n"
        "print(actorx(a))\n"
        "layer(1)\n"
        "print(wrap(true))\n"
        "actorpos(a, 600, 0); waitframe()\n"
        "print(actorx(a))\n");
    {
        static const char *const want[] = {"true", "600", "true", "88"};
        CHECK(rows_after("w", want, 4));
    }

    /* follow moves the main camera, and the other layers with it */
    run_program(
        "gfx(8); tiledef(2, string.rep(\"2\", 64))\n"
        "newmap(100, 80); newmap(100, 80, 2); layer(2); tile(30, 12, 2)\n"
        "parallax(2, 0.5)\n"
        "a = actor(1, 400, 300); follow(a)\n"
        "waitframe()\n"
        "print(\"f=\" .. camx() .. \",\" .. camy())\n");
    CHECK(printed("f=244,184"));
    CHECK(image[4][118] == '2' && image[4][117] == 0 && image[3][118] == 0);   /* layer 2's camera at 122, 92: the tile at 240, 96 is at 118, 4 */

    /* mistakes */
    run_program("gfx(8); newmap(8, 8, 5)\n");
    CHECK(shows("newmap(width, height, layer): the layer is 1 to 4"));
    run_program("gfx(8); layer(0)\n");
    CHECK(shows("layer(n): the layer is 1 to 4"));
    run_program("gfx(8); newmap(8, 8); layer(3); tile(0, 0, 1)\n");
    CHECK(shows("there is no map on layer 3"));
    run_program("gfx(8); newmap(8, 8); a = actor(1, 0, 0); solidlayer(3); actormove(a, 1, 0)\n");
    CHECK(shows("actormove: there is no map on layer 3, the solid layer"));
    run_program("gfx(8); parallax(9, 1)\n");
    CHECK(shows("the layer is 1 to 4"));
    run_program("gfx(8); a = actor(1, 0, 0); actorlayer(a, 5)\n");
    CHECK(shows("actorlayer(actor, n): n is 0"));

    /* four layers, all given back by NEW */
    run_program("gfx(8)\nfor n = 1, 4 do newmap(20, 20, n) end\nwaitframe()\nprint(\"four\")\n");
    CHECK(printed("four") && shown_g.layers[3].kind == TG_LAYER_TILES && allocs > releases);
    CHECK(freed_by_new());
}

/* examples/layers.lua: four layers, the ball behind the panel */
static void test_game_example_layers(void)
{
    static char text[8192];
    snprintf(text, sizeof text, "%s\nprint(\"layers=\" .. layer() .. \" \" .. solidlayer())\n", read_example("layers.lua"));
    seed_count = 0;
    run_held = PLAT_BTN_RIGHT | PLAT_BTN_B;
    run_program(text);
    run_held = 0;
    CHECK(printed("Ready.") && !shows("Error") && printed("layers=4 3"));
    for (int i = 0; i < TG_MAX_LAYERS; i++)
        CHECK(shown_g.layers[i].kind == TG_LAYER_TILES);
    CHECK(image[0][0] == '6' && image[14][319] == '6' && image[15][100] == '7');   /* the panel, where it always is */
}

/* ---- sound ---- */

static uint32_t tone_inc_of(uint32_t hz_q8) { return (uint32_t)(((uint64_t)hz_q8 << 24) / PLAT_AUDIO_RATE); }
static uint32_t note_q8(int note) { return (uint32_t)(440.0f * 256.0f * exp2f(((float)note - 69.0f) / 12.0f) + 0.5f); }   /* as tl_sound.c works it out */
static const snd_voice_t *voice(int v) { return &tsnd.voices[v - 1]; }   /* (Lua's numbering) */

/* the loudest sample of the next `frames` frames the engine mixes */
static int peak(int frames)
{
    int best = 0;
    while (frames > 0) {
        const int n = frames < 2400 ? frames : 2400;
        snd_render(&tsnd, sbuf, n);
        for (int i = 0; i < 2 * n; i++) {
            const int a = sbuf[i] < 0 ? -sbuf[i] : sbuf[i];
            best = a > best ? a : best;
        }
        frames -= n;
    }
    return best;
}

/* run_program, on the disk as it is (with files put there first) */
static void run_here(const char *text)
{
    CHECK(put_file("/sd/t.lua", text) > 0);
    type("LOAD t\nRUN\n");
    CHECK(go() == -1);
}

static int put_bytes(const char *path, const void *data, size_t n)
{
    const int fd = fs_open(path, FS_WRONLY | FS_CREAT | FS_TRUNC);
    if (fd < 0)
        return fd;
    const int w = fs_write(fd, data, n);
    fs_close(fd);
    return w;
}

/* A WAV file: the format (1 is PCM), channels, bits, rate and the sound; `claim` is the size the data chunk says it has (0: the real one); `extra` puts an
 * odd-sized chunk before fmt and another between fmt and data; `no_data` leaves the data chunk out. */
static int put_wav(const char *path, int format, int channels, int bits, int rate, const void *data, uint32_t bytes, uint32_t claim, int extra, int no_data)
{
    static uint8_t b[4096];
    size_t n = 0;
#define PUT4(s) (memcpy(b + n, s, 4), n += 4)
#define LE16(v) (b[n++] = (uint8_t)(v), b[n++] = (uint8_t)((v) >> 8))
#define LE32(v) (LE16(v), LE16((v) >> 16))
    PUT4("RIFF");
    LE32(0);
    PUT4("WAVE");
    if (extra) {
        PUT4("LIST");
        LE32(3);
        b[n++] = 'a', b[n++] = 'b', b[n++] = 'c', b[n++] = 0;   /* (and the pad byte) */
    }
    PUT4("fmt ");
    LE32(16);
    LE16(format);
    LE16(channels);
    LE32((uint32_t)rate);
    LE32((uint32_t)(rate * channels * bits / 8));
    LE16(channels * bits / 8);
    LE16(bits);
    if (extra) {
        PUT4("fact");
        LE32(4);
        LE32(12345);
    }
    if (!no_data) {
        PUT4("data");
        LE32(claim ? claim : bytes);
        memcpy(b + n, data, bytes);
        n += bytes;
    }
    const uint32_t riff = (uint32_t)n - 8;
    memcpy(b + 4, &riff, 4);   /* (the tests run on little-endian machines) */
#undef PUT4
#undef LE16
#undef LE32
    return put_bytes(path, b, n);
}

static void test_sound_notes(void)
{
    /* a tone holds until it is let go; playing() sees it at once, before the audio thread has it */
    run_program("tone(1, \"pulse\", 69)\nprint(playing(1))\nprint(playing(2))\n");
    CHECK(printed("true") && printed("false") && printed("Ready."));
    CHECK(voice(1)->kind == SND_PULSE && voice(1)->inc == tone_inc_of(440 * 256) && voice(1)->duty == 128 && voice(1)->vol == 255);
    CHECK(peak(4800) > 30000);
    type("release(1)\n");
    CHECK(go() == -1 && voice(1)->kind == SND_OFF && peak(4800) == 0);
    type("CLS\nprint(playing(1))\n");
    CHECK(go() == -1 && printed("false") && !printed("true"));

    /* notes by number and by name, and what they sound at */
    run_program("print(\"n\")\nprint(note(\"C4\"))\nprint(note(\"A4\"))\nprint(note(\"Bb3\"))\nprint(note(\"c#\"))\nprint(note(\"C-1\"))\nprint(hz(440))\nprint(hz(880))\n"
                "tone(2, \"triangle\", \"A5\")\ntone(3, \"wave\", 57)\ntone(4, \"noise\", 200)\ntone(5, \"buzz\", 60.5)\n");
    {
        static const char *const want[] = {"60.0", "69.0", "58.0", "61.0", "0.0", "69.0", "81.0"};
        CHECK(rows_after("n", want, 7));
    }
    CHECK(voice(2)->kind == SND_TRIANGLE && voice(2)->inc == tone_inc_of(880 * 256));
    CHECK(voice(3)->kind == SND_WAVE && voice(3)->inc == tone_inc_of(220 * 256) && voice(3)->wave[0] == 0 && voice(3)->wave[8] == 127 && voice(3)->wave[24] == -127);
    CHECK(voice(4)->kind == SND_NOISE && voice(4)->inc == 0xFFFFFFFFu);   /* a noise is clocked at most once a sample */
    CHECK(voice(5)->kind == SND_NOISE_SHORT && voice(5)->inc > tone_inc_of(note_q8(60)) && voice(5)->inc < tone_inc_of(note_q8(61)));

    /* the voice's settings, for the note playing and the next ones; beyond the range is the nearest end */
    run_program("volume(3, 100)\npan(3, -50)\nduty(3, 64)\ntone(3, \"pulse\", 60)\nvolume(4, 999)\npan(4, -999)\nduty(4, -5)\ntone(4, \"pulse\", 60)\n"
                "tone(7, \"pulse\", 60)\nvolume(7, 50)\npan(7, 300)\n"
                "envelope(5, 10, 20, 128, 30)\nenvelope(6, 10, 20, 128, 30)\nenvelope(6)\nmastervolume(128)\n");
    CHECK(voice(3)->vol == 100 && voice(3)->pan == -50 && voice(3)->duty == 64);
    CHECK(voice(4)->vol == 255 && voice(4)->pan == -128 && voice(4)->duty == 0);
    CHECK(voice(7)->vol == 50 && voice(7)->pan == 127);
    CHECK(voice(5)->attack_samples == 480 && voice(5)->decay_samples == 960 && voice(5)->sustain == 128 * 257 && voice(5)->release_samples == 1440);
    CHECK(voice(6)->attack_samples == 0 && voice(6)->decay_samples == 0 && voice(6)->sustain == 65536 && voice(6)->release_samples == 240);
    CHECK(tsnd.master == 129);

    /* a note with a length is let go after it; release lets go at once; silence stops one voice, or all */
    run_program("tone(1, \"pulse\", 60, 100)\ntone(2, \"pulse\", 60)\ntone(3, \"pulse\", 60)\ntone(4, \"pulse\", 60)\nrelease(2)\nsilence(3)\n");
    CHECK(voice(1)->kind == SND_OFF && voice(2)->kind == SND_OFF && voice(3)->kind == SND_OFF && voice(4)->kind == SND_PULSE);
    type("silence()\n");
    CHECK(go() == -1 && voice(4)->kind == SND_OFF);
    run_program("gfx(8)\ntone(1, \"pulse\", 60, 100)\nlocal t = millis()\nwhile playing(1) do waitframe() end\nlocal d = millis() - t\nprint(d >= 100 and d < 250)\n");
    CHECK(printed("true") && !printed("false"));
    /* a note whose envelope decays to nothing ends by itself */
    run_program("gfx(8)\nenvelope(1, 0, 10, 0, 1)\ntone(1, \"pulse\", 60)\nprint(\"p\")\nprint(playing(1))\nfor i = 1, 20 do waitframe() end\nprint(playing(1))\n");
    {
        static const char *const want[] = {"true", "false"};
        CHECK(rows_after("p", want, 2));
    }

    /* slides: to the note over the time; pitch: at once */
    run_program("gfx(8)\ntone(1, \"pulse\", 60)\nslide(1, 72, 200)\nfor i = 1, 30 do waitframe() end\ntone(2, \"pulse\", 60)\npitch(2, \"C5\")\n"
                "tone(3, \"pulse\", 60)\nslide(3, 72, 100000000)\n");
    CHECK(voice(1)->inc == tone_inc_of(note_q8(72)) && voice(2)->inc == tone_inc_of(note_q8(72)));
    CHECK(voice(3)->inc > tone_inc_of(note_q8(60)) && voice(3)->inc < tone_inc_of(note_q8(61)));   /* (a long way to go) */

    /* wavetables */
    run_program("wave(1, \"square\")\ntone(1, \"wave\", 69)\nlocal t = {}\nfor i = 1, 32 do t[i] = (i - 1) / 31 * 2 - 1 end\nwave(2, t)\ntone(2, \"wave\", 69)\n"
                "wave(3, string.rep(\"\\200\", 32))\ntone(3, \"wave\", 69)\nwave(4, \"saw\")\nwave(5, \"triangle\")\n");
    CHECK(voice(1)->wave[0] == 127 && voice(1)->wave[31] == -127 && voice(2)->wave[0] == -127 && voice(2)->wave[31] == 127 && voice(3)->wave[5] == -56);
    CHECK(peak(4800) > 30000);

    /* what is wrong is said */
    run_program("tone(1, \"sine\", 60)\n");
    CHECK(shows("tone: the kind is \"pulse\""));
    run_program("tone(17, \"pulse\", 60)\n");
    CHECK(shows("tone: the voice is 1 to 16"));
    run_program("volume(1.5, 3)\n");
    CHECK(shows("volume: the voice is a whole number"));
    run_program("tone(1, \"pulse\", \"H4\")\n");
    CHECK(shows("tone: a note is a number (60 is middle C)"));
    run_program("tone(1, \"pulse\", 0/0)\n");
    CHECK(shows("tone: a note is a number"));
    run_program("print(note(\"C4x\"))\n");
    CHECK(shows("note: a note is a number"));
    run_program("hz(0)\n");
    CHECK(shows("hz(frequency): the frequency is more than 0"));
    run_program("wave(1, {1, 2})\n");
    CHECK(shows("wave(voice, shape)"));
    run_program("wave(1, \"round\")\n");
    CHECK(shows("wave(voice, shape)"));
}

static void test_sound_samples(void)
{
    /* from a string (8 bits) and from a table (16 bits, looping); a speed */
    run_program("print(sample(1, string.rep(\"\\127\", 100), 8000))\nprint(sample(2, {0, 0.5, -0.5, 1}, 24000, 2, 4))\nplay(1, 1)\nplay(2, 2, 2)\n"
                "print(playing(2))\nsample(3, \"abc\", 8000)\nplay(3, 3)\npitch(3, 0.5)\n");
    CHECK(printed("100") && printed("4") && printed("true") && !shows("Error"));
    const snd_sample_t *s1 = voice(1)->sample, *s2 = voice(2)->sample;
    CHECK(voice(1)->kind == SND_OFF && s1->length == 100 && s1->bits == 8 && s1->rate == 8000 && ((const int8_t *)s1->data)[99] == 127 && s1->loop_end == 0);
    CHECK(voice(2)->kind == SND_PCM && s2->length == 4 && s2->bits == 16 && s2->rate == 24000 && s2->loop_start == 1 && s2->loop_end == 4);
    CHECK(((const int16_t *)s2->data)[1] == 16384 && ((const int16_t *)s2->data)[2] == -16384 && ((const int16_t *)s2->data)[3] == 32767);
    CHECK(voice(2)->inc_int == 1 && voice(2)->inc_frac == 0);   /* 24000 at speed 2 is the 48000 of the output */
    CHECK(voice(3)->sample->length == 3);
    CHECK(allocs == 3 && releases == 0);
    /* a sample made again while it plays: the voice stops first, and the old memory is given back once the audio thread has let go */
    type("sample(2, \"xyz\", 8000)\n");
    CHECK(go() == -1 && voice(2)->kind == SND_OFF && allocs == 4 && releases == 1);
    CHECK(freed_by_new());   /* NEW gives every sample back */

    /* WAV files: 16-bit stereo (made mono), 8-bit mono with a loop, other chunks skipped (one of odd size), a data chunk that is cut off */
    fresh();
    const int16_t st[6] = {1000, 3000, -2000, -4000, 32767, 32767};
    const uint8_t m8[4] = {128, 255, 0, 200};
    const int16_t cut16[2] = {5, -5};
    CHECK(put_wav("/sd/st.wav", 1, 2, 16, 22050, st, sizeof st, 0, 1, 0) > 0);
    CHECK(put_wav("/sd/m8.wav", 1, 1, 8, 11025, m8, sizeof m8, 0, 0, 0) > 0);
    CHECK(put_wav("/sd/cut.wav", 1, 1, 16, 8000, cut16, sizeof cut16, 100, 0, 0) > 0);
    run_here("print(\"w\")\nlocal n, r = loadsample(1, \"st.wav\")\nprint(n)\nprint(r)\nn, r = loadsample(2, \"m8.wav\", 2)\nprint(n)\nprint(r)\n"
             "print(loadsample(3, \"cut.wav\"))\nplay(1, 1)\nplay(2, 2)\nplay(3, 3)\n");
    {
        static const char *const want[] = {"3", "22050", "4", "11025"};
        CHECK(rows_after("w", want, 4) && !shows("Error"));
    }
    const snd_sample_t *w1 = voice(1)->sample, *w2 = voice(2)->sample, *w3 = voice(3)->sample;
    CHECK(w1->bits == 16 && w1->length == 3 && w1->rate == 22050 && ((const int16_t *)w1->data)[0] == 2000 && ((const int16_t *)w1->data)[1] == -3000 &&
          ((const int16_t *)w1->data)[2] == 32767);
    CHECK(w2->bits == 8 && w2->length == 4 && w2->loop_start == 1 && w2->loop_end == 4 && voice(2)->kind == SND_PCM);
    CHECK(((const int8_t *)w2->data)[0] == 0 && ((const int8_t *)w2->data)[1] == 127 && ((const int8_t *)w2->data)[2] == -128 && ((const int8_t *)w2->data)[3] == 72);
    CHECK(w3->length == 2 && ((const int16_t *)w3->data)[1] == -5);

    /* files that are not what loadsample takes */
    const int16_t one[1] = {0};
    CHECK(put_file("/sd/text.wav", "this is not a RIFF file at all") > 0);
    CHECK(put_wav("/sd/f24.wav", 1, 1, 24, 8000, one, 2, 0, 0, 0) > 0);
    CHECK(put_wav("/sd/float.wav", 3, 1, 16, 8000, one, 2, 0, 0, 0) > 0);
    CHECK(put_wav("/sd/nodata.wav", 1, 1, 16, 8000, one, 2, 0, 1, 1) > 0);
    CHECK(put_wav("/sd/empty.wav", 1, 1, 16, 8000, one, 0, 0, 0, 0) > 0);
    static const struct { const char *call, *says; } bad[] = {
        {"loadsample(1, \"text.wav\")", "loadsample: text.wav: not a WAV file"},
        {"loadsample(1, \"f24.wav\")", "f24.wav: not 8- or 16-bit PCM"},
        {"loadsample(1, \"float.wav\")", "float.wav: not 8- or 16-bit PCM"},
        {"loadsample(1, \"nodata.wav\")", "nodata.wav: no sound in it (no data chunk)"},
        {"loadsample(1, \"empty.wav\")", "empty.wav: no sound in it (an empty data chunk)"},
        {"loadsample(1, \"gone.wav\")", "loadsample: cannot open gone.wav"},
        {"loadsample(1, \"m8.wav\", 3, 9)", "loadsample: the loop is two whole numbers, the first and last"},
        {"sample(1, \"\", 8000)", "sample: the data is empty"},
        {"sample(1, \"x\", 50)", "sample: the rate is 100 to 200000"},
        {"sample(33, \"x\", 8000)", "sample: the sample is 1 to 32"},
        {"sample(1, \"xyz\", 8000, 2, 9)", "sample: the loop is two whole numbers"},
        {"sample(1, 5, 8000)", "sample(n, data, rate): the data is"},
        {"play(1, 9)", "play: there is no sample 9"},
        {"sample(1, \"x\", 8000) play(1, 1, 0)", "play: the speed is more than 0"},
        {"sample(1, \"x\", 8000) play(1, 1) pitch(1, -1)", "pitch: a sample's speed is more than 0"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        type("CLS\n");
        char line[200];
        snprintf(line, sizeof line, "%s\n", bad[i].call);
        type(line);
        CHECK(go() == -1);
        if (!shows(bad[i].says))
            printf("expected: %s\n", bad[i].says);
        CHECK(shows(bad[i].says));
    }
    run_program("sample(1, {1, \"a\"}, 8000)\n");
    CHECK(shows("sample: entry 2 of the table is not a number") && allocs == releases);   /* (what it had taken is given back) */
    CHECK(freed_by_new());
}

static void test_sound_lifetime(void)
{
    /* Ctrl+C stops the sound, in a program and at the prompt */
    fresh();
    CHECK(put_file("/sd/hold.lua", "tone(1, \"pulse\", 60)\nwhile true do end\n") > 0);
    type("LOAD hold\nRUN\n^C");
    CHECK(go() == -1 && shows("Break") && voice(1)->kind == SND_OFF && peak(4800) == 0);
    type("tone(2, \"pulse\", 60)\n");
    CHECK(go() == -1 && voice(2)->kind == SND_PULSE);
    type("^C");
    CHECK(go() == -1 && voice(2)->kind == SND_OFF);
    /* a program that ends, even with an error, leaves what it started */
    run_program("tone(1, \"pulse\", 60)\nerror(\"oops\")\n");
    CHECK(shows("oops") && voice(1)->kind == SND_PULSE);
    /* NEW and RUN: quiet, and every voice as it was at first */
    type("volume(2, 10)\nNEW\ntone(2, \"pulse\", 60)\n");
    CHECK(go() == -1 && voice(1)->kind == SND_OFF && voice(2)->kind == SND_PULSE && voice(2)->vol == 255);
    /* BYE (and EDIT) leave quietly */
    type("BYE\n");
    CHECK(go() == TL_EXIT_BYE && peak(64) == 0 && voice(2)->kind == SND_OFF);

    /* many commands at once: the queue fills, and the functions wait for the audio thread to make room */
    run_program("for i = 1, 300 do volume(1, i % 256) end\nprint(\"done\")\n");
    CHECK(printed("done") && voice(1)->vol == 300 % 256);

    /* an audio thread that stops answering, with the queue full: music() and music() say so, NEW keeps the song that plays (the audio thread still
     * reads it), and once the thread runs again the song plays on and the next music() stops it and gives it back (ASan: no use after free, no leak) */
    fresh();
    type("music(\"L c d e f\")\n");
    CHECK(go() == -1 && !shows("Error") && voice(16)->kind == SND_PULSE);
    audio_stalled = 1;
    type("for i = 1, 64 do volume(1, 1) end\nmusic(\"L g a\")\n");
    CHECK(go() == -1 && shows("music: the sound is not answering"));
    type("music()\n");
    CHECK(go() == -1 && shows("the music plays on"));
    type("NEW\n");
    CHECK(go() == -1);
    audio_stalled = 0;
    type("print(musicplaying())\n");
    CHECK(go() == -1 && printed("true") && voice(16)->kind == SND_PULSE && voice(1)->vol == 1);
    type("music()\nprint(musicplaying())\n");
    CHECK(go() == -1 && printed("false") && voice(16)->kind == SND_OFF && !tsnd.song);

    /* a machine without sound: the functions do nothing, and samples are still made */
    sound_on = 0;
    run_program("tone(1, \"pulse\", 60)\nprint(playing(1))\nprint(sample(1, \"abc\", 8000))\nplay(1, 1)\nslide(1, 2, 100)\nsilence()\nprint(\"quiet\")\n");
    CHECK(printed("false") && printed("3") && printed("quiet") && !shows("Error") && voice(1)->kind == SND_OFF);
    CHECK(freed_by_new());
    sound_on = 1;
}

/* examples/sound.lua: all its buttons pressed at every frame for 30 frames, then B */
static void test_sound_example(void)
{
    const char *text = read_example("sound.lua");
    fresh();
    went_down = PLAT_BTN_A | PLAT_BTN_X | PLAT_BTN_Y | PLAT_BTN_UP | PLAT_BTN_START;
    press_b_after = 30;
    CHECK(put_file("/sd/sound.lua", text) > 0);
    type("LOAD sound\nRUN\n");
    CHECK(go() == -1);
    CHECK(printed("Ready.") && !shows("Error"));
    CHECK(pixels('2') == 5 * 32 * 32);                                 /* every square lit: each of the five voices was playing */
    CHECK(voice(5)->sample && voice(5)->sample->length == 2400 && voice(5)->sample->bits == 16);
    for (int v = 1; v <= 5; v++)
        CHECK(voice(v)->kind == SND_OFF);                               /* silence() at the end */
    CHECK(voice(4)->wave[0] == -127 && voice(4)->wave[16] == 127);      /* (the triangle) */
}

/* the inc of a tone voice at a MIDI note, as tilesnd's music works it out */
static uint32_t music_inc(int note) { return (uint32_t)(((uint64_t)snd_midi_hz(note) << 24) / PLAT_AUDIO_RATE); }

static void test_sound_music(void)
{
    /* one track: on the last voice; musicplaying sees it at once, and after the song's two quarter notes (a second at 120) it has ended */
    run_program("gfx(8)\nmusic(\"c4 d4\")\nprint(\"m\")\nprint(musicplaying())\nfor i = 1, 1000 do waitframe() end\nprint(musicplaying())\n");
    {
        static const char *const want[] = {"true", "false"};
        CHECK(rows_after("m", want, 2) && !shows("Error"));
    }
    /* tracks on the last voices, or from the voice given; a song that loops goes on playing at the prompt */
    run_program("music(\"L c1\", \"L e1\", \"L g1\", \"L c1\")\n");
    CHECK(voice(13)->kind == SND_PULSE && voice(14)->inc == music_inc(64) && voice(15)->inc == music_inc(67) && voice(16)->inc == music_inc(60));
    CHECK(voice(12)->kind == SND_OFF);
    type("print(musicplaying())\n");
    CHECK(go() == -1 && printed("true"));
    run_program("music(3, \"L c1\", \"L e1\")\n");
    CHECK(voice(3)->inc == music_inc(60) && voice(4)->inc == music_inc(64) && voice(16)->kind == SND_OFF);

    /* instruments: a tone's settings, a wavetable, a sample with its root note (it plays as recorded there) */
    run_program("instrument(1, \"triangle\", {volume = 100, attack = 10, decay = 20, sustain = 50, release = 30, pan = -20})\n"
                "instrument(2, \"wave\", {wave = \"square\"})\n"
                "instrument(3, \"pulse\", {duty = 32})\n"
                "sample(4, string.rep(\"\\100\", 1000), 24000, 1)\n"
                "instrument(4, \"sample\", {sample = 4, root = \"C5\"})\n"
                "music(1, \"L @1 c1\", \"L @2 c1\", \"L @3 c1\", \"L @4 o5 c1\", \"L @4 o6 c1\", \"L @0 c1\")\n");
    CHECK(!shows("Error"));
    CHECK(voice(1)->kind == SND_TRIANGLE && voice(1)->vol == 100 && voice(1)->pan == -20 && voice(1)->attack_samples == 480 && voice(1)->decay_samples == 960 &&
          voice(1)->sustain == 50 * 257 && voice(1)->release_samples == 1440);
    CHECK(voice(2)->kind == SND_WAVE && voice(2)->wave[0] == 127 && voice(2)->wave[31] == -127);
    CHECK(voice(3)->kind == SND_PULSE && voice(3)->duty == 32);
    CHECK(voice(4)->kind == SND_PCM && voice(4)->inc_int == 0 && voice(4)->inc_frac == 32768);   /* C5, the root: 24000 as recorded */
    CHECK(voice(5)->kind == SND_PCM && voice(5)->inc_int == 1 && voice(5)->inc_frac == 0);       /* an octave up */
    CHECK(voice(6)->kind == SND_PULSE && voice(6)->duty == 128 && voice(6)->vol == 255);         /* @0, as it is at first */
    /* making a sample again that the music plays stops the music (and gives the song back) */
    const int before = releases;
    type("sample(4, \"abc\", 8000)\nprint(musicplaying())\n");
    CHECK(go() == -1 && printed("false") && voice(4)->kind == SND_OFF && voice(1)->kind == SND_OFF && releases == before + 2);

    /* stopping: music(), silence(), Ctrl+C, NEW; a new song in place of one; the music's volume */
    run_program("music(\"L c1\")\n");
    CHECK(voice(16)->kind == SND_PULSE && allocs == releases + 1);
    type("music()\n");
    CHECK(go() == -1 && voice(16)->kind == SND_OFF && allocs == releases);   /* the song is given back */
    type("music(\"L c1\")\nmusic(\"L e1\", \"L g1\")\n");
    CHECK(go() == -1 && voice(15)->inc == music_inc(64) && voice(16)->inc == music_inc(67) && allocs == releases + 1);
    type("silence()\nprint(musicplaying())\n");
    CHECK(go() == -1 && printed("false") && voice(15)->kind == SND_OFF && voice(16)->kind == SND_OFF);
    type("CLS\nmusic(\"L c1\")\nprint(musicplaying())\n");
    CHECK(go() == -1 && printed("true"));
    type("^C");
    CHECK(go() == -1 && voice(16)->kind == SND_OFF);
    type("musicvolume(128)\nmusic(\"L v15 c1\")\n");
    CHECK(go() == -1 && voice(16)->vol == 128);
    CHECK(freed_by_new() && voice(16)->kind == SND_OFF);

    /* what is wrong is said */
    static const struct { const char *call, *says; } bad[] = {
        {"music(\"c x\")", "music: track 1, at 3: 'x' is not an MML command"},
        {"music(\"c\", \"c5\")", "music: track 2, at 1: a length is a number that divides 384"},
        {"music(16, \"c\", \"e\")", "music: 2 tracks from voice 16 do not fit"},
        {"music(17, \"c\")", "music: the voice is 1 to 16"},
        {"music(1)", "music([first voice,] track, ...): 1 to 16 tracks"},
        {"music(\"c\", 5, nil)", "bad argument #3 to 'music'"},
        {"instrument(32, \"pulse\")", "instrument: the instrument is 0 to 31"},
        {"instrument(1, \"sine\")", "instrument: the kind is"},
        {"instrument(1, \"pulse\", {dutty = 3})", "instrument: there is no setting \"dutty\""},
        {"instrument(1, \"pulse\", {1, 2})", "instrument: the settings have names"},
        {"instrument(1, \"sample\")", "instrument: a \"sample\" instrument says which one"},
        {"instrument(1, \"pulse\", {wave = 5})", "instrument: wave is"},
        {"instrument(1, \"sample\", {sample = 40})", "instrument: the sample is 1 to 32"},
        {"instrument(1, \"pulse\", {root = \"X\"})", "instrument: a note is a number"},
    };
    run_program("print(1)\n");
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        char line[200];
        snprintf(line, sizeof line, "CLS\n%s\n", bad[i].call);
        if (strchr(bad[i].call, '{')) {   /* (type() takes braces as key names: those go through a program) */
            snprintf(line, sizeof line, "%s\n", bad[i].call);
            run_program(line);
        } else {
            type(line);
            CHECK(go() == -1);
        }
        if (!shows(bad[i].says))
            printf("expected: %s\n", bad[i].says);
        CHECK(shows(bad[i].says));
    }

    /* without sound: music is still made (and its mistakes found), and given back by NEW */
    sound_on = 0;
    run_program("music(\"L c1\")\nprint(musicplaying())\nmusicvolume(3)\n");
    CHECK(printed("false") && !shows("Error") && allocs == releases + 1);
    run_program("music(\"c y\")\n");
    CHECK(shows("'y' is not an MML command"));
    CHECK(freed_by_new());
    sound_on = 1;
}

/* examples/music.lua: it starts the music and ends; the music plays on at the prompt */
static void test_sound_example_music(void)
{
    run_program(read_example("music.lua"));
    CHECK(printed("Ready.") && !shows("Error"));
    type("print(musicplaying())\n");
    CHECK(go() == -1 && printed("true"));
    CHECK(voice(14)->kind == SND_PULSE && voice(15)->kind == SND_TRIANGLE);   /* (the harmony and the bass have no rests; the lead has, and the drums end by themselves) */
    type("music()\n");
    CHECK(go() == -1 && freed_by_new());
}

/* examples/midi.lua: a MIDI file (a C major chord on the piano for two bars, a kick and a closed hi-hat) played, and made into a program
 * that plays it */
static void test_sound_midi(void)
{
    static const unsigned char mid[] = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96,   /* format 0, one track, 96 ticks a quarter */
        'M', 'T', 'r', 'k', 0, 0, 0, 49,
        0x00, 0xFF, 0x51, 0x03, 0x09, 0x27, 0xC0,              /* 600000 us a quarter note: 100 a minute */
        0x00, 0xC0, 0,                                         /* channel 1: the piano */
        0x00, 0x90, 60, 100, 0x00, 64, 100, 0x00, 67, 100,     /* C E G, the second and third by running status */
        0x00, 0x99, 36, 127, 0x00, 42, 80,                     /* channel 10: kick and closed hi-hat */
        0x0A, 0x89, 36, 0, 0x00, 42, 0,
        0x85, 0x76, 0x80, 60, 0, 0x00, 64, 0, 0x00, 67, 0,     /* eight quarter notes after it began (758 ticks after the drums) the chord ends */
        0x00, 0xFF, 0x2F, 0x00,
    };
    static char module[65536];
    snprintf(module, sizeof module, "%s", read_example("midi.lua"));
    seed_file("/sd/midi.lua", module);
    seed_data("/sd/t.mid", mid, sizeof mid);
    run_program("midi = require(\"midi\")\n"
                "print(midi.play(\"t.mid\"))\n"
                "print(musicplaying())\n"
                "print(select(2, pcall(midi.load, \"nothere.mid\")))\n"
                "print(select(2, pcall(midi.load, \"midi.lua\")))\n"
                "print(select(2, pcall(midi.load, \"t.mid\", {grid = 5})))\n"
                "print(midi.load(\"t.mid\", {voices = 2}).summary)\n");
    CHECK(printed("t.mid: 5 tracks, 5 notes, 0:04") && printed("true") && !shows("Error"));
    /* each track's note on its voice, at the track's volume times the instrument's (the notes may have ended: the loads after play() take
     * a while on the test's clock, but a voice keeps what it played last) */
    CHECK(voice(12)->inc == music_inc(67) && voice(13)->inc == music_inc(64) && voice(14)->inc == music_inc(60) && voice(12)->vol == 12 * 17 * 51 / 255);
    CHECK(voice(15)->vol == 51 && voice(16)->vol == 9 * 17 * 51 / 255);
    CHECK(printed("midi: nothere.mid") && printed("midi: midi.lua is not a MIDI file") && printed("midi: the grid is a division of a whole note"));
    CHECK(printed("t.mid: 2 tracks, 2 notes, 0:04, 3 notes left out (more at once than 2 tracks)"));   /* the top of the chord and the kick */

    /* the program it writes: the instruments, and a track for each line, each as long as the others when it loops */
    run_program("midi = require(\"midi\")\n"
                "print(midi.convert(\"t.mid\", \"song.lua\", {loop = true}))\n"
                "dofile(\"song.lua\")\n"
                "print(musicplaying())\n");
    CHECK(printed("t.mid: 5 tracks, 5 notes, 0:04") && printed("true") && !shows("Error"));
    static char text[4096];
    const int fd = fs_open("/sd/song.lua", FS_RDONLY);
    CHECK(fd >= 0);
    const int n = fs_read(fd, text, sizeof text - 1);
    fs_close(fd);
    text[n > 0 ? n : 0] = 0;
    CHECK(strstr(text, "instrument(1, \"pulse\", {duty = 100, decay = 900, sustain = 70, release = 200, volume = 51})   -- Acoustic Grand Piano\n"));
    CHECK(strstr(text, "instrument(2, \"noise\", {decay = 80, sustain = 0, volume = 51})   -- drums: kick\n"));
    CHECK(strstr(text, "instrument(3, \"buzz\", {decay = 30, sustain = 0, volume = 51})   -- drums: hat\n"));
    CHECK(strstr(text, "[=[L l4 t100 @1 v12 o4 g1 | ^1 |]=],") && strstr(text, "[=[L l4 @1 v12 o4 c1 | ^1 |]=],"));   /* (tied at the bar line) */
    CHECK(strstr(text, "[=[L l4 @2 v15 n60,1 | ^1 |]=],") && strstr(text, "[=[L l4 @3 v9 n127,1 | ^1 |]=])"));   /* (a drum lasts to the next) */
    type("music()\n");
    CHECK(go() == -1 && freed_by_new());
    seed_count = 0;
}

static void test_sound(void)
{
    test_sound_notes();
    test_sound_samples();
    test_sound_lifetime();
    test_sound_example();
    test_sound_music();
    test_sound_example_music();
    test_sound_midi();
}

/* reading the graphics back: tilepix, tilerow, tiledef (and the sprite ones) without the last argument, palette(c) */
static void test_game_readback(void)
{
    run_program(
        "local function p(...)\n"                                                  /* (print would put tabs between) */
        "  local t = {}\n"
        "  for i = 1, select(\"#\", ...) do t[i] = tostring((select(i, ...))) end\n"
        "  print(table.concat(t, \" \"))\n"
        "end\n"
        "gfx(8)\n"
        "local s = \"12\" .. string.rep(\".\", 61) .. \"F\"\n"
        "tiledef(3, s)\n"
        "p(\"p \" .. table.concat({tilepix(3, 0, 0), tilepix(3, 1, 0), tilepix(3, 2, 0), tilepix(3, 7, 7)}, \" \"))\n"
        "p(tiledef(3) == s, tilerow(3, 0), tilerow(3, 7), tilerow(3, 3))\n"
        "tilepix(3, 4, 4, 200)\n"
        "p(\"set \" .. tilepix(3, 4, 4), tilerow(3, 4):byte(5), #tiledef(3))\n"
        "p(\"spr \" .. sprpix(3, 0, 0), sprdef(3) == string.rep(\".\", 64))\n"   /* the sprite sheet is separate */
        "sprdef(5, tiledef(3))\n"
        "sprrow(5, 1, \"ABCDEFGH\")\n"
        "p(\"copy \" .. tostring(sprdef(5):sub(1, 8) == \"12......\"), sprrow(5, 1), sprpix(5, 2, 1), tilerow(3, 1))\n"
        "palette(\"@\", -1, -1, -1)\n"                                              /* another see-through character: the strings use it */
        "p(\"bg \" .. tilerow(3, 0))\n"
        "tilepix(3, 0, 2, 64)\n"                                                    /* the entry of @ itself comes back as @ */
        "p(\"at \" .. tilerow(3, 2), tilepix(3, 0, 2))\n"
        "p(\"rgb \" .. table.concat({palette(\"A\")}, \" \"), table.concat({palette(\"0\")}, \" \"), table.concat({palette(0)}, \" \"))\n"
        "palette(\"Z\", 1, 2, 300 - 297)\n"
        "palette(200, 255, 128, 7)\n"
        "p(\"rgb2 \" .. table.concat({palette(\"Z\")}, \" \"), table.concat({palette(90)}, \" \"), table.concat({palette(200)}, \" \"))\n"
        "gfx(16)\n"
        "tiledef(1, string.rep(\"A\", 255) .. \"B\")\n"
        "p(\"big \" .. #tiledef(1), tilerow(1, 15), tilepix(1, 15, 15), tilepix(1, 14, 15), #tilerow(2, 0), tiledef(2) == string.rep(\".\", 256))\n"
        "p(\"fresh \" .. table.concat({palette(\"Z\")}, \" \"))\n");             /* gfx starts the palette afresh */
    CHECK(printed("p 49 50 0 70"));
    CHECK(printed("true 12...... .......F ........"));
    CHECK(printed("set 200 200 64"));
    CHECK(printed("spr 0 true"));
    CHECK(printed("copy true ABCDEFGH 67 ........"));
    CHECK(printed("bg 12@@@@@@"));
    CHECK(printed("at @@@@@@@@ 64"));
    CHECK(printed("rgb 85 255 85 0 0 0 0 0 0"));
    CHECK(printed("rgb2 1 2 3 1 2 3 255 128 7"));
    CHECK(printed("big 256 AAAAAAAAAAAAAAAB 66 65 16 true"));
    CHECK(printed("fresh 0 0 0") && !shows("Error"));

    /* the getters' mistakes */
    run_program("gfx(8)\nprint(tilepix(256, 0, 0))\n");
    CHECK(shows("tilepix(n, x, y): n 0 to 255, x and y 0 to 7"));
    run_program("gfx(16)\nprint(sprpix(1, 16, 0))\n");
    CHECK(shows("sprpix(n, x, y): n 0 to 255, x and y 0 to 15"));
    run_program("gfx(8)\nprint(tilepix(1, 0, 0, nil))\n");                       /* nil is not a colour: only leaving it out reads */
    CHECK(shows("bad argument #4 to 'tilepix'"));
    run_program("gfx(8)\nprint(tilerow(1, 8))\n");
    CHECK(shows("tilerow(n, y): n is 0 to 255, y 0 to 7"));
    run_program("gfx(8)\nprint(sprdef(-1))\n");
    CHECK(shows("sprdef(n): n is 0 to 255"));
    run_program("gfx(8)\ntiledef(300, \"1\")\n");
    CHECK(shows("tiledef(n, graphics): n is 0 to 255"));
    run_program("gfx(8)\nprint(palette(256))\n");
    CHECK(shows("t.lua:2: palette(colour): the colour is one character, or a number"));   /* (the line wraps) */
    run_program("gfx(8)\nprint(palette(\"AB\"))\n");
    CHECK(shows("the colour is one character"));
}

static void test_game(void)
{
    test_game_draw();
    test_game_readback();
    test_game_tilestr();
    test_game_textmode();
    test_game_actors();
    test_game_anim();
    test_game_world();
    test_game_input_and_errors();
    test_game_example();
    test_game_callbacks();
    test_game_example_anim();
    test_game_example2();
    test_game_follow();
    test_game_layers();
    test_game_example_layers();
    test_file_functions();
    test_tile_flags_program();
}

/* A zip archive of stored files (as zip -0 makes them), in out: its length. */
static size_t zip_of(uint8_t *out, int n, const char *const *names, const char *const *texts, const size_t *lens)
{
    size_t at = 0, local[16], cd;
#define Z16(v) (out[at++] = (uint8_t)(v), out[at++] = (uint8_t)((v) >> 8))
#define Z32(v) (Z16((v) & 0xFFFF), Z16((uint32_t)(v) >> 16))
    for (int i = 0; i < n; i++) {
        const size_t nl = strlen(names[i]), dl = lens ? lens[i] : strlen(texts[i]);
        local[i] = at;
        Z32(0x04034b50u); Z16(10); Z16(0); Z16(0); Z16(0); Z16(0x21); Z32(0); Z32(dl); Z32(dl); Z16(nl); Z16(0);
        memcpy(out + at, names[i], nl);
        at += nl;
        memcpy(out + at, texts[i], dl);
        at += dl;
    }
    cd = at;
    for (int i = 0; i < n; i++) {
        const size_t nl = strlen(names[i]), dl = lens ? lens[i] : strlen(texts[i]);
        Z32(0x02014b50u); Z16(0x031e); Z16(10); Z16(0); Z16(0); Z16(0); Z16(0x21); Z32(0); Z32(dl); Z32(dl); Z16(nl); Z16(0); Z16(0);
        Z16(0); Z16(0); Z32(0); Z32(local[i]);
        memcpy(out + at, names[i], nl);
        at += nl;
    }
    const size_t cd_size = at - cd;
    Z32(0x06054b50u); Z16(0); Z16(0); Z16(n); Z16(n); Z32(cd_size); Z32(cd); Z16(0);
#undef Z16
#undef Z32
    return at;
}

static int put_zip(const char *path, int n, const char *const *names, const char *const *texts)
{
    static uint8_t buf[4096];
    return put_data(path, buf, zip_of(buf, n, names, texts, NULL));
}

/* Zip archives: RUN game.zip as a cartridge (its main.lua run inside it, with require and relative paths finding its files), MOUNT and UNMOUNT,
 * mount() and unmount(), and what cannot be done in an archive */
static void test_archives(void)
{
    static const char *const game_names[] = {"main.lua", "lib.lua", "data.txt", "sub/more.txt"};
    static const char *const game_texts[] = {
        "local lib = require(\"lib\")\nprint(\"main: \" .. lib.hello())\nlocal f = io.open(\"data.txt\")\nprint(\"data: \" .. f:read(\"a\"))\nf:close()\n"
        "print(\"more: \" .. io.open(\"sub/more.txt\"):read(\"a\"))\n",
        "return {hello = function() return \"hi\" end}\n", "from the archive", "and more"};
    static const char *const other_names[] = {"main.lua"};
    static const char *const other_texts[] = {"print(\"the other game\")\n"};
    static const char *const nomain_names[] = {"readme.txt"};
    static const char *const nomain_texts[] = {"no program here"};
    fresh();
    const int allocs_before = allocs, releases_before = releases;
    CHECK(put_zip("/sd/game.zip", 4, game_names, game_texts) > 0 && put_zip("/sd/other.zip", 1, other_names, other_texts) > 0 &&
          put_zip("/sd/nomain.zip", 1, nomain_names, nomain_texts) > 0 && put_file("/sd/notzip.zip", "only text, not an archive at all") > 0 &&
          put_file("/sd/hello.lua", "print(\"hello from the card\")\n") > 0);

    /* the cartridge, twice; the folder is the card's again afterwards */
    type("RUN game.zip\n");
    CHECK(go() == -1 && printed("main: hi") && printed("data: from the archive") && printed("more: and more") && shows("Ready.") && !shows("Error"));
    CHECK(!strcmp(fs_getcwd(), "/sd") && !strcmp(env.cart, "/ar") && !strcmp(env.name, "/ar/main.lua"));
    type("CLS\nRUN\n");
    CHECK(go() == -1 && printed("main: hi") && printed("more: and more") && !strcmp(fs_getcwd(), "/sd"));
    type("CLS\nMOUNT\n");
    CHECK(go() == -1 && shows("/ar") && shows("/sd/game.zip"));
    type("CLS\nFILES /ar\n");
    CHECK(go() == -1 && shows("main.lua") && shows("lib.lua") && shows("data.txt") && shows("sub") && shows("<DIR>"));
    type("CLS\nLIST\n");
    CHECK(go() == -1 && shows("require(\"lib\")"));

    /* nothing can be written there */
    type("CLS\nprint(io.open(\"/ar/new.txt\", \"w\"))\n");
    CHECK(go() == -1 && shows("read-only"));
    type("CLS\nSAVE\n");
    CHECK(go() == -1 && shows("Cannot save /ar/main.lua: read-only"));
    type("CLS\nLOAD game.zip\n");
    CHECK(go() == -1 && shows("game.zip is an archive: RUN it"));

    /* an open file keeps it mounted; UNMOUNT takes it away, and RUN says the program's archive is gone */
    type("CLS\nf = io.open(\"/ar/data.txt\")\nUNMOUNT\n");
    CHECK(go() == -1 && shows("Cannot unmount /ar: a file in it is open."));
    type("CLS\nf:close()\nUNMOUNT\nRUN\n");
    fs_stat_t st;
    CHECK(go() == -1 && fs_stat("/ar/main.lua", &st) == FS_ENOENT && shows("Cannot go to /ar"));
    type("CLS\nUNMOUNT\nMOUNT\n");
    CHECK(go() == -1 && shows("No archive is mounted at /ar.") && shows("No archives mounted."));

    /* one cartridge in place of another; RUN file for a plain program, which is not a cartridge's */
    type("CLS\nRUN game.zip\nRUN other.zip\n");
    CHECK(go() == -1 && printed("the other game"));
    type("CLS\nMOUNT\n");
    CHECK(go() == -1 && shows("/sd/other.zip") && !shows("/sd/game.zip"));
    type("CLS\nRUN hello\n");
    CHECK(go() == -1 && printed("hello from the card") && env.cart[0] == 0 && !strcmp(env.name, "hello.lua"));

    /* mount() and unmount() in Lua, and a second archive under another name */
    type("CLS\nprint(mount(\"game.zip\", \"cart\"))\nprint(io.open(\"/cart/sub/more.txt\"):read(\"a\"))\n");
    CHECK(go() == -1 && printed("true") && printed("and more"));
    type("CLS\nprint(mount(\"game.zip\", \"cart\"))\n");   /* (the same archive again: nothing to do) */
    CHECK(go() == -1 && printed("true"));
    type("CLS\nprint(unmount(\"cart\"))\nprint(unmount(\"cart\"))\n");
    CHECK(go() == -1 && printed("true") && shows("no archive is mounted at /cart"));
    type("CLS\nprint(mount(\"notzip.zip\"))\n");
    CHECK(go() == -1 && shows("cannot mount notzip.zip: not a zip archive"));

    /* what goes wrong */
    type("CLS\nMOUNT notzip.zip\n");
    CHECK(go() == -1 && shows("Cannot mount notzip.zip: not a zip archive"));
    type("CLS\nRUN missing.zip\n");
    CHECK(go() == -1 && shows("Cannot mount missing.zip: no such file or directory"));
    type("CLS\nRUN nomain.zip\n");
    CHECK(go() == -1 && shows("nomain.zip has no main.lua") && fs_stat("/ar/readme.txt", &st) == 0);
    type("CLS\nMOUNT game.zip sd\nUNMOUNT sd\n");
    CHECK(go() == -1 && shows("that name is taken (and not by an archive)") && shows("No archive is mounted at /sd.") && fs_stat("/sd/hello.lua", &st) == 0);
    type("CLS\nMOUNT game.zip a/b\n");
    CHECK(go() == -1 && shows("not a name for a mount"));
    type("CLS\nMOUNT game.zip g1\nMOUNT other.zip g2\nMOUNT nomain.zip g3\n");   /* (with /ar: four mounts and the card) */
    CHECK(go() == -1 && shows("too many archives mounted"));

    /* all of it let go of, and their memory back */
    type("CLS\nUNMOUNT\nUNMOUNT g1\nUNMOUNT g2\nMOUNT\n");
    CHECK(go() == -1 && shows("No archives mounted.") && allocs - allocs_before == releases - releases_before);
}

static long file_bytes(const char *path, uint8_t *out, size_t cap)
{
    const int fd = fs_open(path, FS_RDONLY);
    if (fd < 0)
        return fd;
    long n = 0;
    int r;
    while ((r = fs_read(fd, out + n, cap - (size_t)n)) > 0)
        n += r;
    fs_close(fd);
    return n;
}

static uint16_t rgb565(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

/* loadgfx, loadmap, savegfx, savemap: what is saved comes back the same (saved again, the files are the same bytes), and broken or unfitting files
 * are errors that change nothing */
static void test_tgf_files(void)
{
    static uint8_t a[70000], b[70000];
    fresh();
    CHECK(put_file("/sd/save.lua",
                   "gfx(8)\npalette(\"1\", 10, 200, 30)\npalette(\"A\", 255, 128, 0)\npalette(200, 1, 2, 3)\n"
                   "tiledef(1, string.rep(\"1\", 32) .. string.rep(\"A\", 32))\ntiledef(7, string.rep(\".\", 63) .. \"A\")\nsprdef(2, string.rep(\"A1\", 32))\n"
                   "settileflags(1, 1)\nsettileflags(9, 4)\nanimdef(3, \"pingpong\", 5, \"2 1:7\")\n"
                   "newmap(20, 15)\ntile(0, 0, 1)\ntile(5, 3, 7, 1)\ntile(19, 14, 1, 3)\n"
                   "layer(2)\nnewmap(4, 4)\nwrap(true)\ntile(1, 1, 7, 2)\nlayer(1)\n"
                   "savegfx(\"level.tgf\")\nsavemap(\"level.map\")\nsavemap(\"one.map\", 2)\nprint(\"saved\")\n") > 0);
    CHECK(put_file("/sd/load.lua",
                   "loadgfx(\"level.tgf\")\nloadmap(\"level.map\")\nprint(\"tiles\", gettile(0, 0), gettile(5, 3), gettile(19, 14))\n"
                   "layer(2)\nprint(\"layer 2\", gettile(1, 1), gettile(5, 5))\nlayer(1)\n"
                   "savegfx(\"again.tgf\")\nsavemap(\"again.map\")\nloadmap(\"one.map\", 3)\nsavemap(\"three.map\", 3)\nwaitframe()\nprint(\"loaded\")\n") > 0);
    type("LOAD save\nRUN\n");
    CHECK(go() == -1 && printed("saved") && !shows("Error"));
    type("CLS\nLOAD load\nRUN\n");
    CHECK(go() == -1 && printed("loaded") && !shows("Error"));
    CHECK(printed("tiles   1       7       1") && printed("layer 2 7       7"));   /* (tabs to every 8th column; 5, 5 is 1, 1 again: the map wraps) */
    long na = file_bytes("/sd/level.tgf", a, sizeof a), nb = file_bytes("/sd/again.tgf", b, sizeof b);
    CHECK(na > 0 && na == nb && !memcmp(a, b, (size_t)na));
    CHECK(!memcmp(a, "TGACTGF1\x04\0\0\0\x08\0\0\0", 16));
    if (na != 16 + (12 + 4 + 768) + (12 + 8 + 8 * 64) + (12 + 8 + 3 * 64) + (12 + 4 + 10) + (12 + 4 + 8) + 12)
        printf("level.tgf is %ld bytes\n", na);
    CHECK(na == 16 + (12 + 4 + 768) + (12 + 8 + 8 * 64) + (12 + 8 + 3 * 64) + (12 + 4 + 10) + (12 + 4 + 8) + 12);   /* (tiles to 7, sprites to 2, flags to 9) */
    na = file_bytes("/sd/level.map", a, sizeof a);
    nb = file_bytes("/sd/again.map", b, sizeof b);
    CHECK(na == 16 + (12 + 12 + 2 * 20 * 15) + (12 + 12 + 2 * 4 * 4) + 12 && na == nb && !memcmp(a, b, (size_t)na));
    na = file_bytes("/sd/one.map", a, sizeof a);
    nb = file_bytes("/sd/three.map", b, sizeof b);
    CHECK(na == nb && na == 16 + 12 + 12 + 32 + 12 && a[28] == 2 && b[28] == 3 && a[29] == 1);   /* (the layer, and it wraps) */
    b[28] = 2;
    CHECK(!memcmp(a, b, (size_t)na));
    CHECK(shown_g.palette['1'] == rgb565(10, 200, 30) && shown_g.palette['A'] == rgb565(255, 128, 0) && shown_g.palette[200] == rgb565(1, 2, 3));
    CHECK(image[0][0] == '1' && image[3][7] == '1' && image[4][0] == 'A' && image[7][7] == 'A');

    /* without gfx first, loadgfx makes the graphics, at the file's size */
    CHECK(put_file("/sd/fresh.lua", "loadgfx(\"level.tgf\")\nnewmap(20, 15)\ntile(0, 0, 1)\nwaitframe()\nprint(\"fresh\")\n") > 0);
    type("CLS\nLOAD fresh\nRUN\n");
    CHECK(go() == -1 && printed("fresh") && image[0][0] == '1' && image[7][0] == 'A' && shown_g.palette['1'] == rgb565(10, 200, 30));

    /* what goes wrong, and that it changes nothing */
    CHECK(put_file("/sd/other.lua", "gfx(8)\npalette(\"1\", 1, 1, 1)\ntiledef(1, string.rep(\"A\", 64))\nsavegfx(\"other.tgf\")\nprint(\"other\")\n") > 0);
    type("CLS\nLOAD other\nRUN\n");
    CHECK(go() == -1 && printed("other"));
    na = file_bytes("/sd/other.tgf", a, sizeof a);
    CHECK(put_data("/sd/cut.tgf", a, (size_t)na - 1) > 0 && put_file("/sd/bad.tgf", "this is not a graphics file at all, only text") > 0);
    CHECK(put_file("/sd/errors.lua",
                   "print(pcall(loadgfx, \"nothere.tgf\"))\nprint(pcall(loadgfx, \"bad.tgf\"))\nloadgfx(\"level.tgf\")\nprint(pcall(loadgfx, \"cut.tgf\"))\n"
                   "newmap(20, 15)\ntile(0, 0, 1)\nwaitframe()\n"
                   "print(pcall(loadmap, \"level.tgf\"))\nprint(pcall(loadmap, \"level.map\", 1))\nprint(pcall(savemap, \"x.map\", 4))\n"
                   "print(pcall(loadgfx, \"save.lua\"))\n") > 0);
    type("CLS\nLOAD errors\nRUN\n");
    CHECK(go() == -1 && shows("Ready."));
    CHECK(shows("loadgfx: nothere.tgf: no such file or directory") && shows("loadgfx: bad.tgf: not a TGF file") && shows("loadgfx: cut.tgf: cut short"));
    CHECK(shows("loadmap: level.tgf has no map") && shows("loadmap: level.map has 2 maps") && shows("savemap: there is no map on layer 4"));
    CHECK(image[0][0] == '1' && image[7][0] == 'A' && shown_g.palette['1'] == rgb565(10, 200, 30));   /* (cut.tgf changed nothing) */
    CHECK(put_file("/sd/sizes.lua", "gfx(16)\nprint(pcall(loadgfx, \"level.tgf\"))\nprint(pcall(savemap, \"x.map\"))\n") > 0);
    type("CLS\nLOAD sizes\nRUN\n");
    CHECK(go() == -1 && shows("has tiles of 8 pixels, the graphics now 16 (gfx(8) fi") && shows("savemap: there is no map to save"));
    fs_stat_t st;
    CHECK(fs_stat("/sd/x.map", &st) == FS_ENOENT && fs_stat("/sd/x.map.TMP", &st) == FS_ENOENT);

    /* from a cartridge */
    na = file_bytes("/sd/level.tgf", a, sizeof a);
    static const char *const names[] = {"main.lua", "gfx/level.tgf"};
    const char *texts[] = {"loadgfx(\"gfx/level.tgf\")\nnewmap(20, 15)\ntile(0, 0, 1)\nwaitframe()\nprint(\"cartridge\")\n", (const char *)a};
    const size_t lens[] = {strlen(texts[0]), (size_t)na};
    static uint8_t z[80000];
    CHECK(put_data("/sd/cart.zip", z, zip_of(z, 2, names, texts, lens)) > 0);
    type("CLS\nRUN cart.zip\n");
    CHECK(go() == -1 && printed("cartridge") && image[0][0] == '1' && shown_g.palette['A'] == rgb565(255, 128, 0));
    type("UNMOUNT\n");
    CHECK(go() == -1);
    CHECK(freed_by_new());

    /* examples/gfxfiles.lua, which saves, starts afresh and loads back, until B */
    const char *text = read_example("gfxfiles.lua");
    fresh();
    press_b_after = 40;
    CHECK(put_file("/sd/gfxfiles.lua", text) > 0);
    type("LOAD gfxfiles\nRUN\n");
    CHECK(go() == -1 && shows("Loaded gfxfiles.tgf and gfxfiles.map back") && !shows("Error"));
    CHECK(pixels('1') > 10000 && pixels('2') > 0 && pixels('3') > 0 && pixels('4') > 10000 && shown_g.palette['3'] == rgb565(90, 160, 70));
    CHECK(fs_stat("/sd/gfxfiles.tgf", &st) == 0 && fs_stat("/sd/gfxfiles.map", &st) == 0);
}

/* A program in t.lua, run with these keys typed after RUN (the mouse is as the test set it after fresh, which this calls: set it in `setup`) */
static void run_with_keys(const char *text, const char *keys, void (*setup)(void))
{
    fresh();
    if (setup)
        setup();
    CHECK(put_file("/sd/t.lua", text) > 0);
    type("LOAD t\nRUN\n");
    type(keys);
    CHECK(go() == -1);
}

static void mouse_held(void)
{
    tmouse = (plat_mouse_t){.present = true, .x = 10, .y = 20, .buttons = PLAT_MOUSE_LEFT | PLAT_MOUSE_MIDDLE, .pressed = PLAT_MOUSE_LEFT | PLAT_MOUSE_RIGHT,
                            .wheel = -2};   /* (right went down and up again within the frame: pressed, not held) */
}

/* key(): the keys typed while the program runs, by name; mouse() and click(): the pointer and the buttons, as the machine has them at the frame */
static void test_keys_and_mouse(void)
{
    /* every kind of key, in the order typed; what nothing types is passed over */
    static const char collect[] =
        "local got = {}\n"
        "repeat\n"
        "  local k = key()\n"
        "  got[#got + 1] = k\n"
        "until not k\n"
        "print(#got .. \" keys\")\n"
        "for i = 1, #got, 8 do\n"
        "  print(table.concat(got, \",\", i, math.min(i + 7, #got)))\n"
        "end\n";
    run_with_keys(collect, "aZ 7?{UP}{DOWN}{LEFT}{RIGHT}{HOME}{END}{PGUP}{X128}{PGDN}{INS}{F1}{F12}\n{BS}{TAB}{X200}{ESC}{DEL}^A^Z{NUL}{CR}{FS}", NULL);
    CHECK(printed("26 keys"));
    {
        static const char *const want[] = {"a,Z, ,7,?,up,down,left", "right,home,end,pageup,pagedown,insert,f1,f12",
                                           "enter,backspace,tab,escape,delete,ctrl+a,ctrl+z,ctrl+@", "enter,ctrl+\\", "Ready."};
        CHECK(rows_after("26 keys", want, 5));
    }
    CHECK(env.game == NULL && presents == 0);   /* (no graphics needed) */

    /* Ctrl+C is not a key: it stops the program */
    run_with_keys("local k\nrepeat k = key() until k\nprint(\"got \" .. k)\n", "^C", NULL);
    CHECK(shows("Break") && !shows("got "));

    /* keys typed while the program was busy (kept for it by the environment, as for io.read: the loop is long enough for it to look at the keys) come
     * first, in order */
    run_with_keys("for i = 1, 1000000 do end\nlocal s = \"\"\nrepeat\n  local k = key()\n  s = s .. (k or \"\")\nuntil not k\nprint(\"[\" .. s .. \"]\")\n",
                  "abc", NULL);
    CHECK(printed("[abc]"));

    /* with the picture up */
    run_with_keys("gfx(8)\nlocal n = 0\nrepeat\n  waitframe()\n  n = n + 1\nuntil key() == \"q\" or n > 300\nprint(n <= 300 and \"quit by key\" or \"no key\")\n",
                  "xq", NULL);
    CHECK(printed("quit by key"));

    /* the mouse */
    static const char pointer[] =
        "local function b(...)\n"
        "  local t = {}\n"
        "  for i = 1, select(\"#\", ...) do t[i] = tostring((select(i, ...))) end\n"
        "  return table.concat(t, \" \")\n"
        "end\n"
        "print(\"m \" .. b(mouse()))\n"
        "print(b(click(), click(\"left\"), click(\"right\"), click(2), click(\"Middle\"), click(4), click(6), click(nil)))\n"
        "mouse(700, -5)\n"
        "print(b(mouse()))\n"
        "mouse(12.7, 30)\n"
        "print(b(mouse()))\n";
    run_with_keys(pointer, "", mouse_held);
    {
        static const char *const want[] = {"true true true true false false true true", "319 0 true false true -2", "12 30 true false true -2", "Ready."};
        CHECK(strcmp(row(row_of("m ")), "m 10 20 true false true -2") == 0 && rows_after("m ", want, 4));
    }
    CHECK(mouse_sets == 2 && env.game == NULL && presents == 0);
    /* no mouse: the pointer in the middle, nothing held or clicked, and putting it somewhere does nothing */
    mouse_on = 0;
    run_with_keys("print(\"m \" .. table.concat({tostring(click(\"right\")), mouse()}, \" \", 1, 3))\nprint(select(6, mouse()))\nmouse(1, 2)\nprint(\"m2 \" .. mouse())\n",
                  "", mouse_held);
    mouse_on = 1;
    CHECK(printed("m false 160 120") && printed("0") && printed("m2 160") && !shows("Error"));
    /* mistakes */
    run_with_keys("print(click(\"jump\"))\n", "", NULL);
    CHECK(shows("click(button): \"left\" \"right\" \"middle\", or 1, 2, 4"));
    run_with_keys("print(click(8))\n", "", NULL);
    CHECK(shows("click(button): \"left\" \"right\" \"middle\", or 1, 2, 4"));
    run_with_keys("mouse(\"x\", 1)\n", "", NULL);
    CHECK(shows("bad argument #1 to 'mouse'"));
    run_with_keys("mouse(5)\n", "", NULL);
    CHECK(shows("bad argument #2 to 'mouse'"));
}

/* examples/paint.lua, which runs until Escape: keys given one a frame (so that each frame's picture shows what the one before did), and a mouse that
 * moves as paint_mouse_at says */
static int paint_frame, paint_mouse;
static int feed_keys[64], feed_n, feed_pos;

static void paint_mouse_at(int f)
{
    tmouse.pressed = 0;
    tmouse.wheel = 0;
    if (f == 5) {                                       /* on the blue of the bar (cells 12 to 14): click */
        tmouse = (plat_mouse_t){.present = true, .x = 12 * 8 + 5, .y = 5, .buttons = PLAT_MOUSE_LEFT, .pressed = PLAT_MOUSE_LEFT};
    } else if (f == 6) {
        tmouse.buttons = 0;
    } else if (f >= 10 && f <= 15) {                    /* a stroke along row 10 from cell 2 to cell 22, 4 cells a frame */
        tmouse.x = (int16_t)((2 + (f - 10) * 4) * 8 + 3);
        tmouse.y = 10 * 8 + 3;
        tmouse.buttons = PLAT_MOUSE_LEFT;
        tmouse.pressed = f == 10 ? PLAT_MOUSE_LEFT : 0;
    } else if (f == 16) {
        tmouse.buttons = 0;
    } else if (f == 18) {                               /* 1 chooses black (a key, below) ... */
    } else if (f == 20) {                               /* ... and the right button on a blue cell picks blue up again */
        tmouse.x = 12 * 8 + 1;
        tmouse.buttons = PLAT_MOUSE_RIGHT;
    } else if (f == 21) {
        tmouse.buttons = 0;
        tmouse.wheel = -1;                              /* towards you: the next colour, yellow */
    } else if (f == 22) {
        tmouse.x = 200;
        tmouse.y = 200;
    }
}

static uint32_t paced_wait_frame(void)
{
    const int f = ++paint_frame;
    if (paint_mouse)
        paint_mouse_at(f);
    if (feed_pos < feed_n) {
        CHECK(script_len < 8192);
        script[script_len++] = feed_keys[feed_pos++];
    }
    return t_wait_frame();
}

/* paint.lua run afresh, with these keys (one a frame from the first; "." is a frame with none) and the mouse if asked; it must end by itself */
static void paint_run(const char *text, const char *keys, int with_mouse)
{
    fresh();
    CHECK(put_file("/sd/paint.lua", text) > 0);
    env.hooks.wait_frame = paced_wait_frame;
    paint_frame = 0;
    paint_mouse = with_mouse;
    type(keys);
    feed_n = script_len;
    CHECK(feed_n <= 64);
    for (int i = 0; i < feed_n; i++)
        feed_keys[i] = script[i] == '.' ? 0x1FF : script[i];   /* (a code that types nothing) */
    feed_pos = 0;
    script_len = 0;
    type("LOAD paint\nRUN\n");
    CHECK(go() == -1 && shows("Painted.") && !shows("Error"));
}

#define CELL(x, y) image[(y) * 8 + 1][(x) * 8 + 7]   /* a pixel of the cell that the pointer's sprite does not cover */

static void test_example_paint(void)
{
    const char *text = read_example("paint.lua");
    fs_stat_t st;
    /* with the keys only: the pointer starts in the middle (160, 120: cell 20, 15); 3 chooses red; two cells right and one down; space paints; s saves */
    paint_run(text, "3{RIGHT}{RIGHT}{DOWN} s{ESC}", 0);
    CHECK(CELL(22, 16) == '3' && CELL(20, 15) == '2' && CELL(23, 16) == '2');     /* the red cell on the white canvas */
    CHECK(image[0][36 * 8] == '3' && image[0][30 * 8] == '4');                   /* red chosen; the bar green: saved */
    CHECK(tmouse.x == 176 && tmouse.y == 128 && image[128][176] == '1');         /* the pointer moved, and is drawn there */
    CHECK(fs_stat("/sd/paint.map", &st) == 0);
    /* c clears */
    paint_run(text, "3{RIGHT}{RIGHT}{DOWN} c{ESC}", 0);
    CHECK(CELL(22, 16) == '2' && image[0][36 * 8] == '3' && image[0][30 * 8] == '9');
    /* save, clear, choose black, load: the red cell is back, and the bar shows black chosen */
    paint_run(text, "3{RIGHT}{RIGHT}{DOWN} sc1l{ESC}", 0);
    CHECK(CELL(22, 16) == '3' && image[0][36 * 8] == '1' && image[0][30 * 8] == '4');
    /* l with nothing saved: the bar turns red, and the program goes on */
    paint_run(text, "l{ESC}", 0);
    CHECK(image[0][30 * 8] == '3');

    /* with the mouse: the bar chooses blue; a stroke fills the cells between where the mouse was at each frame; the right button picks blue up again
     * after 1 chose black; the wheel then gives yellow */
    paint_run(text, "..................1.......{ESC}", 1);
    for (int x = 2; x <= 22; x++)
        CHECK(CELL(x, 10) == '5');
    CHECK(CELL(1, 10) == '2' && CELL(23, 10) == '2' && CELL(12, 11) == '2' && CELL(12, 9) == '2');
    CHECK(image[0][36 * 8] == '6' && image[200][200] == '1');
}
#undef CELL

/* What Lua no longer uses goes back when something else needs the memory: after an error (a failed program leaves what it built), before HEAP and
 * the editor, and when a machine whose game memory shares a pool with Lua (the board) has it collect from its alloc hook */
static void test_garbage(void)
{
    static const char big[] = "local t = {}\nfor i = 1, 20000 do t[i] = {i} end\nerror(\"too big\")\n";
    static const char junk[] = "junk = {}\nfor i = 1, 20000 do junk[i] = {i} end\njunk = nil\n";
    fresh();
    CHECK(put_file("/sd/big.lua", big) > 0 && put_file("/sd/junk.lua", junk) > 0);
    type("x = 1\n");
    CHECK(go() == -1);
    const size_t base = lua_bytes;
    type("dofile(\"big.lua\")\n");
    CHECK(go() == -1 && shows("too big") && lua_bytes < base + 64 * 1024);

    fresh();
    CHECK(put_file("/sd/junk.lua", junk) > 0);
    type("dofile(\"junk.lua\")\n");
    CHECK(go() == -1 && lua_bytes > 500 * 1024);   /* (the garbage is still there: nothing has made the collector run) */
    shared_limit = lua_bytes;                      /* full, unless the garbage goes */
    shared_collect = 0;                            /* a hook that does not collect: the environment does not do it for it (that is the machine's choice) */
    type("gfx(8)\n");
    CHECK(go() == -1 && shows("not enough memory for the graphics") && env.game == NULL);
    shared_limit = 0;
    type("dofile(\"junk.lua\")\n");               /* (the error collected the garbage: make it again) */
    CHECK(go() == -1 && lua_bytes > 500 * 1024);
    shared_limit = lua_bytes;
    shared_collect = 1;                            /* the devkit's: Lua collects, from inside gfx(), and the graphics fit */
    type("gfx(8)\nprint(\"drawn\")\n");
    CHECK(go() == -1 && printed("drawn") && env.game != NULL && lua_bytes < 200 * 1024);
    shared_limit = 0;
    shared_collect = 0;
    CHECK(freed_by_new());

    /* HEAP collects first, so what it says is free is free */
    fresh();
    CHECK(put_file("/sd/junk.lua", junk) > 0);
    type("dofile(\"junk.lua\")\n");
    CHECK(go() == -1 && lua_bytes > 500 * 1024);
    type("HEAP\n");
    CHECK(go() == -1 && lua_bytes < 200 * 1024);

    /* the editor, which on the board takes its memory from the same pool, starts with the garbage gone */
    fresh();
    CHECK(put_file("/sd/junk.lua", junk) > 0);
    type("dofile(\"junk.lua\")\n");
    CHECK(go() == -1 && lua_bytes > 500 * 1024);
    type("EDIT junk.lua\n");
    CHECK(go() == TL_EXIT_EDIT && lua_bytes < 200 * 1024);
}

int main(void)
{
    test_basics();
    test_programs();
    test_files();
    test_info_commands();
    test_editing_keys();
    test_input_and_break();
    test_edit_and_exit();
    test_gaps();
    test_game();
    test_sound();
    test_garbage();
    test_archives();
    test_tgf_files();
    test_keys_and_mouse();
    test_example_paint();
    printf("all %d checks passed\n", checks);
    return 0;
}
