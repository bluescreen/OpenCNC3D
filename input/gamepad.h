/*
 * gamepad.h -- a game controller that plays the game the way a mouse and keyboard do.
 *
 * WHY IT SPEAKS MOUSE. Every screen in this program -- the DOS menu, the Enhanced menu,
 * the lobby, the sidebar, the tactical map, the pause dialog -- already answers SDL mouse
 * and keyboard events, and the map scrolls off the polled arrow keys and the polled left
 * button. A pad that produces exactly those works on every screen at once, and no screen
 * has to learn that a pad exists. The stick moves the real pointer (SDL_WarpMouseInWindow,
 * which also updates SDL's own idea of where the mouse is), the buttons become clicks and
 * key presses, and what the game POLLS rather than receives is laid over SDL's answer by
 * the two overlay functions below.
 *
 * TWO WAYS IN, ONE FILE OF LOGIC.
 *   - tools/mac/gamepad-shim/ loads this into an installed build that was compiled without
 *     it, by interposing SDL_PollEvent, SDL_GetKeyboardState and SDL_GetMouseState.
 *   - A build that compiles it in calls gp_frame() before draining its event queue,
 *     gp_mouse_buttons() wherever it reads the held-button mask, and
 *     gp_keyboard_overlay() over the keyboard state it polls.
 *
 * THE LAYOUT is the Switch Pro Controller's, by LABEL (SDL reports Nintendo pads by their
 * printed labels, measured with input/gamepad_probe.c), so A confirms and B cancels the
 * way they do on the console:
 *
 *   left stick   pointer; pressed in (L3), the precise cursor on/off
 *   right stick  scroll the map (the arrow keys)
 *   A            left button: select, order, drag a box while held
 *   B            right button: deselect, cancel
 *   X / Y        S stop / G guard
 *   R tap        A attack-move
 *   R hold       the command ring: right stick picks, release gives the order, B cancels
 *   d-pad        in a mission: control groups 1-4 (up, right, down, left); a tap
 *                selects, a double tap jumps the camera there, L held with it stores
 *                the selection. In menus: the arrow keys.
 *   ZL / ZR      mouse wheel down / up: zoom out / in, or scroll the sidebar under the pointer
 *   +            ESC: pause, options, back out of a menu
 *   -            M radar
 *   R3           X scatter (go to base is on the command ring)
 *   Capture      the controller settings panel (speeds, dead zone), saved per user
 *
 * The N64 original is where the ring comes from: there R pressed once, twice or three
 * times turned the cursor into repair, sell or guard (the US manual, pages 9 and 11).
 * The ring is that cycle laid out at once.
 *
 * Environment, read once:
 *   OPENCNC3D_GAMEPAD=0        switch it off
 *   OPENCNC3D_GAMEPAD_SPEED=f  an extra pointer speed multiplier on top of the setting
 *   OPENCNC3D_GAMEPAD_DEBUG=1  one GAMEPAD| line on stderr per pad, button and click
 */

#ifndef OPENCNC3D_GAMEPAD_H
#define OPENCNC3D_GAMEPAD_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read the pad and turn it into pointer movement and pushed events. Cheap to call as
   often as the caller likes: it does its work at most once every few milliseconds, so it
   can sit in front of every SDL_PollEvent of a drain loop without flooding the queue. It
   opens the controller subsystem itself on the first call, once SDL is up. */
void gp_frame(void);

/* The buttons the pad is holding, as an SDL_BUTTON() mask, to OR into
   SDL_GetMouseState's answer. */
Uint32 gp_mouse_buttons(void);

/* Lay the keys the pad is holding over a copy of SDL_GetKeyboardState's array. Call it
   once per read: the analogue scroll is a duty cycle counted in reads. */
void gp_keyboard_overlay(Uint8 *state, int numkeys);

/* Show the button layout picture on its own, from a program's options page. A pad
   button, a mouse click or a key closes it. */
void gp_show_layout(void);

/* 1 when the pad's own screen takes this event and the program must not see it (a
   click or key closing the layout picture). Call it on every event SDL_PollEvent
   hands back, and drop the event when it answers 1. */
int gp_filter_event(const SDL_Event *e);

/* The pointer speed, 1 (slowest) to 10, from a program that shows it on its own
   options page. The settings panel then leaves that one value to the program. */
void gp_set_cursor_speed(int step);

/* Ctrl or Alt a command-ring order is holding, to OR into SDL_GetModState's answer. */
SDL_Keymod gp_mod_state(void);

/* Draw the pad's own screens (settings panel, command ring, the arrival note) over the
   finished frame. Call it just before SDL_GL_SwapWindow, with that window's context
   current. */
void gp_draw(SDL_Window *w);

/* ---- the build ring ---------------------------------------------------------------
 *
 * ZL held opens a ring of what the sidebar can build, one category at a time; a tap of
 * ZL is still one notch of zoom out. The N64 opened its sidebar on Z (US manual p. 7);
 * this is that sidebar laid around the screen centre so a stick can reach every item.
 *
 * The pad knows nothing about production. A program that has a sidebar hands over these
 * hooks, and every action goes back through them as the click on that cameo would, so
 * the ring cannot build anything the sidebar could not. Without hooks ZL is the wheel.
 */

enum { GP_BUILD_STRUCTURES, GP_BUILD_INFANTRY, GP_BUILD_VEHICLES, GP_BUILD_AIR, GP_BUILD_NCAT };

enum {
    GP_BI_AVAILABLE, /* can be started                                   */
    GP_BI_BUILDING,  /* under construction, `progress` says how far       */
    GP_BI_HOLD,      /* paused                                           */
    GP_BI_READY,     /* finished: a structure waits to be placed, a special to be fired */
    GP_BI_BUSY       /* another item of the same factory is in the way    */
};

typedef struct {
    char label[40];
    unsigned int tex; /* a GL texture holding the cameo, 0 for none        */
    float u, v;       /* the cameo's extent inside that texture, 0..1      */
    float progress;   /* 0..1                                              */
    int state;        /* GP_BI_*                                           */
    int cost;
    int queued;       /* more of it waiting behind the one building        */
} GpBuildItem;

typedef struct {
    int (*active)(void);                       /* a mission with a sidebar is being played */
    int (*count)(int cat);
    int (*item)(int cat, int index, GpBuildItem *out);
    void (*activate)(int cat, int index);      /* the left click on that cameo  */
    void (*cancel)(int cat, int index);        /* the right click: hold, then cancel */
    int (*credits)(void);
    /* how many of the player's units are in control group `slot` (0 = key 1); NULL or
       -1 means unknown, and the ring then shows no count */
    int (*group_size)(int slot);
    /* 1 when something the player can act on (a unit, a building, an enemy) is under
       window point (x, y), 0 when it is bare ground; NULL means no magnetism */
    int (*cursor_over)(int x, int y);
} GpBuildHooks;

/* The program asks for typed text (SDL_StartTextInput) or stops (SDL_StopTextInput):
   while it does, + is Return rather than Escape, so a name entry can be confirmed. */
void gp_text_input(int on);

void gp_set_build_hooks(const GpBuildHooks *hooks);

#ifdef __cplusplus
}
#endif

#endif
