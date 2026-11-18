/*
 * shim.c -- input/gamepad.c, loaded into an installed OpenCNC3D that was built without it.
 *
 * dyld's interposing: the table in __DATA,__interpose tells dyld that every call another
 * image makes to SDL_PollEvent, SDL_GetKeyboardState or SDL_GetMouseState goes to the
 * function beside it here instead. Calls made from THIS image are not redirected, so the
 * replacements below reach the real SDL simply by calling it.
 *
 * It needs the target to be ad-hoc signed without the hardened runtime, which is how the
 * project packages its Mac builds (codesign -dv shows flags=0x2(adhoc)); a build with the
 * hardened runtime ignores DYLD_INSERT_LIBRARIES and simply starts without the pad.
 */

#include "../../../input/gamepad.h"

#include <string.h>

#define GP_INTERPOSE(replacement, original)                                                  \
    __attribute__((used)) static const struct {                                             \
        const void *r, *o;                                                                   \
    } gp_interpose_##original __attribute__((section("__DATA,__interpose"))) = {            \
        (const void *)(unsigned long)&replacement, (const void *)(unsigned long)&original}

/* Every drain loop in the program starts here, so the pad is read wherever the program
   happens to be: menu, lobby, map, dialog. */
static int gp_poll_event(SDL_Event *e)
{
    int r;
    gp_frame();
    while ((r = SDL_PollEvent(e)) && gp_filter_event(e)) {
    }
    return r;
}

/* The map scrolls off the polled arrow keys. The caller gets a copy with the pad's
   arrows laid over it, refreshed on every call, which is how often the program reads it. */
static const Uint8 *gp_get_keyboard_state(int *numkeys)
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

/* A drag box is a left button that is still down: the pad's A has to show in the mask. */
static Uint32 gp_get_mouse_state(int *x, int *y)
{
    return SDL_GetMouseState(x, y) | gp_mouse_buttons();
}

/* Force fire and force move are an order click with Ctrl or Alt held, read from here
   when the button comes up. */
static SDL_Keymod gp_get_mod_state(void)
{
    return (SDL_Keymod)(SDL_GetModState() | gp_mod_state());
}

/* The pad's own screens go over the finished frame, just before it is shown. */
static void gp_gl_swap_window(SDL_Window *w)
{
    gp_draw(w);
    SDL_GL_SwapWindow(w);
}

/* A name entry wants Return: + becomes it while the program is asking for text. */
static void gp_start_text_input(void)
{
    gp_text_input(1);
    SDL_StartTextInput();
}

static void gp_stop_text_input(void)
{
    gp_text_input(0);
    SDL_StopTextInput();
}

GP_INTERPOSE(gp_poll_event, SDL_PollEvent);
GP_INTERPOSE(gp_start_text_input, SDL_StartTextInput);
GP_INTERPOSE(gp_stop_text_input, SDL_StopTextInput);
GP_INTERPOSE(gp_get_keyboard_state, SDL_GetKeyboardState);
GP_INTERPOSE(gp_get_mouse_state, SDL_GetMouseState);
GP_INTERPOSE(gp_get_mod_state, SDL_GetModState);
GP_INTERPOSE(gp_gl_swap_window, SDL_GL_SwapWindow);
