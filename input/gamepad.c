/*
 * gamepad.c -- see gamepad.h.
 *
 * Three screens of its own, all drawn by gp_draw over the finished frame:
 *   - a TOAST, a few seconds when a pad arrives, saying where the two screens are;
 *   - the SETTINGS panel on the Capture button: pointer speed, precision speed, map
 *     scroll speed, dead zone, saved in the program's per-user folder;
 *   - the COMMAND RING on a held R: eight orders around the pointer, picked with the right
 *     stick, given on release. A quick tap of R is still attack-move.
 * While either the panel or the ring is up, the pad talks to it and to nothing else.
 */

#include "gamepad.h"
#include "gamepad_draw.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- tuning ---------------------------------------------------------------------- */

#define GP_PUMP_MS        4      /* at most one pass this often                        */
#define GP_SCAN_MS        1000   /* look for a pad this often while none is open        */
#define GP_SCROLL_DEAD    0.22f  /* the scroll stick starts scrolling beyond this       */
#define GP_TRIGGER_ON     16000  /* ZL/ZR count as pressed beyond this                  */
#define GP_CURVE          2.2f   /* tilt^curve: small tilts are slow and exact          */
#define GP_REPEAT_FIRST   380    /* ms before a held d-pad or trigger starts repeating  */
#define GP_REPEAT_KEY     70     /* ms between d-pad repeats                            */
#define GP_REPEAT_PANEL   110    /* ms between repeats inside the settings panel        */
#define GP_REPEAT_WHEEL   110    /* ms between wheel notches while a trigger is held    */
#define GP_DOUBLE_MS      400    /* a second A this soon and this close is a double     */
#define GP_DOUBLE_PX      6      /* click, which the menus' lists open an entry on      */
#define GP_RING_HOLD_MS   220    /* R held this long opens the ring; shorter is a tap   */
#define GP_RING_PICK      0.50f  /* the right stick picks a slice beyond this tilt      */
#define GP_MOD_MS         150    /* a ring order's Ctrl/Alt stays down this long        */
#define GP_TOAST_MS       5000
#define GP_GROUP_DOUBLE_MS 350   /* a second tap of the same d-pad group this soon jumps */
#define GP_MSG_MS         1400   /* how long "GROUP 2 - 6 UNITS" stays up              */
#define GP_BUILD_PAGE     12     /* items one ring shows; the d-pad pages past that     */

/* ---- settings -------------------------------------------------------------------- */

enum { GP_SET_SPEED, GP_SET_SLOW, GP_SET_SCROLL, GP_SET_DEAD, GP_SET_MAGNET, GP_NSET };

static const struct {
    const char *key, *label;
    int def;
} gp_set_info[GP_NSET] = {
    {"cursor_speed", "CURSOR SPEED", 5},
    {"precision_speed", "PRECISE CURSOR (L3)", 3},
    {"scroll_speed", "MAP SCROLL (R STICK)", 10},
    {"dead_zone", "STICK DEAD ZONE", 4},
    {"cursor_magnetism", "CURSOR MAGNETISM", 7},
};
#define GP_SET_MIN 1
#define GP_SET_MAX 10

/* Panel rows: the settings, then two buttons. */
enum { GP_ROW_LAYOUT = GP_NSET, GP_ROW_RESET, GP_ROW_CLOSE, GP_NROWS };

/* Every value is a step from 1 to 10; these turn a step into what it means. */
static float gp_speed_of(int v) { return 0.40f * powf(1.25f, (float)(v - 1)); }  /* x window widths/s */
static float gp_slow_of(int v) { return 0.10f * (float)v; }                       /* x full speed      */
static float gp_scroll_of(int v) { return 0.10f * (float)v; }                     /* x the game's pan  */
static float gp_dead_of(int v) { return 0.04f + 0.03f * (float)v; }               /* of full tilt      */
/* over something: 1 is off, 10 nearly stops the pointer on it (x speed) */
static float gp_magnet_of(int v) { return 1.0f - 0.075f * (float)(v - 1); }

/* ---- the command ring ------------------------------------------------------------ */

enum { GP_ACT_KEY, GP_ACT_CTRL_CLICK, GP_ACT_ALT_CLICK };

typedef struct {
    const char *label;
    int kind;
    SDL_Keycode key;
} GpRingItem;

/* Clockwise from the top. The keys are the game's own bindings (game/cnc_eyes.cpp, the
   in-mission key switch): A attack-move, S stop, G guard, X scatter, H base, D rebuild
   the last thing built. Ctrl and Alt on an order click are force fire and force move. */
static const GpRingItem gp_ring[] = {
    {"ATTACK", GP_ACT_KEY, SDLK_a},
    {"STOP", GP_ACT_KEY, SDLK_s},
    {"GUARD", GP_ACT_KEY, SDLK_g},
    {"SCATTER", GP_ACT_KEY, SDLK_x},
    {"FORCE FIRE", GP_ACT_CTRL_CLICK, 0},
    {"FORCE MOVE", GP_ACT_ALT_CLICK, 0},
    {"REBUILD", GP_ACT_KEY, SDLK_d},
    {"BASE", GP_ACT_KEY, SDLK_h},
};
#define GP_NRING ((int)(sizeof gp_ring / sizeof gp_ring[0]))

/* ---- state ----------------------------------------------------------------------- */

enum { GP_DPAD_UP, GP_DPAD_DOWN, GP_DPAD_LEFT, GP_DPAD_RIGHT, GP_NDPAD };

typedef struct {
    SDL_GameControllerButton button;
    SDL_Keycode key;
} GpKeyBind;

/* Buttons that are one key press each. R is not here: it is the ring, and a tap of it
   is attack-move. */
static const GpKeyBind gp_keys[] = {
    {SDL_CONTROLLER_BUTTON_X, SDLK_s},            /* stop              */
    {SDL_CONTROLLER_BUTTON_Y, SDLK_g},            /* guard             */
    {SDL_CONTROLLER_BUTTON_START, SDLK_ESCAPE},   /* pause, back       */
    {SDL_CONTROLLER_BUTTON_BACK, SDLK_m},         /* radar             */
    {SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDLK_x},   /* scatter           */
};
#define GP_NKEYS ((int)(sizeof gp_keys / sizeof gp_keys[0]))

static const SDL_GameControllerButton gp_dpad_button[GP_NDPAD] = {
    SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT};
static const SDL_Keycode gp_dpad_key[GP_NDPAD] = {SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT};
static const SDL_Scancode gp_arrow_scan[GP_NDPAD] = {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN,
                                                    SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT};

static struct {
    int ready;            /* subsystem opened                     */
    int off;              /* OPENCNC3D_GAMEPAD=0                  */
    int debug;
    float speed_env;      /* OPENCNC3D_GAMEPAD_SPEED              */
    int set[GP_NSET];
    int host_speed;       /* 1..10 when the program's own options own the pointer speed */
    char cfg_path[1024];
    SDL_GameController *pad;
    Uint32 last_pump, last_scan;
    Uint32 win_id;        /* the window the last events went to   */

    /* the pointer, in window coordinates, with the fraction a frame did not reach */
    float px, py;
    int warped_x, warped_y, have_warp;

    Uint32 mouse_mask;    /* what A and B hold                    */
    int a_down, b_down;
    Uint32 a_last_time;
    int a_last_x, a_last_y;

    int key_down[GP_NKEYS];
    SDL_Keycode key_sent[GP_NKEYS]; /* what each press sent, so its release matches */
    int text_on;          /* the program is asking for typed text */
    int dpad_down[GP_NDPAD];
    Uint32 dpad_next[GP_NDPAD];
    int trig_down[2];     /* ZL, ZR                               */
    Uint32 trig_next[2];

    float scroll_duty[GP_NDPAD]; /* 0..1, how much of the time an arrow is held */
    float scroll_acc[GP_NDPAD];

    SDL_Keymod mod;       /* Ctrl/Alt a ring order is holding     */
    Uint32 mod_until;

    /* the panel */
    int panel;
    int row;
    int pbtn[GP_NDPAD];
    Uint32 pnext[GP_NDPAD];
    int pa, pb, pcap, pstart;
    int layout;           /* the button layout picture is up, over the panel */
    int layout_only;      /* ...opened on its own, from the program's options: closing
                             it closes everything, and a mouse or key closes it too */
    int eat_mouse_up, eat_key_up;
    int swallow;          /* ignore the pad until every button is up */

    /* the ring */
    int r_down;
    Uint32 r_time;
    int ring;
    float ring_x, ring_y;
    int ring_pick;        /* -1 none                              */
    int ring_b;
    int ring_tab;         /* 0 commands (R), 1 control groups (L) */
    int ring_owner;       /* the shoulder button holding it open   */
    int l_down;
    Uint32 l_time;
    int ring_a, ring_ybtn;
    int slow, l3_down;    /* the precise cursor, toggled by L3     */
    int gdpad[GP_NDPAD];  /* the d-pad as group keys, in a mission */
    int glast_slot;
    Uint32 glast_time;
    char msg[48];         /* a short note at the bottom, e.g. GROUP 2 STORED */
    Uint32 msg_until;

    /* the build ring */
    const GpBuildHooks *bh;
    int zl_down;
    Uint32 zl_time;
    int bring;            /* open                                 */
    int bcat, bpage, bpick;
    int bacted;           /* A gave an order in this hold: the release does not repeat it */
    int ba, bb, bdpad[GP_NDPAD];

    int cap_down;
    Uint32 toast_until;
} gp;

void gp_text_input(int on)
{
    gp.text_on = on;
}

void gp_set_build_hooks(const GpBuildHooks *hooks)
{
    gp.bh = hooks;
}

static int gp_build_live(void)
{
    return gp.bh && gp.bh->active && gp.bh->active();
}

static void gp_log(const char *fmt, ...)
{
    va_list ap;
    if (!gp.debug)
        return;
    va_start(ap, fmt);
    fputs("GAMEPAD|", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---- settings file --------------------------------------------------------------- */

static int gp_clamp_set(int v)
{
    return v < GP_SET_MIN ? GP_SET_MIN : v > GP_SET_MAX ? GP_SET_MAX : v;
}

static void gp_defaults(void)
{
    int i;
    for (i = 0; i < GP_NSET; i++)
        gp.set[i] = gp_set_info[i].def;
    if (gp.host_speed)
        gp.set[GP_SET_SPEED] = gp.host_speed;
}

/* A program with the pad compiled in carries the pointer speed on its own Game Controls
   page and hands it over here; from then on the panel leaves that row to the program. */
void gp_set_cursor_speed(int step)
{
    gp.host_speed = gp_clamp_set(step);
    gp.set[GP_SET_SPEED] = gp.host_speed;
}

/* The panel's rows, minus the speed row when the program owns it. */
static int gp_row_shown(int row)
{
    return !(row == GP_SET_SPEED && gp.host_speed);
}

/* gamepad.cfg in the folder SDL_GetPrefPath names for this program: one key=value per
   line. Unknown lines are ignored and a missing file is the defaults. */
static void gp_load(void)
{
    char *pref = SDL_GetPrefPath("Slipgate Ironworks", "OpenCNC3D");
    FILE *f;
    char line[256];
    gp_defaults();
    if (!pref)
        return;
    snprintf(gp.cfg_path, sizeof gp.cfg_path, "%sgamepad.cfg", pref);
    SDL_free(pref);
    f = fopen(gp.cfg_path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        int i;
        if (!eq)
            continue;
        *eq = '\0';
        for (i = 0; i < GP_NSET; i++)
            if (!strcmp(line, gp_set_info[i].key))
                gp.set[i] = gp_clamp_set(atoi(eq + 1));
    }
    fclose(f);
    if (gp.host_speed)
        gp.set[GP_SET_SPEED] = gp.host_speed;
}

static void gp_save(void)
{
    char tmp[1100];
    FILE *f;
    int i;
    if (!gp.cfg_path[0])
        return;
    /* A whole new copy renamed over the old one, so a crash cannot leave half a file. */
    snprintf(tmp, sizeof tmp, "%s.tmp", gp.cfg_path);
    f = fopen(tmp, "w");
    if (!f)
        return;
    fputs("# OpenCNC3D game controller settings, steps 1 to 10\n", f);
    for (i = 0; i < GP_NSET; i++)
        fprintf(f, "%s=%d\n", gp_set_info[i].key, gp.set[i]);
    if (fclose(f) == 0)
        rename(tmp, gp.cfg_path);
    gp_log("saved|%s", gp.cfg_path);
}

/* ---- finding the window and the pad ---------------------------------------------- */

/* The window the player is looking at. Nothing happens while the game is not the
   active application: a pad in a player's hands must not click on a window behind the
   one they are using. */
static SDL_Window *gp_window(void)
{
    SDL_Window *w = SDL_GetKeyboardFocus();
    if (!w)
        return NULL;
    if (SDL_GetWindowFlags(w) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN))
        return NULL;
    return w;
}

static void gp_close_pad(void)
{
    if (gp.pad) {
        gp_log("pad|closed");
        SDL_GameControllerClose(gp.pad);
        gp.pad = NULL;
    }
}

#ifdef __APPLE__
int gp_mac_quiet_system_buttons(void); /* gamepad_mac.m */
#endif

static void gp_find_pad(Uint32 now)
{
    int i, n;
    if (gp.pad && SDL_GameControllerGetAttached(gp.pad))
        return;
    gp_close_pad();
    if (now - gp.last_scan < GP_SCAN_MS && gp.last_scan)
        return;
    gp.last_scan = now;
    n = SDL_NumJoysticks();
    for (i = 0; i < n; i++) {
        if (!SDL_IsGameController(i))
            continue;
        gp.pad = SDL_GameControllerOpen(i);
        if (gp.pad) {
            gp_log("pad|open|%s", SDL_GameControllerName(gp.pad));
#ifdef __APPLE__
            /* Home must not open the Games app in the middle of a mission */
            gp_log("pad|system buttons kept from macOS: %d", gp_mac_quiet_system_buttons());
#endif
            gp.toast_until = now + GP_TOAST_MS;
            gp.swallow = 1;
            return;
        }
    }
}

static int gp_start(void)
{
    const char *v;
    if (gp.ready)
        return gp.ready > 0;
    /* Not before the program has brought SDL up itself: opening a subsystem first would
       initialise SDL on the program's behalf with whatever it had not yet set. */
    if (!SDL_WasInit(SDL_INIT_VIDEO))
        return 0;
    v = getenv("OPENCNC3D_GAMEPAD");
    gp.off = v && !strcmp(v, "0");
    gp.debug = (v = getenv("OPENCNC3D_GAMEPAD_DEBUG")) && strcmp(v, "0") != 0;
    gp.speed_env = (v = getenv("OPENCNC3D_GAMEPAD_SPEED")) ? (float)atof(v) : 1.0f;
    if (!(gp.speed_env > 0.05f && gp.speed_env < 20.0f))
        gp.speed_env = 1.0f;
    if (gp.off) {
        gp.ready = -1;
        return 0;
    }
    gp_load();
    /* Labels, not positions, on Nintendo pads: A is the right-hand confirm button, as on
       the console. Said explicitly, because the default moved between SDL versions. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "1");
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "GAMEPAD|off|%s\n", SDL_GetError());
        gp.ready = -1;
        return 0;
    }
    gp.ready = 1;
    gp_log("ready|speed=%d|slow=%d|scroll=%d|dead=%d|cfg=%s", gp.set[GP_SET_SPEED],
           gp.set[GP_SET_SLOW], gp.set[GP_SET_SCROLL], gp.set[GP_SET_DEAD], gp.cfg_path);
    return 1;
}

/* ---- the events it pushes -------------------------------------------------------- */

static void gp_push_button(Uint32 wid, Uint8 button, int down, Uint8 clicks)
{
    SDL_Event e;
    int x, y;
    SDL_GetMouseState(&x, &y);
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    e.button.timestamp = SDL_GetTicks();
    e.button.windowID = wid;
    e.button.which = 0;
    e.button.button = button;
    e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.button.clicks = clicks;
    e.button.x = x;
    e.button.y = y;
    SDL_PushEvent(&e);
    if (down)
        gp.mouse_mask |= SDL_BUTTON(button);
    else
        gp.mouse_mask &= ~SDL_BUTTON(button);
    gp_log("click|%s|button=%d|clicks=%d|at=%d,%d", down ? "down" : "up", button, clicks, x, y);
}

static void gp_push_key(Uint32 wid, SDL_Keycode key, int down, int repeat)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.timestamp = SDL_GetTicks();
    e.key.windowID = wid;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.repeat = (Uint8)repeat;
    e.key.keysym.sym = key;
    e.key.keysym.scancode = SDL_GetScancodeFromKey(key);
    e.key.keysym.mod = KMOD_NONE;
    SDL_PushEvent(&e);
    if (!repeat)
        gp_log("key|%s|%s", down ? "down" : "up", SDL_GetKeyName(key));
}

static void gp_tap_key(Uint32 wid, SDL_Keycode key)
{
    gp_push_key(wid, key, 1, 0);
    gp_push_key(wid, key, 0, 0);
}

/* A control group key: the game reads the PHYSICAL top-row position (the scancode), so
   it is set explicitly rather than looked up from the keycode, which on AZERTY is not a
   digit at all. `mod` is held in the polled state while the press is read: Ctrl assigns,
   Alt selects and jumps the camera, nothing selects (game/cnc_eyes.cpp, the number row). */
static void gp_group_key(Uint32 wid, int slot, SDL_Keymod mod)
{
    SDL_Event e;
    int k;
    if (mod) {
        gp.mod = mod;
        gp.mod_until = SDL_GetTicks() + GP_MOD_MS;
    }
    for (k = 0; k < 2; k++) {
        memset(&e, 0, sizeof e);
        e.type = k ? SDL_KEYUP : SDL_KEYDOWN;
        e.key.timestamp = SDL_GetTicks();
        e.key.windowID = wid;
        e.key.state = k ? SDL_RELEASED : SDL_PRESSED;
        e.key.keysym.scancode = (SDL_Scancode)(SDL_SCANCODE_1 + slot);
        e.key.keysym.sym = (SDL_Keycode)(SDLK_1 + slot);
        e.key.keysym.mod = (Uint16)mod;
        SDL_PushEvent(&e);
    }
    gp_log("group|%s|%d", mod == KMOD_LCTRL ? "assign" : mod == KMOD_LALT ? "jump" : "select", slot + 1);
}

static void gp_push_wheel(Uint32 wid, int dy)
{
    SDL_Event e;
    int x, y;
    SDL_GetMouseState(&x, &y);
    memset(&e, 0, sizeof e);
    e.type = SDL_MOUSEWHEEL;
    e.wheel.timestamp = SDL_GetTicks();
    e.wheel.windowID = wid;
    e.wheel.which = 0;
    e.wheel.x = 0;
    e.wheel.y = dy;
    e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    e.wheel.preciseX = 0.0f;
    e.wheel.preciseY = (float)dy;
    e.wheel.mouseX = x;
    e.wheel.mouseY = y;
    SDL_PushEvent(&e);
}

/* Let go of everything: the pad went away, the game lost the foreground, or one of the
   pad's own screens took over. Nothing may stay held behind the player's back -- a left
   button stuck down is a band box that never closes. */
static void gp_release_all(Uint32 w)
{
    int i;
    if (w) {
        if (gp.a_down)
            gp_push_button(w, SDL_BUTTON_LEFT, 0, 1);
        if (gp.b_down)
            gp_push_button(w, SDL_BUTTON_RIGHT, 0, 1);
        for (i = 0; i < GP_NKEYS; i++)
            if (gp.key_down[i])
                gp_push_key(w, gp.key_sent[i], 0, 0);
        for (i = 0; i < GP_NDPAD; i++)
            if (gp.dpad_down[i])
                gp_push_key(w, gp_dpad_key[i], 0, 0);
    }
    gp.a_down = gp.b_down = 0;
    gp.mouse_mask = 0;
    gp.r_down = 0;
    gp.l_down = 0;
    gp.ring = 0;
    gp.zl_down = 0;
    gp.bring = 0;
    memset(gp.key_down, 0, sizeof gp.key_down);
    memset(gp.dpad_down, 0, sizeof gp.dpad_down);
    memset(gp.trig_down, 0, sizeof gp.trig_down);
    memset(gp.scroll_duty, 0, sizeof gp.scroll_duty);
}

/* ---- reading the pad ------------------------------------------------------------- */

static float gp_axis(SDL_GameControllerAxis a)
{
    return (float)SDL_GameControllerGetAxis(gp.pad, a) / 32767.0f;
}

static int gp_btn(SDL_GameControllerButton b)
{
    return SDL_GameControllerGetButton(gp.pad, b);
}

static int gp_any_pressed(void)
{
    int b;
    for (b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++)
        if (gp_btn((SDL_GameControllerButton)b))
            return 1;
    return SDL_GameControllerGetAxis(gp.pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > GP_TRIGGER_ON ||
           SDL_GameControllerGetAxis(gp.pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > GP_TRIGGER_ON;
}

/* ---- the pointer ----------------------------------------------------------------- */

static void gp_pointer(SDL_Window *w, float dt, int move)
{
    float sx = gp_axis(SDL_CONTROLLER_AXIS_LEFTX), sy = gp_axis(SDL_CONTROLLER_AXIS_LEFTY);
    float mag = sqrtf(sx * sx + sy * sy), dead = gp_dead_of(gp.set[GP_SET_DEAD]), k, v;
    int ww, wh, mx, my, nx, ny;

    SDL_GetWindowSize(w, &ww, &wh);
    SDL_GetMouseState(&mx, &my);
    /* The real mouse moved since the last warp: it is the truth, and the stick carries
       on from where it is. */
    if (!gp.have_warp || mx != gp.warped_x || my != gp.warped_y) {
        gp.px = (float)mx;
        gp.py = (float)my;
        gp.have_warp = 1;
        gp.warped_x = mx;
        gp.warped_y = my;
    }
    if (!move || mag <= dead)
        return;
    if (mag > 1.0f)
        mag = 1.0f;
    /* Rescale past the dead zone so the slowest speed starts at zero, then curve it. */
    k = (mag - dead) / (1.0f - dead);
    v = powf(k, GP_CURVE) * gp_speed_of(gp.set[GP_SET_SPEED]) * gp.speed_env * (float)ww * dt;
    if (gp.slow)
        v *= gp_slow_of(gp.set[GP_SET_SLOW]);
    /* MAGNETISM, the console RTS's oldest trick (Age of Empires II on Xbox calls it that):
       over anything that can be clicked, the pointer slows, so a small tilt that would
       have run across a soldier stops on him instead. Asked of the program where the
       pointer is now, which is the point the next click goes to. */
    if (gp.set[GP_SET_MAGNET] > 1 && gp.bh && gp.bh->cursor_over && gp_build_live()
        && gp.bh->cursor_over(mx, my))
        v *= gp_magnet_of(gp.set[GP_SET_MAGNET]);
    gp.px += sx / mag * v;
    gp.py += sy / mag * v;
    /* Clamped to the window, so the pointer can rest on an edge and scroll the map the
       way a mouse at the edge of the screen does. */
    if (gp.px < 0.0f) gp.px = 0.0f;
    if (gp.py < 0.0f) gp.py = 0.0f;
    if (gp.px > (float)(ww - 1)) gp.px = (float)(ww - 1);
    if (gp.py > (float)(wh - 1)) gp.py = (float)(wh - 1);
    nx = (int)(gp.px + 0.5f);
    ny = (int)(gp.py + 0.5f);
    if (nx != mx || ny != my) {
        SDL_WarpMouseInWindow(w, nx, ny);
        gp.warped_x = nx;
        gp.warped_y = ny;
    }
}

/* ---- the settings panel ---------------------------------------------------------- */

static void gp_panel_open(Uint32 wid)
{
    gp_release_all(wid);
    gp.panel = 1;
    gp.row = gp_row_shown(0) ? 0 : 1;
    gp.toast_until = 0;
    /* What is already held when the panel opens is not a press inside it. */
    gp.pa = gp_btn(SDL_CONTROLLER_BUTTON_A);
    gp.pb = gp_btn(SDL_CONTROLLER_BUTTON_B);
    gp.pstart = gp_btn(SDL_CONTROLLER_BUTTON_START);
    memset(gp.pbtn, 1, sizeof gp.pbtn);
    gp_log("panel|open");
}

static void gp_panel_close(void)
{
    gp.panel = 0;
    gp.layout = 0;
    gp.layout_only = 0;
    gp.swallow = 1;
    gp_save();
    gp_log("panel|closed");
}

static void gp_panel(Uint32 now)
{
    int i, held, a, b, start;
    /* d-pad and left stick both walk it, with repeat */
    float sx = gp_axis(SDL_CONTROLLER_AXIS_LEFTX), sy = gp_axis(SDL_CONTROLLER_AXIS_LEFTY);
    int dir[GP_NDPAD];
    dir[GP_DPAD_UP] = gp_btn(SDL_CONTROLLER_BUTTON_DPAD_UP) || sy < -0.6f;
    dir[GP_DPAD_DOWN] = gp_btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || sy > 0.6f;
    dir[GP_DPAD_LEFT] = gp_btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || sx < -0.6f;
    dir[GP_DPAD_RIGHT] = gp_btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || sx > 0.6f;

    for (i = 0; i < GP_NDPAD; i++) {
        int fire = 0;
        held = dir[i];
        if (held && !gp.pbtn[i]) {
            fire = 1;
            gp.pnext[i] = now + GP_REPEAT_FIRST;
        } else if (held && (Sint32)(now - gp.pnext[i]) >= 0) {
            fire = 1;
            gp.pnext[i] = now + GP_REPEAT_PANEL;
        }
        gp.pbtn[i] = held;
        if (!fire || gp.layout)
            continue;
        if (i == GP_DPAD_UP || i == GP_DPAD_DOWN) {
            int step = i == GP_DPAD_UP ? GP_NROWS - 1 : 1;
            do
                gp.row = (gp.row + step) % GP_NROWS;
            while (!gp_row_shown(gp.row));
        }
        else if (gp.row < GP_NSET)
            gp.set[gp.row] = gp_clamp_set(gp.set[gp.row] + (i == GP_DPAD_RIGHT ? 1 : -1));
    }

    a = gp_btn(SDL_CONTROLLER_BUTTON_A);
    b = gp_btn(SDL_CONTROLLER_BUTTON_B);
    start = gp_btn(SDL_CONTROLLER_BUTTON_START);
    if (gp.layout) {
        /* the picture is only looked at: any of A, B or + goes back to the panel, or
           out altogether when the program's options opened it */
        if ((a && !gp.pa) || (b && !gp.pb) || (start && !gp.pstart)) {
            if (gp.layout_only)
                gp_panel_close();
            else
                gp.layout = 0;
        }
        gp.pa = a;
        gp.pb = b;
        gp.pstart = start;
        return;
    }
    if (a && !gp.pa) {
        if (gp.row == GP_ROW_LAYOUT)
            gp.layout = 1;
        else if (gp.row == GP_ROW_RESET)
            gp_defaults();
        else if (gp.row == GP_ROW_CLOSE)
            gp_panel_close();
    }
    if ((b && !gp.pb) || (start && !gp.pstart))
        gp_panel_close();
    gp.pa = a;
    gp.pb = b;
    gp.pstart = start;
}

/* ---- the command ring ------------------------------------------------------------ */

static void gp_ring_do(Uint32 wid, int i)
{
    const GpRingItem *it = &gp_ring[i];
    gp_log("ring|%s", it->label);
    if (it->kind == GP_ACT_KEY) {
        gp_tap_key(wid, it->key);
        return;
    }
    /* An order click at the pointer with the modifier held while the game reads it: the
       game takes force fire and force move from SDL_GetModState when the left button
       comes up (game/cnc_eyes.cpp, the order button). */
    gp.mod = it->kind == GP_ACT_CTRL_CLICK ? KMOD_LCTRL : KMOD_LALT;
    gp.mod_until = SDL_GetTicks() + GP_MOD_MS;
    gp_push_button(wid, SDL_BUTTON_LEFT, 1, 1);
    gp_push_button(wid, SDL_BUTTON_LEFT, 0, 1);
}

/* One shoulder button's ring: R the commands (a tap is attack-move), L the control groups
   (a tap does nothing). Only one ring is up at a time; the other button is ignored while
   it is. */
static void gp_ring_button(Uint32 wid, Uint32 now, SDL_GameControllerButton btn, int tab,
                           int *down, Uint32 *since)
{
    int held = gp_btn(btn);
    if (gp.ring && gp.ring_owner != (int)btn) {
        *down = held;
        return;
    }
    if (held && !*down) {
        *down = 1;
        *since = now;
        gp.ring_pick = -1;
    } else if (held && !gp.ring && now - *since >= GP_RING_HOLD_MS) {
        int x, y;
        /* Let go of the map's buttons first: the ring is not an order to the units. */
        if (gp.a_down)
            gp_push_button(wid, SDL_BUTTON_LEFT, 0, 1);
        if (gp.b_down)
            gp_push_button(wid, SDL_BUTTON_RIGHT, 0, 1);
        gp.a_down = gp.b_down = 0;
        SDL_GetMouseState(&x, &y);
        gp.ring = 1;
        gp.ring_owner = (int)btn;
        gp.ring_tab = tab;
        gp.ring_x = (float)x;
        gp.ring_y = (float)y;
        gp.ring_b = gp_btn(SDL_CONTROLLER_BUTTON_B);
        gp.ring_a = gp_btn(SDL_CONTROLLER_BUTTON_A);
        gp.ring_ybtn = gp_btn(SDL_CONTROLLER_BUTTON_Y);
        gp.toast_until = 0;
        gp_log("ring|open|at=%d,%d|%s", x, y, tab ? "groups" : "commands");
    } else if (!held && *down) {
        *down = 0;
        if (gp.ring) {
            gp.ring = 0;
            if (gp.ring_pick >= 0 && gp.ring_tab == 1)
                gp_group_key(wid, gp.ring_pick, KMOD_NONE);
            else if (gp.ring_pick >= 0)
                gp_ring_do(wid, gp.ring_pick);
            else
                gp_log("ring|nothing picked");
            gp.swallow = 1;
        } else if (tab == 0) {
            gp_tap_key(wid, SDLK_a); /* the tap of R: attack-move, as before */
        }
    }
}

/* Returns 1 while a ring owns the pad. */
static int gp_ring_update(Uint32 wid, Uint32 now)
{
    gp_ring_button(wid, now, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0, &gp.r_down, &gp.r_time);
    if (!gp.ring)
        return 0;
    /* The pick follows the stick and STAYS on the last slice it pointed at, so letting
       the stick spring back before releasing the button does not lose the choice. */
    {
        float rx = gp_axis(SDL_CONTROLLER_AXIS_RIGHTX), ry = gp_axis(SDL_CONTROLLER_AXIS_RIGHTY);
        int a = gp_btn(SDL_CONTROLLER_BUTTON_A), b = gp_btn(SDL_CONTROLLER_BUTTON_B);
        int y = gp_btn(SDL_CONTROLLER_BUTTON_Y);
        if (sqrtf(rx * rx + ry * ry) > GP_RING_PICK) {
            float ang = atan2f(rx, -ry); /* clockwise from up */
            int i = (int)floorf(ang / (6.2831853f / (float)GP_NRING) + 0.5f);
            gp.ring_pick = (i % GP_NRING + GP_NRING) % GP_NRING;
        }
        if (b && !gp.ring_b) {
            gp.ring = 0;
            gp.ring_pick = -1;
            gp.swallow = 1;
            gp_log("ring|cancelled");
        }
        /* the groups ring: A stores the selection in the picked group and keeps the ring
           up, Y selects it and jumps the camera there, letting go of L selects it */
        if (gp.ring && gp.ring_tab == 1 && gp.ring_pick >= 0) {
            if (a && !gp.ring_a)
                gp_group_key(wid, gp.ring_pick, KMOD_LCTRL);
            if (y && !gp.ring_ybtn) {
                gp_group_key(wid, gp.ring_pick, KMOD_LALT);
                gp.ring = 0;
                gp.ring_pick = -1;
                gp.swallow = 1;
            }
        }
        gp.ring_a = a;
        gp.ring_b = b;
        gp.ring_ybtn = y;
    }
    return gp.ring;
}

/* ---- the build ring -------------------------------------------------------------- */

static int gp_build_count(int cat)
{
    return gp.bh && gp.bh->count ? gp.bh->count(cat) : 0;
}

/* How many slots the current page has, and the item index of a slot. */
static int gp_build_slots(void)
{
    int n = gp_build_count(gp.bcat) - gp.bpage * GP_BUILD_PAGE;
    if (n > GP_BUILD_PAGE) n = GP_BUILD_PAGE;
    return n < 0 ? 0 : n;
}

static void gp_build_category(int dir)
{
    int k;
    /* Walk to the next category that has something in it; stay if none has. */
    for (k = 1; k <= GP_BUILD_NCAT; k++) {
        int c = ((gp.bcat + dir * k) % GP_BUILD_NCAT + GP_BUILD_NCAT) % GP_BUILD_NCAT;
        if (gp_build_count(c) > 0) {
            gp.bcat = c;
            break;
        }
    }
    gp.bpage = 0;
    gp.bpick = -1;
}

static void gp_build_open(Uint32 wid)
{
    if (gp.a_down) gp_push_button(wid, SDL_BUTTON_LEFT, 0, 1);
    if (gp.b_down) gp_push_button(wid, SDL_BUTTON_RIGHT, 0, 1);
    gp.a_down = gp.b_down = 0;
    gp.bring = 1;
    gp.bpick = -1;
    gp.bacted = 0;
    gp.toast_until = 0;
    if (gp_build_count(gp.bcat) <= 0)
        gp_build_category(+1);
    if (gp.bpage * GP_BUILD_PAGE >= gp_build_count(gp.bcat))
        gp.bpage = 0;
    gp.ba = gp_btn(SDL_CONTROLLER_BUTTON_A);
    gp.bb = gp_btn(SDL_CONTROLLER_BUTTON_B);
    memset(gp.bdpad, 1, sizeof gp.bdpad);
    gp_log("build|open|cat=%d", gp.bcat);
}

static void gp_build_act(int cancel)
{
    int idx = gp.bpage * GP_BUILD_PAGE + gp.bpick;
    if (gp.bpick < 0 || idx >= gp_build_count(gp.bcat))
        return;
    gp_log("build|%s|cat=%d|item=%d", cancel ? "cancel" : "activate", gp.bcat, idx);
    if (cancel) {
        if (gp.bh->cancel) gp.bh->cancel(gp.bcat, idx);
    } else if (gp.bh->activate) {
        gp.bh->activate(gp.bcat, idx);
    }
}

/* ZL: a tap is one notch of zoom out, a hold is the ring. Returns 1 while the ring owns
   the pad. */
static int gp_build_update(Uint32 wid, Uint32 now)
{
    int held, i;
    if (!gp_build_live()) {
        gp.bring = 0;
        gp.zl_down = 0;
        return 0;
    }
    held = SDL_GameControllerGetAxis(gp.pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > GP_TRIGGER_ON;
    if (held && !gp.zl_down) {
        gp.zl_down = 1;
        gp.zl_time = now;
    } else if (held && !gp.bring && now - gp.zl_time >= GP_RING_HOLD_MS) {
        gp_build_open(wid);
    } else if (!held && gp.zl_down) {
        gp.zl_down = 0;
        if (gp.bring) {
            gp.bring = 0;
            if (!gp.bacted)
                gp_build_act(0);
            gp.swallow = 1;
            gp_log("build|closed");
        } else {
            gp_push_wheel(wid, -1);
        }
        return 0;
    }
    if (!gp.bring)
        return 0;

    /* the d-pad: left/right the category, up/down the page */
    {
        int dir[GP_NDPAD];
        for (i = 0; i < GP_NDPAD; i++)
            dir[i] = gp_btn(gp_dpad_button[i]);
        for (i = 0; i < GP_NDPAD; i++) {
            if (dir[i] && !gp.bdpad[i]) {
                int pages = (gp_build_count(gp.bcat) + GP_BUILD_PAGE - 1) / GP_BUILD_PAGE;
                if (i == GP_DPAD_LEFT) gp_build_category(-1);
                else if (i == GP_DPAD_RIGHT) gp_build_category(+1);
                else if (pages > 1) {
                    gp.bpage = (gp.bpage + (i == GP_DPAD_DOWN ? 1 : pages - 1)) % pages;
                    gp.bpick = -1;
                }
            }
            gp.bdpad[i] = dir[i];
        }
    }
    /* the right stick picks a slot, and the pick stays where it last pointed */
    {
        float rx = gp_axis(SDL_CONTROLLER_AXIS_RIGHTX), ry = gp_axis(SDL_CONTROLLER_AXIS_RIGHTY);
        int n = gp_build_slots(), slots = n < 4 ? 4 : n;
        if (n > 0 && sqrtf(rx * rx + ry * ry) > GP_RING_PICK) {
            float a = atan2f(rx, -ry);
            int k = (int)floorf(a / (6.2831853f / (float)slots) + 0.5f);
            k = (k % slots + slots) % slots;
            if (k < n && k != gp.bpick) {
                gp.bpick = k;
                gp.bacted = 0;
            }
        }
        if (gp.bpick >= n)
            gp.bpick = -1;
    }
    /* A orders it now and keeps the ring up, so five tanks are five presses; B is the
       sidebar's right click on it: hold, then cancel. */
    {
        int a = gp_btn(SDL_CONTROLLER_BUTTON_A), b = gp_btn(SDL_CONTROLLER_BUTTON_B);
        if (a && !gp.ba) {
            gp_build_act(0);
            gp.bacted = 1;
        }
        if (b && !gp.bb) {
            gp_build_act(1);
            gp.bacted = 1;
        }
        gp.ba = a;
        gp.bb = b;
    }
    return 1;
}

/* ---- buttons in play ------------------------------------------------------------- */

static void gp_buttons(Uint32 w, Uint32 now)
{
    int i, held;

    /* L3, pressing the left stick in, switches the precise cursor on and off */
    held = gp_btn(SDL_CONTROLLER_BUTTON_LEFTSTICK);
    if (held && !gp.l3_down) {
        gp.slow = !gp.slow;
        gp_log("cursor|%s", gp.slow ? "precise" : "normal");
    }
    gp.l3_down = held;

    held = gp_btn(SDL_CONTROLLER_BUTTON_A);
    if (held != gp.a_down) {
        Uint8 clicks = 1;
        if (held) {
            int x, y;
            SDL_GetMouseState(&x, &y);
            if (gp.a_last_time && now - gp.a_last_time < GP_DOUBLE_MS &&
                abs(x - gp.a_last_x) <= GP_DOUBLE_PX && abs(y - gp.a_last_y) <= GP_DOUBLE_PX)
                clicks = 2;
            gp.a_last_time = clicks == 2 ? 0 : now;
            gp.a_last_x = x;
            gp.a_last_y = y;
        }
        gp.a_down = held;
        gp_push_button(w, SDL_BUTTON_LEFT, held, clicks);
    }
    held = gp_btn(SDL_CONTROLLER_BUTTON_B);
    if (held != gp.b_down) {
        gp.b_down = held;
        gp_push_button(w, SDL_BUTTON_RIGHT, held, 1);
    }

    for (i = 0; i < GP_NKEYS; i++) {
        held = gp_btn(gp_keys[i].button);
        if (held != gp.key_down[i]) {
            gp.key_down[i] = held;
            if (held) {
                /* + is Escape, except while the program waits for typed text: then it
                   is Return, the only way to confirm a name entry from the pad */
                gp.key_sent[i] = gp_keys[i].button == SDL_CONTROLLER_BUTTON_START && gp.text_on
                                     ? SDLK_RETURN : gp_keys[i].key;
            }
            gp_push_key(w, gp.key_sent[i], held, 0);
        }
    }

    /* IN A MISSION THE D-PAD IS THE CONTROL GROUPS, the N64's C buttons: up, right, down
       and left are groups 1 to 4. A tap selects the group, a second tap of the same one
       straight after selects it and jumps the camera there, and L held with it stores
       the current selection in it. Everywhere else it is the arrow keys. */
    if (gp_build_live()) {
        static const int slot_of[GP_NDPAD] = {0, 2, 3, 1}; /* up, down, left, right */
        for (i = 0; i < GP_NDPAD; i++) {
            held = gp_btn(gp_dpad_button[i]);
            if (gp.dpad_down[i]) { /* an arrow held from a menu: let it go */
                gp_push_key(w, gp_dpad_key[i], 0, 0);
                gp.dpad_down[i] = 0;
            }
            if (held && !gp.gdpad[i]) {
                const int slot = slot_of[i];
                int size;
                if (gp_btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) {
                    gp_group_key(w, slot, KMOD_LCTRL);
                    snprintf(gp.msg, sizeof gp.msg, "GROUP %d STORED", slot + 1);
                    gp.glast_time = 0;
                } else if (slot == gp.glast_slot && gp.glast_time
                           && now - gp.glast_time < GP_GROUP_DOUBLE_MS) {
                    gp_group_key(w, slot, KMOD_LALT);
                    snprintf(gp.msg, sizeof gp.msg, "GROUP %d", slot + 1);
                    gp.glast_time = 0;
                } else {
                    gp_group_key(w, slot, KMOD_NONE);
                    size = gp.bh && gp.bh->group_size ? gp.bh->group_size(slot) : -1;
                    if (size == 0)
                        snprintf(gp.msg, sizeof gp.msg, "GROUP %d IS EMPTY - L + D-PAD STORES", slot + 1);
                    else if (size > 0)
                        snprintf(gp.msg, sizeof gp.msg, "GROUP %d - %d UNIT%s", slot + 1, size, size == 1 ? "" : "S");
                    else
                        snprintf(gp.msg, sizeof gp.msg, "GROUP %d", slot + 1);
                    gp.glast_slot = slot;
                    gp.glast_time = now;
                }
                gp.msg_until = now + GP_MSG_MS;
            }
            gp.gdpad[i] = held;
        }
    } else
    /* The d-pad repeats like a held key, so a long list can be walked. */
    for (i = 0; i < GP_NDPAD; i++) {
        held = gp_btn(gp_dpad_button[i]);
        if (held && !gp.dpad_down[i]) {
            gp_push_key(w, gp_dpad_key[i], 1, 0);
            gp.dpad_next[i] = now + GP_REPEAT_FIRST;
        } else if (held && (Sint32)(now - gp.dpad_next[i]) >= 0) {
            gp_push_key(w, gp_dpad_key[i], 1, 1);
            gp.dpad_next[i] = now + GP_REPEAT_KEY;
        } else if (!held && gp.dpad_down[i]) {
            gp_push_key(w, gp_dpad_key[i], 0, 0);
        }
        gp.dpad_down[i] = held;
    }

    /* ZL and ZR are a wheel that keeps turning while held. In a mission ZL belongs to
       the build ring instead (gp_build_update), and a tap of it is one notch. */
    for (i = gp_build_live() ? 1 : 0; i < 2; i++) {
        SDL_GameControllerAxis a = i ? SDL_CONTROLLER_AXIS_TRIGGERRIGHT : SDL_CONTROLLER_AXIS_TRIGGERLEFT;
        held = SDL_GameControllerGetAxis(gp.pad, a) > GP_TRIGGER_ON;
        if (held && !gp.trig_down[i]) {
            gp_push_wheel(w, i ? +1 : -1);
            gp.trig_next[i] = now + GP_REPEAT_FIRST;
            gp_log("wheel|%s", i ? "up" : "down");
        } else if (held && (Sint32)(now - gp.trig_next[i]) >= 0) {
            gp_push_wheel(w, i ? +1 : -1);
            gp.trig_next[i] = now + GP_REPEAT_WHEEL;
        }
        gp.trig_down[i] = held;
    }

    /* The right stick holds arrow keys in the polled state only -- no key events, which
       a menu would read as navigation. The game pans at one fixed speed while an arrow
       is down, so a light tilt holds the arrow for only part of the frames: that is
       what makes the scroll analogue, and the setting scales it. */
    {
        float rx = gp_axis(SDL_CONTROLLER_AXIS_RIGHTX), ry = gp_axis(SDL_CONTROLLER_AXIS_RIGHTY);
        float top = gp_scroll_of(gp.set[GP_SET_SCROLL]);
        float v[GP_NDPAD];
        v[GP_DPAD_UP] = -ry;
        v[GP_DPAD_DOWN] = ry;
        v[GP_DPAD_LEFT] = -rx;
        v[GP_DPAD_RIGHT] = rx;
        for (i = 0; i < GP_NDPAD; i++) {
            float d = (v[i] - GP_SCROLL_DEAD) / (1.0f - GP_SCROLL_DEAD);
            gp.scroll_duty[i] = d <= 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d) * top;
        }
    }
}

/* ---- one pass -------------------------------------------------------------------- */

void gp_frame(void)
{
    static Uint64 last_counter;
    Uint32 now;
    Uint64 counter;
    float dt;
    SDL_Window *w;
    int cap;

    if (!gp_start())
        return;
    now = SDL_GetTicks();
    if (gp.last_pump && now - gp.last_pump < GP_PUMP_MS)
        return;
    gp.last_pump = now;
    if (gp.mod && (Sint32)(now - gp.mod_until) >= 0)
        gp.mod = KMOD_NONE;

    counter = SDL_GetPerformanceCounter();
    dt = last_counter ? (float)((double)(counter - last_counter) / (double)SDL_GetPerformanceFrequency())
                      : 0.0f;
    last_counter = counter;
    if (dt > 0.1f)
        dt = 0.1f; /* a stalled frame must not throw the pointer across the screen */

    gp_find_pad(now);
    w = gp_window();
    if (!gp.pad || !w) {
        static int was_idle = -1;
        int idle = !gp.pad ? 1 : 2;
        if (idle != was_idle)
            gp_log("idle|%s", idle == 1 ? "no pad" : "game window not focused");
        was_idle = idle;
        /* To the window that had the presses, so it sees them end. */
        gp_release_all(gp.win_id);
        if (!gp.layout_only) /* the picture opened from the options needs no pad */
            gp.panel = 0;
        gp.have_warp = 0;
        return;
    }
    if (gp.win_id != SDL_GetWindowID(w)) {
        gp_release_all(gp.win_id);
        gp.win_id = SDL_GetWindowID(w);
    }

    /* Capture opens and closes the settings panel from anywhere. */
    cap = gp_btn(SDL_CONTROLLER_BUTTON_MISC1);
    if (cap && !gp.cap_down && !gp.swallow) {
        if (gp.panel)
            gp_panel_close();
        else
            gp_panel_open(gp.win_id);
    }
    gp.cap_down = cap;

    if (gp.panel) {
        gp_pointer(w, dt, 0);
        gp_panel(now);
        return;
    }
    /* After the panel or the ring, nothing counts until every button is up again, so the
       B that closed the panel is not also a right click on the map. */
    if (gp.swallow) {
        if (gp_any_pressed()) {
            gp_pointer(w, dt, 1);
            return;
        }
        gp.swallow = 0;
        gp_log("input|live");
    }
    if (!gp.bring && gp_ring_update(gp.win_id, now)) {
        memset(gp.scroll_duty, 0, sizeof gp.scroll_duty);
        gp_pointer(w, dt, 0); /* the ring is anchored where it opened */
        return;
    }
    if (gp.swallow) /* the ring just closed */
        return;
    if (!gp.ring && gp_build_update(gp.win_id, now)) {
        memset(gp.scroll_duty, 0, sizeof gp.scroll_duty);
        gp_pointer(w, dt, 0);
        return;
    }
    if (gp.swallow) /* the build ring just closed */
        return;
    gp_pointer(w, dt, 1);
    gp_buttons(gp.win_id, now);
}

void gp_show_layout(void)
{
    SDL_Window *w = SDL_GetKeyboardFocus();
    if (!w)
        w = SDL_GetMouseFocus();
    if (!gp_start())
        return;
    if (w)
        gp.win_id = SDL_GetWindowID(w);
    gp_release_all(gp.win_id);
    gp.panel = gp.layout = gp.layout_only = 1;
    gp.toast_until = 0;
    if (gp.pad) {
        /* the A that pressed the menu's button is not also the press that closes it */
        gp.pa = gp_btn(SDL_CONTROLLER_BUTTON_A);
        gp.pb = gp_btn(SDL_CONTROLLER_BUTTON_B);
        gp.pstart = gp_btn(SDL_CONTROLLER_BUTTON_START);
        memset(gp.pbtn, 1, sizeof gp.pbtn);
    }
    gp_log("layout|open");
}

int gp_filter_event(const SDL_Event *e)
{
    /* the release of the click or key that closed the picture belongs to it too */
    if (gp.eat_mouse_up && e->type == SDL_MOUSEBUTTONUP) {
        gp.eat_mouse_up = 0;
        return 1;
    }
    if (gp.eat_key_up && e->type == SDL_KEYUP) {
        gp.eat_key_up = 0;
        return 1;
    }
    if (!(gp.panel && gp.layout_only))
        return 0;
    switch (e->type) {
    case SDL_MOUSEBUTTONDOWN:
        gp_panel_close();
        gp.eat_mouse_up = 1;
        return 1;
    case SDL_KEYDOWN:
        if (!e->key.repeat) {
            gp_panel_close();
            gp.eat_key_up = 1;
        }
        return 1;
    case SDL_MOUSEBUTTONUP:
    case SDL_KEYUP:
    case SDL_MOUSEWHEEL:
    case SDL_TEXTINPUT:
        return 1;
    default:
        return 0;
    }
}

Uint32 gp_mouse_buttons(void)
{
    return gp.mouse_mask;
}

SDL_Keymod gp_mod_state(void)
{
    return gp.mod;
}

void gp_keyboard_overlay(Uint8 *state, int numkeys)
{
    int i;
    for (i = 0; i < GP_NDPAD; i++) {
        int on = gp.dpad_down[i] && !gp.panel && !gp.ring && !gp.bring;
        /* One read a frame, which is how often the game polls: the accumulator turns a
           duty of 0.4 into the arrow being down on two frames out of five. */
        if (!on && gp.scroll_duty[i] > 0.0f) {
            gp.scroll_acc[i] += gp.scroll_duty[i];
            if (gp.scroll_acc[i] >= 1.0f) {
                gp.scroll_acc[i] -= 1.0f;
                on = 1;
            }
        } else if (gp.scroll_duty[i] <= 0.0f) {
            gp.scroll_acc[i] = 0.0f;
        }
        if (on && (int)gp_arrow_scan[i] < numkeys)
            state[gp_arrow_scan[i]] = 1;
    }
}

/* ---- drawing --------------------------------------------------------------------- */

static const GpColor GP_SHADE = {0.0f, 0.0f, 0.0f, 0.55f};
static const GpColor GP_PLATE = {0.07f, 0.08f, 0.07f, 0.94f};
static const GpColor GP_EDGE = {0.55f, 0.60f, 0.52f, 1.0f};
static const GpColor GP_TEXT = {0.92f, 0.94f, 0.90f, 1.0f};
static const GpColor GP_DIM = {0.60f, 0.64f, 0.58f, 1.0f};
static const GpColor GP_HOT = {0.20f, 0.78f, 0.38f, 1.0f};
static const GpColor GP_HOT_TEXT = {0.02f, 0.06f, 0.03f, 1.0f};
static const GpColor GP_SLOT = {0.22f, 0.25f, 0.22f, 1.0f};
static const GpColor GP_SEG = {0.08f, 0.10f, 0.08f, 0.86f};

static float gp_ui_px(float h)
{
    float px = floorf(h / 300.0f + 0.5f);
    return px < 2.0f ? 2.0f : px;
}

/* Text centred on (cx, cy) inside a box `maxw` wide: on one line if it fits, else broken
   at the space nearest its middle, and only then made smaller, a whole font pixel at a
   time so it stays crisp. A ring slot is narrow and a label like FORCE FIRE is not. */
/* The font size gp_text_fit would settle on, so a set of labels can share one size. */
static float gp_fit_px(float maxw, float px, const char *s)
{
    const char *sp = NULL, *p;
    size_t n = strlen(s), half = n / 2;
    for (p = s; *p; p++)
        if (*p == ' ' && (!sp || labs((long)(p - s) - (long)half) < labs((long)(sp - s) - (long)half)))
            sp = p;
    for (; px > 1.0f; px -= 1.0f) {
        float w = gpd_text_width(px, s);
        if (w <= maxw)
            return px;
        if (sp) {
            float a = (float)(sp - s), b = (float)(n - (size_t)(sp - s) - 1);
            w = (6.0f * (a > b ? a : b) - 1.0f) * px;
            if (w <= maxw)
                return px;
        }
    }
    return 1.0f;
}

static void gp_text_fit(float cx, float cy, float maxw, float px, const char *s, GpColor c)
{
    char a[64], b[64];
    const char *sp = NULL, *p;
    size_t n = strlen(s), half = n / 2;
    float w;
    for (p = s; *p; p++)
        if (*p == ' ' && (!sp || labs((long)(p - s) - (long)half) < labs((long)(sp - s) - (long)half)))
            sp = p;
    if (gpd_text_width(px, s) <= maxw || !sp) {
        while (px > 1.0f && gpd_text_width(px, s) > maxw)
            px -= 1.0f;
        gpd_text_centered(cx, cy - 3.5f * px, px, s, c);
        return;
    }
    snprintf(a, sizeof a, "%.*s", (int)(sp - s), s);
    snprintf(b, sizeof b, "%s", sp + 1);
    w = gpd_text_width(px, a) > gpd_text_width(px, b) ? gpd_text_width(px, a) : gpd_text_width(px, b);
    while (px > 1.0f && w > maxw) {
        px -= 1.0f;
        w = gpd_text_width(px, a) > gpd_text_width(px, b) ? gpd_text_width(px, a) : gpd_text_width(px, b);
    }
    gpd_text_centered(cx, cy - 8.0f * px, px, a, c);
    gpd_text_centered(cx, cy + 1.0f * px, px, b, c);
}

/* One line of text, centred, made smaller rather than wrapped when it does not fit. */
static void gp_text_line(float cx, float y, float maxw, float px, const char *s, GpColor c)
{
    while (px > 1.0f && gpd_text_width(px, s) > maxw)
        px -= 1.0f;
    gpd_text_centered(cx, y, px, s, c);
}

static void gp_draw_panel(float W, float H)
{
    float px = gp_ui_px(H), lh = 13.0f * px, pw = 70.0f * 6.0f * px * 0.62f, ph, x, y;
    int i, shown = 0;
    char buf[32];
    if (pw > W - 20.0f)
        pw = W - 20.0f;
    for (i = 0; i < GP_NROWS; i++)
        shown += gp_row_shown(i);
    ph = lh * ((float)shown + 4.2f);
    x = floorf((W - pw) * 0.5f);
    y = floorf((H - ph) * 0.5f);
    gpd_rect(0, 0, W, H, GP_SHADE);
    gpd_rect(x, y, pw, ph, GP_PLATE);
    gpd_frame(x, y, pw, ph, px, GP_EDGE);
    gpd_text_centered(x + pw * 0.5f, y + lh * 0.7f, px * 1.3f, "CONTROLLER SETTINGS", GP_TEXT);

    for (i = 0, shown = 0; i < GP_NROWS; i++) {
        float ry, tx = x + 3.0f * px * 6.0f * 0.5f;
        int sel = i == gp.row;
        if (!gp_row_shown(i))
            continue;
        ry = y + lh * (2.2f + (float)shown++);
        if (sel)
            gpd_rect(x + px * 2, ry - px * 3, pw - px * 4, lh - px, (GpColor){0.20f, 0.78f, 0.38f, 0.16f});
        if (i < GP_NSET) {
            float bx = x + pw * 0.56f, bw = (pw * 0.34f) / (float)GP_SET_MAX;
            int s;
            gpd_text(tx, ry, px, gp_set_info[i].label, sel ? GP_TEXT : GP_DIM);
            for (s = 0; s < GP_SET_MAX; s++)
                gpd_rect(bx + (float)s * bw, ry, bw - px, 7.0f * px,
                         s < gp.set[i] ? (sel ? GP_HOT : GP_EDGE) : GP_SLOT);
            if (i == GP_SET_MAGNET && gp.set[i] == 1)
                snprintf(buf, sizeof buf, "OFF");
            else
                snprintf(buf, sizeof buf, "%d", gp.set[i]);
            gpd_text(bx + bw * GP_SET_MAX + px * 4, ry, px, buf, sel ? GP_TEXT : GP_DIM);
        } else {
            gpd_text(tx, ry, px, i == GP_ROW_LAYOUT ? "BUTTON LAYOUT..." : i == GP_ROW_RESET ? "RESET TO DEFAULTS" : "CLOSE",
                     sel ? GP_TEXT : GP_DIM);
        }
    }
    gpd_text_centered(x + pw * 0.5f, y + ph - lh * 1.1f, px * 0.85f > 1.0f ? floorf(px * 0.85f) : 1.0f,
                      "D-PAD: CHOOSE   LEFT/RIGHT: CHANGE   A: OK   B: CLOSE", GP_DIM);
}

/* The label a ring slot carries: a command, or "GROUP n" on the groups page. */
static const char *gp_ring_label(int i, char *buf, size_t n)
{
    if (gp.ring_tab == 0)
        return gp_ring[i].label;
    snprintf(buf, n, "GROUP %d", i + 1);
    return buf;
}

static void gp_draw_ring(float W, float H)
{
    float px = gp_ui_px(H), r1 = H * 0.30f, r0 = r1 * 0.34f, cx = gp.ring_x, cy = gp.ring_y;
    float step = 6.2831853f / (float)GP_NRING, gap = 0.025f, lpx, slotw, tabh = 13.0f * px;
    int i, groups = gp.ring_tab == 1;
    char buf[48];
    /* Kept whole on screen even when opened at an edge, the tabs over it and the hint
       under it included. */
    if (cx < r1 + 4) cx = r1 + 4;
    if (cy < r1 + tabh + 8) cy = r1 + tabh + 8;
    if (cx > W - r1 - 4) cx = W - r1 - 4;
    if (cy > H - r1 - tabh - 8) cy = H - r1 - tabh - 8;
    lpx = px;
    slotw = 2.0f * ((r0 + r1) * 0.5f) * sinf(step * 0.5f) * 0.92f;
    for (i = 0; i < GP_NRING; i++) {
        float f = gp_fit_px(slotw, px, gp_ring_label(i, buf, sizeof buf));
        if (f < lpx) lpx = f;
    }

    /* which ring this is, over it */
    {
        const char *t = groups ? "CONTROL GROUPS" : "COMMANDS";
        float tw = gpd_text_width(px, t) + 10.0f * px;
        gpd_rect(cx - tw * 0.5f, cy - r1 - tabh - 4.0f * px, tw, 11.0f * px, GP_PLATE);
        gpd_text_centered(cx, cy - r1 - tabh - 2.0f * px, px, t, GP_HOT);
    }

    for (i = 0; i < GP_NRING; i++) {
        float a = (float)i * step, mid = (r0 + r1) * 0.5f, sx = cx + sinf(a) * mid, sy = cy - cosf(a) * mid;
        int sel = i == gp.ring_pick, size = -1;
        gpd_sector(cx, cy, r0, r1, a - step * 0.5f + gap, a + step * 0.5f - gap, sel ? GP_HOT : GP_SEG);
        if (groups && gp.bh && gp.bh->group_size)
            size = gp.bh->group_size(i);
        if (groups) {
            /* a group slot: its number, big, and under it how many are in it */
            const GpColor c = sel ? GP_HOT_TEXT : (size == 0 ? GP_DIM : GP_TEXT);
            snprintf(buf, sizeof buf, "%d", i + 1);
            gpd_text_centered(sx, sy - (size >= 0 ? 11.0f : 7.0f) * px, px * 2.0f, buf, c);
            if (size >= 0) {
                snprintf(buf, sizeof buf, size == 0 ? "EMPTY" : size == 1 ? "1 UNIT" : "%d UNITS", size);
                gp_text_line(sx, sy + 6.0f * px, slotw, px > 2.0f ? px - 1.0f : px, buf,
                             sel ? GP_HOT_TEXT : GP_DIM);
            }
        } else {
            /* the slot's width at the middle radius, less a margin, is all the label gets */
            gp_text_fit(sx, sy, slotw, lpx, gp_ring_label(i, buf, sizeof buf), sel ? GP_HOT_TEXT : GP_TEXT);
        }
    }
    gpd_disc(cx, cy, r0 - 3.0f * px, GP_PLATE);
    gp_text_fit(cx, cy - 4.0f * px, (r0 - 3.0f * px) * 1.7f, px,
                gp.ring_pick >= 0 ? gp_ring_label(gp.ring_pick, buf, sizeof buf) : groups ? "GROUPS" : "COMMANDS",
                gp.ring_pick >= 0 ? GP_HOT : GP_TEXT);
    gpd_text_centered(cx, cy + 6.0f * px, px > 2.0f ? px - 1.0f : px,
                      gp.ring_pick >= 0 ? (groups ? "RELEASE L" : "RELEASE R") : "R STICK", GP_DIM);

    /* what the buttons do on this page, under the ring */
    gp_text_line(cx, cy + r1 + 5.0f * px, W * 0.94f, px > 2.0f ? px - 1.0f : px,
                 groups ? "RELEASE L: SELECT   A: STORE SELECTION   Y: JUMP THERE   B: CANCEL"
                        : "RELEASE R: GIVE ORDER   B: CANCEL",
                 GP_DIM);
}

static const char *const gp_build_cat_name[GP_BUILD_NCAT] = {"STRUCTURES", "INFANTRY",
                                                              "VEHICLES", "AIRCRAFT"};

static void gp_draw_build(float W, float H)
{
    float px = gp_ui_px(H), cx = W * 0.5f, cy = H * 0.49f, r1 = H * 0.37f, r0 = r1 * 0.40f;
    int n = gp_build_slots(), slots = n < 4 ? 4 : n, i, total = gp_build_count(gp.bcat);
    float step = 6.2831853f / (float)slots, gap = 0.018f, tabw, tx, lpx = px;
    char buf[64];
    GpBuildItem it;

    gpd_rect(0, 0, W, H, (GpColor){0.0f, 0.0f, 0.0f, 0.35f});

    /* the category tabs over the ring */
    tabw = 0.0f;
    for (i = 0; i < GP_BUILD_NCAT; i++)
        tabw += gpd_text_width(px, gp_build_cat_name[i]) + 14.0f * px;
    tx = cx - tabw * 0.5f;
    for (i = 0; i < GP_BUILD_NCAT; i++) {
        float w = gpd_text_width(px, gp_build_cat_name[i]) + 10.0f * px, ty = cy - r1 - 22.0f * px;
        int sel = i == gp.bcat, empty = gp_build_count(i) <= 0;
        gpd_rect(tx, ty, w, 11.0f * px, sel ? GP_HOT : GP_PLATE);
        gpd_text(tx + 5.0f * px, ty + 2.0f * px, px, gp_build_cat_name[i],
                 sel ? GP_HOT_TEXT : (empty ? GP_SLOT : GP_TEXT));
        tx += w + 4.0f * px;
    }

    /* every slot's cameo is the same size, so one label size fits all of them */
    {
        float mid = (r0 + r1) * 0.5f, chord = 2.0f * mid * sinf(step * 0.5f) * 0.78f;
        float ch = (r1 - r0) * 0.58f, cw = ch * 4.0f / 3.0f;
        if (cw > chord) cw = chord;
        for (i = 0; i < n; i++)
            if (gp.bh->item(gp.bcat, gp.bpage * GP_BUILD_PAGE + i, &it) && !it.tex) {
                float f = gp_fit_px(cw * 0.92f, px, it.label);
                if (f < lpx) lpx = f;
            }
    }
    for (i = 0; i < slots; i++) {
        float a = (float)i * step, mid = (r0 + r1) * 0.5f, sx = cx + sinf(a) * mid, sy = cy - cosf(a) * mid;
        int sel = i == gp.bpick;
        gpd_sector(cx, cy, r0, r1, a - step * 0.5f + gap, a + step * 0.5f - gap,
                   sel ? GP_HOT : GP_SEG);
        if (i >= n || !gp.bh->item(gp.bcat, gp.bpage * GP_BUILD_PAGE + i, &it))
            continue;
        {
            /* the cameo, as big as the slot allows, 4:3 like the sidebar's */
            float chord = 2.0f * mid * sinf(step * 0.5f) * 0.78f, ch = (r1 - r0) * 0.58f, cw = ch * 4.0f / 3.0f;
            float dim = it.state == GP_BI_BUSY ? 0.35f : 1.0f;
            if (cw > chord) { cw = chord; ch = cw * 0.75f; }
            if (it.tex)
                gpd_image(sx - cw * 0.5f, sy - ch * 0.5f, cw, ch, it.tex, it.u, it.v,
                          (GpColor){dim, dim, dim, 1.0f});
            else {
                gpd_rect(sx - cw * 0.5f, sy - ch * 0.5f, cw, ch, GP_SLOT);
                gp_text_fit(sx, sy - ch * 0.12f, cw * 0.92f, lpx, it.label, GP_TEXT);
            }
            gpd_frame(sx - cw * 0.5f, sy - ch * 0.5f, cw, ch, px > 2.0f ? px - 1.0f : 1.0f,
                      sel ? GP_HOT_TEXT : GP_EDGE);
            /* what it is doing: a progress bar under it, READY / HOLD over it */
            if (it.state == GP_BI_BUILDING || it.state == GP_BI_HOLD) {
                gpd_rect(sx - cw * 0.5f, sy + ch * 0.5f + px, cw, 2.0f * px, GP_SLOT);
                gpd_rect(sx - cw * 0.5f, sy + ch * 0.5f + px, cw * it.progress, 2.0f * px,
                         it.state == GP_BI_HOLD ? GP_DIM : GP_HOT);
            }
            if (it.state == GP_BI_READY || it.state == GP_BI_HOLD) {
                const char *t = it.state == GP_BI_READY ? "READY" : "HOLD";
                float bpx = px > 2.0f ? px - 1.0f : px, tw = gpd_text_width(bpx, t) + 4.0f * bpx;
                float by = sy + ch * 0.5f - 9.0f * bpx;
                gpd_rect(sx - tw * 0.5f, by, tw, 9.0f * bpx, it.state == GP_BI_READY ? GP_HOT : GP_PLATE);
                gpd_text_centered(sx, by + bpx, bpx, t, it.state == GP_BI_READY ? GP_HOT_TEXT : GP_DIM);
            }
            if (it.queued > 0) {
                snprintf(buf, sizeof buf, "+%d", it.queued);
                gpd_text(sx + cw * 0.5f - gpd_text_width(px, buf) - px, sy - ch * 0.5f + px, px, buf, GP_TEXT);
            }
        }
    }

    /* the hub: the picked item, or how to use the ring */
    gpd_disc(cx, cy, r0 - 3.0f * px, GP_PLATE);
    if (gp.bpick >= 0 && gp.bh->item(gp.bcat, gp.bpage * GP_BUILD_PAGE + gp.bpick, &it)) {
        static const char *const st[] = {"", "BUILDING", "ON HOLD", "READY", "FACTORY BUSY"};
        gp_text_fit(cx, cy - 14.0f * px, (r0 - 3.0f * px) * 1.7f, px, it.label, GP_HOT);
        snprintf(buf, sizeof buf, "$%d", it.cost);
        gpd_text_centered(cx, cy - 2.0f * px, px, buf, GP_TEXT);
        if (it.state == GP_BI_BUILDING)
            snprintf(buf, sizeof buf, "BUILDING %d%%", (int)(it.progress * 100.0f + 0.5f));
        else
            snprintf(buf, sizeof buf, "%s", st[it.state >= 0 && it.state <= GP_BI_BUSY ? it.state : 0]);
        gpd_text_centered(cx, cy + 8.0f * px, px, buf, it.state == GP_BI_BUSY ? GP_DIM : GP_TEXT);
    } else if (n == 0) {
        gpd_text_centered(cx, cy - 3.5f * px, px, "NOTHING TO BUILD", GP_DIM);
    } else {
        gpd_text_centered(cx, cy - 8.0f * px, px, "BUILD", GP_TEXT);
        gpd_text_centered(cx, cy + 2.0f * px, px, "R STICK", GP_DIM);
    }
    {
        char page[48] = "";
        if (total > GP_BUILD_PAGE)
            snprintf(page, sizeof page, "     PAGE %d/%d: D-PAD UP/DOWN", gp.bpage + 1,
                     (total + GP_BUILD_PAGE - 1) / GP_BUILD_PAGE);
        snprintf(buf, sizeof buf, "CREDITS $%d%s", gp.bh->credits ? gp.bh->credits() : 0, page);
        gp_text_fit(cx, cy + r1 + 9.0f * px, W * 0.94f, px, buf, GP_TEXT);
    }
    gp_text_fit(cx, cy + r1 + 20.0f * px, W * 0.94f, px > 2.0f ? px - 1.0f : px,
                "A: BUILD   B: HOLD/CANCEL   D-PAD: CATEGORY   RELEASE ZL: BUILD AND CLOSE", GP_DIM);
}

/* ---- the button layout picture --------------------------------------------------- */

/* A Pro Controller in plain shapes, every control with a line to what it does here. The
   words follow the program: with a sidebar to build from, ZL is the build ring. */
typedef struct {
    float x, y;        /* the control, in controller units (0..1 across) */
    float r;           /* its radius: the line stops at its edge, so the letter shows */
    const char *name;
    const char *what;
} GpLayoutTag;

/* The width of one row of a column: the name, a gap, what it does. */
static float gp_row_w(float px, const GpLayoutTag *t)
{
    return gpd_text_width(px, t->name) + 6.0f * px + gpd_text_width(px, t->what);
}

/* THE PRO CONTROLLER, drawn. Units are the pad's width; (0,0) is its top-left. The body
   is one silhouette made of overlapping round shapes -- two lobes, a centre plate and two
   grips swept down and out -- drawn three times: a soft shadow, a light rim, the face. */
static void gp_pad_silhouette(float ox, float oy, float k, float grow, GpColor c)
{
    int i;
#define PX(v) (ox + (v) * k)
#define PY(v) (oy + (v) * k)
    gpd_round_rect(PX(0.16f) - grow, PY(0.075f) - grow, 0.68f * k + 2 * grow, 0.27f * k + 2 * grow,
                   0.09f * k + grow, c);
    gpd_ellipse(PX(0.235f), PY(0.205f), 0.135f * k + grow, 0.128f * k + grow, c);
    gpd_ellipse(PX(0.765f), PY(0.205f), 0.135f * k + grow, 0.128f * k + grow, c);
    /* the grips: a chain of discs, thinning as they go down and out */
    for (i = 0; i <= 10; i++) {
        float t = (float)i / 10.0f, r = (0.125f - 0.030f * t) * k + grow;
        float lx = 0.245f - 0.075f * t, y = 0.24f + 0.27f * t;
        gpd_disc(PX(lx), PY(y), r, c);
        gpd_disc(PX(1.0f - lx), PY(y), r, c);
    }
    /* the bridge between the grips' tops, under the face plate */
    gpd_round_rect(PX(0.30f) - grow, PY(0.25f) - grow, 0.40f * k + 2 * grow, 0.14f * k + 2 * grow,
                   0.06f * k + grow, c);
#undef PX
#undef PY
}

static void gp_draw_pad(float ox, float oy, float k, float tpx)
{
    const GpColor shadow = {0.0f, 0.0f, 0.0f, 0.45f}, rim = {0.36f, 0.38f, 0.40f, 1.0f};
    const GpColor face = {0.15f, 0.16f, 0.17f, 1.0f};
    const GpColor gloss = {1.0f, 1.0f, 1.0f, 0.035f}, well = {0.07f, 0.075f, 0.08f, 1.0f};
    const GpColor cap = {0.27f, 0.28f, 0.30f, 1.0f}, capTop = {0.33f, 0.345f, 0.36f, 1.0f};
    const GpColor btn = {0.24f, 0.25f, 0.27f, 1.0f}, btnRim = {0.45f, 0.47f, 0.49f, 1.0f};
    const GpColor shoulder = {0.22f, 0.23f, 0.245f, 1.0f}, trigger = {0.17f, 0.18f, 0.19f, 1.0f};
    const GpColor mark = {0.62f, 0.64f, 0.66f, 1.0f};
    float e = 0.006f * k; /* the rim's width */
    int i;
#define GPX(v) (ox + (v) * k)
#define GPY(v) (oy + (v) * k)
    /* ZL / ZR behind, L / R in front: rounded bars along the top edge */
    gpd_round_rect(GPX(0.12f), GPY(0.000f), 0.17f * k, 0.060f * k, 0.025f * k, rim);
    gpd_round_rect(GPX(0.12f) + e, GPY(0.000f) + e, 0.17f * k - 2 * e, 0.060f * k - 2 * e, 0.022f * k, trigger);
    gpd_round_rect(GPX(0.71f), GPY(0.000f), 0.17f * k, 0.060f * k, 0.025f * k, rim);
    gpd_round_rect(GPX(0.71f) + e, GPY(0.000f) + e, 0.17f * k - 2 * e, 0.060f * k - 2 * e, 0.022f * k, trigger);
    gpd_round_rect(GPX(0.115f), GPY(0.045f), 0.21f * k, 0.060f * k, 0.028f * k, rim);
    gpd_round_rect(GPX(0.115f) + e, GPY(0.045f) + e, 0.21f * k - 2 * e, 0.060f * k - 2 * e, 0.025f * k, shoulder);
    gpd_round_rect(GPX(0.675f), GPY(0.045f), 0.21f * k, 0.060f * k, 0.028f * k, rim);
    gpd_round_rect(GPX(0.675f) + e, GPY(0.045f) + e, 0.21f * k - 2 * e, 0.060f * k - 2 * e, 0.025f * k, shoulder);

    /* the body: shadow, rim, face, a lighter plate down the middle and a faint gloss */
    gp_pad_silhouette(ox + 0.010f * k, oy + 0.016f * k, k, 0.0f, shadow);
    gp_pad_silhouette(ox, oy, k, e, rim);
    gp_pad_silhouette(ox, oy, k, 0.0f, face);
    /* a soft light from above on each lobe, the way the matte plastic catches it */
    gpd_ellipse(GPX(0.235f), GPY(0.135f), 0.10f * k, 0.040f * k, gloss);
    gpd_ellipse(GPX(0.765f), GPY(0.135f), 0.10f * k, 0.040f * k, gloss);

    /* the two sticks: a dark well, the cap, its raised top and a grip ring */
    {
        static const float sx[2] = {0.27f, 0.62f}, sy[2] = {0.215f, 0.345f};
        for (i = 0; i < 2; i++) {
            gpd_disc(GPX(sx[i]), GPY(sy[i]), 0.066f * k, rim);
            gpd_disc(GPX(sx[i]), GPY(sy[i]), 0.062f * k, well);
            gpd_disc(GPX(sx[i]) + 0.004f * k, GPY(sy[i]) + 0.006f * k, 0.047f * k, shadow);
            gpd_disc(GPX(sx[i]), GPY(sy[i]), 0.047f * k, cap);
            gpd_disc(GPX(sx[i]), GPY(sy[i]), 0.036f * k, rim);
            gpd_disc(GPX(sx[i]), GPY(sy[i]), 0.033f * k, capTop);
        }
    }

    /* the d-pad: a raised cross with an arrow on each arm */
    {
        float cx = GPX(0.38f), cy = GPY(0.345f), a = 0.034f * k, w = 0.012f * k, t = 0.010f * k;
        gpd_round_rect(cx - a - e, cy - w - e, 2 * (a + e), 2 * (w + e), 0.008f * k, rim);
        gpd_round_rect(cx - w - e, cy - a - e, 2 * (w + e), 2 * (a + e), 0.008f * k, rim);
        gpd_round_rect(cx - a, cy - w, 2 * a, 2 * w, 0.006f * k, btn);
        gpd_round_rect(cx - w, cy - a, 2 * w, 2 * a, 0.006f * k, btn);
        gpd_disc(cx, cy, 0.006f * k, well);
        gpd_tri(cx, cy - a + 0.004f * k, cx - t * 0.6f, cy - a + 0.004f * k + t, cx + t * 0.6f, cy - a + 0.004f * k + t, mark);
        gpd_tri(cx, cy + a - 0.004f * k, cx - t * 0.6f, cy + a - 0.004f * k - t, cx + t * 0.6f, cy + a - 0.004f * k - t, mark);
        gpd_tri(cx - a + 0.004f * k, cy, cx - a + 0.004f * k + t, cy - t * 0.6f, cx - a + 0.004f * k + t, cy + t * 0.6f, mark);
        gpd_tri(cx + a - 0.004f * k, cy, cx + a - 0.004f * k - t, cy - t * 0.6f, cx + a - 0.004f * k - t, cy + t * 0.6f, mark);
    }

    /* A B X Y, each with a rim and its letter */
    for (i = 0; i < 4; i++) {
        static const float bx[4] = {0.77f, 0.72f, 0.72f, 0.67f}, by[4] = {0.215f, 0.270f, 0.160f, 0.215f};
        static const char *const bl[4] = {"A", "B", "X", "Y"};
        gpd_disc(GPX(bx[i]) + 0.003f * k, GPY(by[i]) + 0.005f * k, 0.028f * k, shadow);
        gpd_disc(GPX(bx[i]), GPY(by[i]), 0.028f * k, btnRim);
        gpd_disc(GPX(bx[i]), GPY(by[i]), 0.0245f * k, btn);
        gpd_text_centered(GPX(bx[i]), GPY(by[i]) - 3.5f * tpx, tpx, bl[i], mark);
    }

    /* - and + as small pills with their sign, capture a rounded square with a dot, home a
       ring with a dot */
    gpd_round_rect(GPX(0.368f), GPY(0.122f), 0.034f * k, 0.026f * k, 0.010f * k, btn);
    gpd_rect(GPX(0.377f), GPY(0.1335f), 0.016f * k, 0.003f * k, mark);
    gpd_round_rect(GPX(0.598f), GPY(0.122f), 0.034f * k, 0.026f * k, 0.010f * k, btn);
    gpd_rect(GPX(0.607f), GPY(0.1335f), 0.016f * k, 0.003f * k, mark);
    gpd_rect(GPX(0.6135f), GPY(0.1275f), 0.003f * k, 0.015f * k, mark);
    gpd_round_rect(GPX(0.405f), GPY(0.200f), 0.030f * k, 0.030f * k, 0.007f * k, btnRim);
    gpd_round_rect(GPX(0.408f), GPY(0.203f), 0.024f * k, 0.024f * k, 0.005f * k, btn);
    gpd_disc(GPX(0.42f), GPY(0.215f), 0.007f * k, well);
    gpd_disc(GPX(0.58f), GPY(0.215f), 0.018f * k, btnRim);
    gpd_disc(GPX(0.58f), GPY(0.215f), 0.015f * k, btn);
    gpd_disc(GPX(0.58f), GPY(0.215f), 0.006f * k, mark);
#undef GPX
#undef GPY
}

static void gp_draw_layout(float W, float H)
{
    float px = gp_ui_px(H), tpx, cw, k, ox, oy, ch, lx, rx, rowh, i0, top;
    int i, build = gp.bh != NULL;
    const GpColor line = {0.60f, 0.66f, 0.56f, 0.85f};
    /* each side top to bottom in the order its controls sit on the pad, so no two lines
       cross: (x, y) is the control in controller units, 0..1 across */
    const GpLayoutTag left[] = {
        {0.12f, 0.022f, 0.0f, "ZL", build ? "HOLD: BUILD RING   TAP: ZOOM OUT" : "ZOOM OUT"},
        {0.115f, 0.075f, 0.0f, "L", "HOLD + D-PAD: STORE GROUP"},
        {0.368f, 0.135f, 0.0f, "-", "RADAR"},
        {0.27f, 0.215f, 0.066f, "L STICK", "CURSOR   PRESS: PRECISE ON/OFF"},
        {0.405f, 0.215f, 0.0f, "CAPTURE", "CONTROLLER SETTINGS"},
        {0.346f, 0.345f, 0.0f, "D-PAD", "GROUPS 1-4   2X: JUMP THERE"},
    };
    const GpLayoutTag right[] = {
        {0.88f, 0.022f, 0.0f, "ZR", "ZOOM IN"},
        {0.885f, 0.075f, 0.0f, "R", "TAP: ATTACK-MOVE   HOLD: COMMAND RING"},
        {0.632f, 0.135f, 0.0f, "+", "PAUSE, OPTIONS, BACK, CONTINUE"},
        {0.72f, 0.160f, 0.028f, "X", "STOP"},
        {0.77f, 0.215f, 0.028f, "A", "SELECT, ORDER   HOLD: BOX"},
        {0.67f, 0.215f, 0.028f, "Y", "GUARD"},
        {0.72f, 0.270f, 0.028f, "B", "DESELECT, CANCEL"},
        {0.62f, 0.345f, 0.066f, "R STICK", "SCROLL MAP   PRESS: SCATTER"},
    };
    const int nl = (int)(sizeof left / sizeof left[0]), nr = (int)(sizeof right / sizeof right[0]);

    tpx = px > 2.0f ? px - 1.0f : px;     /* the words, one size under the title */
    cw = W * 0.30f;
    if (cw > H * 0.62f) cw = H * 0.62f;
    /* NOTHING MAY OVERLAP. Each column has to fit between the screen edge and the pad
       with room for its lines; when it does not, the words get a size smaller, and when
       they are already at the smallest, the pad does. */
    for (;;) {
        float wl = 0.0f, wr = 0.0f, gap;
        for (i = 0; i < nl; i++) if (gp_row_w(tpx, &left[i]) > wl) wl = gp_row_w(tpx, &left[i]);
        for (i = 0; i < nr; i++) if (gp_row_w(tpx, &right[i]) > wr) wr = gp_row_w(tpx, &right[i]);
        gap = 22.0f * tpx;
        if (wl + gap + cw * 0.5f <= W * 0.5f - 8.0f && wr + gap + cw * 0.5f <= W * 0.5f - 8.0f)
            break;
        if (tpx > 1.0f) tpx -= 1.0f;
        else if (cw > W * 0.12f) cw *= 0.9f;
        else break;
    }
    k = cw;
    ch = 0.64f * k;                       /* the pad's height, grips included */
    ox = (W - cw) * 0.5f;
    lx = ox - 22.0f * tpx;                /* where the words end (left) and start (right) */
    rx = ox + cw + 22.0f * tpx;
    rowh = 13.0f * tpx;
    /* THE WHOLE BLOCK CENTRED: title, pad or taller column, the two ring lines. The pad
       sits so that the taller column, centred on it, clears the title. */
    {
        float body = ch > rowh * (float)nr ? ch : rowh * (float)nr;
        float titleh = 13.0f * px + 7.0f * tpx, block = titleh + 16.0f * tpx + body + 4.0f * rowh;
        top = (H - block) * 0.5f;
        if (top < 8.0f) top = 8.0f;
        oy = top + titleh + 16.0f * tpx + body * 0.5f - ch * 0.5f;
    }

    gpd_rect(0, 0, W, H, (GpColor){0.0f, 0.0f, 0.0f, 0.82f});
    gpd_text_centered(W * 0.5f, top, px * 1.3f, "CONTROLLER BUTTON LAYOUT", GP_TEXT);
    gpd_text_centered(W * 0.5f, top + 13.0f * px, tpx, "NINTENDO SWITCH PRO CONTROLLER", GP_DIM);

#define GPX(v) (ox + (v) * k)
#define GPY(v) (oy + (v) * k)
    gp_draw_pad(ox, oy, k, tpx);

    /* each column centred on the pad's height; a straight line from the end of each
       row to its control */
    i0 = oy + ch * 0.5f - rowh * (float)nl * 0.5f;
    for (i = 0; i < nl; i++) {
        float ty = i0 + rowh * (float)i, wn = gpd_text_width(tpx, left[i].name);
        float wd = gpd_text_width(tpx, left[i].what), x = lx - wd - 6.0f * tpx - wn;
        gpd_text(x, ty, tpx, left[i].name, GP_HOT);
        gpd_text(lx - wd, ty, tpx, left[i].what, GP_TEXT);
        {
            float ex = GPX(left[i].x) - left[i].r * k, ey = GPY(left[i].y);
            gpd_line(lx + 4.0f * tpx, ty + 3.5f * tpx, ex, ey, tpx * 0.6f, line);
            gpd_disc(ex, ey, 1.8f * tpx, GP_HOT);
        }
    }
    i0 = oy + ch * 0.5f - rowh * (float)nr * 0.5f;
    for (i = 0; i < nr; i++) {
        float ty = i0 + rowh * (float)i, wn = gpd_text_width(tpx, right[i].name);
        gpd_text(rx, ty, tpx, right[i].name, GP_HOT);
        gpd_text(rx + wn + 6.0f * tpx, ty, tpx, right[i].what, GP_TEXT);
        {
            float ex = GPX(right[i].x) + right[i].r * k, ey = GPY(right[i].y);
            gpd_line(ex, ey, rx - 4.0f * tpx, ty + 3.5f * tpx, tpx * 0.6f, line);
            gpd_disc(ex, ey, 1.8f * tpx, GP_HOT);
        }
    }
#undef GPX
#undef GPY

    {
        float colbot = oy + ch * 0.5f + rowh * (float)nr * 0.5f, by = oy + ch + 0.06f * k;
        if (by < colbot + rowh) by = colbot + rowh;
        gp_text_line(W * 0.5f, by, W * 0.94f, tpx,
                     "COMMAND RING, HOLD R: ATTACK - STOP - GUARD - SCATTER - FORCE FIRE - FORCE MOVE - REBUILD - BASE",
                     GP_TEXT);
        gp_text_line(W * 0.5f, by + rowh, W * 0.94f, tpx,
                     "CONTROL GROUPS: D-PAD UP 1 - RIGHT 2 - DOWN 3 - LEFT 4.  L + D-PAD STORES.  IN MENUS THE D-PAD MOVES",
                     GP_TEXT);
        if (build)
            gp_text_line(W * 0.5f, by + 2.0f * rowh, W * 0.94f, tpx,
                         "BUILD RING, HOLD ZL: R STICK PICKS, A BUILDS, B HOLDS/CANCELS, D-PAD CHANGES CATEGORY",
                         GP_TEXT);
        gp_text_line(W * 0.5f, H - 16.0f * tpx, W * 0.94f, tpx, "A OR B: BACK", GP_DIM);
    }
}

static void gp_draw_toast(float W, float H)
{
    float px = gp_ui_px(H);
    const char *l1 = "CONTROLLER READY";
    const char *l2 = gp.bh ? "CAPTURE: SETTINGS   HOLD R: COMMANDS   HOLD ZL: BUILD"
                           : "CAPTURE: SETTINGS    HOLD R: COMMAND RING";
    float tw = gpd_text_width(px, l2) + 16.0f * px, th = 26.0f * px, x = (W - tw) * 0.5f, y = H * 0.08f;
    (void)W;
    gpd_rect(x, y, tw, th, GP_PLATE);
    gpd_frame(x, y, tw, th, px, GP_EDGE);
    gpd_text_centered(W * 0.5f, y + 4.0f * px, px, l1, GP_HOT);
    gpd_text_centered(W * 0.5f, y + 15.0f * px, px, l2, GP_TEXT);
}

void gp_draw(SDL_Window *w)
{
    float W, H;
    int toast = gp.toast_until && (Sint32)(SDL_GetTicks() - gp.toast_until) < 0;
    if (gp.ready <= 0 || !w || SDL_GetWindowID(w) != gp.win_id)
        return;
    int note = gp.msg_until && (Sint32)(SDL_GetTicks() - gp.msg_until) < 0;
    if (!gp.panel && !gp.ring && !gp.bring && !toast && !(gp.text_on && gp.pad) && !(gp.slow && gp.pad)
        && !note)
        return;
    if (!gpd_begin(w, &W, &H))
        return;
    if (gp.panel && gp.layout)
        gp_draw_layout(W, H);
    else if (gp.panel)
        gp_draw_panel(W, H);
    else if (gp.ring)
        gp_draw_ring(W, H);
    else if (gp.bring)
        gp_draw_build(W, H);
    else if (toast)
        gp_draw_toast(W, H);
    else if (gp.text_on && gp.pad) {
        /* a name or a message is being typed: say how the pad gets past it */
        float px = gp_ui_px(H), tw = gpd_text_width(px, "+: CONTINUE") + 10.0f * px;
        gpd_rect(W * 0.5f - tw * 0.5f, H - 20.0f * px, tw, 11.0f * px, GP_PLATE);
        gpd_text_centered(W * 0.5f, H - 18.0f * px, px, "+: CONTINUE", GP_HOT);
    }
    if (note && !gp.panel && !gp.ring && !gp.bring) {
        /* the d-pad's group note, just above the bottom edge */
        float px = gp_ui_px(H), tw = gpd_text_width(px, gp.msg) + 12.0f * px;
        gpd_rect(W * 0.5f - tw * 0.5f, H - 34.0f * px, tw, 12.0f * px, GP_PLATE);
        gpd_frame(W * 0.5f - tw * 0.5f, H - 34.0f * px, tw, 12.0f * px, 1.0f, GP_EDGE);
        gpd_text_centered(W * 0.5f, H - 31.5f * px, px, gp.msg, GP_HOT);
    }
    if (gp.slow && gp.pad && !gp.panel) {
        /* the precise cursor is on: a small badge, so a slow pointer is never a mystery */
        float px = gp_ui_px(H), tw = gpd_text_width(px, "PRECISE CURSOR") + 10.0f * px;
        gpd_rect(8.0f * px, H - 20.0f * px, tw, 11.0f * px, GP_PLATE);
        gpd_text(13.0f * px, H - 18.0f * px, px, "PRECISE CURSOR", GP_HOT);
    }
    gpd_end();
}

#ifdef GP_PREVIEW
/* ---- a preview for layout work (input/gamepad_preview.c) --------------------------
   Draws one of the pad's screens with made-up content into the current GL context, so
   its layout can be looked at without a pad, a mission or a hand on the stick. */
static const char *const gpv_names[] = {"POWER PLANT", "ADV. POWER PLANT", "BARRACKS",
    "TIBERIUM REFINERY", "COMMUNICATIONS CENTER", "WEAPONS FACTORY", "GUARD TOWER",
    "SANDBAG WALL", "HELIPAD", "REPAIR FACILITY", "ADVANCED GUARD TOWER", "TIBERIUM SILO",
    "ADVANCED COMMUNICATIONS CENTER", "CONCRETE WALL"};
static int gpv_active(void) { return 1; }
static int gpv_count(int cat) { return cat == 0 ? 14 : cat == 1 ? 5 : cat == 2 ? 7 : 0; }
static int gpv_item(int cat, int i, GpBuildItem *o)
{
    (void)cat;
    memset(o, 0, sizeof *o);
    snprintf(o->label, sizeof o->label, "%s", gpv_names[i % 14]);
    o->cost = 300 + 100 * i;
    o->state = i == 1 ? GP_BI_BUILDING : i == 2 ? GP_BI_READY : i == 3 ? GP_BI_HOLD : i == 5 ? GP_BI_BUSY : 0;
    o->progress = 0.45f;
    o->queued = i == 1 ? 2 : 0;
    return 1;
}
static int gpv_credits(void) { return 4250; }
static int gpv_group(int slot) { return slot == 0 ? 6 : slot == 1 ? 1 : slot == 2 ? 12 : 0; }
static const GpBuildHooks gpv_hooks = {gpv_active, gpv_count, gpv_item, NULL, NULL, gpv_credits, gpv_group};

void gp_preview(SDL_Window *w, int which)
{
    float W, H;
    int ww, wh;
    SDL_GetWindowSize(w, &ww, &wh);
    gp.ready = 1;
    gp.win_id = SDL_GetWindowID(w);
    gp_defaults();
    gp.panel = which == 0 || which == 3;
    gp.layout = which == 3;
    gp.ring = which == 1 || which == 4;
    gp.ring_tab = which == 4;
    gp.bring = which == 2;
    gp.ring_x = ww * 0.5f;
    gp.ring_y = wh * 0.5f;
    gp.ring_pick = 4;
    gp.bh = &gpv_hooks;
    gp.bcat = 0;
    gp.bpage = 0;
    gp.bpick = 1;
    gp.row = 1;
    if (gpd_begin(w, &W, &H)) {
        if (which == 3) gp_draw_layout(W, H);
        else if (which == 0) gp_draw_panel(W, H);
        else if (which == 1 || which == 4) gp_draw_ring(W, H);
        else gp_draw_build(W, H);
        gpd_end();
    }
}
#endif
