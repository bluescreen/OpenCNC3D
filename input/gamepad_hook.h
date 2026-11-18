/*
 * gamepad_hook.h -- forced into every translation unit of the program (app/build.sh,
 * -include), so the game controller is heard on every screen without any screen knowing.
 *
 * Five SDL calls are routed through input/gamepad.c, the same five the Mac shim
 * (tools/mac/gamepad-shim) interposes on a build that was compiled without this:
 *
 *   SDL_PollEvent         reads the pad before every drain of the queue
 *   SDL_GetKeyboardState  the map scrolls off the polled arrows: the right stick's are added
 *   SDL_GetMouseState     a drag box is a held left button: A's is added
 *   SDL_GetModState       force fire / force move from the command ring hold Ctrl / Alt
 *   SDL_GL_SwapWindow     the pad's own screens are drawn over the finished frame
 *
 * The wrappers are defined BEFORE the macros, so inside them the names still mean SDL.
 * input/gamepad.c and input/gamepad_draw.c are compiled without this header.
 */

#ifndef OPENCNC3D_GAMEPAD_HOOK_H
#define OPENCNC3D_GAMEPAD_HOOK_H

#include <SDL.h>
#include <string.h>
#include "gamepad.h"

static inline int gp_hook_PollEvent(SDL_Event *e)
{
    int r;
    gp_frame();
    while ((r = SDL_PollEvent(e)) && gp_filter_event(e)) {
    }
    return r;
}

static inline const Uint8 *gp_hook_GetKeyboardState(int *numkeys)
{
    static Uint8 copy[SDL_NUM_SCANCODES];
    int n = 0;
    const Uint8 *real = SDL_GetKeyboardState(&n);
    if (n > SDL_NUM_SCANCODES)
        n = SDL_NUM_SCANCODES;
    memcpy(copy, real, (size_t)n);
    gp_keyboard_overlay(copy, n);
    if (numkeys)
        *numkeys = n;
    return copy;
}

static inline Uint32 gp_hook_GetMouseState(int *x, int *y)
{
    return SDL_GetMouseState(x, y) | gp_mouse_buttons();
}

static inline SDL_Keymod gp_hook_GetModState(void)
{
    return (SDL_Keymod)(SDL_GetModState() | gp_mod_state());
}

static inline void gp_hook_GL_SwapWindow(SDL_Window *w)
{
    gp_draw(w);
    SDL_GL_SwapWindow(w);
}

static inline void gp_hook_StartTextInput(void)
{
    gp_text_input(1);
    SDL_StartTextInput();
}

static inline void gp_hook_StopTextInput(void)
{
    gp_text_input(0);
    SDL_StopTextInput();
}

#define SDL_PollEvent gp_hook_PollEvent
#define SDL_StartTextInput gp_hook_StartTextInput
#define SDL_StopTextInput gp_hook_StopTextInput
#define SDL_GetKeyboardState gp_hook_GetKeyboardState
#define SDL_GetMouseState gp_hook_GetMouseState
#define SDL_GetModState gp_hook_GetModState
#define SDL_GL_SwapWindow gp_hook_GL_SwapWindow

#endif
