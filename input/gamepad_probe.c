/*
 * gamepad_probe.c -- print what SDL makes of a game controller, button by button.
 *
 * The first step of controller support: before any mapping is written, find out which
 * SDL button a physical button on the pad really is. Nintendo pads are the reason. Their
 * A is on the RIGHT and B at the BOTTOM, the opposite of an Xbox pad, and SDL2 and SDL3
 * disagree about whether SDL's "A" means the button labelled A or the button in the
 * Xbox A position. The game runs SDL2's API on SDL3 (sdl2-compat), so the answer has to
 * be measured with the game's own libraries, not assumed.
 *
 * Build and run against the SDL the installed game ships:
 *
 *   cc -O2 -Wall input/gamepad_probe.c $(sdl2-config --cflags --libs) -o input/gamepad_probe
 *   DYLD_LIBRARY_PATH=~/Applications/OpenCNC3D input/gamepad_probe
 *
 * It needs no window. Every line is one fact: a pad arriving or leaving, a button going
 * down or up, an axis crossing a quarter of its travel. Ctrl+C ends it.
 */

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

static const char *type_name(SDL_GameControllerType t)
{
    switch (t) {
    case SDL_CONTROLLER_TYPE_XBOX360: return "Xbox 360";
    case SDL_CONTROLLER_TYPE_XBOXONE: return "Xbox One";
    case SDL_CONTROLLER_TYPE_PS3: return "PS3";
    case SDL_CONTROLLER_TYPE_PS4: return "PS4";
    case SDL_CONTROLLER_TYPE_PS5: return "PS5";
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return "Nintendo Switch Pro";
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return "Joy-Con (L)";
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return "Joy-Con (R)";
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "Joy-Con pair";
    case SDL_CONTROLLER_TYPE_VIRTUAL: return "virtual";
    case SDL_CONTROLLER_TYPE_UNKNOWN: return "unknown";
    default: return "other";
    }
}

static void open_pad(int index)
{
    SDL_GameController *gc;
    char *mapping;
    SDL_version v;

    if (!SDL_IsGameController(index)) {
        printf("PAD|skip|index=%d|joystick=%s|no game controller mapping\n", index,
               SDL_JoystickNameForIndex(index));
        return;
    }
    gc = SDL_GameControllerOpen(index);
    if (!gc) {
        printf("PAD|fail|index=%d|%s\n", index, SDL_GetError());
        return;
    }
    SDL_GetVersion(&v);
    printf("PAD|open|index=%d|name=%s|type=%s|vendor=%04x|product=%04x|sdl=%d.%d.%d\n", index,
           SDL_GameControllerName(gc), type_name(SDL_GameControllerGetType(gc)),
           SDL_GameControllerGetVendor(gc), SDL_GameControllerGetProduct(gc), v.major, v.minor,
           v.patch);
    mapping = SDL_GameControllerMapping(gc);
    if (mapping) {
        printf("PAD|mapping|%s\n", mapping);
        SDL_free(mapping);
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    SDL_Event e;
    int i, quit = 0;
    Sint16 last_axis[SDL_CONTROLLER_AXIS_MAX] = {0};

    (void)argc;
    (void)argv;
    /* A terminal program never has keyboard focus, and SDL drops pad input for an
       application without it unless told otherwise. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    printf("PROBE|ready|joysticks=%d|press each button once: A B X Y, L R ZL ZR, + -, "
           "sticks, d-pad. Ctrl+C ends.\n", SDL_NumJoysticks());
    fflush(stdout);

    while (!quit && SDL_WaitEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            quit = 1;
            break;
        case SDL_CONTROLLERDEVICEADDED:
            open_pad(e.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            printf("PAD|removed|instance=%d\n", e.cdevice.which);
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
            printf("BTN|%s|sdl=%s|id=%d\n", e.type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up",
                   SDL_GameControllerGetStringForButton((SDL_GameControllerButton)e.cbutton.button),
                   e.cbutton.button);
            break;
        case SDL_CONTROLLERAXISMOTION: {
            /* One line per quarter of travel, not one per sample. */
            int a = e.caxis.axis, now = e.caxis.value / 8192, was = last_axis[a] / 8192;
            if (a >= 0 && a < SDL_CONTROLLER_AXIS_MAX && now != was) {
                printf("AXIS|sdl=%s|value=%d\n",
                       SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)a), e.caxis.value);
                last_axis[a] = e.caxis.value;
            }
            break;
        }
        default:
            break;
        }
        fflush(stdout);
    }
    for (i = 0; i < SDL_NumJoysticks(); i++) { /* closed by SDL_Quit */ }
    SDL_Quit();
    return 0;
}
