#include "../ISS/include/ISS.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

// Debug: replicate Raycast's "moveWindowWithMouseClick" — hold a synthetic
// left-mouse button on the window's title bar (zero motion = no drift) while
// running the compat-ID dance, then release. Tests whether the held click is
// what makes the (otherwise dead-on-Tahoe) dance actually relocate the window.
static void post_mouse(CGEventType t, CGPoint p) {
    CGEventRef e = CGEventCreateMouseEvent(NULL, t, p, kCGMouseButtonLeft);
    if (e) { CGEventSetFlags(e, 0); CGEventPost(kCGHIDEventTap, e); CFRelease(e); }
}
static bool click_move(unsigned int wid, unsigned long long sid, double gx, double gy) {
    CGPoint g = { gx, gy };
    CGWarpMouseCursorPosition(g);
    post_mouse(kCGEventMouseMoved, g);
    post_mouse(kCGEventLeftMouseDown, g);
    usleep(30 * 1000);
    bool ok = iss_move_window_raw(wid, sid);   // compat-ID dance (incl. its 1s sleep)
    post_mouse(kCGEventLeftMouseUp, g);
    return ok;
}

static void print_usage(const char *progName) {
    fprintf(stderr,
        "Usage:\n"
        "  %s [left|right|mv-left|mv-right|index <n>]\n"
        "  %s move-window <windowID> <targetSpaceID>   # debug: compat-ID dance\n"
        "  %s add-remove  <windowID> <targetSpaceID>   # debug: AddWindowsToSpaces + RemoveWindowsFromSpaces\n"
        "  %s prefer <windowID> <0|1>                  # debug: SLSSetWindowPrefersCurrentSpace\n",
        progName, progName, progName, progName);
}

int main(int argc, char **argv) {
    if (!iss_init()) {
        fprintf(stderr, "Failed to initialize ISS (event tap). Check accessibility and input monitoring permissions.\n");
        return 1;
    }

    ISSDirection direction = ISSDirectionLeft;
    bool useIndex = false;
    bool moveAndFollow = false;
    bool rawMove = false;
    unsigned int targetIndex = 0;
    unsigned int rawWindowID = 0;
    unsigned long long rawSpaceID = 0;

    bool addRemove = false;
    bool prefer = false;
    bool preferValue = false;
    bool clickMove = false;
    double clickX = 0, clickY = 0;

    if (argc > 1 && strcmp(argv[1], "click-move") == 0) {
        if (argc < 6) { fprintf(stderr, "usage: %s click-move <wid> <sid> <gx> <gy>\n", argv[0]); iss_destroy(); return 1; }
        rawWindowID = (unsigned int)strtoul(argv[2], NULL, 10);
        rawSpaceID = strtoull(argv[3], NULL, 10);
        clickX = atof(argv[4]);
        clickY = atof(argv[5]);
        clickMove = true;
    } else if (argc > 1 && strcmp(argv[1], "prefer") == 0) {
        if (argc < 4) { print_usage(argv[0]); iss_destroy(); return 1; }
        char *endPtr = NULL;
        long wid = strtol(argv[2], &endPtr, 10);
        long flag = strtol(argv[3], &endPtr, 10);
        if (wid <= 0) { fprintf(stderr, "windowID must be positive.\n"); iss_destroy(); return 1; }
        prefer = true;
        rawWindowID = (unsigned int)wid;
        preferValue = (flag != 0);
    } else if (argc > 1 && (strcmp(argv[1], "move-window") == 0 || strcmp(argv[1], "add-remove") == 0)) {
        if (argc < 4) { print_usage(argv[0]); iss_destroy(); return 1; }
        char *endPtr = NULL;
        long wid = strtol(argv[2], &endPtr, 10);
        if (endPtr == argv[2] || wid <= 0) {
            fprintf(stderr, "windowID must be a positive integer.\n");
            iss_destroy();
            return 1;
        }
        long long sid = strtoll(argv[3], &endPtr, 10);
        if (endPtr == argv[3] || sid <= 0) {
            fprintf(stderr, "targetSpaceID must be a positive integer.\n");
            iss_destroy();
            return 1;
        }
        rawWindowID = (unsigned int)wid;
        rawSpaceID = (unsigned long long)sid;
        if (strcmp(argv[1], "move-window") == 0) rawMove = true;
        else addRemove = true;
    } else if (argc > 1) {
        if (!strcmp(argv[1], "right") || !strcmp(argv[1], "r") || !strcmp(argv[1], "1")) {
            direction = ISSDirectionRight;
        } else if (!strcmp(argv[1], "left") || !strcmp(argv[1], "l") || !strcmp(argv[1], "0")) {
            direction = ISSDirectionLeft;
        } else if (!strcmp(argv[1], "mv-right") || !strcmp(argv[1], "mr")) {
            direction = ISSDirectionRight;
            moveAndFollow = true;
        } else if (!strcmp(argv[1], "mv-left") || !strcmp(argv[1], "ml")) {
            direction = ISSDirectionLeft;
            moveAndFollow = true;
        } else if (!strcmp(argv[1], "index") || !strcmp(argv[1], "i")) {
            if (argc < 3) {
                print_usage(argv[0]);
                iss_destroy();
                return 1;
            }
            char *endPtr = NULL;
            long parsed = strtol(argv[2], &endPtr, 10);
            if (endPtr == argv[2] || parsed < 1) {
                fprintf(stderr, "Index must be a positive integer.\n");
                iss_destroy();
                return 1;
            }
            useIndex = true;
            targetIndex = (unsigned int)(parsed - 1); // convert to zero-based
        } else {
            print_usage(argv[0]);
            iss_destroy();
            return 1;
        }
    }

    bool success = false;
    if (clickMove) {
        success = click_move(rawWindowID, rawSpaceID, clickX, clickY);
    } else if (rawMove) {
        success = iss_move_window_raw(rawWindowID, rawSpaceID);
    } else if (addRemove) {
        success = iss_move_window_add_remove(rawWindowID, rawSpaceID);
    } else if (prefer) {
        success = iss_set_window_prefers_current(rawWindowID, preferValue);
    } else if (useIndex) {
        success = iss_switch_to_index(targetIndex);
    } else if (moveAndFollow) {
        success = iss_switch_and_follow(direction);
    } else {
        success = iss_switch(direction);
    }

    // The move-and-follow path posts synthetic mouse/keyboard events and
    // schedules a delayed (~300 ms) re-focus via dispatch_after on the main
    // queue. Unlike the GUI app, this one-shot CLI otherwise tears down and
    // exits before WindowServer finishes processing the carry-drag and before
    // the delayed re-focus block fires. Pump the main runloop briefly so the
    // CLI reproduces the app's runtime behavior and is a faithful test harness.
    if (moveAndFollow) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.7, false);
    }

    if (!success) {
        fprintf(stderr, "Switch request failed. Check space bounds or permissions.\n");
        iss_destroy();
        return 1;
    }

    iss_destroy();
    return 0;
}
