# Project Summary

## Problem Statement
Legacy desktop 3D games rely on native windowing APIs, desktop OpenGL fixed-function pipelines, blocking file I/O, and native audio hardware abstractions that cannot execute directly inside sandboxed web browsers.

## Solution Architecture
GTA3-WASM compiles the C++ `re3` engine to `wasm32-unknown-emscripten` and bridges each desktop subsystem to modern Web APIs:
- **Graphics:** Translated OpenGL rendering calls to WebGL 2.0 (OpenGL ES 3.0) with updated GLSL precision qualifiers.
- **Execution Loop:** Replaced the blocking native main loop with non-blocking `emscripten_set_main_loop` callbacks.
- **Audio:** Cross-compiled `openal-soft` and `libsndfile` to WebAssembly, routing sound effects and radio streams to WebAudio.
- **Storage & Saves:** Mounted user-provided game assets into an in-memory virtual filesystem (`MEMFS`) and persisted save slots across browser sessions via IndexedDB (`IDBFS`).
- **Input:** Mapped DOM keyboard events and browser Pointer Lock mouse deltas to engine input states.

## Validation Summary
- **Build Footprint:** ~12 MB compressed JS/WASM bundle (excluding user-supplied game assets).
- **Runtime Performance:** Stable 30–60 FPS on Chromium-based browsers at 1080p with ~310 MB WASM linear heap usage.
- **Functional Testing:** Verified asset mounting (`TC-01`), 3D gameplay rendering and controls (`TC-02`), and IndexedDB save/reload persistence (`TC-03`).
