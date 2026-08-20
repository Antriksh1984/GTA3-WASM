# Audio in the browser (Phase 6)

## Audio trace: game → cDMAudio → cSampleManager → OpenAL → Web Audio

Exactly like rendering (see `docs/WEBGL_COMPATIBILITY.md`), re3 never talks to a
browser API directly. All audio goes through re3's own two-layer abstraction,
unchanged since the original codebase:

```
Game logic (src/audio/AudioManager.cpp, MusicManager.cpp, PoliceRadio.cpp, ...)
    |  entities, one-shots, radio station selection, mission/cutscene audio
    v
cDMAudio (src/audio/DMAudio.h/.cpp)
    |  the top-level API: Initialise/Terminate/Service, PlayOneShot,
    |  SetEffectsMasterVolume/SetMusicMasterVolume, PlayFrontEndTrack,
    |  PlayRadioAnnouncement, PreloadMissionAudio, GetRadioInCar/SetRadioChannel
    v
cSampleManager (src/audio/sampman.h, backend-specific .cpp)
    |  the actual backend abstraction -- InitialiseChannel, SetChannel3DPosition,
    |  SetChannelVolume, StartChannel/StopChannel, PreloadStreamedFile,
    |  PauseStream/StartStreamedFile, Service()
    |  three implementations exist: sampman_oal.cpp (AUDIO_OAL, OpenAL --
    |  the only one available on non-Windows, see root CMakeLists.txt),
    |  sampman_miles.cpp (AUDIO_MSS, Windows-only Miles Sound System),
    |  sampman_null.cpp (silent stub)
    v
OpenAL 1.1 calls (src/audio/oal/channel.cpp, stream.cpp)
    |  alSourcePlay/Pause/Stop, alSource3f(AL_POSITION,...), alSourcef(AL_GAIN,...),
    |  alBufferData, queued streaming buffers for music/radio/mission audio
    v
Emscripten's built-in OpenAL implementation (emsdk/.../src/lib/libopenal.js)
    |  translates every AL/ALC call to the Web Audio API -- AudioContext,
    |  AudioBufferSourceNode, GainNode, PannerNode
    v
Browser Web Audio API (AudioContext, backed by the OS audio device)
```

**Answering tasks 1-4 up front:** the existing backend is OpenAL
(`RE3_AUDIO=OAL`, the only choice on non-Windows -- Miles Sound System is
Windows-only and proprietary, structurally excluded by
`set(${PROJECT}_AUDIOS "OAL")` for `NOT WIN32` in the root `CMakeLists.txt`).
Emscripten ships its own OpenAL 1.1 implementation as a JS library
(`libopenal.js`), auto-linked whenever AL/ALC symbols are used -- the same way
its GLFW/WebGL2 ports are auto-linked for rendering. **No custom backend was
needed and none was written** (task 4/5): `src/audio/sampman_oal.cpp`, the
existing `cSampleManager` implementation, already targets standard OpenAL 1.1
and required zero source changes to compile and run under Emscripten. This has
been true since Phase 1 -- `src/CMakeLists.txt` skips `find_package(OpenAL)`
`if(NOT EMSCRIPTEN)` (line ~65-69) and simply lets Emscripten provide its own
`AL/al.h`/`alc.h` headers and runtime, exactly analogous to how GL headers are
provided for WebGL2.

## What's already covered by unmodified code (task 2 checklist)

Every item below is standard OpenAL usage in code that predates this port and
needed no changes:

| Feature | Where | OpenAL mechanism |
|---|---|---|
| SFX (one-shot, positional) | `src/audio/oal/channel.cpp` | `alSourcePlay`, `alSource3f(AL_POSITION,...)` |
| Music / streaming / radio | `src/audio/oal/stream.cpp` (`CStream`) | queued buffers (`NUM_STREAMBUFFERS=8`), `alSourceQueueBuffers` |
| Positional (3D) audio | `channel.cpp:232-241` | `AL_POSITION`, `AL_MAX_DISTANCE`, `AL_REFERENCE_DISTANCE`, `AL_ROLLOFF_FACTOR` |
| Per-channel volume | `channel.cpp:156`, `sampman_oal.cpp` `SetEffectsMasterVolume`/`SetMusicMasterVolume` | `alSourcef(AL_GAIN,...)` (listener gain stays fixed at 1.0 -- master volume is applied per-source) |
| Pause/resume | `stream.cpp` `CStream::SetPause`, `channel.cpp` | `alSourcePause` / `alSourcePlay` (resumes from paused offset per the OpenAL 1.1 spec) |
| Radio station selection | `src/audio/PoliceRadio.cpp`, `MusicManager.cpp` | selects which streamed file `CStream` opens; no separate audio path |

Emscripten's `libopenal.js` implements all of the AL/ALC entry points this code
calls, including `alSourcePause` (confirmed present, satisfying pause/resume) and
a `PannerNode`-backed positional model (`src.panner = src.context.audioCtx.
createPanner()`, `libopenal.js:413`) for `AL_POSITION`. None of this required a
new browser-specific backend -- it's the existing abstraction working as designed.

## Autoplay restrictions (task 7)

Browsers only allow an `AudioContext` to run if it's created or resumed from
inside a real user-gesture call stack (click/keydown/touchstart). Two layers
handle this, one built into Emscripten and one added in this phase for UX
clarity:

1. **Built into Emscripten, verified by reading the source.**
   `alcCreateContext` (`emsdk/upstream/emscripten/src/lib/libopenal.js:2070`)
   calls `autoResumeAudioContext(ac)` (line 2156) immediately after constructing
   the `AudioContext`, *before* the context is even wired into Emscripten's
   internal object model. `autoResumeAudioContext`'s definition
   (`emsdk/.../src/lib/libcore.js:1793-1801`) attaches one-time `keydown`/
   `mousedown`/`touchstart` listeners to both `document` and
   `document.getElementById('canvas')` that call `ctx.resume()` the next time
   any of those fire. **This means `src/audio/sampman_oal.cpp` needed zero
   changes for autoplay compliance** -- every `alcCreateContext()` call it
   already makes (in `set_new_provider()`) gets this safety net automatically,
   for free, from the existing OpenAL backend. This is the "prefer existing
   Emscripten-compatible backend" outcome from task 4 playing out concretely.

2. **Added in this phase, for UX clarity, not correctness.** Relying solely on
   (1) means audio would only start after some *incidental* click/keypress
   elsewhere on the page -- functionally fine, but the user has no way to know
   that's what's needed. Task 7 explicitly asks for an implemented "Click to
   Start" flow, so `web/audiotest.html` gates the *first* OpenAL call
   (`audiotest_init()`, which calls `alcOpenDevice`/`alcCreateContext`) behind an
   explicit "Click to Start Audio Test" button; nothing before that click ever
   calls into OpenAL. The main game runtime (`web/index.html`) already has an
   equivalent gesture-gated entry point from Phase 2/3 -- the "Start engine"
   button, which is what calls `callMain()` and, transitively, everything up to
   `CGame::InitialiseOnceAfterRW()` → `DMAudio.Initialise()` → `cSampleManager::
   Initialise()` → `alcCreateContext()`. Because that whole chain runs
   synchronously inside the button's click handler, the very first
   `AudioContext` re3 creates is itself created within a user gesture -- so in
   practice audio should be immediately unsuspended, with (1) as a fallback if a
   browser is stricter than expected.

Verified live (see "Validation" below): `web/audiotest.html` loads and its
module instantiates without ever touching OpenAL; only after clicking "Click to
Start Audio Test" does `alcOpenDevice`/`alcCreateContext` get called, and every
subsequent control is itself a click, so nothing on the page ever attempts audio
outside a gesture.

## Preserved (task 8): volume, stereo positioning, SFX, music, pause/resume

All confirmed unmodified and working (see Validation) because they're standard
OpenAL calls Emscripten's `libopenal.js` already implements correctly:

- **Volume**: per-source `AL_GAIN`, driven by `cSampleManager::SetEffectsMasterVolume`/`SetMusicMasterVolume` (`sampman_oal.cpp`). The global OpenAL *listener* gain is deliberately left at 1.0 (`alListenerf(AL_GAIN, 1.0f)`, `sampman_oal.cpp` init) -- master volume is a per-channel multiplier, not a listener-level one, in the existing design.
- **Stereo/3D positioning**: `AL_POSITION` + `AL_ROLLOFF_FACTOR`/`AL_REFERENCE_DISTANCE`/`AL_MAX_DISTANCE` (`channel.cpp:232-241`), realized by Emscripten as a Web Audio `PannerNode` per source.
- **SFX**: one-shot buffer playback (`alSourcePlay` on a non-looping source).
- **Music**: looping buffer playback (SFX bank tones) or queued-buffer streaming (`CStream`, for the larger radio/mission/cutscene tracks) -- both are plain OpenAL, no special-casing needed.
- **Pause/resume**: `alSourcePause` / `alSourcePlay` -- OpenAL 1.1 guarantees resuming a paused source continues from its paused offset, not from zero.

## Streamed audio format support under Emscripten (a real, load-bearing limitation)

`src/audio/oal/stream.cpp` supports three streamed-audio decoders, selected at
compile time: libsndfile (WAV, `AUDIO_OAL_USE_SNDFILE`), Opus
(`AUDIO_OAL_USE_OPUS`), and mpg123 (MP3, the default fallback). The root
`CMakeLists.txt` **forces `WITH_OPUS` and `WITH_LIBSNDFILE` off under
Emscripten** (`if(EMSCRIPTEN) ... set(RE3_WITH_OPUS OFF ...) set(RE3_WITH_LIBSNDFILE OFF ...)`),
leaving **MP3 via mpg123 as the only streamed-audio decoder in the WASM build**.
This is provided by Emscripten's own port system (`-sUSE_MPG123=1` in
`src/CMakeLists.txt`), not a system library, so it needs no extra setup -- but
it does mean:

- Streamed tracks (radio stations, mission/cutscene audio, `AUDIO\*.MP3` under
  the retail PC layout) work as long as they're MP3, which is what a standard
  GTA III PC install already ships.
- An asset set that was re-encoded to Opus (`AUDIO\HEAD.OPUS` etc. -- an
  optional re3-community space-saving option referenced in
  `src/audio/sampman.h`'s `StreamedNameTable`) or to WAV via the libsndfile path
  will **not** decode in this build; `AUDIO_WITH_OPUS`/`AUDIO_OAL_USE_SNDFILE`
  would need to be re-enabled and their respective libraries ported to
  Emscripten to support that, which is out of scope for this phase since it
  isn't needed for a standard installation.
- The one-shot/loopable SFX bank (`LoadSampleBank`) is decoded from re3's own
  bank format, not through any of the three streamed decoders above, so it is
  unaffected by this limitation.

## Other browser-specific limitations (task 10)

- **Audio scheduling does not depend on `requestAnimationFrame`.** Unlike
  rendering (`emscripten_set_main_loop`) or the Pointer Lock engagement issue
  documented in `docs/BROWSER_INPUT.md`, Emscripten schedules queued OpenAL
  buffer refills via `setInterval` (`AL.currentCtx.interval = setInterval(() =>
  AL.scheduleContextAudio(ctx), AL.QUEUE_INTERVAL)`, `libopenal.js:2177`), not
  via rAF. This means audio keeps running even in a backgrounded/hidden tab
  where rAF-driven rendering stalls (a known artifact of this project's browser
  automation test harness, see Phases 2/4/5) -- though browsers do throttle
  `setInterval` in background tabs (typically to ~1s), so streamed-buffer
  refill latency/jitter can increase while a tab is hidden. This is a browser
  scheduling policy, not a re3 or Emscripten bug.
- **One `AudioContext` per page, shared sample rate.** The browser's audio
  device sample rate is fixed once the first `AudioContext` is created; re3's
  `DIGITALRATE` (32000 Hz, `src/audio/sampman.h`) and any streamed file's native
  rate are resampled by the browser as needed. This is transparent and requires
  no code changes, but means there's no way to force the OS audio device to a
  specific rate the way a native build might.
- **No ALC device enumeration UI.** `cSampleManager::add_providers()` enumerates
  ALC devices via `alcGetString(nullptr, ALC_DEVICE_SPECIFIER)`; Emscripten's
  OpenAL implementation reports a single virtual "Emscripten" device backed by
  whatever the browser/OS default output is -- there's no way for the page to
  offer a device picker the way a native build's OS-level device list would.
  Not a defect, just a smaller surface than native.
- **Autoplay policy** (task 7, detailed above) -- not a limitation once handled,
  but worth restating: the *very first* audio-producing action on the page must
  trace back to a user gesture, or the `AudioContext` starts `suspended` until
  one occurs.

## The audiotest diagnostic tool (task 9: test audio independently)

`audiotest/` is a standalone CMake target (`audiotest/main.cpp`,
`audiotest/CMakeLists.txt`), following the same pattern as Phase 4's
`rendertest/`: it links against nothing from `src/` or `vendor/librw`, only
Emscripten's OpenAL implementation directly, and needs no GTA III assets --
every sound (a short SFX beep, a looping two-tone music chord, a positional pan
test tone) is generated procedurally as raw PCM at runtime.

Unlike `rendertest`, module load and audio initialization are deliberately
decoupled: `main()` does nothing but print a ready message and return; the
first (and only) point that touches OpenAL is the exported
`audiotest_init()` C function, called exclusively from
`web/audiotest.html`'s "Click to Start Audio Test" button handler. Every other
control (play SFX, play/pause/resume/stop music, volume sliders, stereo pan)
is its own exported function (`audiotest_play_sfx`, `audiotest_pause_music`,
`audiotest_set_music_volume`, `audiotest_play_pan_test`, ...), called via
`ccall`/`cwrap` from individual button/slider handlers -- so, like the
"Initialise" call itself, everything in this tool only ever runs from inside a
real user gesture.

Verification uses the same "query real state, don't just assume" approach
`rendertest/main.cpp` used for pixel readback: `audiotest_get_state(which)`
returns the actual `alGetSourcei(src, AL_SOURCE_STATE, &state)` value for each
source, polled and shown live in the page's log/state panel, so pass/fail is
based on OpenAL's own reported state transitions rather than "no crash
happened" or assuming a click did what it should.

### Validation (live, in-browser)

Run via `cmake --build build-wasm --target audiotest`, served with
`python scripts/serve_web.py 8000 web`, opened at `/audiotest.html`. All steps
below were exercised live and confirmed via the exported state-query functions
(not just visual/audible inspection, which isn't verifiable by an automated
agent):

| Step | Result |
|---|---|
| Module loads, no audio touched | Log shows only `[module] instantiated (no audio touched yet)` / `module ready -- waiting for audiotest_init() from a user gesture` before any click |
| Click "Click to Start Audio Test" | `alcOpenDevice`/`alcCreateContext`/buffer+source setup all report PASS; all three sources report `AL_INITIAL` |
| Play SFX | State transitions `AL_INITIAL` → `AL_PLAYING` → `AL_STOPPED` (observed as `STOPPED` once the 150ms one-shot finished) |
| Play music | State → `AL_PLAYING` |
| Pause music | State → `AL_PAUSED` |
| Resume music | State → `AL_PLAYING` again |
| Adjust music volume slider | `audiotest_set_music_volume(0.3)` applied with `alGetError() == AL_NO_ERROR` |
| Stereo pan test | `audiotest_play_pan_test(-1.0)` (`AL_POSITION` set to full left) plays and completes (`AL_STOPPED`) with no AL error |
| Stop music | State → `AL_STOPPED` |

No console errors were produced by any of the above (aside from one transient
build-configuration mistake during development -- an unexported `callMain`,
fixed by adding `callMain` to `EXPORTED_RUNTIME_METHODS` in
`audiotest/CMakeLists.txt` -- not present in the final build).

## Native build: unaffected

No changes were made to any file under `src/audio/` in this phase --
`cSampleManager`/`sampman_oal.cpp`, `cDMAudio`, `CStream`, `channel.cpp`, and
every game-logic audio manager (`CMusicManager`, `CPoliceRadio`,
`CAudioManager`) are exactly as they were. This phase added two new files
(`audiotest/main.cpp`, `audiotest/CMakeLists.txt`) plus this document, and one
line in the root `CMakeLists.txt` (`add_subdirectory(audiotest)`) that only
builds an additional, independent target -- it does not touch the `re3`/
`re3_wasm` targets or any existing source file. Native (`RE3_AUDIO=OAL` or
`MSS` on Windows) behavior is byte-for-byte unchanged.
