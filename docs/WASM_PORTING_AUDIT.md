# re3 WebAssembly Porting Audit

## Architecture Overview
The re3 project is a reverse-engineered port of the Grand Theft Auto III engine. The engine is written in C++ and uses a platform abstraction layer (`src/skel`) to handle OS-specific tasks such as window creation, input, and event polling. Rendering is handled by `librw`, an open-source RenderWare implementation. The game loop is traditionally owned by the native application using a `while(true)` loop that dispatches events and frame updates.

## Build System Analysis
- **Build Systems:** The project supports both CMake (`CMakeLists.txt`) and Premake5 (`premake5.lua`).
- **Targets:** The primary target is the `re3` executable, statically linked with the `librw` library.
- **Platform-Specific Flags:** Build files heavily use conditional compilation for Windows, Linux, BSD, and macOS. They define configurations for rendering backends (e.g., `RW_D3D9`, `RW_GL3`) and audio backends (e.g., `AUDIO_OAL`, `AUDIO_MSS`).
- **WASM Migration:** Emscripten integrates natively with CMake. A new CMake toolchain file for Emscripten (`emcmake cmake ..`) can be used without altering the core structure.

## Dependency Table

| Dependency | Purpose | WASM-Compatible | Notes |
|---|---|---|---|
| **librw** | Rendering engine | Yes | Included as source in `vendor/librw`. Can be compiled to WASM. |
| **glfw3** | Window & Input | Yes | Emscripten provides a built-in GLFW3 port (`-s USE_GLFW=3`). |
| **OpenAL** | Audio backend | Yes | Emscripten provides built-in OpenAL support. |
| **mpg123** | MP3 decoding | Yes (Requires config) | Pure C library, can be compiled to WASM from source. |
| **libsndfile**| WAV decoding | Yes (Requires config) | Pure C library, can be compiled to WASM from source. |
| **libogg/opus**| Opus decoding | Yes | Pure C libraries, can be compiled to WASM. |
| **Miles SDK** | Audio backend | **No (Requires replacement)** | Proprietary binaries. Must use `AUDIO_OAL` for WASM. |
| **DirectX SDK** | Rendering | **No** | Windows-only. Must use `RW_GL3` (OpenGL 3 / WebGL 2). |

## Platform-Specific Code Table

| Abstraction | Location | WASM Implementation Path |
|---|---|---|
| **OS Abstraction** | `src/skel/crossplatform.cpp` | Emscripten acts as a POSIX environment, so existing Linux/POSIX paths will mostly work. |
| **Windowing/Events** | `src/skel/glfw/glfw.cpp` | Emscripten's built-in GLFW3 wrapper will intercept these calls. |
| **File I/O** | `src/core/FileMgr.cpp` | `myfopen` wraps `fcaseopen`. Emscripten's virtual filesystem (MEMFS/IDBFS) supports standard C I/O. |
| **Timers** | `src/core/Timer.cpp` | POSIX timers or `emscripten_get_now()` can be used. |
| **Main Loop** | `src/skel/glfw/glfw.cpp` & `main.cpp` | The blocking `while(true)` loop must be replaced with `emscripten_set_main_loop`. |

## Rendering Analysis
- **Backend:** The engine uses `librw` (in `vendor/librw`).
- **APIs:** It supports D3D8, D3D9, and OpenGL 3 (via GLFW).
- **WASM Mapping:** The `librw`'s `gl3` backend is heavily compatible with WebGL2 (which is essentially OpenGL ES 3.0). Emscripten can translate most GL3 calls directly to WebGL2 using `-s USE_WEBGL2=1`. Some desktop-specific OpenGL features (if any) may need `#ifdef EMSCRIPTEN` fallbacks in `librw`.

## Audio Analysis
- **Backend:** The engine supports Miles Sound System (MSS) and OpenAL (OAL).
- **WASM Mapping:** MSS cannot be used on WebAssembly. The OpenAL backend (`AUDIO_OAL`) is required. Emscripten translates OpenAL calls directly to the Web Audio API.

## Input Analysis
- **Handling:** Input is managed through GLFW (`src/skel/glfw/glfw.cpp`), processing keyboard and mouse events.
- **WASM Mapping:** Emscripten's GLFW3 port automatically maps browser keyboard and mouse events (DOM events) to GLFW callbacks. No major engine changes are required here.

## Filesystem/Assets Analysis
- **Mechanism:** Assets are loaded using C standard I/O (e.g., `fopen`, `fread`), wrapped in `CFileMgr`.
- **WASM Mapping:** Browsers cannot access the user's local filesystem synchronously. We must use Emscripten's virtual filesystem. The user's GTA III data folder will need to be packaged via Emscripten's `--preload-file` or dynamically mounted using IDBFS for persistence (save games).

## Threading Analysis
- **Usage:** Threading is primarily used for asynchronous CD streaming (asset loading) in `src/core/CdStreamPosix.cpp` and `src/core/CdStream.cpp`.
- **APIs:** It uses `pthread` on POSIX and `CreateThread` on Windows.
- **WASM Mapping:** To support asynchronous asset loading, Emscripten's `pthreads` support (which uses Web Workers and `SharedArrayBuffer`) must be enabled (`-s USE_PTHREADS=1`). If `SharedArrayBuffer` is unavailable due to browser security restrictions, a synchronous fallback or Emscripten's async file I/O will be necessary.

## Networking Analysis
- **Usage:** The core single-player game does not use any networking functionality. It is completely offline.

## Estimated Migration Difficulty
**Moderate.** The engine's existing POSIX compatibility, GLFW integration, and OpenAL backend significantly reduce the workload. The primary challenges will be:
1. Refactoring the main `while(true)` loop to use `emscripten_set_main_loop`.
2. Compiling and linking dependencies (mpg123, libsndfile) to WASM.
3. Overcoming potential WebGL2 incompatibilities in `librw`.
4. Designing a mechanism to package or mount the massive game assets into the browser.

## Recommended Porting Order
1. **Toolchain Setup:** Configure CMake to use the Emscripten toolchain (`emcmake`).
2. **Dependency Compilation:** Compile `mpg123`, `libsndfile`, and `librw` to static WASM libraries.
3. **Audio & Video Configurations:** Enforce `AUDIO_OAL` and `RW_GL3` in the build process.
4. **Main Loop Refactor:** Modify `glfw.cpp` to conditionally replace the blocking loop with `emscripten_set_main_loop`.
5. **Filesystem Integration:** Set up Emscripten's `IDBFS` for save games and create a script to preload basic assets.
6. **WebGL2 Debugging:** Resolve any shader or rendering pipeline errors in `librw`.

## Exact Files Likely to Require Modification
- `CMakeLists.txt` (to add Emscripten toolchain conditions and linker flags)
- `src/skel/glfw/glfw.cpp` (to refactor the main loop)
- `src/core/main.cpp` (to expose the loop tick function globally)
- `src/core/CdStreamPosix.cpp` (if `pthread` adjustments are needed for Web Workers)
- `vendor/librw/src/gl/gl3hw.cpp` or similar (for potential WebGL2 tweaks)
