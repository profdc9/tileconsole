/* SPDX-License-Identifier: MIT */
#include "plat_sdl.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

#include "mouse_acc.h"
#include "platform.h"
#include "tilegfx.h"

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static tg_t pending, shown;
static bool have_state;

static uint32_t frame_no;
static uint64_t next_frame, frame_ticks, start_ticks;
static bool quit;

static plat_buttons_t held[PLAT_MAX_PLAYERS], pressed[PLAT_MAX_PLAYERS], released[PLAT_MAX_PLAYERS];
static plat_buttons_t key_state[PLAT_MAX_PLAYERS];   /* kept up to date by the key events */
static SDL_GameController *pad[PLAT_MAX_PLAYERS];     /* the first two controllers are players 0 and 1 */
static mouse_acc_t mouse;

static uint16_t framebuffer[TG_WIDTH * TG_HEIGHT];

static SDL_AudioDeviceID audio_dev;
static plat_audio_fn audio_fn;
static void *audio_user;

plat_buttons_t plat_sdl_button_for_key(SDL_Keycode key)
{
    switch (key) {
    case SDLK_UP:     return PLAT_BTN_UP;
    case SDLK_DOWN:   return PLAT_BTN_DOWN;
    case SDLK_LEFT:   return PLAT_BTN_LEFT;
    case SDLK_RIGHT:  return PLAT_BTN_RIGHT;
    case SDLK_z:      return PLAT_BTN_B;   /* Z and X sit like the pad's right and lower face buttons */
    case SDLK_x:      return PLAT_BTN_A;
    case SDLK_a:      return PLAT_BTN_X;
    case SDLK_s:      return PLAT_BTN_Y;
    case SDLK_RETURN: return PLAT_BTN_START;
    case SDLK_RSHIFT: return PLAT_BTN_SELECT;
    default:          return 0;
    }
}

static void open_controller(int device_index)
{
    if (!SDL_IsGameController(device_index))
        return;
    SDL_GameController *c = SDL_GameControllerOpen(device_index);
    if (!c)
        return;
    for (int p = 0; p < PLAT_MAX_PLAYERS; p++) {
        if (pad[p] == c)
            return;   /* already have it */
    }
    for (int p = 0; p < PLAT_MAX_PLAYERS; p++)
        if (!pad[p]) {
            pad[p] = c;
            return;
        }
    SDL_GameControllerClose(c);   /* more than two: not used */
}

static void close_controller(SDL_JoystickID instance)
{
    for (int p = 0; p < PLAT_MAX_PLAYERS; p++)
        if (pad[p] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad[p])) == instance) {
            SDL_GameControllerClose(pad[p]);
            pad[p] = NULL;
        }
}

/* A controller's state as platform buttons: the D-pad and the left stick (well off centre) are the directions. */
static plat_buttons_t read_controller(SDL_GameController *c)
{
    plat_buttons_t b = 0;
    const int stick = 10000;   /* about 30% of the way over */
    const int ax = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX), ay = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP) || ay < -stick) b |= PLAT_BTN_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ay > stick) b |= PLAT_BTN_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || ax < -stick) b |= PLAT_BTN_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || ax > stick) b |= PLAT_BTN_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A)) b |= PLAT_BTN_A;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B)) b |= PLAT_BTN_B;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X)) b |= PLAT_BTN_X;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y)) b |= PLAT_BTN_Y;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START)) b |= PLAT_BTN_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK)) b |= PLAT_BTN_SELECT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) b |= PLAT_BTN_L;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) b |= PLAT_BTN_R;
    return b;
}

static uint8_t sdl_button_bit(Uint8 button)
{
    switch (button) {
    case SDL_BUTTON_LEFT: return PLAT_MOUSE_LEFT;
    case SDL_BUTTON_RIGHT: return PLAT_MOUSE_RIGHT;
    case SDL_BUTTON_MIDDLE: return PLAT_MOUSE_MIDDLE;
    default: return 0;
    }
}

bool plat_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
        return false;
    SDL_InitSubSystem(SDL_INIT_AUDIO);   /* a machine without audio can still run the game */
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);   /* and one without controllers */
    mouse_acc_init(&mouse);
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        open_controller(i);

    window = SDL_CreateWindow("tilegame", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 2 * TG_WIDTH, 2 * TG_HEIGHT, 0);
    if (!window)
        return false;
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer)
        return false;
    SDL_RenderSetLogicalSize(renderer, TG_WIDTH, TG_HEIGHT);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, TG_WIDTH, TG_HEIGHT);
    if (!texture)
        return false;

    frame_ticks = SDL_GetPerformanceFrequency() / PLAT_FPS;
    start_ticks = SDL_GetPerformanceCounter();
    next_frame = start_ticks + frame_ticks;
    tg_init(&pending);
    shown = pending;
    return true;
}

void plat_shutdown(void)
{
    for (int p = 0; p < PLAT_MAX_PLAYERS; p++)
        if (pad[p])
            SDL_GameControllerClose(pad[p]);
    memset(pad, 0, sizeof pad);
    if (audio_dev)
        SDL_CloseAudioDevice(audio_dev);
    audio_dev = 0;
    if (texture)
        SDL_DestroyTexture(texture);
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);
    texture = NULL;
    renderer = NULL;
    window = NULL;
    SDL_Quit();
}

void plat_present(const tg_t *g)
{
    pending = *g;
    have_state = true;
}

const uint16_t *plat_sdl_framebuffer(void) { return framebuffer; }

static void compose_and_show(void)
{
    if (have_state) {
        shown = pending;
        have_state = false;
    }
    uint32_t line[TG_WIDTH];
    for (int y = 0; y < TG_HEIGHT; y++) {
        tg_render_line(&shown, y, line);
        for (int x = 0; x < TG_WIDTH; x++)
            framebuffer[y * TG_WIDTH + x] = (uint16_t)line[x];
    }
    SDL_UpdateTexture(texture, NULL, framebuffer, TG_WIDTH * 2);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

static void handle_events(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT)
            quit = true;
        else if (e.type == SDL_MOUSEMOTION)
            mouse_acc_move(&mouse, e.motion.xrel, e.motion.yrel, 0);
        else if (e.type == SDL_MOUSEWHEEL)
            mouse_acc_move(&mouse, 0, 0, e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y);
        else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
            const uint8_t bit = sdl_button_bit(e.button.button);
            mouse_acc_buttons(&mouse, e.type == SDL_MOUSEBUTTONDOWN ? (uint8_t)(mouse.buttons | bit) : (uint8_t)(mouse.buttons & ~bit));
        } else if (e.type == SDL_CONTROLLERDEVICEADDED)
            open_controller(e.cdevice.which);
        else if (e.type == SDL_CONTROLLERDEVICEREMOVED)
            close_controller(e.cdevice.which);
        else if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
            const plat_buttons_t b = plat_sdl_button_for_key(e.key.keysym.sym);
            if (e.type == SDL_KEYDOWN)
                key_state[0] |= b;
            else
                key_state[0] &= (plat_buttons_t)~b;
        }
    }
}

uint32_t plat_wait_frame(void)
{
    compose_and_show();

    /* Pace to 60 Hz; if we are running behind, do not try to catch up on lost frames. */
    uint64_t now = SDL_GetPerformanceCounter();
    if (now < next_frame)
        SDL_Delay((Uint32)((next_frame - now) * 1000 / SDL_GetPerformanceFrequency()));
    now = SDL_GetPerformanceCounter();
    next_frame = (now > next_frame + frame_ticks ? now : next_frame) + frame_ticks;

    handle_events();
    for (int p = 0; p < PLAT_MAX_PLAYERS; p++) {
        const plat_buttons_t was = held[p], is = key_state[p] | (pad[p] ? read_controller(pad[p]) : 0);
        pressed[p] = is & (plat_buttons_t)~was;
        released[p] = was & (plat_buttons_t)~is;
        held[p] = is;
    }
    mouse_acc_frame(&mouse);
    return ++frame_no;
}

bool plat_quit_requested(void) { return quit; }

uint32_t plat_millis(void)
{
    return (uint32_t)((SDL_GetPerformanceCounter() - start_ticks) * 1000 / SDL_GetPerformanceFrequency());
}

uint32_t plat_frame(void) { return frame_no; }

void plat_debug_line(char line[41]) { line[0] = 0; }

int plat_input_diag(char (*lines)[41], int max_lines)
{
    int n = 0, pads = 0;
    for (int p = 0; p < PLAT_MAX_PLAYERS; p++)
        pads += pad[p] != NULL;
    if (n < max_lines)
        snprintf(lines[n++], 41, "SDL: %d CONTROLLER(S), MOUSE %s", pads, mouse.present ? "YES" : "NO");
    return n;
}

plat_mouse_t plat_mouse(void) { return mouse.frame; }
void plat_mouse_set_position(int x, int y) { mouse_acc_set_position(&mouse, x, y); }

plat_buttons_t plat_buttons(int p) { return p >= 0 && p < PLAT_MAX_PLAYERS ? held[p] : 0; }
plat_buttons_t plat_buttons_pressed(int p) { return p >= 0 && p < PLAT_MAX_PLAYERS ? pressed[p] : 0; }
plat_buttons_t plat_buttons_released(int p) { return p >= 0 && p < PLAT_MAX_PLAYERS ? released[p] : 0; }

static void audio_callback(void *user, Uint8 *stream, int len)
{
    (void)user;
    plat_audio_fn fn = audio_fn;
    if (fn)
        fn((int16_t *)stream, len / 4, audio_user);
    else
        memset(stream, 0, (size_t)len);
}

bool plat_audio_start(plat_audio_fn fn, void *user)
{
    if (!SDL_WasInit(SDL_INIT_AUDIO))
        return false;
    if (!audio_dev) {
        SDL_AudioSpec want = {.freq = PLAT_AUDIO_RATE, .format = AUDIO_S16SYS, .channels = 2, .samples = 512,
                              .callback = audio_callback};
        SDL_AudioSpec have;
        audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (!audio_dev)
            return false;
    }
    SDL_LockAudioDevice(audio_dev);
    audio_user = user;
    audio_fn = fn;
    SDL_UnlockAudioDevice(audio_dev);
    SDL_PauseAudioDevice(audio_dev, fn ? 0 : 1);
    return true;
}
