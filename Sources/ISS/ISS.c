#include "include/ISS.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CGEventTypes.h>
#include <assert.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <float.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const CGEventField kCGSEventTypeField = (CGEventField)55;
static const CGEventField kCGEventGestureHIDType = (CGEventField)110;
static const CGEventField kCGEventGestureSwipeMotion = (CGEventField)123;
static const CGEventField kCGEventGestureSwipeProgress = (CGEventField)124;
static const CGEventField kCGEventGestureSwipeVelocityX = (CGEventField)129;
static const CGEventField kCGEventGestureSwipeVelocityY = (CGEventField)130;
static const CGEventField kCGEventGesturePhase = (CGEventField)132;

// See IOHIDEventType enum in IOHIDFamily
static const uint32_t kIOHIDEventTypeDockSwipe = 23;

typedef uint32_t CGSEventType;
enum {
    kCGSEventScrollWheel = 22,
    kCGSEventZoom = 28,
    kCGSEventGesture = 29,
    kCGSEventDockControl = 30,
    kCGSEventFluidTouchGesture = 31,
};

typedef CF_ENUM(uint8_t, CGSGesturePhase) {
    kCGSGesturePhaseNone = 0,
    kCGSGesturePhaseBegan = 1,
    kCGSGesturePhaseChanged = 2,
    kCGSGesturePhaseEnded = 4,
    kCGSGesturePhaseCancelled = 8,
    kCGSGesturePhaseMayBegin = 128,
};

// Limited subset of motion constants observed in synthetic Dock swipe traces.
typedef CF_ENUM(uint16_t, CGGestureMotion) {
    kCGGestureMotionHorizontal = 1,
};

typedef int32_t CGSConnectionID;
typedef uint64_t CGSSpaceID;

extern CFArrayRef CGSCopyManagedDisplaySpaces(CGSConnectionID connection, CFStringRef display) __attribute__((weak_import));
extern CFStringRef CGSCopyActiveMenuBarDisplayIdentifier(CGSConnectionID connection) __attribute__((weak_import));
extern CGSConnectionID CGSMainConnectionID(void) __attribute__((weak_import));
extern CGSSpaceID CGSGetActiveSpace(CGSConnectionID connection) __attribute__((weak_import));

extern void CGSMoveWindowsToManagedSpace(CGSConnectionID connection,
                                         CFArrayRef windowIDs,
                                         CGSSpaceID spaceID) __attribute__((weak_import));

// macOS Sonoma+ restricts CGSMoveWindowsToManagedSpace to same-connection
// windows. Raycast's binary (nm + otool) shows they use the legacy compatID/
// workspace path instead. We match their sequence.
extern CGError CGSSpaceSetCompatID(CGSConnectionID cid,
                                   CGSSpaceID sid,
                                   int compatID) __attribute__((weak_import));
extern CGError CGSSetWindowListWorkspace(CGSConnectionID cid,
                                         CGWindowID *wids,
                                         int count,
                                         int workspace) __attribute__((weak_import));

extern void CGSAddWindowsToSpaces(CGSConnectionID cid,
                                  CFArrayRef windows,
                                  CFArrayRef spaces) __attribute__((weak_import));
extern void CGSRemoveWindowsFromSpaces(CGSConnectionID cid,
                                       CFArrayRef windows,
                                       CFArrayRef spaces) __attribute__((weak_import));
extern CFArrayRef CGSCopySpacesForWindows(CGSConnectionID cid,
                                          int mask,
                                          CFArrayRef windows) __attribute__((weak_import));

typedef CGError (*SLSSetWindowPrefersCurrentSpaceFn)(CGSConnectionID, CGWindowID, bool);

extern AXError _AXUIElementGetWindow(AXUIElementRef element,
                                     CGWindowID *outID) __attribute__((weak_import));

// Symbolic hotkey support — lets us fetch the user-configured keyboard
// shortcut for "Switch to Desktop N" (IDs 118..133) and post it.
typedef int CGSSymbolicHotKey;
extern CGError CGSGetSymbolicHotKeyValue(CGSSymbolicHotKey hotKey,
                                          unsigned short *outChar,
                                          CGKeyCode *outKeyCode,
                                          CGEventFlags *outFlags) __attribute__((weak_import));
extern bool CGSIsSymbolicHotKeyEnabled(CGSSymbolicHotKey hotKey) __attribute__((weak_import));
extern CGError CGSSetSymbolicHotKeyEnabled(CGSSymbolicHotKey hotKey, bool enabled) __attribute__((weak_import));

static CFMachPortRef globalTap = NULL;
static CFRunLoopSourceRef globalSource = NULL;

// Overlay detection state
static bool overlayDetectionEnabled = false;

// Swipe override state
static bool swipeOverrideEnabled = false;
static bool swipeTracking = false;
static bool swipeFired = false;

// Gesture speed state
static double gestureSpeed = 2000.0;

static ISSSwitchCallback switchCallback = NULL;

// Predictions dictionary: DisplayID (CFStringRef) -> Index (CFNumberRef)
static CFMutableDictionaryRef predictionsDict = NULL;

static bool get_prediction(const char *displayID, unsigned int *outIndex) {
    if (!displayID || !predictionsDict) return false;
    
    CFStringRef key = CFStringCreateWithCString(NULL, displayID, kCFStringEncodingUTF8);
    const void *value = CFDictionaryGetValue(predictionsDict, key);
    CFRelease(key);

    if (value) {
        CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, outIndex);
        return true;
    }
    return false;
}

static void set_prediction(const char *displayID, unsigned int index) {
    if (!displayID || !predictionsDict) return;
    
    CFStringRef key = CFStringCreateWithCString(NULL, displayID, kCFStringEncodingUTF8);
    CFNumberRef val = CFNumberCreate(NULL, kCFNumberIntType, &index);
    CFDictionarySetValue(predictionsDict, key, val);
    CFRelease(key);
    CFRelease(val);
}

static bool extract_space_info_from_display(CFDictionaryRef displayDict,
                                            CGSSpaceID activeSpace,
                                            bool hasActiveSpace,
                                            ISSSpaceInfo *outInfo,
                                            CFMutableArrayRef outSpaceIDs);
static bool load_space_info_for_display(ISSSpaceInfo *info,
                                        bool useCursorDisplay,
                                        CFMutableArrayRef outSpaceIDs);
static bool iss_perform_switch_gesture(ISSDirection direction, double velocity);
static bool iss_switch_with_info(const ISSSpaceInfo *info, ISSDirection direction);
static bool iss_should_block_switch(const ISSSpaceInfo *info, ISSDirection direction);

// Perform a swipe-override switch: get space info, compute target, switch,
// and notify the handler with the target index.
static void swipe_override_switch(ISSDirection dir) {
    ISSSpaceInfo info;
    if (!iss_get_space_info(&info)) {
        iss_perform_switch_gesture(dir, gestureSpeed);
        return;
    }

    unsigned int predicted;
    unsigned int current = get_prediction(info.displayID, &predicted) ? predicted : info.currentIndex;
    unsigned int target = dir == ISSDirectionLeft ? current - 1 : current + 1;

    if (iss_switch_with_info(&info, dir)) {
        set_prediction(info.displayID, target);
        if (switchCallback) { switchCallback(target); }
    }
}

static CGEventRef eventTapCallback(CGEventTapProxy proxy, CGEventType type,
                                   CGEventRef event, void *refcon) {
    (void)proxy;
    (void)refcon;

    // Re-enable if the system disabled our tap for being too slow
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        if (globalTap) CGEventTapEnable(globalTap, true);
        return event;
    }

    if (!swipeOverrideEnabled) return event;

    CGSEventType eventType =
        (CGSEventType)CGEventGetIntegerValueField(event, kCGSEventTypeField);

    // Pass through synthetic events (non-HID source). Real gesture events
    // from the trackpad have sourcePid == 0 (HID kernel).
    if (eventType == kCGSEventDockControl || eventType == kCGSEventGesture) {
        pid_t sourcePid = (pid_t)CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID);
        if (sourcePid != 0) return event;
    }

    if (eventType == kCGSEventDockControl) {
        uint32_t hidType =
            (uint32_t)CGEventGetIntegerValueField(event, kCGEventGestureHIDType);
        if (hidType != kIOHIDEventTypeDockSwipe) return event;

        uint16_t motion =
            (uint16_t)CGEventGetIntegerValueField(event, kCGEventGestureSwipeMotion);
        if (motion != kCGGestureMotionHorizontal) return event;

        CGSGesturePhase phase =
            (CGSGesturePhase)CGEventGetIntegerValueField(event, kCGEventGesturePhase);

        switch (phase) {
        case kCGSGesturePhaseBegan:
            if (iss_is_expose_active()) return event;
            swipeTracking = true;
            swipeFired = false;
            return NULL;

        case kCGSGesturePhaseChanged: {
            if (!swipeTracking) return event;
            if (!swipeFired) {
                double progress =
                    CGEventGetDoubleValueField(event, kCGEventGestureSwipeProgress);
                if (progress != 0.0) {
                    ISSDirection dir =
                        progress > 0 ? ISSDirectionRight : ISSDirectionLeft;
                    swipeFired = true;
                    swipe_override_switch(dir);
                }
            }
            return NULL;
        }

        case kCGSGesturePhaseEnded: {
            if (!swipeTracking) return event;
            if (!swipeFired) {
                double velocity =
                    CGEventGetDoubleValueField(event, kCGEventGestureSwipeVelocityX);
                if (velocity != 0.0) {
                    ISSDirection dir =
                        velocity > 0 ? ISSDirectionRight : ISSDirectionLeft;
                    swipeFired = true;
                    swipe_override_switch(dir);
                }
            }
            swipeTracking = false;
            swipeFired = false;
            return NULL;
        }

        case kCGSGesturePhaseCancelled:
            swipeTracking = false;
            swipeFired = false;
            return NULL;

        default:
            return swipeTracking ? NULL : event;
        }
    }

    // Suppress companion gesture events during active swipe tracking
    if (eventType == kCGSEventGesture && swipeTracking) {
        return NULL;
    }

    return event;
}

static bool cgs_symbols_available(void) {
    return (&CGSMainConnectionID != NULL) &&
           (&CGSGetActiveSpace != NULL) &&
           (&CGSCopyManagedDisplaySpaces != NULL);
}

static bool extract_space_info_from_display(CFDictionaryRef displayDict,
                                            CGSSpaceID activeSpace,
                                            bool hasActiveSpace,
                                            ISSSpaceInfo *outInfo,
                                            CFMutableArrayRef outSpaceIDs) {
    if (!displayDict || !outInfo) {
        return false;
    }

    memset(outInfo->displayID, 0, sizeof(outInfo->displayID));
    CFStringRef identifier = (CFStringRef)CFDictionaryGetValue(displayDict, CFSTR("Display Identifier"));
    if (identifier && CFGetTypeID(identifier) == CFStringGetTypeID()) {
        CFStringGetCString(identifier, outInfo->displayID, sizeof(outInfo->displayID), kCFStringEncodingUTF8);
    }

    const void *spacesValue = CFDictionaryGetValue(displayDict, CFSTR("Spaces"));
    if (!spacesValue || CFGetTypeID(spacesValue) != CFArrayGetTypeID()) {
        return false;
    }

    // Try to get current space from display dict (more accurate per-display)
    CGSSpaceID displayActiveSpace = 0;
    const void *currentSpaceValue = CFDictionaryGetValue(displayDict, CFSTR("Current Space"));
    if (currentSpaceValue && CFGetTypeID(currentSpaceValue) == CFDictionaryGetTypeID()) {
        CFDictionaryRef currentSpaceDict = (CFDictionaryRef)currentSpaceValue;
        CFNumberRef currentSpaceID = (CFNumberRef)CFDictionaryGetValue(currentSpaceDict, CFSTR("id64"));
        if (currentSpaceID && CFGetTypeID(currentSpaceID) == CFNumberGetTypeID()) {
            CFNumberGetValue(currentSpaceID, kCFNumberSInt64Type, &displayActiveSpace);
        }
    }
    
    // Use display-specific active space if available, otherwise use global
    CGSSpaceID targetActiveSpace = displayActiveSpace != 0 ? displayActiveSpace : activeSpace;
    bool hasTargetActiveSpace = displayActiveSpace != 0 || hasActiveSpace;

    CFArrayRef spaces = (CFArrayRef)spacesValue;
    const CFIndex spaceCount = CFArrayGetCount(spaces);

    unsigned int totalSpaces = 0;
    unsigned int activeIndex = 0;
    bool foundActive = false;

    for (CFIndex i = 0; i < spaceCount; i++) {
        const void *spaceValue = CFArrayGetValueAtIndex(spaces, i);
        if (!spaceValue || CFGetTypeID(spaceValue) != CFDictionaryGetTypeID()) {
            continue;
        }

        CFDictionaryRef spaceDict = (CFDictionaryRef)spaceValue;
        CFNumberRef idNumber = (CFNumberRef)CFDictionaryGetValue(spaceDict, CFSTR("id64"));
        if (!idNumber || CFGetTypeID(idNumber) != CFNumberGetTypeID()) {
            continue;
        }

        CGSSpaceID candidate = 0;
        if (CFNumberGetValue(idNumber, kCFNumberSInt64Type, &candidate)) {
            if (!foundActive && hasTargetActiveSpace && candidate == targetActiveSpace) {
                activeIndex = totalSpaces;
                foundActive = true;
            }
            if (outSpaceIDs) {
                CFNumberRef boxed = CFNumberCreate(NULL, kCFNumberSInt64Type, &candidate);
                if (boxed) {
                    CFArrayAppendValue(outSpaceIDs, boxed);
                    CFRelease(boxed);
                }
            }
            totalSpaces++;
        }
    }

    if (totalSpaces == 0 || (hasTargetActiveSpace && !foundActive)) {
        return false;
    }

    outInfo->spaceCount = totalSpaces;
    outInfo->currentIndex = foundActive ? activeIndex : 0;
    return true;
}

static bool load_space_info_for_display(ISSSpaceInfo *info,
                                        bool useCursorDisplay,
                                        CFMutableArrayRef outSpaceIDs) {
    if (!cgs_symbols_available()) {
        fprintf(stderr, "ISS: required CGS symbols missing\n");
        return false;
    }

    CGSConnectionID connection = CGSMainConnectionID();
    if (connection == 0) {
        fprintf(stderr, "ISS: CGSMainConnectionID returned 0\n");
        return false;
    }

    CGSSpaceID activeSpace = 0;
    bool hasActiveSpace = false;
    if (&CGSGetActiveSpace != NULL) {
        activeSpace = CGSGetActiveSpace(connection);
        if (activeSpace != 0) {
            hasActiveSpace = true;
        } else {
            fprintf(stderr, "ISS: CGSGetActiveSpace returned 0\n");
            return false;
        }
    }

    // Get display identifier based on mode
    CFStringRef activeDisplayIdentifier = NULL;
    
    if (useCursorDisplay) {
        // Get display where cursor is located
        CGEventRef tempEvent = CGEventCreate(NULL);
        CGPoint cursorLocation = CGEventGetLocation(tempEvent);
        CFRelease(tempEvent);
        
        CGDirectDisplayID cursorDisplay = 0;
        uint32_t cursorDisplayCount = 0;
        
        if (CGGetDisplaysWithPoint(cursorLocation, 1, &cursorDisplay, &cursorDisplayCount) == kCGErrorSuccess && cursorDisplayCount > 0) {
            CFUUIDRef displayUUID = CGDisplayCreateUUIDFromDisplayID(cursorDisplay);
            if (displayUUID) {
                activeDisplayIdentifier = CFUUIDCreateString(NULL, displayUUID);
                CFRelease(displayUUID);
            }
        }
    } else {
        // Get menubar display
        if (&CGSCopyActiveMenuBarDisplayIdentifier != NULL) {
            activeDisplayIdentifier = CGSCopyActiveMenuBarDisplayIdentifier(connection);
        }
    }

    CFArrayRef displays = CGSCopyManagedDisplaySpaces(connection, activeDisplayIdentifier);
    if (!displays && activeDisplayIdentifier) {
        displays = CGSCopyManagedDisplaySpaces(connection, NULL);
    }
    if (!displays) {
        if (activeDisplayIdentifier) {
            CFRelease(activeDisplayIdentifier);
        }
        return false;
    }

    const CFIndex displayCount = CFArrayGetCount(displays);
    CFDictionaryRef targetDisplay = NULL;
    CFDictionaryRef fallbackDisplay = NULL;

    for (CFIndex i = 0; i < displayCount; i++) {
        const void *displayValue = CFArrayGetValueAtIndex(displays, i);
        if (!displayValue || CFGetTypeID(displayValue) != CFDictionaryGetTypeID()) {
            continue;
        }

        CFDictionaryRef displayDict = (CFDictionaryRef)displayValue;

        if (!fallbackDisplay) {
            fallbackDisplay = displayDict;
        }

        if (!activeDisplayIdentifier || targetDisplay) {
            continue;
        }

        CFStringRef identifier = (CFStringRef)CFDictionaryGetValue(displayDict, CFSTR("Display Identifier"));
        if (identifier && CFGetTypeID(identifier) == CFStringGetTypeID() && CFEqual(identifier, activeDisplayIdentifier)) {
            targetDisplay = displayDict;
        }
    }

    if (!targetDisplay) {
        targetDisplay = fallbackDisplay;
    }

    bool success = false;
    if (targetDisplay) {
        success = extract_space_info_from_display(targetDisplay, activeSpace, hasActiveSpace, info, outSpaceIDs);
    }

    if (activeDisplayIdentifier) {
        CFRelease(activeDisplayIdentifier);
    }
    CFRelease(displays);

    return success;
}

static bool iss_should_block_switch(const ISSSpaceInfo *info, ISSDirection direction) {
    if (!info) {
        return false;
    }
    if (info->spaceCount == 0) {
        return true;
    }

    unsigned int predicted;
    unsigned int current = get_prediction(info->displayID, &predicted) ? predicted : info->currentIndex;

    if (direction == ISSDirectionLeft) {
        return current == 0;
    }

    return current + 1 >= info->spaceCount;
}

bool iss_can_move(ISSSpaceInfo info, ISSDirection direction) {
    return !iss_should_block_switch(&info, direction);
}

static bool iss_post_dock_swipe(CGSGesturePhase phase, ISSDirection direction, double velocity) {
    const bool isRight = (direction == ISSDirectionRight);
    // Empirically, ±FLT_TRUE_MIN used in this way makes switching instant.
    const double progress = isRight ? (double)FLT_TRUE_MIN : -(double)FLT_TRUE_MIN;

    // Velocity of gesture based on speed setting
    const double vel = isRight ? velocity : -velocity;

    CGEventRef ev = CGEventCreate(NULL);
    if (!ev) {
        return false;
    }
    CGEventSetIntegerValueField(ev, kCGSEventTypeField, kCGSEventDockControl);
    CGEventSetIntegerValueField(ev, kCGEventGestureHIDType, kIOHIDEventTypeDockSwipe);
    CGEventSetIntegerValueField(ev, kCGEventGesturePhase, phase);
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipeProgress, progress);
    CGEventSetIntegerValueField(ev, kCGEventGestureSwipeMotion, kCGGestureMotionHorizontal);
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipeVelocityX, vel);
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipeVelocityY, vel);
    CGEventPost(kCGSessionEventTap, ev);
    CFRelease(ev);
    return true;
}

static bool iss_perform_switch_gesture(ISSDirection direction, double velocity) {
    // Send three gesture events--began, changed, and ended
    // If we only send two then mission control doesn't work.
    return iss_post_dock_swipe(kCGSGesturePhaseBegan,   direction, velocity)
        && iss_post_dock_swipe(kCGSGesturePhaseChanged, direction, velocity)
        && iss_post_dock_swipe(kCGSGesturePhaseEnded,   direction, velocity);
}

/** @brief Walks a CGWindowListCopyWindowInfo result
 *
 * Used for trying to determine if Exposé or Mission Control is active.
 *
 * @param windowList The window list to scan
 * @param outLayer18Count The count of layer-18 windows
 * @param outLayer20Count The count of layer-20 windows
 */
static void scan_dock_window_list(CFArrayRef windowList,
                                  int *outLayer18Count,
                                  int *outLayer20Count) {
    *outLayer18Count = 0;
    *outLayer20Count = 0;
    CFIndex count = CFArrayGetCount(windowList);
    for (CFIndex i = 0; i < count; i++) {
        CFDictionaryRef info = (CFDictionaryRef)CFArrayGetValueAtIndex(windowList, i);
        CFStringRef owner = (CFStringRef)CFDictionaryGetValue(info, CFSTR("kCGWindowOwnerName"));
        if (!owner || !CFEqual(owner, CFSTR("Dock"))) continue;
        int layer = 0;
        CFNumberRef layerNum = (CFNumberRef)CFDictionaryGetValue(info, CFSTR("kCGWindowLayer"));
        if (layerNum) {
            CFNumberGetValue(layerNum, kCFNumberIntType, &layer);
        }
        if (layer == 18) {
            (*outLayer18Count)++;
            continue;
        }
        if (layer == 20) {
            (*outLayer20Count)++;
        }
    }
}

// Testable helpers
bool iss_is_expose_detected_in_window_list(CFArrayRef windowList) {
    int layer18Count = 0;
    int layer20Count = 0;
    scan_dock_window_list(windowList, &layer18Count, &layer20Count);
    // App Exposé: layer-18 present, at least one layer-20, AND count(layer=20) <= count(layer=18)
    return layer18Count > 0 && layer20Count > 0 && layer20Count <= layer18Count;
}

bool iss_is_mission_control_detected_in_window_list(CFArrayRef windowList) {
    int layer18Count = 0;
    int layer20Count = 0;
    scan_dock_window_list(windowList, &layer18Count, &layer20Count);
    // Mission Control: layer-18 present AND count(layer=20) > count(layer=18)
    return layer18Count > 0 && layer20Count > layer18Count;
}

/// Returns true when App Exposé is active (1-2 layer-20 windows)
/// This heuristic is empirical and may not work in all cases.
bool iss_is_expose_active(void) {
    if (!overlayDetectionEnabled) return false;
    CFArrayRef windowList = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!windowList) return false;
    bool result = iss_is_expose_detected_in_window_list(windowList);
    CFRelease(windowList);
    return result;
}

/// Returns true when Mission Control is active (3+ layer-20 windows)
/// This heuristic is empirical and may not work in all cases.
bool iss_is_mission_control_active(void) {
    if (!overlayDetectionEnabled) return false;
    CFArrayRef windowList = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!windowList) return false;
    bool result = iss_is_mission_control_detected_in_window_list(windowList);
    CFRelease(windowList);
    return result;
}

void iss_set_overlay_detection_enabled(bool enabled) {
    overlayDetectionEnabled = enabled;
}

bool iss_init(void) {
    // Ensure diagnostic fprintf(stderr, ...) flushes immediately in release builds.
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    fprintf(stderr, "ISS: iss_init entered (pid=%d)\n", getpid());
    fflush(stderr);

    if (globalTap) {
        return true;
    }

    if (!predictionsDict) {
        predictionsDict = CFDictionaryCreateMutable(NULL, 0, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }

    CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp)
        | (1ULL << kCGSEventGesture) | (1ULL << kCGSEventDockControl);
    globalTap = CGEventTapCreate(
        kCGSessionEventTap,
        kCGHeadInsertEventTap,
        kCGEventTapOptionDefault,
        mask,
        eventTapCallback,
        NULL
    );

    if (!globalTap) {
        return false;
    }

    globalSource = CFMachPortCreateRunLoopSource(NULL, globalTap, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), globalSource, kCFRunLoopCommonModes);
    CGEventTapEnable(globalTap, true);

    return true;
}

void iss_destroy(void) {
    if (predictionsDict) {
        CFRelease(predictionsDict);
        predictionsDict = NULL;
    }
    if (globalTap) {
        CGEventTapEnable(globalTap, false);
        if (globalSource) {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), globalSource, kCFRunLoopCommonModes);
            CFRelease(globalSource);
            globalSource = NULL;
        }
        CFRelease(globalTap);
        globalTap = NULL;
    }
}

bool iss_get_space_info(ISSSpaceInfo *info) {
    if (!info) {
        return false;
    }

    memset(info, 0, sizeof(*info));
    return load_space_info_for_display(info, true, NULL);
}

bool iss_get_menubar_space_info(ISSSpaceInfo *info) {
    if (!info) {
        return false;
    }

    memset(info, 0, sizeof(*info));
    return load_space_info_for_display(info, false, NULL);
}

static bool iss_switch_with_info(const ISSSpaceInfo *info, ISSDirection direction) {
    if (iss_should_block_switch(info, direction)) {
        return false;
    }
    if (!iss_perform_switch_gesture(direction, gestureSpeed)) {
        return false;
    }

    return true;
}

bool iss_switch(ISSDirection direction) {
    ISSSpaceInfo info;
    if (iss_get_space_info(&info)) {
        unsigned int predicted;
        unsigned int current = get_prediction(info.displayID, &predicted) ? predicted : info.currentIndex;
        unsigned int target = direction == ISSDirectionLeft ? current - 1 : current + 1;

        if (!iss_switch_with_info(&info, direction)) {
            return false;
        }
        set_prediction(info.displayID, target);
        if (switchCallback) { switchCallback(target); }
        return true;
    }

    return iss_perform_switch_gesture(direction, gestureSpeed);
}

// Populates outputs with:
//   - wid: the focused window's CGWindowID
//   - grab: a point on its title bar for the synthetic drag
//   - axWindow (optional, owned by caller; released with CFRelease): the
//     AXUIElement reference so the caller can perform a re-raise after the
//     move. Pass NULL if you don't need it.
static bool iss_get_focused_window_and_grab_point(CGWindowID *outID,
                                                   CGPoint *outGrabPoint,
                                                   AXUIElementRef *outAxWindow) {
    if (!outID || !outGrabPoint) return false;
    if (&_AXUIElementGetWindow == NULL) return false;

    AXUIElementRef sys = AXUIElementCreateSystemWide();
    if (!sys) return false;

    AXUIElementRef focusedApp = NULL;
    AXError err = AXUIElementCopyAttributeValue(
        sys, kAXFocusedApplicationAttribute, (CFTypeRef *)&focusedApp);
    CFRelease(sys);
    if (err != kAXErrorSuccess || !focusedApp) return false;

    AXUIElementRef focusedWin = NULL;
    err = AXUIElementCopyAttributeValue(
        focusedApp, kAXFocusedWindowAttribute, (CFTypeRef *)&focusedWin);
    CFRelease(focusedApp);
    if (err != kAXErrorSuccess || !focusedWin) return false;

    // Fullscreen windows live on their own Space and cannot be moved.
    CFTypeRef isFullscreen = NULL;
    if (AXUIElementCopyAttributeValue(focusedWin,
            CFSTR("AXFullScreen"), &isFullscreen) == kAXErrorSuccess && isFullscreen) {
        bool fs = CFGetTypeID(isFullscreen) == CFBooleanGetTypeID() &&
                  CFBooleanGetValue((CFBooleanRef)isFullscreen);
        CFRelease(isFullscreen);
        if (fs) { CFRelease(focusedWin); return false; }
    }

    CGWindowID wid = 0;
    err = _AXUIElementGetWindow(focusedWin, &wid);
    if (err != kAXErrorSuccess || wid == 0) { CFRelease(focusedWin); return false; }

    // Window frame from AX (global screen coords, y grows down).
    CGPoint winPos = {0, 0};
    CGSize  winSize = {0, 0};
    AXValueRef posVal = NULL, sizeVal = NULL;
    if (AXUIElementCopyAttributeValue(focusedWin,
            kAXPositionAttribute, (CFTypeRef *)&posVal) == kAXErrorSuccess && posVal) {
        AXValueGetValue(posVal, kAXValueCGPointType, &winPos);
        CFRelease(posVal);
    }
    if (AXUIElementCopyAttributeValue(focusedWin,
            kAXSizeAttribute, (CFTypeRef *)&sizeVal) == kAXErrorSuccess && sizeVal) {
        AXValueGetValue(sizeVal, kAXValueCGSizeType, &winSize);
        CFRelease(sizeVal);
    }

    // Grab point: horizontal midpoint, 3px below the top edge. Matches
    // Hammerspoon's 2026 drag-and-switch technique; avoids the traffic-light
    // buttons on the left and works reliably across apps with unusual
    // title-bar layouts.
    CGPoint grab = {
        winPos.x + winSize.width / 2.0,
        winPos.y + 3.0
    };

    *outID = wid;
    *outGrabPoint = grab;
    if (outAxWindow) {
        *outAxWindow = focusedWin;  // transfer ownership to caller
    } else {
        CFRelease(focusedWin);
    }
    return true;
}


// Returns a newly allocated CFArray of CFNumber<CGSSpaceID>, or NULL.
static CFArrayRef iss_copy_spaces_for_window(CGSConnectionID cid, CGWindowID wid) {
    if (&CGSCopySpacesForWindows == NULL) return NULL;
    CFNumberRef widNum = CFNumberCreate(NULL, kCFNumberSInt32Type, &wid);
    if (!widNum) return NULL;
    const void *values[1] = { widNum };
    CFArrayRef wids = CFArrayCreate(NULL, values, 1, &kCFTypeArrayCallBacks);
    CFRelease(widNum);
    if (!wids) return NULL;
    // Mask 7 = user-visible + fullscreen + system (yabai uses 7)
    CFArrayRef spaces = CGSCopySpacesForWindows(cid, 7, wids);
    CFRelease(wids);
    return spaces;
}

// Add-and-remove alternative: CGSAddWindowsToSpaces then CGSRemoveWindowsFromSpaces.
// Doesn't use the restricted CGSSetWindowListWorkspace path at all.
// Mark a window to prefer the current space. If this sticks across a space
// switch, the window "follows" the viewport (this matches what the user sees
// with Raycast's "Move Window to Next Desktop").
bool iss_set_window_prefers_current(unsigned int windowID, bool prefer) {
    fprintf(stderr, "ISS: iss_set_window_prefers_current(wid=%u, prefer=%d)\n",
            windowID, prefer ? 1 : 0);
    if (&CGSMainConnectionID == NULL) return false;
    CGSConnectionID cid = CGSMainConnectionID();

    SLSSetWindowPrefersCurrentSpaceFn fn = (SLSSetWindowPrefersCurrentSpaceFn)
        dlsym(RTLD_DEFAULT, "SLSSetWindowPrefersCurrentSpace");
    if (!fn) {
        fprintf(stderr, "ISS:   SLSSetWindowPrefersCurrentSpace not found\n");
        return false;
    }
    CGError e = fn(cid, (CGWindowID)windowID, prefer);
    fprintf(stderr, "ISS:   SLSSetWindowPrefersCurrentSpace returned %d\n", (int)e);
    return e == kCGErrorSuccess;
}

bool iss_move_window_add_remove(unsigned int windowID, unsigned long long targetSpaceID) {
    fprintf(stderr, "ISS: iss_move_window_add_remove(wid=%u, sid=%llu)\n",
            windowID, targetSpaceID);
    if (&CGSMainConnectionID == NULL) return false;
    CGSConnectionID cid = CGSMainConnectionID();

    CGWindowID wid = (CGWindowID)windowID;
    CFNumberRef widNum = CFNumberCreate(NULL, kCFNumberSInt32Type, &wid);
    if (!widNum) return false;
    const void *wvals[1] = { widNum };
    CFArrayRef windows = CFArrayCreate(NULL, wvals, 1, &kCFTypeArrayCallBacks);
    CFRelease(widNum);
    if (!windows) return false;

    // Get current spaces for this window to know what to remove it from.
    CFArrayRef currentSpaces = iss_copy_spaces_for_window(cid, wid);
    if (currentSpaces) {
        CFIndex n = CFArrayGetCount(currentSpaces);
        fprintf(stderr, "ISS:   window currently on %ld space(s):", (long)n);
        for (CFIndex i = 0; i < n; i++) {
            CGSSpaceID s = 0;
            CFNumberGetValue((CFNumberRef)CFArrayGetValueAtIndex(currentSpaces, i),
                             kCFNumberSInt64Type, &s);
            fprintf(stderr, " %llu", (unsigned long long)s);
        }
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "ISS:   could not enumerate current spaces\n");
    }

    CGSSpaceID tsid = (CGSSpaceID)targetSpaceID;
    CFNumberRef tsidNum = CFNumberCreate(NULL, kCFNumberSInt64Type, &tsid);
    const void *svals[1] = { tsidNum };
    CFArrayRef targetSpaces = CFArrayCreate(NULL, svals, 1, &kCFTypeArrayCallBacks);
    CFRelease(tsidNum);

    if (&CGSAddWindowsToSpaces != NULL) {
        CGSAddWindowsToSpaces(cid, windows, targetSpaces);
        fprintf(stderr, "ISS:   CGSAddWindowsToSpaces posted\n");
    } else {
        fprintf(stderr, "ISS:   CGSAddWindowsToSpaces unavailable\n");
    }

    if (currentSpaces && &CGSRemoveWindowsFromSpaces != NULL) {
        CGSRemoveWindowsFromSpaces(cid, windows, currentSpaces);
        fprintf(stderr, "ISS:   CGSRemoveWindowsFromSpaces posted\n");
    }

    CFRelease(targetSpaces);
    if (currentSpaces) CFRelease(currentSpaces);
    CFRelease(windows);
    return true;
}

static void iss_log_spaces_for_window(const char *label, CGSConnectionID cid, CGWindowID wid) {
    CFArrayRef spaces = iss_copy_spaces_for_window(cid, wid);
    if (!spaces) {
        fprintf(stderr, "ISS:   %s wid=%u spaces=<nil>\n", label, wid);
        return;
    }
    CFIndex n = CFArrayGetCount(spaces);
    fprintf(stderr, "ISS:   %s wid=%u spaces=[", label, wid);
    for (CFIndex i = 0; i < n; i++) {
        CFNumberRef num = (CFNumberRef)CFArrayGetValueAtIndex(spaces, i);
        long long sid = 0;
        CFNumberGetValue(num, kCFNumberSInt64Type, &sid);
        fprintf(stderr, "%s%lld", i == 0 ? "" : ",", sid);
    }
    fprintf(stderr, "]\n");
    CFRelease(spaces);
}

bool iss_move_window_raw(unsigned int windowID, unsigned long long targetSpaceID) {
    fprintf(stderr, "ISS: iss_move_window_raw(wid=%u, sid=%llu)\n",
            windowID, targetSpaceID);
    if (&CGSMainConnectionID == NULL) {
        fprintf(stderr, "ISS:   CGSMainConnectionID unavailable\n");
        return false;
    }
    CGSConnectionID cid = CGSMainConnectionID();
    fprintf(stderr, "ISS:   cid=%d\n", (int)cid);

    if (&CGSSpaceSetCompatID == NULL || &CGSSetWindowListWorkspace == NULL) {
        fprintf(stderr, "ISS:   compat-ID symbols unavailable\n");
        return false;
    }

    // Matches Raycast's macOS >= 14.5 path exactly:
    //   CGSSpaceSetCompatID(cid, sid, 0x79616265)
    //   CGSSetWindowListWorkspace(cid, &wid, 1, 0x79616265)
    //   CGSSpaceSetCompatID(cid, sid, 0)
    //   sleep(1)
    // Return values are intentionally ignored (Raycast does not check them).
    const int compatID = 0x79616265;

    iss_log_spaces_for_window("BEFORE", cid, (CGWindowID)windowID);

    (void)CGSSpaceSetCompatID(cid, (CGSSpaceID)targetSpaceID, compatID);
    CGWindowID widArray[1] = { (CGWindowID)windowID };
    CGError setWorkspaceRet = CGSSetWindowListWorkspace(cid, widArray, 1, compatID);
    (void)CGSSpaceSetCompatID(cid, (CGSSpaceID)targetSpaceID, 0);
    fprintf(stderr, "ISS:   compat-ID dance posted; setWorkspaceRet=%d (ignored)\n", (int)setWorkspaceRet);

    iss_log_spaces_for_window("POST-DANCE", cid, (CGWindowID)windowID);

    fprintf(stderr, "ISS:   sleeping 1s (matching Raycast)...\n");
    sleep(1);

    iss_log_spaces_for_window("POST-SLEEP", cid, (CGWindowID)windowID);

    return true;
}

// Posts a key-down / key-up pair for the user-configured "Switch to Desktop N"
// symbolic hotkey (where N is 1-based, 1..16). Enables the hotkey if the user
// has it disabled in System Settings. Returns false if the symbol lookup fails
// or the hotkey has no configured keyboard shortcut.
static bool iss_post_switch_to_desktop_hotkey(unsigned int targetIndexOneBased) {
    if (targetIndexOneBased < 1 || targetIndexOneBased > 16) return false;
    if (&CGSGetSymbolicHotKeyValue == NULL) {
        fprintf(stderr, "ISS:   CGSGetSymbolicHotKeyValue unavailable\n");
        return false;
    }

    CGSSymbolicHotKey hotKey = (CGSSymbolicHotKey)(118 + targetIndexOneBased - 1);
    CGKeyCode keyCode = 0;
    CGEventFlags flags = 0;
    CGError err = CGSGetSymbolicHotKeyValue(hotKey, NULL, &keyCode, &flags);
    if (err != kCGErrorSuccess) {
        fprintf(stderr, "ISS:   CGSGetSymbolicHotKeyValue(%d) err=%d\n",
                (int)hotKey, (int)err);
        return false;
    }
    if (&CGSIsSymbolicHotKeyEnabled != NULL &&
        &CGSSetSymbolicHotKeyEnabled != NULL &&
        !CGSIsSymbolicHotKeyEnabled(hotKey)) {
        (void)CGSSetSymbolicHotKeyEnabled(hotKey, true);
        fprintf(stderr, "ISS:   enabled symbolic hotkey %d\n", (int)hotKey);
    }

    CGEventRef keyDown = CGEventCreateKeyboardEvent(NULL, keyCode, true);
    CGEventRef keyUp   = CGEventCreateKeyboardEvent(NULL, keyCode, false);
    if (!keyDown || !keyUp) {
        if (keyDown) CFRelease(keyDown);
        if (keyUp) CFRelease(keyUp);
        return false;
    }
    CGEventSetFlags(keyDown, flags);
    CGEventSetFlags(keyUp, 0);
    CGEventPost(kCGHIDEventTap, keyDown);
    CGEventPost(kCGHIDEventTap, keyUp);
    CFRelease(keyDown);
    CFRelease(keyUp);
    fprintf(stderr, "ISS:   posted symbolic hotkey %d (keycode=%u flags=0x%llx)\n",
            (int)hotKey, keyCode, (unsigned long long)flags);
    return true;
}

// Moves the focused window to the adjacent Space and switches there using
// Silica/Amethyst's technique: simulate a user-initiated drag on the window's
// title bar, trigger the built-in "Switch to Desktop N" symbolic hotkey, then
// release the drag. WindowServer carries the held window along as part of its
// normal drag-across-Spaces UX — bypassing the macOS 14.5+ window-move
// authorization gate entirely.
bool iss_switch_and_follow(ISSDirection direction) {
    fprintf(stderr, "ISS: iss_switch_and_follow(direction=%s)\n",
            direction == ISSDirectionLeft ? "left" : "right");

    ISSSpaceInfo info;
    memset(&info, 0, sizeof(info));
    if (!load_space_info_for_display(&info, true, NULL)) {
        fprintf(stderr, "ISS:   failed to load space info\n");
        return false;
    }
    if (iss_should_block_switch(&info, direction)) {
        fprintf(stderr, "ISS:   switch blocked by bounds\n");
        return false;
    }

    // Resolve target space index (1-based for the symbolic hotkey).
    unsigned int predicted;
    unsigned int current = get_prediction(info.displayID, &predicted) ? predicted : info.currentIndex;
    unsigned int targetZeroBased = (direction == ISSDirectionLeft) ? current - 1 : current + 1;
    unsigned int targetOneBased = targetZeroBased + 1;
    fprintf(stderr, "ISS:   current=%u target=%u (1-based=%u)\n",
            current, targetZeroBased, targetOneBased);

    CGWindowID wid = 0;
    CGPoint grab = {0, 0};
    AXUIElementRef axWin = NULL;
    if (!iss_get_focused_window_and_grab_point(&wid, &grab, &axWin)) {
        fprintf(stderr, "ISS:   no movable focused window; aborting\n");
        return false;
    }

    // Log the window owner for debugging.
    CFArrayRef windowList = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, wid);
    const char *ownerDesc = "?";
    char ownerBuf[256];
    if (windowList && CFArrayGetCount(windowList) > 0) {
        CFDictionaryRef wi = (CFDictionaryRef)CFArrayGetValueAtIndex(windowList, 0);
        CFStringRef owner = (CFStringRef)CFDictionaryGetValue(wi, CFSTR("kCGWindowOwnerName"));
        if (owner && CFGetTypeID(owner) == CFStringGetTypeID() &&
            CFStringGetCString(owner, ownerBuf, sizeof(ownerBuf), kCFStringEncodingUTF8)) {
            ownerDesc = ownerBuf;
        }
    }
    fprintf(stderr, "ISS:   window id=%u owner=%s grab=(%.1f, %.1f)\n",
            wid, ownerDesc, grab.x, grab.y);
    if (windowList) CFRelease(windowList);

    // Build the four drag-sequence events.
    CGEventRef mv   = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved,       grab, kCGMouseButtonLeft);
    CGEventRef down = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseDown,    grab, kCGMouseButtonLeft);
    CGEventRef drag = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseDragged, grab, kCGMouseButtonLeft);
    CGEventRef up   = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseUp,      grab, kCGMouseButtonLeft);
    if (!mv || !down || !drag || !up) {
        fprintf(stderr, "ISS:   CGEventCreate failed\n");
        if (mv) CFRelease(mv);
        if (down) CFRelease(down);
        if (drag) CFRelease(drag);
        if (up) CFRelease(up);
        if (axWin) CFRelease(axWin);
        return false;
    }
    CGEventSetFlags(mv, 0);
    CGEventSetFlags(down, 0);
    CGEventSetFlags(drag, 0);
    CGEventSetFlags(up, 0);

    fprintf(stderr, "ISS:   posting mouse-move/down/drag\n");
    CGEventPost(kCGHIDEventTap, mv);
    CGEventPost(kCGHIDEventTap, down);
    CGEventPost(kCGHIDEventTap, drag);

    // Give WindowServer a tick to register the drag as active.
    usleep(10 * 1000);

    // Fire the built-in Space-switch keyboard shortcut. This commits
    // WindowServer to the "carry dragged window across Spaces" state machine.
    fprintf(stderr, "ISS:   posting symbolic hotkey for desktop %u\n", targetOneBased);
    bool switched = iss_post_switch_to_desktop_hotkey(targetOneBased);

    // Empirically: posting a dock-swipe here does NOT interrupt the Space-slide
    // animation — WindowServer is already committed to the slide initiated by
    // the symbolic hotkey. The animation is a hard floor (~250 ms on Apple
    // Silicon) for this carry-by-drag technique. Raycast and Amethyst hit the
    // same wall.

    // 20 ms settle. Dropping below this risks WindowServer canceling the move
    // if mouse-up fires before the drag-carry commits.
    usleep(20 * 1000);

    fprintf(stderr, "ISS:   posting mouse-up (release drag)\n");
    CGEventPost(kCGHIDEventTap, up);

    // Re-focus the moved window. Two passes:
    //   Immediate: quick feedback if macOS hasn't restored focus yet.
    //   Delayed (~300 ms): safety net that fires AFTER macOS's space-focus
    //   restoration kicks in, so we win the race. Without this, focus
    //   intermittently lands on whatever was previously frontmost on the
    //   target Space.
    if (axWin) {
        pid_t winPid = 0;
        if (AXUIElementGetPid(axWin, &winPid) == kAXErrorSuccess && winPid > 0) {
            AXUIElementRef appEl = AXUIElementCreateApplication(winPid);
            if (appEl) {
                (void)AXUIElementSetAttributeValue(appEl,
                    kAXFrontmostAttribute, kCFBooleanTrue);
                CFRelease(appEl);
            }
        }
        (void)AXUIElementPerformAction(axWin, kAXRaiseAction);

        // Retain axWin for the delayed block; transfer ownership to block.
        AXUIElementRef pinnedAx = (AXUIElementRef)CFRetain(axWin);
        CFRelease(axWin);
        axWin = NULL;

        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
            pid_t pid = 0;
            if (AXUIElementGetPid(pinnedAx, &pid) == kAXErrorSuccess && pid > 0) {
                AXUIElementRef appEl = AXUIElementCreateApplication(pid);
                if (appEl) {
                    (void)AXUIElementSetAttributeValue(appEl,
                        kAXFrontmostAttribute, kCFBooleanTrue);
                    CFRelease(appEl);
                }
            }
            AXError raiseErr = AXUIElementPerformAction(pinnedAx, kAXRaiseAction);
            if (raiseErr != kAXErrorSuccess) {
                fprintf(stderr, "ISS:   delayed AXRaise returned %d\n", (int)raiseErr);
            }
            CFRelease(pinnedAx);
        });
    }

    CFRelease(mv);
    CFRelease(down);
    CFRelease(drag);
    CFRelease(up);

    if (switched) {
        set_prediction(info.displayID, targetZeroBased);
        if (switchCallback) { switchCallback(targetZeroBased); }
    }
    return switched;
}

bool iss_switch_to_index(unsigned int targetIndex) {
    ISSSpaceInfo info;
    if (!iss_get_space_info(&info)) {
        return false;
    }

    assert(info.spaceCount > 0);

    bool outOfBounds = targetIndex >= info.spaceCount;
    if (outOfBounds) {
        targetIndex = info.spaceCount - 1;
    }

    unsigned int predicted;
    unsigned int currentIndex = get_prediction(info.displayID, &predicted) ? predicted : info.currentIndex;

    if (currentIndex == targetIndex) {
        return !outOfBounds;
    }

    ISSDirection direction = currentIndex < targetIndex ? ISSDirectionRight : ISSDirectionLeft;
    unsigned int steps = direction == ISSDirectionRight ? (targetIndex - currentIndex) : (currentIndex - targetIndex);

    // Multiply velocity by number of steps for faster multi-space switching
    double velocity = gestureSpeed * steps;

    for (unsigned int i = 0; i < steps; i++) {
        if (!iss_perform_switch_gesture(direction, velocity)) {
            return false;
        }
    }

    set_prediction(info.displayID, targetIndex);
    if (switchCallback) { switchCallback(targetIndex); }
    return !outOfBounds;
}

void iss_set_swipe_override(bool enabled) {
    swipeOverrideEnabled = enabled;
    if (!enabled) {
        swipeTracking = false;
        swipeFired = false;
    }
}

void iss_set_gesture_speed(double speed) {
    gestureSpeed = speed;
}

void iss_reset_predictions(void) {
    if (predictionsDict) {
        CFDictionaryRemoveAllValues(predictionsDict);
    }
}

void iss_set_switch_callback(ISSSwitchCallback callback) {
    switchCallback = callback;
}
