# Weekly Project Journal — Antriksh (1024031034)

## Week 1: Upstream `re3` Porting Audit & Feasibility Analysis
- Audited the upstream `re3` C++ codebase to identify platform-specific dependencies (Win32/POSIX windowing, OpenGL desktop calls, OpenAL audio, and synchronous file I/O).
- Documented subsystem porting requirements and technical risks in `docs/WASM_PORTING_AUDIT.md`.

## Week 2: Emscripten Toolchain & CMake Build System
- Vendored the Emscripten SDK (`emsdk`) and authored the root `CMakeLists.txt` targeting `wasm32-unknown-emscripten`.
- Cross-compiled core third-party audio dependencies (`libsndfile` and `openal-soft`) under `vendor/` and documented the build pipeline in `docs/WASM_BUILD.md`.

## Week 3: Browser Main Loop & Threading Adaptation
- Refactored the synchronous native game loop into a non-blocking browser frame callback compatible with `emscripten_set_main_loop` (`docs/BROWSER_MAIN_LOOP.md`).
- Resolved pthreads and shared-memory synchronization constraints for browser worker execution (`docs/THREADING_WASM.md`).

## Week 4: WebGL2 Rendering & Shader Compatibility
- Adapted the rendering backend to target WebGL2 / OpenGL ES 3.0, resolving fixed-function pipeline incompatibilities and GLSL precision qualifiers (`docs/WEBGL_COMPATIBILITY.md`).
- Built the standalone `rendertest/` WASM diagnostic harness to isolate and validate texture uploads and draw calls.

## Week 5: WebAssembly Audio Subsystem Porting
- Integrated OpenAL-soft with WebAudio backend output and connected streaming sound effects and radio audio decoding (`docs/AUDIO_WASM.md`).
- Built the `audiotest/` diagnostic harness to verify buffer queuing and eliminate audio crackling under frame drops.

## Week 6: Browser Input Shims, Asset Mounting & First Boot
- Implemented browser keyboard and mouse Pointer Lock input shims mapping DOM events to engine input states (`docs/BROWSER_INPUT.md`).
- Implemented virtual filesystem asset mounting (`docs/GAME_ASSETS.md`) and achieved the first playable browser boot (`docs/FIRST_BOOT.md`).

## Week 7: Persistent IndexedDB Save System
- Implemented `web/saves.js` with a dedicated `IDBFS` mount, coalesced async filesystem syncs, and C++-to-JS save-directory notification hooks (`docs/SAVES_WASM.md`).
- Verified reliable save-file creation, overwrite, and reload persistence without data corruption.

## Week 8: Menu-to-Gameplay Debugging & Stability Hardening
- Resolved state-transition freezes between the frontend menu, loading screen, and active 3D gameplay (`docs/MENU_TO_GAMEPLAY_DEBUGGING.md`).
- Fixed memory-alignment and asset-streaming edge cases to stabilize extended gameplay sessions (`docs/WASM_GAMEPLAY_STABILITY.md`).

## Week 9: End-User Manual & Final Technical Review
- Authored `manual.md` detailing build flags, Emscripten memory settings, and asset packaging instructions.
- Reviewed system architecture diagrams and technical sections of the final project report (`project-report-final/main.tex`) ahead of final evaluation.
