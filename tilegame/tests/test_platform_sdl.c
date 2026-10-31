/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
/* Headless tests of the SDL backend, using SDL's dummy video and audio drivers. */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat_sdl.h"
#include "platform.h"
#include "tg_text.h"

static int checks;
#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static void push_text(const char *utf8)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_TEXTINPUT;
    strncpy(e.text.text, utf8, sizeof e.text.text - 1);
    SDL_PushEvent(&e);
}

static void push_key_mod(SDL_Keycode key, Uint16 mod, bool down, int repeat)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.keysym.sym = key;
    e.key.keysym.mod = mod;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.repeat = (Uint8)repeat;
    SDL_PushEvent(&e);
}

static void push_key(SDL_Keycode key, bool down, int repeat)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.keysym.sym = key;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.repeat = (Uint8)repeat;
    SDL_PushEvent(&e);
}

static void test_timing(void)
{
    const uint32_t f0 = plat_wait_frame();
    CHECK(plat_frame() == f0);
    const uint32_t t0 = plat_millis();
    for (int i = 1; i <= 30; i++)
        CHECK(plat_wait_frame() == f0 + (uint32_t)i);
    const uint32_t dt = plat_millis() - t0;
    CHECK(dt >= 480 && dt <= 560);          /* 30 frames at 60 Hz is 500 ms */
    CHECK(plat_frame() == f0 + 30);
    const uint32_t a = plat_millis();
    SDL_Delay(5);
    CHECK(plat_millis() >= a);
}

static void test_input(void)
{
    plat_wait_frame();
    CHECK(plat_buttons(0) == 0 && plat_buttons_pressed(0) == 0);

    push_key(SDLK_z, true, 0);
    push_key(SDLK_RIGHT, true, 0);
    push_key(SDLK_x, true, 1);            /* an auto-repeat event for a key that was never pressed: ignored */
    plat_wait_frame();
    CHECK(plat_buttons(0) == (PLAT_BTN_B | PLAT_BTN_RIGHT));
    CHECK(plat_buttons_pressed(0) == (PLAT_BTN_B | PLAT_BTN_RIGHT));
    CHECK(plat_buttons_released(0) == 0);

    plat_wait_frame();                    /* still held, but no longer "just pressed" */
    CHECK(plat_buttons(0) == (PLAT_BTN_B | PLAT_BTN_RIGHT));
    CHECK(plat_buttons_pressed(0) == 0);

    push_key(SDLK_z, false, 0);
    push_key(SDLK_RETURN, true, 0);
    plat_wait_frame();
    CHECK(plat_buttons(0) == (PLAT_BTN_RIGHT | PLAT_BTN_START));
    CHECK(plat_buttons_released(0) == PLAT_BTN_B);
    CHECK(plat_buttons_pressed(0) == PLAT_BTN_START);

    /* A press and release that both happen within one frame is not lost to the edge detection of the next. */
    push_key(SDLK_RIGHT, false, 0);
    push_key(SDLK_RETURN, false, 0);
    push_key(SDLK_UP, true, 0);
    push_key(SDLK_UP, false, 0);
    plat_wait_frame();
    CHECK(plat_buttons(0) == 0);
    CHECK(plat_buttons_released(0) == (PLAT_BTN_RIGHT | PLAT_BTN_START));

    /* The sampled state is stable between frames. */
    push_key(SDLK_s, true, 0);
    CHECK(plat_buttons(0) == 0);
    plat_wait_frame();
    CHECK(plat_buttons(0) == PLAT_BTN_Y);
    push_key(SDLK_s, false, 0);
    CHECK(plat_buttons(0) == PLAT_BTN_Y);
    plat_wait_frame();

    /* No other players, and bad player numbers, give nothing. */
    CHECK(plat_buttons(1) == 0 && plat_buttons(-1) == 0 && plat_buttons(PLAT_MAX_PLAYERS) == 0);
    CHECK(plat_sdl_button_for_key(SDLK_q) == 0);
    CHECK(plat_sdl_button_for_key(SDLK_RSHIFT) == PLAT_BTN_SELECT);
}

static void test_present(void)
{
    static tg_t g;
    tg_init(&g);
    g.palette[0] = 0xF800;
    plat_present(&g);
    const uint16_t before = plat_sdl_framebuffer()[0];
    CHECK(before != 0xF800);              /* not shown until the next frame */
    plat_wait_frame();
    const uint16_t *fb = plat_sdl_framebuffer();
    for (int i = 0; i < TG_WIDTH * TG_HEIGHT; i++)
        CHECK(fb[i] == 0xF800);

    g.palette[0] = 0x07E0;                /* changing our copy does not change what was presented */
    plat_wait_frame();
    CHECK(plat_sdl_framebuffer()[0] == 0xF800);
    plat_present(&g);
    plat_wait_frame();
    CHECK(plat_sdl_framebuffer()[TG_WIDTH * TG_HEIGHT - 1] == 0x07E0);

    /* A tile layer reaches the screen with its scroll applied. */
    static uint8_t tile[64];
    memset(tile, 5, sizeof tile);
    static uint16_t map[4] = {0, 0, 0, 0};
    g.palette[5] = 0x001F;
    g.layers[0] = (tg_layer_t){.kind = TG_LAYER_TILES, .tile_log2 = 3, .depth = 8, .map_w_log2 = 1, .map_h_log2 = 1,
                               .map = map, .tiles = tile};
    plat_present(&g);
    plat_wait_frame();
    CHECK(plat_sdl_framebuffer()[100 * TG_WIDTH + 100] == 0x001F);
}

/* Text mode: a snapshot of the whole screen reaches the picture, pixel for pixel, and the mode can be left again. */
static uint16_t ref_text_pixel(const tg_text_t *t, int x, int y)
{
    const int ty = y - 40;
    if (ty < 0 || ty >= 400)
        return 0;
    const uint16_t cell = t->cells[(ty / 16) * 80 + x / 8];
    return t->palette[((t->font[(cell & 255) * 16 + ty % 16] >> (7 - x % 8)) & 1) ? (cell >> 8) & 15 : (cell >> 12) & 15];
}

static void test_text_mode(void)
{
    static tg_text_t screen;
    srand(5);
    for (int i = 0; i < TG_TEXT_CELLS; i++)
        screen.cells[i] = (uint16_t)rand();
    for (size_t i = 0; i < sizeof screen.font; i++)
        screen.font[i] = (uint8_t)rand();
    for (int i = 0; i < 16; i++)
        screen.palette[i] = (uint16_t)(rand() | 0x0021);
    static tg_t g;
    tg_init(&g);
    g.mode = TG_MODE_TEXT;
    g.text = &screen;
    plat_present(&g);
    screen.cells[0] ^= 0xFFFF;                                    /* the screen is copied: this is too late to show */
    screen.palette[3] ^= 0x1234;
    plat_wait_frame();
    const uint16_t *d = plat_sdl_display();
    tg_text_t shown = screen;
    shown.cells[0] ^= 0xFFFF;                                     /* what was presented */
    shown.palette[3] ^= 0x1234;
    for (int y = 0; y < TG_DISPLAY_HEIGHT; y++)
        for (int x = 0; x < TG_DISPLAY_WIDTH; x++)
            CHECK(d[y * TG_DISPLAY_WIDTH + x] == ref_text_pixel(&shown, x, y));

    plat_present(&g);                                             /* now with the change */
    plat_wait_frame();
    for (int y = 0; y < TG_DISPLAY_HEIGHT; y += 3)
        for (int x = 0; x < TG_DISPLAY_WIDTH; x++)
            CHECK(d[y * TG_DISPLAY_WIDTH + x] == ref_text_pixel(&screen, x, y));

    /* Back to tile mode: the picture is the tile frame, each source pixel doubled in both directions. */
    static tg_t tiles;
    tg_init(&tiles);
    tiles.palette[0] = 0x07E0;
    tiles.palette[9] = 0xF81F;
    static uint8_t bitmap[TG_WIDTH * TG_HEIGHT];
    for (int i = 0; i < TG_WIDTH * TG_HEIGHT; i++)
        bitmap[i] = (uint8_t)((i / TG_WIDTH + i % TG_WIDTH) % 3 ? 0 : 9);
    tiles.layers[0] = (tg_layer_t){.kind = TG_LAYER_BITMAP, .bitmap = bitmap};
    plat_present(&tiles);
    plat_wait_frame();
    d = plat_sdl_display();
    const uint16_t *fb = plat_sdl_framebuffer();
    int pink = 0;
    for (int y = 0; y < TG_HEIGHT; y++)
        for (int x = 0; x < TG_WIDTH; x++) {
            const uint16_t c = fb[y * TG_WIDTH + x];
            pink += c == 0xF81F;
            CHECK(d[2 * y * TG_DISPLAY_WIDTH + 2 * x] == c && d[2 * y * TG_DISPLAY_WIDTH + 2 * x + 1] == c);
            CHECK(d[(2 * y + 1) * TG_DISPLAY_WIDTH + 2 * x] == c && d[(2 * y + 1) * TG_DISPLAY_WIDTH + 2 * x + 1] == c);
        }
    CHECK(pink > 1000 && pink < TG_WIDTH * TG_HEIGHT);
}

/* The text mode cursor blinks by the frame number the backend gives each frame, so it blinks without being presented again. */
static void test_cursor_blink(void)
{
    static tg_text_t t;
    tg_text_init(&t);
    for (int i = 0; i < TG_TEXT_CELLS; i++)
        t.cells[i] = TG_TEXT_CELL(' ', TG_TEXT_ATTR(TG_TEXT_YELLOW, TG_TEXT_BLUE));   /* spaces: only the cursor is yellow */
    tg_text_set_cursor(&t, 10, 3);
    tg_text_show_cursor(&t, true);
    tg_text_set_cursor_shape(&t, 0, 15);
    static tg_t g;
    tg_init(&g);
    g.mode = TG_MODE_TEXT;
    g.text = &t;
    plat_present(&g);                                      /* once: no more presenting while it blinks */
    const uint16_t yellow = t.palette[TG_TEXT_YELLOW];
    int states[100], changes = 0;
    uint32_t frames[100];
    for (int i = 0; i < 100; i++) {
        frames[i] = plat_wait_frame();
        const uint16_t *d = plat_sdl_display();
        const int on = d[(40 + 3 * 16 + 8) * TG_DISPLAY_WIDTH + 10 * 8 + 3] == yellow;   /* a pixel in the middle of the cursor cell */
        CHECK(on == (d[(40 + 3 * 16 + 8) * TG_DISPLAY_WIDTH + 10 * 8 + 4] == yellow));
        states[i] = on;
        if (i && states[i] != states[i - 1]) {
            changes++;
        }
    }
    CHECK(changes >= 5 && changes <= 7);                    /* 100 frames: the state flips every 16 */
    for (int i = 0; i < 100; i++)                           /* it follows the frame number alone: the picture shown has number (returned - 1) */
        CHECK(states[i] == (int)(((frames[i] - 1) / TG_CURSOR_BLINK_FRAMES) % 2 == 0));
    tg_text_set_cursor_blink(&t, false);                    /* steady: on in every frame */
    plat_present(&g);
    plat_wait_frame();
    for (int i = 0; i < 40; i++) {
        plat_wait_frame();
        CHECK(plat_sdl_display()[(40 + 3 * 16 + 8) * TG_DISPLAY_WIDTH + 10 * 8 + 3] == yellow);
    }
}

/* Typed characters: the window's text, ASCII only, and the keys that make no text. */
static void test_chars(void)
{
    plat_wait_frame();
    while (plat_getchar() >= 0)
        ;
    CHECK(plat_getchar() == -1);
    push_text("Hi!");
    push_text("caf\xC3\xA9 \xE2\x82\xAC");              /* "café €": only the ASCII is kept */
    push_text("\x01\x1F\x7F");                            /* control codes and DEL are not text */
    push_text("~");
    plat_wait_frame();
    const char *want = "Hi!caf ~";
    for (const char *p = want; *p; p++)
        CHECK(plat_getchar() == *p);
    CHECK(plat_getchar() == -1);

    push_key_mod(SDLK_RETURN, 0, true, 0);
    push_key_mod(SDLK_BACKSPACE, 0, true, 0);
    push_key_mod(SDLK_TAB, 0, true, 0);
    push_key_mod(SDLK_ESCAPE, 0, true, 0);
    push_key_mod(SDLK_DELETE, 0, true, 0);
    push_key_mod(SDLK_KP_ENTER, 0, true, 0);
    push_key_mod(SDLK_UP, 0, true, 0);
    push_key_mod(SDLK_LEFT, 0, true, 1);                    /* an auto-repeat counts */
    push_key_mod(SDLK_LEFT, 0, false, 0);                   /* a release does not */
    push_key_mod(SDLK_HOME, 0, true, 0);
    push_key_mod(SDLK_PAGEDOWN, 0, true, 0);
    push_key_mod(SDLK_PAGEUP, 0, true, 0);
    push_key_mod(SDLK_END, 0, true, 0);
    push_key_mod(SDLK_INSERT, 0, true, 0);
    push_key_mod(SDLK_DOWN, 0, true, 0);
    push_key_mod(SDLK_RIGHT, 0, true, 0);
    push_key_mod(SDLK_F1, 0, true, 0);
    push_key_mod(SDLK_F12, 0, true, 0);
    push_key_mod(SDLK_F5, KMOD_LSHIFT, true, 0);
    plat_wait_frame();
    static const int keys[] = {10, 8, 9, 27, 127, 10, PLAT_KEY_UP, PLAT_KEY_LEFT, PLAT_KEY_HOME, PLAT_KEY_PAGEDOWN,
                               PLAT_KEY_PAGEUP, PLAT_KEY_END, PLAT_KEY_INSERT, PLAT_KEY_DOWN, PLAT_KEY_RIGHT, PLAT_KEY_F(1), PLAT_KEY_F(12), PLAT_KEY_F(5)};
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
        CHECK(plat_getchar() == keys[i]);
    CHECK(plat_getchar() == -1);

    /* Ctrl with a letter or one of three punctuation keys; the plain letter makes no character from a key press (its text arrives as text). */
    push_key_mod(SDLK_a, KMOD_LCTRL, true, 0);
    push_key_mod(SDLK_z, KMOD_RCTRL, true, 0);
    push_key_mod(SDLK_LEFTBRACKET, KMOD_LCTRL, true, 0);
    push_key_mod(SDLK_BACKSLASH, KMOD_LCTRL, true, 0);
    push_key_mod(SDLK_RIGHTBRACKET, KMOD_LCTRL, true, 0);
    push_key_mod(SDLK_SPACE, KMOD_LCTRL, true, 0);
    push_key_mod(SDLK_a, 0, true, 0);
    push_key_mod(SDLK_SPACE, 0, true, 0);
    push_key_mod(SDLK_1, KMOD_LCTRL, true, 0);
    plat_wait_frame();
    static const int ctrl[] = {1, 26, 27, 28, 29, 0};
    for (size_t i = 0; i < sizeof ctrl / sizeof *ctrl; i++)
        CHECK(plat_getchar() == ctrl[i]);
    CHECK(plat_getchar() == -1);

    /* The queue holds 64 and drops the rest; the buttons are unaffected by typing. */
    for (int round = 0; round < 4; round++) {                /* more than it holds, and round the ring more than once */
        for (int i = 0; i < 100; i++) {
            char one[2] = {(char)('a' + i % 26), 0};
            push_text(one);
        }
        plat_wait_frame();
        for (int i = 0; i < 64; i++)
            CHECK(plat_getchar() == 'a' + i % 26);
        CHECK(plat_getchar() == -1);
        for (int i = 0; i < 40; i++)                         /* and a part-read queue, filled up again */
            push_text("p");
        plat_wait_frame();
        for (int i = 0; i < 25; i++)
            CHECK(plat_getchar() == 'p');
        for (int i = 0; i < 49; i++)
            push_text("q");
        plat_wait_frame();
        for (int i = 0; i < 15; i++)
            CHECK(plat_getchar() == 'p');
        for (int i = 0; i < 49; i++)
            CHECK(plat_getchar() == 'q');
        CHECK(plat_getchar() == -1);
    }
    push_key(SDLK_z, true, 0);
    push_text("z");
    plat_wait_frame();
    CHECK(plat_getchar() == 'z' && plat_getchar() == -1);
    CHECK(plat_buttons(0) & PLAT_BTN_B);                    /* Z is the B button too */
    push_key(SDLK_z, false, 0);
    push_key(SDLK_RETURN, false, 0);                        /* let go of everything pressed above that is also a button */
    push_key(SDLK_UP, false, 0);
    push_key(SDLK_DOWN, false, 0);
    push_key(SDLK_RIGHT, false, 0);
    push_key(SDLK_LEFT, false, 0);
    push_key(SDLK_a, false, 0);
    plat_wait_frame();
    plat_wait_frame();
    CHECK(plat_buttons(0) == 0);
    CHECK(plat_sdl_char_for_key(SDLK_a, 0) == -1 && plat_sdl_char_for_key(SDLK_F13, 0) == -1 && plat_sdl_char_for_key(SDLK_UNKNOWN, KMOD_CTRL) == -1);
}

static volatile long audio_frames, audio_calls;
static volatile int audio_user_ok;
static int audio_marker;
static void audio_fn(int16_t *lr, int frames, void *user)
{
    audio_user_ok = user == &audio_marker;
    for (int i = 0; i < frames; i++) {
        lr[2 * i] = 100;
        lr[2 * i + 1] = -100;
    }
    audio_frames += frames;
    audio_calls++;
}

static void test_audio(void)
{
    if (!plat_audio_start(audio_fn, &audio_marker)) {
        printf("(no audio device: audio test skipped)\n");
        return;
    }
    const uint32_t t0 = plat_millis();
    const long f0 = audio_frames;
    for (int i = 0; i < 30; i++)
        plat_wait_frame();
    const uint32_t dt = plat_millis() - t0;
    const long got = audio_frames - f0;
    CHECK(audio_calls > 0 && audio_user_ok);
    /* The right number of frames for the time that passed, to within a couple of buffers. */
    const long want = (long)dt * PLAT_AUDIO_RATE / 1000;
    CHECK(got > want * 7 / 10 && got < want * 13 / 10);

    CHECK(plat_audio_start(NULL, NULL));  /* stop */
    SDL_Delay(50);
    const long stopped = audio_frames;
    for (int i = 0; i < 6; i++)
        plat_wait_frame();
    CHECK(audio_frames == stopped);
}


static void push_mouse_motion(int dx, int dy)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_MOUSEMOTION;
    e.motion.xrel = dx;
    e.motion.yrel = dy;
    SDL_PushEvent(&e);
}

static void push_mouse_button(int button, bool down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    e.button.button = (Uint8)button;
    e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    SDL_PushEvent(&e);
}

static void test_mouse(void)
{
    plat_wait_frame();
    CHECK(!plat_mouse().present && plat_mouse().buttons == 0);     /* until it is seen */

    push_mouse_motion(10, -4);
    push_mouse_motion(3, 1);
    push_mouse_button(SDL_BUTTON_LEFT, true);
    SDL_Event w;
    memset(&w, 0, sizeof w);
    w.type = SDL_MOUSEWHEEL;
    w.wheel.y = 2;
    SDL_PushEvent(&w);
    plat_wait_frame();
    plat_mouse_t m = plat_mouse();
    CHECK(m.present && m.dx == 13 && m.dy == -3 && m.wheel == 2);
    CHECK(m.buttons == PLAT_MOUSE_LEFT && m.pressed == PLAT_MOUSE_LEFT && m.released == 0);
    CHECK(m.x == TG_WIDTH / 2 + 13 && m.y == TG_HEIGHT / 2 - 3);

    push_mouse_button(SDL_BUTTON_RIGHT, true);
    push_mouse_button(SDL_BUTTON_LEFT, false);
    plat_wait_frame();
    m = plat_mouse();
    CHECK(m.dx == 0 && m.dy == 0 && m.wheel == 0);                   /* movement is per frame */
    CHECK(m.buttons == PLAT_MOUSE_RIGHT && m.pressed == PLAT_MOUSE_RIGHT && m.released == PLAT_MOUSE_LEFT);
    CHECK(m.x == TG_WIDTH / 2 + 13);                                  /* the pointer stays put */
    push_mouse_button(SDL_BUTTON_MIDDLE, true);
    push_mouse_button(SDL_BUTTON_X1, true);                           /* a button that is not one of the three: ignored */
    plat_wait_frame();
    CHECK(plat_mouse().buttons == (PLAT_MOUSE_RIGHT | PLAT_MOUSE_MIDDLE));
    push_mouse_button(SDL_BUTTON_RIGHT, false);
    push_mouse_button(SDL_BUTTON_MIDDLE, false);
    plat_wait_frame();

    push_mouse_motion(5000, 5000);
    plat_wait_frame();
    CHECK(plat_mouse().x == TG_WIDTH - 1 && plat_mouse().y == TG_HEIGHT - 1);
    plat_mouse_set_position(20, 30);
    plat_wait_frame();
    CHECK(plat_mouse().x == 20 && plat_mouse().y == 30);
}

/* Controllers, made with SDL's virtual joysticks. */
static int virtual_index = -1;

static SDL_Joystick *attach_virtual(const char *name)
{
    SDL_VirtualJoystickDesc d;
    SDL_zero(d);
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = SDL_CONTROLLER_AXIS_MAX;
    d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    d.name = name;
    d.vendor_id = 0x1209;    /* the only ids the test lets SDL use (see main), so a real pad plugged in does not interfere */
    d.product_id = 0x0001;
    virtual_index = SDL_JoystickAttachVirtualEx(&d);
    if (virtual_index < 0)
        return NULL;
    return SDL_JoystickOpen(virtual_index);
}

static void test_controllers(void)
{
    SDL_Joystick *one = attach_virtual("test pad one");
    if (!one || !SDL_IsGameController(virtual_index)) {
        printf("(no virtual game controllers here: controller test skipped)\n");
        return;
    }
    plat_wait_frame();                                                /* the device-added event */
    plat_wait_frame();
    CHECK(plat_buttons(0) == 0);
    printf("  virtual game controller attached (device %d)\n", virtual_index);

    SDL_JoystickSetVirtualButton(one, SDL_CONTROLLER_BUTTON_A, 1);
    SDL_JoystickSetVirtualButton(one, SDL_CONTROLLER_BUTTON_DPAD_LEFT, 1);
    SDL_JoystickSetVirtualButton(one, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 1);
    plat_wait_frame();
    CHECK(plat_buttons(0) == (PLAT_BTN_A | PLAT_BTN_LEFT | PLAT_BTN_R));
    CHECK(plat_buttons_pressed(0) == (PLAT_BTN_A | PLAT_BTN_LEFT | PLAT_BTN_R));
    plat_wait_frame();
    CHECK(plat_buttons_pressed(0) == 0);

    SDL_JoystickSetVirtualButton(one, SDL_CONTROLLER_BUTTON_A, 0);
    SDL_JoystickSetVirtualButton(one, SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0);
    SDL_JoystickSetVirtualButton(one, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0);
    SDL_JoystickSetVirtualButton(one, SDL_CONTROLLER_BUTTON_B, 1);
    SDL_JoystickSetVirtualButton(one, SDL_CONTROLLER_BUTTON_X, 1);
    SDL_JoystickSetVirtualButton(one, SDL_CONTROLLER_BUTTON_Y, 1);
    SDL_JoystickSetVirtualButton(one, SDL_CONTROLLER_BUTTON_START, 1);
    SDL_JoystickSetVirtualButton(one, SDL_CONTROLLER_BUTTON_BACK, 1);
    SDL_JoystickSetVirtualButton(one, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 1);
    plat_wait_frame();
    CHECK(plat_buttons(0) == (PLAT_BTN_B | PLAT_BTN_X | PLAT_BTN_Y | PLAT_BTN_START | PLAT_BTN_SELECT | PLAT_BTN_L));
    SDL_JoystickSetVirtualButton(one, SDL_CONTROLLER_BUTTON_B, 0);
    SDL_JoystickSetVirtualButton(one, SDL_CONTROLLER_BUTTON_X, 0);
    SDL_JoystickSetVirtualButton(one, SDL_CONTROLLER_BUTTON_Y, 0);
    SDL_JoystickSetVirtualButton(one, SDL_CONTROLLER_BUTTON_START, 0);
    SDL_JoystickSetVirtualButton(one, SDL_CONTROLLER_BUTTON_BACK, 0);
    SDL_JoystickSetVirtualButton(one, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0);

    /* The left stick: well over counts, a little does not. */
    SDL_JoystickSetVirtualAxis(one, SDL_CONTROLLER_AXIS_LEFTX, 32000);
    SDL_JoystickSetVirtualAxis(one, SDL_CONTROLLER_AXIS_LEFTY, -32000);
    plat_wait_frame();
    CHECK(plat_buttons(0) == (PLAT_BTN_RIGHT | PLAT_BTN_UP));
    SDL_JoystickSetVirtualAxis(one, SDL_CONTROLLER_AXIS_LEFTX, 5000);
    SDL_JoystickSetVirtualAxis(one, SDL_CONTROLLER_AXIS_LEFTY, 32000);
    plat_wait_frame();
    CHECK(plat_buttons(0) == PLAT_BTN_DOWN);
    SDL_JoystickSetVirtualAxis(one, SDL_CONTROLLER_AXIS_LEFTY, 0);
    plat_wait_frame();
    CHECK(plat_buttons(0) == 0);

    /* The keyboard and the controller are both player 0, and add up. */
    SDL_JoystickSetVirtualButton(one, SDL_CONTROLLER_BUTTON_A, 1);
    push_key(SDLK_RIGHT, true, 0);
    plat_wait_frame();
    CHECK(plat_buttons(0) == (PLAT_BTN_A | PLAT_BTN_RIGHT));
    push_key(SDLK_RIGHT, false, 0);
    SDL_JoystickSetVirtualButton(one, SDL_CONTROLLER_BUTTON_A, 0);
    plat_wait_frame();
    CHECK(plat_buttons(0) == 0 && plat_buttons(1) == 0);

    /* A second controller is player 1, with its own buttons. */
    SDL_Joystick *two = attach_virtual("test pad two");
    CHECK(two != NULL);
    plat_wait_frame();
    plat_wait_frame();
    SDL_JoystickSetVirtualButton(two, SDL_CONTROLLER_BUTTON_B, 1);
    plat_wait_frame();
    CHECK(plat_buttons(1) == PLAT_BTN_B && plat_buttons(0) == 0);
    SDL_JoystickSetVirtualButton(two, SDL_CONTROLLER_BUTTON_B, 0);

    /* Unplugging the first: its buttons go away. */
    SDL_JoystickSetVirtualButton(one, SDL_CONTROLLER_BUTTON_A, 1);
    plat_wait_frame();
    CHECK(plat_buttons(0) == PLAT_BTN_A);
    SDL_JoystickClose(one);
    SDL_JoystickDetachVirtual(0);   /* the first virtual joystick made */
    plat_wait_frame();
    plat_wait_frame();
    CHECK(plat_buttons(0) == 0);
    SDL_JoystickClose(two);
}

static void test_quit(void)
{
    CHECK(!plat_quit_requested());
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_QUIT;
    SDL_PushEvent(&e);
    plat_wait_frame();
    CHECK(plat_quit_requested());
}

int main(void)
{
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    setenv("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT", "0x1209/0x0001", 1);   /* ignore real controllers */
    CHECK(plat_init());
    test_timing();
    test_input();
    test_present();
    test_text_mode();
    test_cursor_blink();
    test_chars();
    test_audio();
    test_mouse();
    test_controllers();
    test_quit();
    plat_shutdown();
    printf("all %d checks passed\n", checks);
    return 0;
}
