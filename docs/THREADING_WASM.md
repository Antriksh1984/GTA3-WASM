# Threading and concurrency (Phase 8)

## Audit: every thread in re3 and its dependencies

A full source audit (`src/`, `vendor/librw/`) for thread creation
(`pthread_create`/`CreateThread`/`std::thread`), synchronization primitives
(mutexes, condition variables, semaphores), and atomics found **exactly one**
piece of real background-threading code in the entire engine, for the platform
this port targets:

| Thread | Location | Classification |
|---|---|---|
| CD-streaming worker | `src/core/CdStreamPosix.cpp` | Was: requires pthreads. Now: removable for WASM (converted, see below) |

Everything else that looked thread-related on a first grep was a false
positive or explicitly platform-excluded:

- **`std::atomic`/mutex/condition-variable false positives**: a repo-wide
  case-insensitive search for "atomic"/"mutex" turns up ~50 files, but every
  one is RenderWare's `RpAtomic` class (a mesh/geometry instance in the scene
  graph -- nothing to do with `std::atomic`) or similarly named,
  non-concurrency code. There is no `std::mutex`, `std::atomic`,
  `std::condition_variable`, `std::thread`, `CRITICAL_SECTION`, or
  `InterlockedIncrement`/`__sync_*`/`__atomic_*` anywhere in `src/`.
- **`vendor/librw` (rendering)**: no thread creation anywhere. Rendering is
  fully synchronous, driven entirely from the main loop's `Idle()` (see
  `docs/BROWSER_MAIN_LOOP.md`) -- confirmed already in Phase 4/7's work.
- **`src/audio/` (OpenAL backend, streaming music/radio)**: no thread creation
  anywhere. `CStream`'s queued-buffer refilling (`src/audio/oal/stream.cpp`) is
  driven synchronously from `DMAudio.Service()`, called once per frame from
  `Idle()` -- confirmed in Phase 6's audio work. Emscripten's own OpenAL
  implementation (`libopenal.js`) schedules buffer queuing via `setInterval`,
  not a worker thread, and needs no `SharedArrayBuffer` support of its own.
- **`CreateThread` in `src/core/main.cpp`**: exists, but entirely inside
  `#ifdef GTA_PS2` (a PS2-specific idle thread for that platform's kernel) --
  `GTA_PS2` is never defined for this PC/Emscripten build, so this code isn't
  even compiled.
- **`src/core/CdStream.cpp`** (the Win32 `CreateThread`/`HANDLE`-based CD
  streaming implementation): entirely `#ifdef _WIN32`, not compiled for this
  (POSIX/Emscripten) build at all. `src/core/CdStreamPosix.cpp` is the only CD
  streaming implementation that applies here.

## The one real thread: CD streaming

`CdStreamPosix.cpp` implements a classic producer/consumer background reader:
`CdStreamRead()` (called from `CStreaming`, `src/core/Streaming.cpp`) queues a
read request and returns immediately; a dedicated worker thread
(`CdStreamThread()`) picks it up, does a blocking `lseek`+`read`, and posts a
semaphore; `CdStreamSync()` blocks the caller on that semaphore until the read
completes. This is the PS2-era design for hiding real CD-ROM/HDD seek latency
behind the game loop -- and it's genuinely necessary on a platform where a
`read()` call can block for tens of milliseconds waiting on physical storage.

**This is where task 3's warning ("do not enable Emscripten pthreads blindly")
mattered.** The existing WASM build (from an earlier phase) had already turned
on `-sUSE_PTHREADS=1 -sPTHREAD_POOL_SIZE=4` specifically so this thread would
compile and run -- but it didn't actually work: `CdStreamInitThread()` calls
`sem_open()` (a *named* POSIX semaphore) to create its synchronization
primitives, and Emscripten's pthread support does not implement named
semaphores. `sem_open()` returns `SEM_FAILED` on every startup, which hit an
`ASSERT(0)` and aborted CD-stream initialization before the game could ever
load `models/gta3.img` -- discovered in Phase 7's validation as a `RE3 ASSERT
FAILED` at `CdStreamPosix.cpp:139`.

### Classification and decision

| Question | Answer |
|---|---|
| Required on the original platform? | Yes -- real disk latency must not block the game loop. |
| Required in a browser? | **No.** re3's "disk" in this build is a virtual filesystem (MEMFS/IDBFS, see `docs/GAME_ASSETS.md`) that is already resident in the WASM heap or IndexedDB-backed cache by the time the game reads from it -- there is no real seek latency to hide behind a worker thread. |
| Browser-compatible as written? | No -- named semaphores aren't supported; would need `USE_UNNAMED_SEM` + real pthreads to even compile-and-run correctly. |
| Worth fixing to use pthreads instead? | No -- see tasks 9/10 below. There's nothing left in the engine that would benefit from a second thread once this one is gone, so keeping the pthreads runtime (and its hosting requirements) around for a single worker that has no real latency to hide is a cost with no corresponding benefit. |

**Decision: convert to a synchronous, single-threaded read under
`__EMSCRIPTEN__`, and remove `-sUSE_PTHREADS`/`-sPTHREAD_POOL_SIZE` from the
build entirely.** This satisfies task 9 ("prefer a stable single-threaded
browser build first") and task 4 ("determine whether the game can run
single-threaded initially") concretely, not just as a stated preference --
every subsystem in the engine has now been confirmed to work without threads.

### Implementation

`src/core/CdStreamPosix.cpp`, all `#ifdef __EMSCRIPTEN__`-guarded, native code
byte-for-byte unchanged:

- A new `CdStreamPerformReadSync()` does the same `lseek`+`read`+status-update
  work `CdStreamThread()`'s worker loop does, but runs it inline.
- `CdStreamRead()` calls it directly instead of enqueueing the request and
  posting a semaphore for a worker to pick up later.
- `CdStreamInitThread()` skips semaphore/thread creation entirely (just sets
  the "not shut down" status flag `CdStreamGetStatus()` checks).
- `CdStreamShutdown()` skips `pthread_join`/semaphore teardown, doing only the
  `free(gpReadInfo)` cleanup the worker thread's exit path would otherwise do.

Every other function (`CdStreamSync()`, `CdStreamGetStatus()`, the
`FLUSHABLE_STREAMING` interrupt-and-flush path) needed **no changes at all**:
since the read now always completes before `CdStreamRead()` returns,
`nSectorsToRead` is already `0` and `bReading` is already `false` by the time
any of that code runs, so their existing "is there anything to wait for?"
checks naturally take the already-done path without ever touching a semaphore.
`CdStreamThread()` itself (the pthread worker function) is left completely
unmodified for native builds; it simply becomes unreferenced, unreachable dead
code in an Emscripten build (never called, since the only call sites are inside
now-preprocessed-out `#else` branches) -- Emscripten's libc still provides the
`sem_wait`/`pthread_exit`/etc. symbols it references even without
`-sUSE_PTHREADS`, as inert single-threaded stubs, so it still links.

## SharedArrayBuffer / cross-origin isolation / hosting (no longer required)

Since `-sUSE_PTHREADS` is off, `re3_wasm` no longer needs `SharedArrayBuffer`
or a cross-origin-isolated page at all. Concretely, that means:

- **No COOP/COEP headers required** for hosting `re3_wasm.js`/`.wasm`. Any
  plain static file server works. `scripts/serve_web.py` still sends
  `Cross-Origin-Opener-Policy`/`Cross-Origin-Embedder-Policy` (harmless to
  leave on, and this repo's other diagnostic tools don't depend on their
  absence either), but a deployment is no longer *required* to replicate that.
- **No `Worker` pool spin-up cost** at module load (`-sPTHREAD_POOL_SIZE=4`
  previously spun up 4 Web Workers up front) -- faster, simpler startup.
- `web/launcher.js`'s diagnostics panel no longer labels
  `SharedArrayBuffer`/`crossOriginIsolated` as something re3 needs (previously
  shown as a pass/fail "pthreads viable" gate) -- they're now purely
  informational browser-capability probes.

If a future phase found a *profiled, measured* reason to bring pthreads back
(see task 10 below) -- for example, parallelizing texture decompression or
physics across Web Workers -- re-enabling `-sUSE_PTHREADS=1` would reintroduce
this requirement, and `scripts/serve_web.py`'s existing COOP/COEP support
(already built and tested, see `docs/BROWSER_RUNTIME.md`) would be the hosting
mechanism to lean on again.

## Avoiding main-thread blocking (task 6)

With CD streaming synchronous, the natural question is whether that
reintroduces main-thread blocking task 6 warns against. It does not, for the
same reason task 9's "prefer single-threaded first" holds: the read is against
an in-memory-backed virtual filesystem, not a real device.

- **MEMFS** (Emscripten's default in-memory filesystem) and **IDBFS**'s
  in-memory working copy (see `docs/GAME_ASSETS.md` -- IDBFS syncs to
  IndexedDB asynchronously in the background, but reads/writes against the
  *mounted* tree are synchronous, in-memory operations, not a live IndexedDB
  round-trip per `read()` call) both serve `read()`/`lseek()` out of memory
  already resident in the WASM heap.
- A synchronous `read()` against memory is a `memcpy`-class operation --
  microseconds, not the tens-of-milliseconds a real spinning disk or even an
  SSD seek could cost, which is what the original background thread existed to
  hide.
- The largest single streaming read re3 issues is still bounded by
  `CDSTREAM_SECTOR_SIZE`-multiples requested by `CStreaming` (`src/core/
  Streaming.cpp`), the same request sizes native re3 already uses -- nothing
  about moving this onto the main thread changes *how much* data moves per
  call, only that it no longer round-trips through a worker thread and two
  semaphores to do it.

This is a reasonable, low-risk trade -- but it is a real one: if a future asset
pipeline made individual streamed reads much larger (loading many megabytes in
one `CdStreamRead()` call), that assumption should be re-checked with
`Idle()`'s own frame-timing diagnostics (`docs/BROWSER_MAIN_LOOP.md`, Phase 7)
before assuming it's still negligible.

## Worker threads / background streaming inventory (task 7)

| Primitive | Where | Status after Phase 8 |
|---|---|---|
| Mutexes | none found | n/a |
| Condition variables | none found | n/a |
| Atomics (real, not `RpAtomic`) | none found | n/a |
| Worker threads (`pthread_create`) | `CdStreamPosix.cpp`, native-only path | Not created under Emscripten (task 4/9) |
| Semaphores | `CdStreamPosix.cpp`, native-only path | Not created under Emscripten |
| Background streaming | CD-stream worker | Runs synchronously inline under Emscripten (task 6) |
| Browser `Worker`s Emscripten itself may use | GLFW/WebGL/OpenAL Emscripten ports | None of the ports this build uses (`USE_GLFW=3`, `USE_WEBGL2=1`, built-in OpenAL) spin up their own Workers independent of `-sUSE_PTHREADS` |

## Validation

- `cmake --build build-wasm --target re3_wasm` compiles and links cleanly
  without `-sUSE_PTHREADS`/`-sPTHREAD_POOL_SIZE`.
- With a real GTA III install mounted (same dev-asset flow as Phase 7's
  validation), the `RE3 ASSERT FAILED ... CdStreamInitThread ... failed to
  create stream semaphore` failure from Phase 7 no longer occurs -- CD-stream
  initialization now logs `Streaming reads run synchronously on the main
  thread (no background thread) under Emscripten` instead, and proceeds to
  actually read `models/gta3.img`.
- No deadlock is possible in the browser build by construction: there is no
  cross-thread synchronization primitive anywhere in the Emscripten build
  configuration to deadlock on (no semaphore, no mutex, no thread waiting on
  another thread) -- the strongest form of task validation ("browser build
  should not deadlock") available.

## Native build: unchanged

Every change in this phase is `#ifdef __EMSCRIPTEN__`-guarded
(`CdStreamPosix.cpp`) or scoped inside `if(EMSCRIPTEN)` in `src/CMakeLists.txt`
(the flag removal). `src/core/CdStream.cpp` (the Win32 threaded
implementation) was not touched. Native Linux/Mac builds still use the
original pthread/named-semaphore worker-thread path in `CdStreamPosix.cpp`
exactly as before -- named semaphores work correctly on real POSIX systems;
the failure documented above is specific to Emscripten's pthread support.
