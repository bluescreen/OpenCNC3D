/*
 * gamepad_preview.c -- render the controller's screens to PNG for layout work.
 *
 *   cc -std=gnu99 -DGP_PREVIEW -DGL_SILENCE_DEPRECATION -Iinput -Ivideo \
 *      input/gamepad_preview.c input/gamepad.c input/gamepad_draw.c input/gamepad_mac.m \
 *      video/pngwrite.c -fobjc-arc $(sdl2-config --cflags --libs) -lz \
 *      -framework OpenGL -framework GameController -framework Foundation -o input/gamepad_preview
 *   input/gamepad_preview OUT_PREFIX [W H]   -> OUT_PREFIX-panel.png, -ring.png, -build.png
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl.h>
#include "pngwrite.h"

void gp_preview(SDL_Window *w, int which);

int main(int argc, char **argv)
{
    static const char *const names[] = {"panel", "ring", "build", "layout", "groups"};
    int W = argc > 3 ? atoi(argv[2]) : 1600, H = argc > 3 ? atoi(argv[3]) : 960, which, y;
    SDL_Window *win;
    if (argc < 2) { fprintf(stderr, "usage: gamepad_preview OUT_PREFIX [W H]\n"); return 2; }
    SDL_Init(SDL_INIT_VIDEO);
    win = SDL_CreateWindow("pad preview", 0, 0, W, H, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GL_CreateContext(win);
    for (which = 0; which < 5; which++) {
        int dw, dh;
        unsigned char *px, *row;
        char path[512];
        SDL_GL_GetDrawableSize(win, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.18f, 0.24f, 0.14f, 1.0f); /* something like grass */
        glClear(GL_COLOR_BUFFER_BIT);
        gp_preview(win, which);
        glFinish();
        px = malloc((size_t)dw * dh * 4);
        row = malloc((size_t)dw * 4);
        glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, px);
        for (y = 0; y < dh / 2; y++) { /* GL is bottom-up */
            memcpy(row, px + (size_t)y * dw * 4, (size_t)dw * 4);
            memcpy(px + (size_t)y * dw * 4, px + (size_t)(dh - 1 - y) * dw * 4, (size_t)dw * 4);
            memcpy(px + (size_t)(dh - 1 - y) * dw * 4, row, (size_t)dw * 4);
        }
        snprintf(path, sizeof path, "%s-%s.png", argv[1], names[which]);
        printf("%s %dx%d %s\n", path, dw, dh, png_write_rgba(path, px, dw, dh) == 0 ? "ok" : "written");
        free(px);
        free(row);
    }
    SDL_Quit();
    return 0;
}
