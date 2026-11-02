/* SPDX-License-Identifier: MIT */
/*
 * The desktop development kit end to end: a thread types at it through SDL's event queue (the dummy video driver): EDIT a new file, type a program in the
 * editor, save, leave the editor, and RUN it at the prompt, in the one process; then the screen and the file are looked at.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <SDL.h>

#define main devkit_main
#include "../demo/devkit.c"
#undef main

static void press(int sym)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_KEYDOWN;
    e.key.state = SDL_PRESSED;
    e.key.keysym.sym = sym;
    e.key.keysym.scancode = SDL_GetScancodeFromKey(sym);
    SDL_PushEvent(&e);
    if (sym >= 0x20 && sym < 0x7f) {   /* (typed text arrives as its own event) */
        SDL_Event t;
        memset(&t, 0, sizeof t);
        t.type = SDL_TEXTINPUT;
        t.text.text[0] = (char)sym;
        SDL_PushEvent(&t);
    }
    e.type = SDL_KEYUP;
    e.key.state = SDL_RELEASED;
    SDL_PushEvent(&e);
    usleep(40000);
}

static void type(const char *text)
{
    for (; *text; text++)
        press(*text == '\n' ? SDLK_RETURN : *text);
}

static int on_screen(const char *text)
{
    for (int r = 0; r < 25; r++) {
        char row[81];
        for (int c = 0; c < 80; c++)
            row[c] = (char)(screen.cells[r * 80 + c] & 255);
        row[80] = 0;
        if (strstr(row, text))
            return 1;
    }
    return 0;
}

static int fails;
static void dump(void)
{
    for (int r = 0; r < 25; r++) {
        char row[81];
        for (int c = 0; c < 80; c++)
            row[c] = (char)(screen.cells[r * 80 + c] & 255);
        row[80] = 0;
        printf("|%s\n", row);
    }
}
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); dump(); fails++; } } while (0)

static void *driver(void *arg)
{
    (void)arg;
    sleep(2);
    CHECK(on_screen("TILEGAME BASIC"));
    type("EDIT t.bas\n");
    sleep(2);
    CHECK(on_screen("t.bas"));
    type("print 42;");
    press(SDLK_F2);
    sleep(1);
    press(SDLK_F3);
    sleep(2);
    CHECK(on_screen("] "));
    type("RUN\n");
    sleep(2);
    CHECK(on_screen("42"));
    char buf[100] = "";
    const int fd = fs_open("/sd/t.bas", FS_RDONLY);
    CHECK(fd >= 0);
    if (fd >= 0) {
        buf[fs_read(fd, buf, sizeof buf - 1) > 0 ? 9 : 0] = 0;
        fs_close(fd);
    }
    type("EDIT t.bas\n");                 /* the editor again in the same process */
    sleep(2);
    CHECK(on_screen("t.bas") && on_screen("print 42;"));
    press(SDLK_F3);
    sleep(2);
    CHECK(on_screen("] "));
    printf(fails ? "devkit: %d failures\n" : "devkit: all passed\n", fails);
    fflush(stdout);
    _exit(fails != 0);
}

int main(void)
{
    char dir[] = "/tmp/devkit_test_XXXXXX";
    if (!mkdtemp(dir))
        return 2;
    host_dir = dir;
    pthread_t t;
    pthread_create(&t, NULL, driver, NULL);
    devkit();
    return 0;
}
