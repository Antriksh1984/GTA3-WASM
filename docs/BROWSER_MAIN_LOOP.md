# The main loop in the browser (Phase 7)

## Trace: from `main()` to a rendered frame

```
main() (src/skel/glfw/glfw.cpp / src/skel/win/win.cpp -- same PC state machine)
    |  psInitialize() -> RsEventHandler(rsINITIALIZE) -> CGame::InitialiseOnceBeforeRW()
    |  psSelectDevice() -> RwEngineSetVideoMode() -> psPostRWinit()
    v
The PC game-state machine, one state transition (or one Idle() call) per loop
iteration: GS_START_UP -> GS_INIT_ONCE -> GS_INIT_FRONTEND -> GS_FRONTEND ->
GS_INIT_PLAYING_GAME -> GS_PLAYING_GAME (steady state during gameplay)
    v
Idle(arg) (src/core/main.cpp), called via RsEventHandler(rsIDLE, ...) once the
frame limiter (below) says it's time:
    1. CTimer::Update()              -- timers / simulation timestep
    2. CGame::Process()              -- simulation tick (world, peds, vehicles,
    |                                    script, physics -- starts with
    |                                    CPad::UpdatePads(), input polling)
    3. DMAudio.Service()             -- audio updates
    4. CRenderer::ConstructRenderList / PreRender / RenderScene / Render2dStuff /
       RenderMenus / DoFade / DoRWStuffEndOfFrame   -- rendering
```

**The loop that calls all this** lives at the bottom of `main()` in
`src/skel/glfw/glfw.cpp`, and is *the same source* for native and Emscripten --
compiled differently by one `#ifdef __EMSCRIPTEN__`:

```cpp
#ifdef __EMSCRIPTEN__
    auto tick = []() {
#else
    while (!RsGlobal.quit && !FrontEndMenuManager.m_bWantToRestart && !glfwWindowShouldClose(window)) {
#endif
        glfwPollEvents();
        switch (gGameState) { /* ... state machine, calls RsEventHandler(rsIDLE, ...) in GS_PLAYING_GAME ... */ }
#ifdef __EMSCRIPTEN__
        if (RsGlobal.quit || glfwWindowShouldClose(window) || FrontEndMenuManager.m_bWantToRestart)
            emscripten_cancel_main_loop();
    };
    emscripten_set_main_loop(tick, 0, 1);
#else
    }
#endif
```

**Answering tasks 3/4 up front: this was already correct** before this phase --
`tick` is a plain closure and `emscripten_set_main_loop(tick, 0, 1)` hands control
back to the browser after every call. There is no `while(true)` on the browser's
main thread: the second argument (`fps=0`) tells Emscripten to drive `tick()` off
`requestAnimationFrame` rather than a fixed interval, and Emscripten calls it,
returns to the browser's event loop, and calls it again next rAF -- exactly the
non-blocking pattern task 4 asks for. Nothing in this phase needed to change that
wiring; the work here was auditing it, instrumenting it, and fixing one thing
that was silently preventing the state machine from ever reaching
`GS_PLAYING_GAME` in a browser at all (see "Fix" below).

## Simulation timestep vs. render rate (tasks 5, 6, 8)

GTA III's simulation was never fixed-timestep -- it's variable-timestep, scaled
by real elapsed wall-clock time, and this is untouched, unmodified, shared code
(`src/core/Timer.cpp`):

```cpp
void CTimer::Update(void) {
    ...
    frameTime = (double)(timer - oldPcTimer) * ms_fTimeScale;   // real elapsed ms
    ...
    ms_fTimeStep = frameTime / 1000.0f * 50.0f;                 // normalized to a 50fps reference
    ...
    ms_fTimeStep = Min(3.0f, ms_fTimeStep);                     // clamp a single frame's step
    if ((m_snTimeInMilliseconds - m_snPreviousTimeInMilliseconds) > 60)
        m_snTimeInMilliseconds = m_snPreviousTimeInMilliseconds + 60;   // clamp a single frame's delta
}
```

Every piece of movement/physics/animation code in the engine scales by
`CTimer::ms_fTimeStep`, not by a frame count -- so simulation speed is already
independent of how often `Idle()` actually runs, whether that's 30 Hz, 60 Hz, or
a variable-refresh display's native rate. This satisfies task 6 ("separate
simulation timestep from rendering framerate") **without any browser-specific
code**: the separation already exists, because native GTA III needed it too (a
30Hz-targeting PS2-era engine still had to run correctly on PCs with monitors at
60, 75, 100+ Hz). The two clamps above (`Min(3.0f, ...)` on the timestep, 60ms on
the millisecond delta) are what satisfies task 8 ("prevent simulation speed from
depending incorrectly on browser FPS") -- a single abnormally large gap between
frames (a GC pause, a slow asset load, a backgrounded tab resuming, see task 9
below) produces one clamped, bounded step instead of the simulation "catching up"
all at once.

## Frame-rate limiting and refresh-rate support (task 7)

A separate, also-unmodified mechanism decides *how often* `Idle()` gets called at
all, independent of the timestep-normalization above:

```cpp
case GS_PLAYING_GAME: {
    float ms = (float)CTimer::GetCurrentTimeInCycles() / (float)CTimer::GetCyclesPerMillisecond();
    if (!CMenuManager::m_PrefsFrameLimiter || (1000.0f / (float)RsGlobal.maxFPS) < ms)
        RsEventHandler(rsIDLE, (void *)TRUE);
    break;
}
```

`RsGlobal.maxFPS` defaults to **30** (`src/skel/skeleton.cpp:410`) and
`CMenuManager::m_PrefsFrameLimiter` defaults to **on** (`src/core/Frontend.cpp:153`)
-- both exactly matching native PC re3's defaults, both changeable through the
game's own Options menu (the "Frame Limiter" toggle) exactly as on native, no
browser-specific UI needed. Combined with the rAF-paced `tick()` above:

- **30 FPS** (default): `tick()` fires every rAF callback (matching the display's
  refresh rate, e.g. 60 or 120 Hz), but `Idle()` only actually runs once enough
  real time (33.3ms) has passed since the last one -- most `tick()` calls are a
  `glfwPollEvents()` + a skipped `case`, not a full simulate+render.
- **60 FPS**: raise `RsGlobal.maxFPS` (via the same debug-menu var already wired
  at `src/core/re3.cpp:935`, or any settings UI built on top of it) -- the same
  elapsed-time check now gates at 16.7ms instead.
- **Variable refresh rate**: turning the frame limiter off removes the gate
  entirely, so `Idle()` runs on every `tick()` -- i.e. every rAF callback --
  which tracks whatever rate the browser/display is actually presenting at,
  including adaptive-sync displays. This is the same "uncapped" behavior native
  re3 has always offered.

No code changes were needed for any of this -- it's the existing, general
elapsed-time-based limiter working correctly against a rAF-paced browser loop.

## Browser tab throttling (task 9)

`requestAnimationFrame` -- what `emscripten_set_main_loop(tick, 0, 1)` is built
on -- stops firing entirely while a tab is hidden or backgrounded. That means
`tick()`, and everything inside it, simply stops being called; nothing spins,
polls, or burns CPU in the background. The only risk is the *next*
`CTimer::Update()` once the tab becomes visible again, whose measured delta would
otherwise include the entire hidden duration. The clamps described above already
bound the damage to a single 60ms-equivalent step -- but this phase adds the more
precise, purpose-built fix: `CTimer::Suspend()`/`CTimer::Resume()`, the same pair
already used natively for "this real time gap must not count as elapsed game
time" (long collision precomputation in `Collision.cpp`, script pauses in
`Script.cpp`/`Script4.cpp`/`Script6.cpp`).

Two new exports (`src/skel/glfw/glfw.cpp`, `#ifdef __EMSCRIPTEN__`):

```cpp
extern "C" EMSCRIPTEN_KEEPALIVE void re3_OnBrowserTabHidden(void)  { CTimer::Suspend(); }
extern "C" EMSCRIPTEN_KEEPALIVE void re3_OnBrowserTabVisible(void) { CTimer::Resume(); }
```

`web/perf.js`'s `setupTabThrottling()` calls these from the page's
`visibilitychange` event, tracking its own hidden/visible state so the two are
always called in matched pairs regardless of how a browser fires the event
(repeated events, or a resume without a preceding hide, would otherwise leave
`CTimer`'s internal suspend depth unbalanced) -- and, like Phase 5's focus-loss
recovery, is only armed once the engine's `main()` is actually about to run.

## Frame timing diagnostics (task 10)

re3 already has a native frame-timing profiler (`src/core/timebars.cpp`,
active whenever `TIMEBARS` is defined -- the default for a non-`FINAL` build,
unaffected by this phase) and a native rolling FPS counter
(`FramesPerSecond`/`FramesPerSecondCounter`, `src/core/main.cpp`). Both still
work exactly as before. Neither is reachable from browser-side JS, though: the
timebar display draws directly into the rendered 3D scene and requires toggling
`gbShowTimebars` through the native debug menu, and `FramesPerSecond` is a
C++-only global with no exported getter.

This phase adds a small, independent, `#ifdef __EMSCRIPTEN__`-guarded
instrumentation block directly in `Idle()` (`src/core/main.cpp`), using
`emscripten_get_now()` (a high-resolution, `performance.now()`-backed timer) to
bracket exactly the same three phases already described above:

```cpp
Idle(void *arg) {
    CTimer::Update();
    g_re3FrameStartMs = emscripten_get_now();          // frame start
    ...
    CGame::Process(); DMAudio.Service();                // simulation + audio
    g_re3SimEndMs = emscripten_get_now();               // sim end
    ...
    if (arg == nil) return;
    double renderStartMs = emscripten_get_now();        // render start
    ... construct render list, PreRender, RenderScene, Render2dStuff, RenderMenus, DoFade ...
    // render end: g_re3RenderTimeMs, g_re3FrameTimeMs, g_re3SimTimeMs, g_re3FPS (EMA-smoothed) updated here
}
```

Four read-only getters expose these to JS: `re3_GetFPS()`, `re3_GetFrameTimeMs()`,
`re3_GetSimTimeMs()`, `re3_GetRenderTimeMs()` (all `double`, all return 0 before
the first frame has rendered). `web/perf.js`'s `setupFrameTimingPanel()` polls
them once per `requestAnimationFrame` and renders them into a new "Frame timing"
section in `web/index.html`'s existing diagnostics sidepanel, next to the
WebGL/WASM/input diagnostics already there from Phases 2-5.

## A blocking bug found (and fixed) along the way

Validating this phase live -- mounting a real GTA III install and clicking
"Start engine" -- surfaced a pre-existing bug that stopped the game from ever
reaching `GS_PLAYING_GAME` in a browser at all, which would have made every task
above unobservable in practice. `psSelectDevice()` (`src/skel/glfw/glfw.cpp`)
searches RenderWare's video-mode list for one flagged `rwVIDEOMODEEXCLUSIVE`
("exclusive fullscreen") matching the requested width/height/depth, and aborts
device selection entirely (`return FALSE`) if none is found. GLFW's Emscripten
port never reports a canvas as an exclusive-fullscreen video mode -- a browser
`<canvas>` has no OS-level concept of one -- so this search could never succeed,
and the very first attempt to start the engine failed immediately with `WARNING:
Cannot find desired video mode, selecting device cancelled`, before the game
state machine ever left `GS_START_UP`.

Fixed with a narrow, `#ifdef __EMSCRIPTEN__`-guarded fallback: if no exclusive
mode is found, fall back to the windowed mode already found in the same search
loop, rather than aborting. Real fullscreen continues to be handled entirely
through the browser's own Fullscreen API (`web/input.js`, from Phase 5) -- a
separate, browser-layer concern from RenderWare's video-mode model, same as
Pointer Lock. This is unrelated to frame timing/main-loop pacing, but is included
here because it was discovered during this phase's validation and directly
blocked observing any of the above.

## Validation

- `cmake --build build-wasm --target re3_wasm` compiles and links cleanly with
  all changes (61 pre-existing warnings, none introduced by this phase, 0
  errors).
- `re3_GetFPS`/`GetFrameTimeMs`/`GetSimTimeMs`/`GetRenderTimeMs` confirmed
  callable via `ccall` from the browser console before the engine starts,
  returning `0` (the documented pre-first-frame value) with no exceptions.
- With a real GTA III install mounted via the dev-asset server
  (`python scripts/serve_web.py <port> web --dev-assets gamefiles`,
  `?devAssets=1`), clicking "Start engine" previously failed immediately with
  the video-mode warning above (`main()` returned `0`, a real synchronous exit
  after `CGame::FinalShutdown()` -- confirmed in the log by the
  `GS_START_UP` -> shutdown sequence with no state progress in between). After
  the fix, device selection succeeds: the log shows a real `OpenGL version:
  OpenGL ES 3.0 (...)` line (RenderWare's GL3 device actually opened) where it
  previously never got that far, and `main()` returns `undefined` rather than a
  number -- the signature of Emscripten's `emscripten_set_main_loop(..., 1)`
  unwinding the C call stack via its internal "throw to yield" mechanism, i.e.
  `tick()` was successfully registered and control returned to the browser
  event loop, rather than the engine exiting synchronously.
- **Known environment limitation, not a defect**: the browser automation harness
  used for this validation reports `document.hidden === true` /
  `visibilityState === "hidden"` for its entire lifetime, and
  `computer{action:"screenshot"}` fails outright with "the Browser pane is not
  displayed, so the page is not compositing frames" -- i.e. this harness's tab
  is never actually composited, which is *why* it's permanently hidden, not an
  intermittent flake. The same artifact was documented in Phases 2, 4, and 5.
  `requestAnimationFrame` never fires in that state for *any* caller -- not just
  the engine's `tick()`, but this phase's own `web/perf.js` polling loop too,
  confirmed by its diagnostics table remaining completely empty
  (`#perf-table.innerHTML === ""`) even minutes after the engine registered its
  main loop. A real user's foreground tab does not have this problem. This
  phase's correctness argument for the in-gameplay behavior (variable-timestep
  scaling, frame-rate limiting, tab-suspend clamping) therefore rests on direct
  source tracing of the shared, unmodified `CTimer`/state-machine code (above)
  plus confirming the loop *registers* correctly, rather than an observed FPS
  counter -- the same "verify from code when the harness can't render a live
  frame" approach used for input focus-loss recovery in Phase 5. The JS-side
  diagnostics table logic itself was separately confirmed correct by manually
  invoking its row-creation code with synthetic values in the browser console.

## Native build: unaffected

Every change in this phase is either `#ifdef __EMSCRIPTEN__`-guarded (the
`Idle()` instrumentation and its four getters in `src/core/main.cpp`, the two new
`re3_OnBrowserTab*` exports and the video-mode fallback in
`src/skel/glfw/glfw.cpp`) or purely additive new files (`web/perf.js`, this
document). `src/core/Timer.cpp`, the state-machine loop structure, and the
frame-limiter logic were not touched at all -- they were already shared,
platform-neutral code doing the right thing. Native builds compile and behave
exactly as before.
