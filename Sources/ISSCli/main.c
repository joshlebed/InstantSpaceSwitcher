#include "../ISS/include/ISS.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    if (argc > 1 && strcmp(argv[1], "prefer") == 0) {
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
    if (rawMove) {
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

    if (!success) {
        fprintf(stderr, "Switch request failed. Check space bounds or permissions.\n");
        iss_destroy();
        return 1;
    }

    iss_destroy();
    return 0;
}
