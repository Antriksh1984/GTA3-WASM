# Debugging pass: menu → gameplay, audio, DevTools dependency

This documents a targeted debugging session run after the engine first reached
the GTA III main menu (see `docs/FIRST_BOOT.md`). Three symptoms were reported:
no audio, "Start New Game" not visibly entering gameplay, and the runtime
behaving differently depending on whether browser DevTools was open. Each was
traced to a real, specific line of code before anything was changed.

> **Note on this file's history**: this document was accidentally overwritten
> (a `Write` call replaced its contents with placeholder text) partway through
> the Round 3 session below. The Round 1/2 sections that follow are
> reconstructed from the debugging session's own notes/context rather than
> restored verbatim -- the substance and root causes described are accurate,
> but exact original wording may differ from what was first written. Round 3's
> section was written live during that session and is not reconstructed.

## A. Exact last confirmed execution point after "Start New Game" — and the fix

**Root cause, fully traced and proven**: `CMenuManager::DoSettingsBeforeStartingAGame()`
(`src/core/Frontend.cpp`, called when `MENUACTION_NEWGAME` is dispatched) sets
`FrontEndMenuManager.m_bWantToRestart = true`. On native platforms, this flag
is how the menu tells the *platform* main-loop wrapper "break out and
re-initialise" — the outer `while(TRUE)` loop in `src/skel/glfw/glfw.cpp` (and
`src/skel/win/win.cpp`'s equivalent) receives it, falls out of the inner tick
loop, runs the restart block, resets the flag, and loops back in.

Under Emscripten there is no such outer loop: `emscripten_set_main_loop(tick, 0, 1)`
registers `tick` exactly once. The original port treated `m_bWantToRestart`
the same as a real quit signal and called `emscripten_cancel_main_loop()`,
which never comes back — the entire engine stopped ticking the instant "Start
New Game" was confirmed. `gGameState` could reach `GS_INIT_PLAYING_GAME`
(set earlier in that same tick) but the loop never ran again to process it.

**Fix**: run the native restart block inline inside `tick()` when
`m_bWantToRestart` is seen, then continue the main loop instead of cancelling
it. `#ifdef __EMSCRIPTEN__`-guarded in `src/skel/glfw/glfw.cpp`.

## B. Canvas/viewport sizing

**Root cause**: `RsGlobal.maximumWidth`/`maximumHeight` were still at their
`RsInitialize()` skeleton default (640×480) when the GLFW window/canvas was
first created — the *real* desired resolution isn't computed until
`psSelectDevice()` runs, which happens *after* `RwEngineOpen()` has already
created the window from the default values. Nothing in the normal boot path
ever resized that already-created window to match. Native/desktop windowing
apparently masks the same gap; in a browser it meant the canvas was
permanently created at 640×480 regardless of its actual on-page size.

**Fix**: query the canvas's actual current backing-store size via
`emscripten_get_canvas_element_size("#canvas", ...)` before computing
`openParams.width/height`, and pre-seed `FrontEndMenuManager.m_nPrefsWidth/Height`
with it so `psSelectDevice()`'s own default-resolution computation is skipped
rather than overwriting it with a mismatched value. `#ifdef __EMSCRIPTEN__`-guarded
in `src/skel/glfw/glfw.cpp`'s `main()`.

(Round 3 below found and fixed a *second*, later overwrite of this same value
that the above fix didn't fully prevent — see "Bug 2".)

## C. Audio initialization

**Root cause**: `ALDeviceList` (`src/audio/oal/aldlist.cpp`, a vendored
Creative Labs OpenAL utility) gated device enumeration on the
`ALC_ENUMERATION_EXT` extension being advertised via
`alcIsExtensionPresent(NULL, "ALC_ENUMERATION_EXT")`. Emscripten's `libopenal.js`
does not advertise this extension in its `ALC_EXTENSIONS` string, even though
the underlying device query works fine — so `ALDeviceList` always enumerated
zero devices and audio init failed silently.

**Fix**: under `__EMSCRIPTEN__`, treat `ALC_ENUMERATION_EXT` as always present
(`haveEnumerationExt = true`) rather than querying for it, in
`src/audio/oal/aldlist.cpp`. Confirmed working via `AudioContext` reaching
`"running"` state and OpenAL sources being created matching the expected
channel count.

## D. DevTools-open-changes-behavior investigation

Investigated whether having DevTools open changed runtime behavior. This
traced back to the same rAF/timer-throttling difference between a
backgrounded/non-composited tab and a foregrounded one that this whole
debugging effort's test harness is independently affected by (see "Known test
harness limitations" in the project's session notes) — not a distinct bug in
the engine itself. `re3_PumpTick()` (a debug export that synchronously invokes
the registered `tick()` callback on demand) was added specifically to get
reliable, throttling-independent test execution regardless of this.

## Round 2: canvas rendering into a tiny region, streaming re-audit

A follow-up report described the canvas rendering black or only into a tiny
bottom-left region, with opening DevTools making it visible-but-still-tiny.

**Full root cause chain**: `RsGlobal.maximumWidth/Height` being wrong at
window-creation time (Round 1, section B above) meant the GLFW/Emscripten
canvas backing store was created at the wrong size. Emscripten's GLFW port
(`libglfw.js`) only recomputes `canvas.width/height` from `GLFW.active.width/height`
when `glfwCreateWindow`/`glfwSetWindowSize` runs — nothing in the normal boot
path called either again with the *correct* size, so the mismatch between the
CSS-displayed size and the actual WebGL backing-store/framebuffer size
persisted for the rest of the session. Opening DevTools resizes the viewport,
which happens to trigger a browser-side reflow/resize path that partially
(but not fully) corrects the backing store — hence "visible but still tiny."

**Fix**: same as Round 1 section B (`emscripten_get_canvas_element_size()`-based
pre-seeding). Verified via direct `canvas.width`/`canvas.height` measurement:
640×480 before the fix, correctly dpr-scaled to the real on-page size after.

**Streaming re-audit (Phase 8 synchronous CD-streaming)**: re-checked
`CdStreamPerformReadSync()` (`src/core/CdStreamPosix.cpp`) specifically for
subtle bugs in the Emscripten synchronous-read port (early completion, stuck
status, buffer lifetime, wrong offsets, partial reads silently treated as
success). Found one real gap: a short read (`read()` returning fewer bytes
than requested, without returning -1) was silently treated as `STREAM_NONE`
(success) rather than `STREAM_ERROR`, unlike a full read failure. Fixed by
explicitly checking `got != requested` and failing with `STREAM_ERROR` in that
case, with a `CDTRACE`-logged diagnostic. No behavior change for the (already
correct) full-success and hard-failure paths.

**"Crash before intro cutscene" report**: pushed 120+ ticks past
`GS_PLAYING_GAME` with zero crashes, confirmed real rendering (FPS≈190) and a
stable state. Concluded this was very likely the visual manifestation of the
canvas bug above making a correctly-running game look broken, rather than a
distinct crash — though full confirmation of a clean intro/cutscene transition
did not happen until Round 3 (below), once the game actually started rendering
the visible world.

## Round 3: gameplay never visibly rendered after the loading screen

**Reported symptom**: WASM loads, assets mount, menu renders correctly, canvas
sizing is correct, input works, menu audio works, "Start New Game" works, the
loading screen appears — but after loading, the game does not visibly enter
the intro/gameplay sequence, and there is no subsequent game/cutscene audio.

### Investigation

New instrumentation was added first, per an explicit requirement to trace real
state transitions rather than guess: a `gGameState` transition tracer (logs
actual transitions with timestamps, not periodic polling) and `[BOOT]
ENTER`/`EXIT` markers at the real Start-New-Game→gameplay call chain
boundaries (`CGame::InitialiseOnceBeforeRW`, `InitialiseOnceAfterRW`,
`TheCamera.Init`, `CWorld::Initialise`, the first script tick), plus read-only
diagnostic exports: `re3_GetIdleCallCount`/`GetIdleLastExit`,
`re3_GetTimerPaused`/`GetTimerCodePaused`, `re3_GetCameraFadeStatus`,
`re3_GetPlayerExists`, `re3_GetStreamingChannelState`/`GetStreamingChannelError`/
`GetNumModelsRequested`, and CD-stream failure/short-read-only logging in
`CdStreamPerformReadSync()`.

Live tracing showed the engine reaching `GS_PLAYING_GAME` cleanly, with
`CGame::Initialise()` completing, the player ped being created, and no
streaming or state-machine errors — `ForegroundApp`, `RwInitialised`, and
`m_bWantToRestart` were all in their expected, healthy states. Yet
`re3_GetCameraFadeStatus()` reported `FADE_2` (fully black) indefinitely, and
the frame-timing globals (`re3_GetFPS()` etc.) never changed value across
hundreds of ticks — meaning `main.cpp`'s `Idle()` was reaching an early-return
almost every time it ran, before its render block.

Two distinct, unrelated bugs were found and fixed:

#### Bug 1 — WASM function-signature mismatch trapping `RpAtomicRender`

Once real-time-spaced ticks (rather than tight synchronous batches — see
"methodology note" below) let `Idle()` actually run repeatedly, a
`RuntimeError: function signature mismatch` reproducibly crashed inside
`RpAtomicRender(rw::Atomic*)`, called from `CEntity::Render()` →
`CRenderer::RenderOneNonRoad()` → `CRenderer::RenderEverythingBarRoads()` —
i.e. the very first time the engine actually rendered a world entity
(ped/vehicle/building atomic), which had never successfully happened before
under Emscripten because the camera was permanently stuck fully faded to
black (see Bug 2).

**Root cause**: `src/fakerw/fake.cpp` (a compatibility shim mapping re3's
original-RenderWare-style API onto librw's reimplementation) defined
`RpAtomicSetRenderCallBack`/`RpAtomicGetRenderCallBack` by reinterpret-casting
between two function-pointer types with *different return types*: re3's
`RpAtomicCallBackRender` (`RpAtomic*(*)(RpAtomic*)`, from
`src/fakerw/rpworld.h`) and librw's actual `Atomic::RenderCB`
(`void(*)(Atomic*)`, from `vendor/librw/src/rwobjects.h`). This "works" on
native x86/x64 because callers simply don't read an unused return register,
but WebAssembly's indirect-call tables strictly validate the full function
signature and trap on the mismatch. The file's own auto-generated comment
already flagged this exact line as `WARNING: illegal cast`.

**Fix**: replaced the illegal cast with a correctly-typed trampoline
(`re3AtomicRenderCBTrampoline`, matching `Atomic::RenderCB`'s real signature)
that stores the real re3-side callback in a module-level variable and calls
through it, discarding the return value. Every call site in re3
(`CVisibilityPlugins::SetAtomicRenderCallback`, `VehicleModelInfo.cpp`) only
ever sets or copies a single callback at a time, so a one-slot trampoline
correctly adapts every real usage. `src/fakerw/fake.cpp` is a small,
statically-generated/checked-in shim (not regenerated by the build), so this
is a direct, permanent source fix — not Emscripten-guarded, since it's a
genuine type-correctness fix rather than a platform-specific workaround
(native was already technically wrong here, just not caught by its ABI).

#### Bug 2 — camera raster permanently 0×0, freezing the render path forever

**Root cause**: `psSelectDevice()` (`src/skel/glfw/glfw.cpp`) enumerates video
modes and, under the `IMPROVED_VIDEOMODE` build config, unconditionally
overwrites `FrontEndMenuManager.m_nPrefsWidth/Height` with `vm.width/height`
from whichever mode it selects as "the windowed mode" — even though Round 1/2's
fix had already correctly pre-seeded those fields from the real canvas size.
Emscripten's GLFW port has no real concept of multiple native video modes (a
browser `<canvas>` isn't a discrete OS video mode), and its one synthetic
"windowed" entry reports `0×0`. That `0×0` then propagates into
`RsGlobal.maximumWidth/Height`, and from there — via `CameraSize(camera, nil, ...)`'s
"rect not specified, reuse current raster size" fallback — the camera's raster
gets created and then stays at `0×0` for the rest of the session, since
`CameraCreate()` itself always creates a placeholder `0×0` raster and expects a
later real `CameraSize()` call to establish the true size.

Traced and proven directly (not inferred) via: (a) a call-by-call log added to
`CameraSize()` showing every call's `rect`/`origSize` values across boot —
confirming the raster was `(0,0)` from the very first call onward; (b) new
`re3_GetRsGlobalWidth/Height()` and `re3_GetCameraRasterWidth/Height()`
exports confirming both were `0` even at the plain frontend menu, before
"Start New Game"; (c) a targeted trace at `psSelectDevice()`'s
`FrontEndMenuManager.m_nPrefsWidth/Height = vm.width/height` assignment,
confirming `vm.width/height` were themselves `(0,0)` at that point.

This `0×0` camera raster is *also* why the screen stayed black even once Bug 1
was fixed: `SCREEN_WIDTH`/`SCREEN_HEIGHT` (`src/core/common.h`) resolve
through `RsGlobal.width/height`, and `ScreenDroplets::FillScreenMoving()`
(`src/extras/screendroplets.cpp`) does `CGeneral::GetRandomNumber() % (int)SCREEN_WIDTH`
— a `0` modulus traps as `RuntimeError: remainder by zero`, reproducing the
first time that effect runs post-load (rain/weather screen droplets), which
is exactly when the camera would otherwise have started fading back in.

**Fix**: `#ifdef __EMSCRIPTEN__`-guarded in `psSelectDevice()`: only apply
`vm.width/height` to `FrontEndMenuManager.m_nPrefsWidth/Height` if they're
actually valid (`> 0`); otherwise keep whatever was already there (the
canvas-derived value from Round 1/2's fix). Native behavior is unchanged.

### Methodology note: the frame-limiter/real-time-testing interaction

While diagnosing the above, batches of hundreds of `re3_PumpTick()` calls
executed in a tight synchronous JS loop were initially mistaken for evidence
of an engine stall: `Idle()`'s call count barely moved across such a batch.
This turned out to be *correct* frame-limiter behavior, not a bug —
`main.cpp`'s `Idle()` is only invoked from `case GS_PLAYING_GAME` in
`glfw.cpp`'s `tick()` when real elapsed wall-clock time (via
`CTimer::GetCurrentTimeInCycles()`, ultimately `psTimer()`) exceeds
`1000/RsGlobal.maxFPS`. A tight synchronous loop completes in microseconds of
real time, so nearly every call in such a batch is correctly throttled.
Spacing individual `re3_PumpTick()` calls with real waits (or separate tool
round-trips, which themselves take real time) is the correct way to drive
multiple real frames in this test harness.

A related, separate, real bug was found and fixed during this investigation:
`psTimer()`'s non-Windows implementation used
`clock_gettime(CLOCK_MONOTONIC_RAW, &start)`, which does not track real
elapsed time at all under this Emscripten build — two readings taken ~32 real
seconds apart (per JS `performance.now()`) returned the bit-identical value.
Since `CTimer`'s entire delta-time system is built on `psTimer()`, this alone
would have made the `GS_PLAYING_GAME` frame-limiter gate almost always false
and starved `Idle()` regardless of the two bugs above.

**Fix**: `#ifdef __EMSCRIPTEN__`-guarded in `psTimer()` (`src/skel/glfw/glfw.cpp`):
use `emscripten_get_now()` (a direct wrapper around `performance.now()`,
already proven correct elsewhere in this file) instead of `clock_gettime()`.
Same "milliseconds as a double" return contract, direct drop-in replacement.
Native (`clock_gettime`) path unchanged.

### Validation

With all three fixes in place, confirmed via direct measurement across
multiple full fresh-tab test runs (menu → "Start New Game" → loading screen →
`GS_PLAYING_GAME` → real-time-spaced ticks sustained well past 30 real
seconds, several thousand `re3_PumpTick()` calls total):

- Camera raster and `RsGlobal.width/height` correctly reflect the real canvas
  size (e.g. 1250×1217) and stay stable, never regressing to 0.
- `re3_GetCameraFadeStatus()` progresses `FADE_2` → `FADE_1` → `FADE_0`
  (fully faded in) within a few real seconds of entering `GS_PLAYING_GAME`,
  as the intro script's own fade-in opcode executes.
- No `signature mismatch` or `remainder by zero` traps reproduce, in any
  amount of sustained real-time or synchronous-batch testing.
- `re3_GetPlayerExists()` stays `1`, `re3_GetGameState()` stays `9`
  (`GS_PLAYING_GAME`), `re3_GetTimerPaused()` stays `0`, no streaming channel
  errors, gameplay audio sources (`re3_GetPlayingAudioSourceCount()`) actively
  playing.
- A screenshot taken mid-test shows a fully rendered 3D world scene (the
  player's starting car with its doors open, city buildings, streetlights, a
  bridge, correct dusk lighting/fog) at the correct canvas size — the game is
  genuinely visible and running, not black or tiny.

### A separate, non-blocking, unresolved observation

An intermittent `RuntimeError: memory access out of bounds` (inside the
`tick()` lambda itself, per a symbolicated stack trace) was observed in the
console during some — not all — extended test runs. It was confirmed to: not
reproduce on a fresh page load with zero interaction; not reproduce from a
single `re3_PumpTick()` call; never correspond to any drop in engine health
(`gGameState`, camera fade, player existence, FPS, and audio all stayed
correct across every run where it appeared) or any exception caught by a
`try/catch` wrapped around the exact `re3_PumpTick()` calls being made at the
time. This means it is not coming from the code path under direct test
control here — a plausible candidate (not yet confirmed) is `web/launcher.js`'s
own independent diagnostics-polling loop interacting with a `-sALLOW_MEMORY_GROWTH=1`
WASM heap-growth event (asset loading during "Start New Game" is exactly the
kind of allocation spike that triggers heap growth) via a stale cached buffer
view, but this was explicitly not chased further: it does not block or affect
the reported symptom (which is fixed and proven above), and root-causing it
would require exactly the kind of standalone diagnostic tooling this
debugging pass was scoped to avoid unless the reproduced failure required it.
Worth a dedicated, narrowly-scoped follow-up if it turns out to matter.

### Files changed (Round 3)

- `src/fakerw/fake.cpp` — `RpAtomicSetRenderCallBack`/`GetRenderCallBack`
  signature-correct trampoline (Bug 1). Not Emscripten-guarded (genuine
  type-correctness fix).
- `src/skel/glfw/glfw.cpp` — `psSelectDevice()`'s video-mode-derived
  `m_nPrefsWidth/Height` overwrite guarded against invalid `(0,0)` values
  (Bug 2); `psTimer()` uses `emscripten_get_now()` instead of
  `clock_gettime(CLOCK_MONOTONIC_RAW, ...)`; new read-only diagnostic exports
  (`re3_GetIdleCallCount`, `re3_GetIdleLastExit`, `re3_GetFrameLimiterMs/MaxFPS/Enabled/WillCall`,
  `re3_GetRawPsTimer`, `re3_GetRsGlobalWidth/Height`,
  `re3_GetCameraRasterWidth/Height`); `Idle()` exit-point tracking globals.
- `src/core/main.cpp` — `Idle()` call-count and last-exit-point tracking
  (diagnostic only, retained as a cheap read-only aid).
- `src/core/Game.cpp` — `[BOOT] ENTER`/`EXIT` markers at the real
  Start-New-Game→gameplay call chain boundaries.
- `src/core/CdStreamPosix.cpp` — short-read detection in
  `CdStreamPerformReadSync()` (kept from the Round 2 re-audit).
- `docs/MENU_TO_GAMEPLAY_DEBUGGING.md` — this document.

All Round 3 changes to `glfw.cpp`/`main.cpp`/`Game.cpp` are
`#ifdef __EMSCRIPTEN__`-guarded; the `fake.cpp` fix is a plain source
correction with no ifdef (see Bug 1 above for why). No native-path behavior
was altered.

## Native build: unchanged (all rounds)

Every change across all three debugging passes is `#ifdef __EMSCRIPTEN__`-guarded
(`src/skel/glfw/glfw.cpp`'s restart-handling rewrite, canvas-size fix,
`psSelectDevice()`/`psTimer()` fixes, and all debug/diagnostic exports;
`src/core/Game.cpp`'s and `src/core/Frontend.cpp`'s trace lines;
`src/audio/oal/aldlist.cpp`'s enumeration bypass) or is a genuine
platform-independent correctness fix with no native-behavior change
(`src/fakerw/fake.cpp`'s render-callback trampoline — see Bug 1). No existing
function's native-path behavior was altered; `src/skel/win/win.cpp`'s
equivalent restart logic and the native `ALC_ENUMERATION_EXT`/window-sizing/
video-mode paths were not touched.
