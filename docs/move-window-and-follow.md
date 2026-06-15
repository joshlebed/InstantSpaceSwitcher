# Move window & follow — implementation notes (WIP)

Status of the `move-window-and-follow` feature: moving the focused window to the
adjacent Space and switching there with it. This is a fork addition on top of
upstream InstantSpaceSwitcher (which only does instant Space *switching*).

**TL;DR:** works reliably and drift-free for normal Cocoa windows and Spotify
(CEF). Electron apps (Claude, ChatGPT) do **not** work yet — even Raycast can't
move them — because their in-app title-bar drag eats the Space-switch keystroke.
Options for fixing that are listed at the bottom.

---

## How it's wired up

The hotkey never reaches macOS as a normal shortcut. The chain is:

```
Karabiner (nav layer)        ISS app                       WindowServer
  caps+cmd+d  ─►  ⌥⇧⌘F16  ─►  iss_switch_and_follow(left)  ─►  carry + switch
  caps+cmd+f  ─►  ⌥⌘F16   ─►  iss_switch_and_follow(right)
```

ISS registers global hotkeys for those F16 combos (see `HotkeyConfiguration` /
`HotKeyManager`) and calls into the C core (`Sources/ISS/ISS.c`).

Karabiner config lives in the dotfiles repo (`~/.config/karabiner/`), not here.

---

## The move technique (what actually works on macOS 26)

### Dead ends — all gated by Apple on macOS 14.5+ / 26

The "move a window to a managed Space by ID" private APIs are **no-ops** on
macOS 26 (Tahoe) from a normal (non-SIP) process. Verified directly:

| API | Result on 26.5 |
| --- | --- |
| `CGSSetWindowListWorkspace` (compat-ID dance, magic `0x79616265` "yabe") | returns `kCGErrorNotImplemented (1006)`, window does not move |
| `CGSMoveWindowsToManagedSpace` | no-op |
| `CGSAddWindowsToSpaces` / `CGSRemoveWindowsFromSpaces` | no-op |

`iss_move_window_raw`, `iss_move_window_add_remove`, and the `move-window` /
`add-remove` ISSCli debug commands exist to exercise these and confirm they're
dead. They are kept only as probes.

### What Raycast actually does (decompiled)

`Raycast.app` → `WindowManagementService.moveWindowWithMouseClick(in:to:)`,
gated on `isOSVersionAtLeast(14,5)`:

- **≥ 14.5 (our case):** runs the **same** compat-ID dance above — which is
  dead — *plus* a synthetic mouse click. The dance is dead weight; the click is
  what moves the window.
- **< 14.5:** `CGSMoveWindowsToManagedSpace`.

The real mechanism is a **drag-carry**: warp the hardware cursor to the title
bar, hold left-mouse-down, switch Spaces (the held window rides along via
WindowServer's native drag-across-Spaces UX), release. Specifically Raycast:

1. `CGDisplayMoveCursorToPoint` — warp the **real** cursor to the title bar.
2. `CGEventSourceCreate(kCGEventSourceStatePrivate)`; create `LeftMouseDown` +
   `LeftMouseUp` at the **same** point (`+5, +20` from the window's top-left),
   with timestamps. Zero motion ⇒ no window drift.
3. Switch Spaces (Ctrl+number symbolic hotkey) between the down and up.
4. Restore the cursor.

### Our implementation (`iss_switch_and_follow`)

Replicates the above and drops the dead dance entirely (so it's *faster* than
Raycast — no 1 s sleep):

- grab point `(left + 5, top + 20)` (`iss_grab_point_for_frame`, env-tunable via
  `ISS_GRAB_DX` / `ISS_GRAB_DY`);
- `CGWarpMouseCursorPosition` to the grab point, save & restore the cursor;
- private `CGEventSource`, timestamped `LeftMouseDown` → Switch-to-Desktop
  hotkey → `LeftMouseUp`;
- **zero cursor motion** between down and up — the window never translates, only
  the Space slides under it.

The key fidelity details that make **AX-hostile apps (Spotify)** engage where the
previous "post a `mouseMoved` event" approach didn't: the **real cursor warp**
and the **`(+5,+20)` grab point** (a draggable title-bar region). Verified
pixel-identical position before/after (zero drift) for both Chrome and Spotify.

### Why no `mouseMoved` / no wiggle

- A posted `kCGEventMouseMoved` is not enough for Spotify to hand the drag to
  WindowServer; a real `CGWarpMouseCursorPosition` is.
- Any cursor **motion** to "arm" the drag (the old wiggle) displaces the window,
  and AX-hostile windows expose no AX handle to restore the position ⇒ permanent
  drift. Zero-motion is mandatory.

---

## Focus follow

After a successful carry, macOS focuses whatever was previously frontmost on the
**target** Space (often Chrome). We re-activate the moved window's app by **PID**
(works even for AX-hostile windows with no AX handle), immediately and again
after ~300 ms to win the race against macOS's focus restoration. An AX handle,
when available, also raises the specific window. See `iss_refocus_window`.

---

## Unsupported-app handling (#1 — implemented)

For apps that won't engage the drag, the Space still switches but the window
stays behind, and the re-focus would then yank the viewport back — a janky
forward+snap-back. Now we **detect** it:

- snapshot the window's Space before the move (`iss_window_primary_space`);
- after mouse-up, poll (~20 ms × up to 10) for the Space to change — exits fast
  on success, only a genuinely-stuck window waits the full ~200 ms;
- if it **didn't** carry: revert to the source desktop, skip the re-focus, and
  return `false` so the app **beeps** ("can't move this window").

This makes the failure a clean no-op-with-feedback instead of a janky double
jump. It does **not** yet make Electron apps move — that's the open problem.

---

## Open problem: Electron apps (Claude, ChatGPT)

All three of Spotify/Claude/ChatGPT are Chromium, but the shell differs:

- **Spotify (CEF)** + Cocoa apps: the title-bar mouse-down starts a
  **WindowServer-native** window drag ⇒ a keyboard Space-switch carries it. ✅
- **Claude / ChatGPT (Electron):** the drag region calls Chromium's
  `performWindowDragWithEvent:`, which runs a **nested event-tracking loop in
  the app**. That loop **swallows the `Ctrl+number` Space-switch keystroke**, so
  the carry can't complete. This is also why **Raycast can't move them** — same
  keyboard-based technique, same wall.

So a fix must not depend on a keyboard switch landing while an Electron drag loop
is running.

### Options (cheapest → heaviest)

- **A. Gesture switch instead of keyboard.** ISS's instant-switch already uses a
  synthetic **dock-swipe gesture** (`iss_post_dock_swipe`), a WindowServer-level
  event that may switch the Space *while* Electron's nested loop eats keystrokes.
  If the gesture carries the held window, Electron just works — no SIP, no
  slowdown. Untested; directly targets the root cause; cheap.
- **B. Motion + restore.** Electron *does* engage with cursor motion (Claude
  carried in earlier wiggle sweeps); wiggle to engage, then restore the exact
  frame via AX so there's no drift. Risk: still hits the keyboard-eaten issue,
  and depends on Electron exposing a writable AX frame (inconsistent).
- **C. Sticky-tag trick.** Make the window appear on all Spaces, switch, then
  un-stick it. No drag needed. Likely gated (`CGSSetWindowTags` is the same
  family as the already-dead `CGSAddWindowsToSpaces`) — but a 10-min probe.
- **D. Mission Control automation (robust fallback).** Trigger Mission Control
  and drag the window's **thumbnail** to the target desktop. Works for **every**
  app (no app-drag involved), drift-free, no SIP. Downsides: ~0.5–1 s, fragile
  (navigate the Dock's MC accessibility tree or compute scaled thumbnail
  positions). Best wired as a fallback when #1's detector reports no-carry.
- **E. yabai + partial SIP disable.** Reliable for everything, instant — but
  needs SIP off, which is the exact thing ISS was built to avoid.

**Recommended order:** try **A** (gesture switch) and **C** (sticky probe)
first; if both fail, build **D** (MC fallback) for full coverage without SIP.

---

## Debug tooling (ISSCli)

```
ISSCli mv-left | mv-right                 # the real move-and-follow path
ISSCli move-window <wid> <spaceID>        # probe: compat-ID dance (dead on 26)
ISSCli add-remove  <wid> <spaceID>        # probe: Add/RemoveWindowsFromSpaces (dead)
ISSCli click-move  <wid> <spaceID> <gx> <gy>  # probe: click + dance
ISSCli prefer <wid> <0|1>                 # probe: SLSSetWindowPrefersCurrentSpace
```

The CLI pumps the runloop after a move so it faithfully reproduces the GUI app
as a test harness. Note: when run from a terminal the terminal is frontmost, so
the focused-window resolver grabs the *terminal's* window — drive tests with the
target genuinely frontmost (or target a known window id).

Env knobs: `ISS_GRAB_DX`, `ISS_GRAB_DY`.

---

## Build / install

```
./dist/build.sh                            # universal release → build/InstantSpaceSwitcher.app
# re-sign with Developer ID so TCC/Accessibility carries across rebuilds:
codesign --force --deep --options runtime \
  --entitlements <ents> \
  --sign "Developer ID Application: … (Q65U6C65ZZ)" build/InstantSpaceSwitcher.app
# then quit + replace /Applications/InstantSpaceSwitcher.app + relaunch
```

The bundled CLI shares the bundle's signing identity, so it keeps Accessibility.
