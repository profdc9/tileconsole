/* SPDX-License-Identifier: MIT
 *
 * Desktop demo: a 16x16 8 bpp parallax background, an 8x8 4 bpp foreground, bouncing sprites and an optional
 * per-line wave. Arrow keys scroll, W toggles the wave, Esc quits.
 *
 *   demo_sdl                 window, 640x480
 *   demo_sdl --shot f.ppm N  run N frames without a window and save the last as a PPM
 *   demo_sdl --bench         time the compositor
 */
#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scene.h"
#include "tilegfx.h"

static void render_frame(tg_t *g, uint32_t *frame)
{
    for (int line = 0; line < TG_HEIGHT; line++)
        tg_render_line(g, line, frame + line * TG_WIDTH);
}

static void save_ppm(const char *name, const uint32_t *frame)
{
    FILE *f = fopen(name, "wb");
    if (!f) { perror(name); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", TG_WIDTH, TG_HEIGHT);
    for (int i = 0; i < TG_WIDTH * TG_HEIGHT; i++) {
        const uint16_t c = (uint16_t)frame[i];
        const uint8_t rgb[3] = {(uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 63) << 2), (uint8_t)((c & 31) << 3)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    static tg_t g;
    static uint32_t frame[TG_WIDTH * TG_HEIGHT];
    scene_setup(&g);

    if (argc >= 4 && !strcmp(argv[1], "--shot")) {
        const int n = atoi(argv[3]);
        for (int f = 0; f < n; f++) {
            scene_animate(&g, f, f * 2, argc > 4);
            render_frame(&g, frame);
        }
        save_ppm(argv[2], frame);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "--bench")) {
        const int n = 600;
        const Uint64 t0 = SDL_GetPerformanceCounter();
        for (int f = 0; f < n; f++) {
            scene_animate(&g, f, f * 2, true);
            render_frame(&g, frame);
        }
        const double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency() / n;
        printf("%.3f ms per frame, %.2f us per line (this desktop; the board will be slower)\n", ms, ms * 1000.0 / TG_HEIGHT);
        return 0;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("tilegfx", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, TG_WIDTH, TG_HEIGHT);
    if (!win || !ren || !tex) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    int scroll = 0, frame_no = 0;
    bool wave_on = false, run = true;
    while (run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE))
                run = false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_w)
                wave_on = !wave_on;
        }
        const Uint8 *k = SDL_GetKeyboardState(NULL);
        scroll += (k[SDL_SCANCODE_RIGHT] ? 3 : 0) - (k[SDL_SCANCODE_LEFT] ? 3 : 0) + 1;
        scene_animate(&g, frame_no++, scroll, wave_on);
        render_frame(&g, frame);
        /* Each 32-bit word holds two copies of the pixel; the texture wants one 16-bit pixel. */
        static uint16_t px[TG_WIDTH * TG_HEIGHT];
        for (int i = 0; i < TG_WIDTH * TG_HEIGHT; i++)
            px[i] = (uint16_t)frame[i];
        SDL_UpdateTexture(tex, NULL, px, TG_WIDTH * 2);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
    }
    SDL_Quit();
    return 0;
}
