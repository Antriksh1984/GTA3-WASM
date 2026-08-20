# Browser Input (Phase 5)

## re3's existing input abstraction (task 1)

```
Browser KeyboardEvent / MouseEvent / Gamepad API
        |
Emscripten's GLFW3 port (emsdk/.../src/lib/libglfw.js)
        |  translates DOM events into GLFW key codes / mouse button state /
        |  raw joystick axes+buttons -- exactly as it would for a native GLFW app
        v
src/skel/glfw/glfw.cpp
        |  keypressCB(): GLFW key code -> keymap[] -> RsKeyCodes -> RsEventHandler(rsKEYDOWN/UP)
        |  CapturePad(): glfwGetJoystickButtons/Axes -> RsEventHandler(rsPADBUTTONDOWN/UP)
        |  CPad::UpdateMouse() (src/core/Pad.cpp): glfwGetCursorPos/glfwGetMouseButton,
        |  polled once per frame, not event-driven
        v
src/skel/events.cpp -- platform-independent, knows nothing about GLFW or the browser
        |  KeyboardHandler/PadHandler: RsKeyCodes -> CPad::TempKeyState fields,
        |  ControllerConfig button states
        v
CPad::UpdatePads() (src/core/Pad.cpp) -- copies Temp* into New*/Old* once per frame
        v
Game code: CPad::GetPad(0)->GetSprint(), ->GetLeftStickX(), etc. (src/core/Pad.h)
```

**This entire pipeline is unmodified by this phase.** `src/skel/events.cpp` in
particular is platform-independent -- it has never heard of GLFW, the browser, or
Emscripten, and doesn't need to. Everything in this document is either (a) already
correct, validated by tracing and live-testing it, or (b) a fix at the browser/DOM
layer, strictly outside this pipeline (task 11: gameplay input semantics are
untouched -- `CPad`, `CControllerConfigManager`, and everything under
`src/skel/events.cpp` have zero changes in this phase).

## What already worked, unmodified (tasks 2, 3, 4)

- **Keyboard**: `keymap[]` in `src/skel/glfw/glfw.cpp` already maps WASD, arrows,
  Enter, Escape, Shift (L/R), Ctrl (L/R), Space, Tab, digits, and F1-F12 to
  `RsKeyCodes`. Emscripten's GLFW3 port delivers real DOM `KeyboardEvent`s through
  this unchanged.
- **Mouse**: `CPad::UpdateMouse()` polls `glfwGetCursorPos`/`glfwGetMouseButton`
  once per frame (not event-driven) and computes its own deltas -- this already
  works under Emscripten's GLFW3 port without changes.
- **Gamepad (buttons/axes)**: `CapturePad()` already has a graceful fallback for
  when SDL-style "mapped" gamepad data isn't available (`ControlsManager.m_NewState.
  isGamepad == false`): raw `glfwGetJoystickButtons`/`glfwGetJoystickAxes` values
  are used directly (`src/core/ControllerConfig.cpp`'s `UpdateJoyButtonState()`,
  the `else` branch). Since Phase 1 already had to set `isGamepad = false` under
  `__EMSCRIPTEN__` (Emscripten's GLFW3 port doesn't implement
  `glfwGetGamepadState`/`glfwUpdateGamepadMappings`, the SDL-style mapped API), this
  fallback path is what actually runs -- and Emscripten's own joystick functions
  pull straight from the browser's Gamepad API, which already normalizes most
  controllers to the "standard" button/axis layout, so this works for the common
  case. What's genuinely missing: `MapIdToButtonId`'s semantic button-name mapping
  (raw index is used instead), and the L2/R2-trigger-as-button special case in
  `CapturePad()` (guarded out under `__EMSCRIPTEN__` since it needs
  `GLFWgamepadstate`). Both are minor relative to having buttons/sticks work at all.

## Two real Emscripten-GLFW gaps found and fixed (task 8)

Investigating "release stuck keys / stop pointer lock / prevent stuck input states
on focus loss" turned up two genuine bugs in this Emscripten version's GLFW3 port
(`emsdk/upstream/emscripten/src/lib/libglfw.js`), not just missing engine code:

1. **`glfwSetWindowFocusCallback`'s callback is stored but never invoked.**
   `GLFW.onBlur` exists and runs on browser `blur`, but it only synthesizes
   key-release events for currently-held keys (`GLFW.onKeyChanged(i, 0)` for each) --
   it never calls the registered `windowFocusFunc`. This is *why* keyboard keys
   don't get stuck on focus loss (Emscripten's own blur handler already releases
   them) while `windowFocusCB` in `src/skel/glfw/glfw.cpp` is effectively dead code
   under Emscripten (kept as-is; it's harmless, and native builds still use it).
   Mouse buttons have no equivalent safety net (`CPad::UpdateMouse()` polls
   `glfwGetMouseButton`, which has nothing to reset if a `mouseup` never arrives),
   so they -- and gamepad/joystick temp state, for uniformity -- needed one.

2. **`GLFW_CURSOR_HIDDEN` is an unimplemented no-op**, and **`glfwGetInputMode`'s
   WASM import isn't linkable** in this Emscripten version (confirmed by an actual
   `LinkError: ... "glfwGetInputMode": function import requires a callable` when
   this phase first tried to use it -- not just a documentation gap). Concretely:
   `src/skel/glfw/glfw.cpp`'s existing `_InputInitialiseMouse()` call to
   `glfwSetInputMode(..., GLFW_CURSOR_HIDDEN)` silently does nothing under
   Emscripten (logs a console warning, native behavior unaffected since that call
   is unguarded/shared code that works fine natively). Only `GLFW_CURSOR_NORMAL`
   and `GLFW_CURSOR_DISABLED` are implemented.

### Fix: `re3_OnBrowserFocusLost()` (`src/skel/glfw/glfw.cpp`, `__EMSCRIPTEN__`-only)

```c
extern "C" EMSCRIPTEN_KEEPALIVE void re3_OnBrowserFocusLost(void) {
    CPad::GetPad(0)->Clear(false);
    CPad::GetPad(1)->Clear(false);
}
```

An `EMSCRIPTEN_KEEPALIVE`-exported function, called from `web/input.js` on `window`
`blur` and on the page becoming hidden (`document.visibilitychange`). It calls
`CPad::Clear()` -- the exact same function `src/skel/glfw/glfw.cpp` already calls at
startup (`psPostRWinit()`) -- which resets `NewState`/`OldState`/`TempKeyState`/
`NewKeyState`/`OldKeyState`/`NewMouseControllerState`/etc. for both pads in one call.
No new state-clearing logic was written; this only triggers the engine's own
existing reset from a browser-specific signal GLFW doesn't deliver reliably. Guarded
with `RsGlobal.ps != nil` since `PSGLOBAL(window)` isn't valid until partway through
`psInitialize()` (called partway through `main()`), and `web/input.js` only arms the
listener once `runEngineMain()` is about to call `callMain()`.

## Pointer lock (task 6)

Given `glfwGetInputMode`/`GLFW_CURSOR_HIDDEN` are unusable (above), pointer lock is
handled **entirely in `web/input.js`**, through the standard DOM Pointer Lock API,
not through GLFW at all:

- **Engaging** requires a genuine user gesture (browsers reject
  `requestPointerLock()` otherwise, and nothing in a `requestAnimationFrame`-driven
  C++ loop can ever provide one) -- `web/input.js`'s `setupPointerLock()` wires a
  small "Click to enable mouse look" pill (`#pointerlock-hint` in
  `web/index.html`) whose click handler calls `canvas.requestPointerLock()`
  directly.
- **Releasing** needs no gesture: `document.exitPointerLock()` is called on browser
  focus loss (alongside `re3_OnBrowserFocusLost()`, above) and whenever
  `re3_IsMenuActive()` -- a second tiny read-only `EMSCRIPTEN_KEEPALIVE` getter,
  returning `FrontEndMenuManager.m_bMenuActive` -- reports the menu just became
  active while locked (`setupMenuAwarePointerLockRelease()`, polled once per frame
  while actually locked). Menu mouse navigation needs absolute cursor position;
  pointer lock only ever reports relative movement deltas, so this keeps menu
  mouse-hover working correctly.
- **Why this still works without touching GLFW's cursor mode at all**: Emscripten's
  shared `Browser.pointerLock` flag -- which is what `glfwGetCursorPos()`'s delta
  calculation in `CPad::UpdateMouse()` actually reads via `Browser.
  calculateMouseEvent()` -- is kept in sync by a `pointerlockchange` listener
  Emscripten's core browser support registers itself, independent of *how* the lock
  was requested. So engaging/releasing lock purely from JS, bypassing
  `glfwSetInputMode` entirely, still correctly feeds into the engine's existing
  mouse-delta code with zero C++ changes to that logic.

`src/core/Pad.cpp`'s `CPad::UpdateMouse()` has a comment explaining this (no
functional change -- the `#else`/GLFW branch used by both native and Emscripten
builds is otherwise untouched).

## Fullscreen (task 7)

Unchanged from Phase 2 (`web/launcher.js`'s `setupFullscreen()`): a topbar button
calling `viewport.requestFullscreen()`/`document.exitFullscreen()`, with a
`fullscreenchange` listener that re-runs the existing canvas-resize logic. This
phase only adds a live "Fullscreen: active/inactive" line to the input debug panel.

## Debug overlay (task 10)

`web/input.js`'s `setupDebugOverlay()`, shown in the sidepanel's "Input debug"
section (toggle via the topbar's "Input" button). Deliberately reads raw browser
events directly (its own `keydown`/`keyup`/`mousemove`/`mousedown`/`wheel`
listeners, `navigator.getGamepads()` polled every animation frame) rather than
asking the engine for its interpreted state -- so it stays useful for verifying what
the *browser* is delivering even if re3 itself never starts, and is a genuinely
independent check on the C++ side's behavior rather than trusting it. Shows: which
keys are currently held (as chips, by `event.code`), live mouse position/movement
delta/buttons, pointer lock and fullscreen status, and any connected gamepad's id,
mapping, held buttons (with values), and axis values.

## Validation

Live-tested in this session (see the Phase 5 session log for the exact sequence):

- **Keyboard**: dispatched real `keydown`/`keyup` events and confirmed the debug
  overlay's held-keys list updates correctly, including clearing on `keyup`.
- **preventDefault**: confirmed `ArrowUp`/`F5` are prevented; confirmed `Space`/`Tab`
  are prevented when no interactive element has focus, and correctly *not*
  prevented when a page button is focused (respecting normal keyboard use of this
  page's own UI); confirmed `KeyW`/`Enter` are never prevented (re3 needs them).
- **Focus-loss recovery**: dispatched a real `blur` event and confirmed, end to end,
  that `web/input.js` called `ccall('re3_OnBrowserFocusLost', ...)` successfully
  (logged, no error) and that the debug overlay's own held-key tracking cleared.
- **Fullscreen**: unchanged code path from Phase 2, already validated then.
- **Pointer lock**: `requestPointerLock()` was rejected in this specific
  browser-automation pane with `WrongDocumentError: The root document of this
  element is not valid for pointer lock` -- a known Chrome error for pages embedded
  without pointer-lock permission delegated to their frame, i.e. an artifact of
  *this development environment's* preview embedding, not of the page or its code
  (the same class of limitation as the `document.hidden`/suspended-`requestAnimationFrame`
  issue noted in docs/BROWSER_RUNTIME.md's Phase 2 section). The `pointerlockerror`
  handler itself was confirmed to fire and log correctly. This should be re-verified
  once the page is served standalone (not embedded) in a real browser tab.
- **Controller**: no physical gamepad was available to test against in this
  environment; the code path was verified by tracing (above) rather than by live
  button presses. `navigator.getGamepads()`/`gamepadconnected` support is confirmed
  present in this browser via the diagnostics panel's "Gamepad API: true".
- **Engine regression check**: `re3_wasm` was rebuilt after every change in this
  phase and re-run against the same real GTA III install used in Phase 3/4 --
  `main()` still returns cleanly, reaching the same (unrelated, already-documented)
  `CdStreamPosix.cpp` semaphore blocker, confirming none of the input changes
  affected engine startup.
- **Native input**: every C++ change in this phase is either
  `#ifdef __EMSCRIPTEN__`-guarded (the two new exported functions) or a
  comment-only addition inside the shared GLFW/`#else` branch of
  `CPad::UpdateMouse()` (no executable change at all) -- confirmed by inspection,
  consistent with the "native input must remain unchanged" requirement. Actually
  compiling the native target remains blocked by this environment having no native
  C/C++ toolchain installed at all, per every earlier phase's notes.
