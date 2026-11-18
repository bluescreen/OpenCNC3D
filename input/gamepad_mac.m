/*
 * gamepad_mac.m -- keep macOS from taking the controller's system buttons.
 *
 * On macOS 26 the Home button of a game controller opens the Games app, and the
 * Capture/Share button belongs to the system as well. That happens in the GameController
 * framework before any application sees the press, so on the TV the player is thrown
 * out of the game by the button in the middle of the pad.
 *
 * An application may ask for those presses itself: every button GameController binds to
 * a system gesture has a preferredSystemGestureState, and setting it to Disabled hands
 * the press to the application while it is in front. SDL does exactly that for the pads
 * it opens through GameController (SDL_mfijoystick.m), but a Switch Pro Controller is
 * opened through its HIDAPI driver instead, so nobody asks on its behalf. This does,
 * for every connected controller, now and whenever one connects.
 */

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gp_mac_debug(void)
{
    const char *v = getenv("OPENCNC3D_GAMEPAD_DEBUG");
    return v && strcmp(v, "0") != 0;
}

static int gp_mac_quiet(GCController *c)
{
    int n = 0;
    if (@available(macOS 11.0, *)) {
        GCPhysicalInputProfile *p = c.physicalInputProfile;
        for (NSString *key in p.buttons) {
            GCControllerButtonInput *b = p.buttons[key];
            if (gp_mac_debug())
                fprintf(stderr, "GAMEPAD|mac|%s|button=%s|system=%d|state=%ld\n",
                        c.vendorName.UTF8String ?: "?", key.UTF8String, b.isBoundToSystemGesture ? 1 : 0,
                        (long)b.preferredSystemGestureState);
            if (b.isBoundToSystemGesture) {
                b.preferredSystemGestureState = GCSystemGestureStateDisabled;
                n++;
            }
        }
    }
    return n;
}

/* -> how many buttons were taken back from the system, across every controller */
int gp_mac_quiet_system_buttons(void)
{
    static id observer;
    int n = 0;
    @autoreleasepool {
        if (gp_mac_debug())
            fprintf(stderr, "GAMEPAD|mac|controllers=%lu\n", (unsigned long)[GCController controllers].count);
        for (GCController *c in [GCController controllers])
            n += gp_mac_quiet(c);
        if (!observer)
            observer = [[NSNotificationCenter defaultCenter]
                addObserverForName:GCControllerDidConnectNotification
                            object:nil
                             queue:[NSOperationQueue mainQueue]
                        usingBlock:^(NSNotification *note) {
                            int k = gp_mac_quiet((GCController *)note.object);
                            if (gp_mac_debug())
                                fprintf(stderr, "GAMEPAD|mac|connected later|system buttons kept: %d\n", k);
                        }];
    }
    return n;
}
