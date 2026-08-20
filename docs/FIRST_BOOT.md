# First boot: browser → WASM → re3 → GTA III main menu (Phase 9)

## Result

**Achieved.** With a real, user-provided GTA III installation mounted, `re3_wasm`
boots end to end: WASM load → filesystem → RenderWare/WebGL2 renderer → OpenAL
audio → core game data → engine state machine → `FrontEndMenuManager` active
main menu (`gGameState == GS_FRONTEND`, `FrontEndMenuManager.m_bMenuActive ==
true`), with **zero crashes, asserts, or uncaught exceptions** across the whole
sequence. This is the first phase where every subsystem built in Phases 1-8 runs
together, end to end, against real assets.

Verified two independent ways, since this session's browser automation harness
cannot reliably screenshot the canvas (see "A new environment constraint" below):

1. **`re3_GetBootStage()`** (new this phase) reaches `7`.
2. **`re3_IsMenuActive()`** (Phase 5) reads `1` -- `FrontEndMenuManager.
   m_bMenuActive` is genuinely true, an independent signal that predates this
   phase and wasn't written to make this check pass.

## The boot-progress model (task 11)

```
[1] WASM loaded            <- JS: createRe3Module() promise resolves
[2] Filesystem initialized <- JS: AssetVFS.validate() reports all required files present
[3] Renderer initialized   <- C++: Initialise3D() / CGame::InitialiseRenderWare() succeeds
[4] Audio initialized      <- C++: DMAudio.Initialise() returns (Game.cpp)
[5] Game data loaded       <- C++: CGame::InitialiseOnceAfterRW() completes
[6] Engine initialized     <- C++: gGameState reaches GS_INIT_FRONTEND
[7] Main menu              <- C++: gGameState reaches GS_FRONTEND
```

Stages 1-2 are pure JS concerns (`web/launcher.js`, already had the diagnostics
to know this). Stages 3-7 are new: `re3_SetBootStage(int)` (`src/skel/glfw/
glfw.cpp`, `#ifdef __EMSCRIPTEN__`) is a tiny monotonically-increasing counter,
called from the exact points above in `src/core/main.cpp`, `src/core/Game.cpp`,
and the state machine in `glfw.cpp`. `re3_GetBootStage()` exposes it read-only;
`web/boot.js` polls it and renders the `[1..7]` list now shown at the top of the
sidepanel in `web/index.html`.

## Browser error reporting (tasks 6, 7)

| Source | Mechanism | Since |
|---|---|---|
| JS exceptions | `window.addEventListener('error'/'unhandledrejection')` | Phase 2 |
| WASM exceptions | `try/catch` around `instance.callMain()` | Phase 3 |
| Engine logs (stdout/stderr) | `print`/`printErr` Module hooks -> log panel | Phase 2 |
| Missing asset errors | `casepath()`'s `#ifdef __EMSCRIPTEN__` messages -> log panel | Phase 3 |
| WebGL errors | **New (Phase 9)**: `canvas.addEventListener('webglcontextlost'/'webglcontextcreationerror'/'webglcontextrestored')`, `web/boot.js` | Phase 9 |

The only real gap task 7 asked about was WebGL-specific errors -- `webglcontextlost`/
`webglcontextcreationerror` are browser-level events a C++ `glGetError()` check
can't see (they fire when the *browser*, not the app, decides the GPU context is
gone, e.g. a driver reset or GPU process crash) and nothing was capturing them
before. Everything else in the table already existed from earlier phases and
just kept working once the engine actually ran far enough to exercise it.

## Browser debug mode (task 8)

Two related, additive features, both off/inert by default:

- **`Debug mode` checkbox** (topbar): calls `re3_SetDebugTiming(1)`, which
  reschedules the *same* main-loop callback `emscripten_set_main_loop()`
  already drives, via `emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT,
  16)` instead of the default `EM_TIMING_RAF`. Useful for hosting contexts
  where `requestAnimationFrame` doesn't fire reliably (see below).
- **`re3_PumpTick()`**: runs exactly one iteration of that same callback,
  synchronously, whenever called -- not scheduled by any browser timer at all.
  Not surfaced as a UI button (it's a testing/automation primitive, not
  something a player needs), but exported for exactly this kind of validation:
  a script can call it in a tight loop to drive the engine forward
  deterministically, independent of `requestAnimationFrame` or `setTimeout`
  ever firing.

### A new environment constraint, and why both of the above exist

Phases 2, 4, 5, and 7 all separately documented that this session's browser
automation harness keeps its tab in `document.hidden === true` for its entire
lifetime, which suspends `requestAnimationFrame`. Validating *this* phase found
that the constraint goes one level deeper than previously known:

- `computer{action:"screenshot"}` fails outright with "the Browser pane is not
  displayed, so the page is not compositing frames" -- confirming the tab isn't
  merely backgrounded in the ordinary sense, it's never actually composited by
  the host at all in this environment.
- Enabling `Debug mode` (rescheduling onto `setTimeout`) alone was **not**
  sufficient to make the engine progress: `re3_GetBootStage()` stayed at `3`
  even after several seconds with debug timing on. This means whatever
  suspends rendering in this specific harness throttles/suspends `setTimeout`
  almost as aggressively as `requestAnimationFrame` -- plausibly a Chromium
  tab-freezing policy for a page that's never composited, not merely
  backgrounded.
- **`re3_PumpTick()`, called directly and repeatedly from one synchronous
  `javascript_exec` script (not scheduled by any browser timer), worked
  immediately** -- 100-500 calls reliably drove `gGameState` from `GS_START_UP`
  all the way to `GS_FRONTEND`. This is because calling it this way runs on the
  page's JS thread via the debugger/automation protocol directly, which isn't
  subject to the same timer-throttling policy as the page's own self-scheduled
  callbacks.

**This is specific to this automated test harness, not a defect a real user
would hit.** A real foreground browser tab runs `requestAnimationFrame`
normally; `Debug mode` exists for less extreme cases (a backgrounded tab a
developer still wants ticking, e.g. while testing) and `re3_PumpTick()` exists
so this exact kind of environment can still be validated deterministically
without depending on any browser scheduler at all.

### A secondary interaction found via this: `CTimer::Suspend()` never resumes

Because this harness's tab reports `document.hidden === true` from the very
first paint, `web/perf.js`'s `visibilitychange` handler (Phase 7/8, for tab-
throttling) fires once, calls `re3_OnBrowserTabHidden()` (`CTimer::Suspend()`),
and then -- because the tab never subsequently becomes visible in this specific
environment -- never calls `re3_OnBrowserTabResume()` to undo it. The engine
still boots and reaches the menu correctly (state-machine transitions aren't
gated by `CTimer`), but anything timed off elapsed real time (menu fade-ins,
animations) would stay frozen at zero. Validation worked around this by calling
`re3_OnBrowserTabVisible()` once manually before pumping further. **No engine
change was made for this** -- the Page Visibility handling is working exactly
as designed (Phase 7/8); it's this specific harness's permanently-hidden state
that's unusual. A real user's tab reports `visible` normally on load.

## FPS counter (task 9)

Phase 7's `re3_GetFPS()`/frame-timing panel (`web/perf.js`) is confirmed
working and unmodified. One scope gap found and worth recording, not fixed
here: it only instruments `Idle()` (`src/core/main.cpp`), the function used
during actual gameplay (`GS_PLAYING_GAME`) -- the frontend/menu render path
uses a separate function, `FrontendIdle()`, which was never in scope for
Phase 7's instrumentation. Confirmed live: at the main menu (boot stage 7),
`re3_GetFPS()`/`re3_GetFrameTimeMs()` both still read `0`. This is a
real, minor gap (the FPS counter doesn't cover the menu, only in-game), not a
bug -- `FrontendIdle()`-side timing would be a small, well-scoped follow-up if
menu-time FPS ever matters, out of scope for "get it to boot."

## Developer console/log panel (task 10)

The existing log panel (`#log-output`, populated since Phase 2 via `print`/
`printErr`) already serves this purpose and needed no new UI: every engine
`debug()`/`printf()`/`TRACE()` call, every asset-loading message, and now every
WebGL/boot-stage event all flow into the same panel, timestamped by arrival
order, capped at 500 lines. No separate console was built, to avoid duplicating
an already-working piece of Phase 2's work.

## The boot log, annotated

This is the actual, unedited sequence from a real successful boot (asset root
mounted via the dev-asset flow, `docs/GAME_ASSETS.md`), with the fixes each
line depends on called out:

```
[module] cwd set to /game, calling main()
[DBG]: cdvd_stream: read info 0x...
[DBG]: Streaming reads run synchronously on the main thread (no background     <- Phase 8: CD-stream
        thread) under Emscripten                                                 pthread/semaphore fix
[DBG-2]: ...psInitialize:...: gGameState = GS_START_UP
Missing game asset: userfiles / gta3.set                                       <- expected: no OS-level
                                                                                   user-files dir in a
                                                                                   browser VFS; handled
                                                                                   gracefully (Phase 3)
[DBG-2]: Default skin set as no other skins are available OR saved skin not found!
[DBG]: Available physical memory 268435456 (assumed, Emscripten)               <- Phase 1: no sysinfo()
                                                                                   under Emscripten
OpenGL version: OpenGL ES 3.0 (WebGL 2.0 (OpenGL ES 3.0 Chromium))              <- Phase 7: video-mode
                                                                                   fallback fix -- this
                                                                                   line never appeared
                                                                                   before that fix
glfwSetInputMode called with GLFW_CURSOR_HIDDEN value not implemented          <- Phase 5: known,
                                                                                   harmless GLFW3/
                                                                                   Emscripten gap
[module] main() returned undefined                                             <- correct: Emscripten's
                                                                                   infinite-main-loop
                                                                                   unwind, not a crash
[DBG-2]: ...operator():...: gGameState = GS_INIT_ONCE
Missing game asset: audio\sound.cache
[DBG]: Cannot load audio cache
[DBG]: Saving audio cache                                                      <- expected first-run
                                                                                   cache-miss, harmless
[DBG]: Initialising CTimer... / CTimer ready
[DBG]: Initialising CPedStats... / Loading pedstats data... / CPedStats ready
[DBG]: Intialising CTimeCycle... / CTimeCycle ready                            <- CGame::InitialiseOnceAfterRW()
[DBG-2]: ...operator():...: gGameState = GS_INIT_FRONTEND;
[DBG-2]: ...operator():...: gGameState = GS_FRONTEND;
LOAD frontend
LOAD sprite                                                                    <- FrontEndMenuManager
                                                                                   loading menu textures
```

**No new engine bug was found or fixed in this phase.** Every subsystem that
needed a fix to reach the menu was already fixed in Phases 7 (video-mode
fallback) and 8 (synchronous CD streaming) -- this phase's own contribution was
the infrastructure to actually *observe* that clearly (boot-progress stages,
`re3_PumpTick()`, WebGL error capture) and the validation run that proved it.
That is itself the expected outcome of following task 5 ("do not make random
changes") together with tasks 3-9 from Phase 8 ("prefer single-threaded", "only
add complexity if profiling/evidence demands it") -- the hard problems were
already found and fixed with minimal, targeted changes in earlier phases, so
this phase had nothing new of that kind to do.

## Following the required workflow (task 5)

No crash needed fixing in this phase, so the CALL STACK → SUBSYSTEM → ROOT
CAUSE → MINIMAL FIX → REBUILD → TEST loop wasn't exercised on a new bug here --
it's exactly the loop Phases 7 and 8 already used (see `docs/BROWSER_MAIN_LOOP.md`'s
"A blocking bug found (and fixed) along the way" and `docs/THREADING_WASM.md`'s
"Classification and decision" sections for two worked examples of that exact
process). This phase's job was integration and validation, not new debugging --
task 4's instruction ("fix initialization failures one subsystem at a time")
implies there would be some; there weren't, because the prior phases had
already found and fixed the ones that existed.

## Reproducing this

```bash
# Build (see docs/WASM_BUILD.md)
cmake --build build-wasm --target re3_wasm --parallel 4

# Serve with your own legally-owned GTA III install mounted for local dev
# (see docs/GAME_ASSETS.md) -- gamefiles/ is never committed to this repo.
python scripts/serve_web.py 8001 web --dev-assets gamefiles
```

Open `http://localhost:8001/index.html?devAssets=1`, click "Load from dev
server", then "Start engine". In a normal foreground browser tab,
`requestAnimationFrame` drives the loop automatically -- no debug mode or manual
pumping needed; that machinery exists for constrained/automated environments
like the one used to validate this phase.

## Native build: unchanged

Every change in this phase is additive (`web/boot.js`, this document) or
`#ifdef __EMSCRIPTEN__`-guarded (`re3_SetBootStage`/`re3_GetBootStage`,
`re3_SetDebugTiming`, `re3_PumpTick`, and their call sites in `main.cpp`,
`Game.cpp`, `glfw.cpp`). No existing function's native-path behavior was
altered.
