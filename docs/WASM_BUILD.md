# Building re3 for WebAssembly

This document explains how to compile the `re3` engine to WebAssembly using Emscripten.

## Prerequisites

- **Emscripten SDK (emsdk)**: You must have the Emscripten SDK installed and activated.
  - See the [Emscripten documentation](https://emscripten.org/docs/getting_started/downloads.html) for installation instructions.
  - Ensure that `emcmake` and `emcc` are available in your PATH.
- **CMake**: Version 3.8 or higher is required.

## Build Instructions

To automate the build process, a script is provided.

1. Open a terminal where the Emscripten SDK is activated.
2. Run the build script from the root of the repository:

   ```bash
   bash scripts/build_wasm.sh
   ```

This script will:
- Create a `build-wasm` directory.
- Configure CMake using `emcmake` to use the Emscripten toolchain.
- Build the `re3_wasm` target (`re3_wasm.js` + `re3_wasm.wasm`), copying the result
  into `web/build/`.

Note: `scripts/build_wasm.sh` currently runs a full `cmake --build .` (every target
in the project, including native librw tools), not a scoped `--target re3_wasm`
build. If you only want the WASM target, build it directly instead:
```bash
cmake --build build-wasm --target re3_wasm --parallel 4
```

## Output Files

The build generates these artifacts in `build-wasm/src/`:
- `re3_wasm.wasm`: The compiled WebAssembly module.
- `re3_wasm.js`: The Emscripten JavaScript glue code. Built with `MODULARIZE=1` /
  `EXPORT_NAME=createRe3Module`, so it defines a `createRe3Module()` factory function
  instead of auto-running against a global `Module` -- see `web/launcher.js`.

There is no generated `.html` shell (`SUFFIX` is set to `.js`, not `.html`, in
`src/CMakeLists.txt`) -- the browser-facing entry point is the hand-written
`web/index.html` instead. As a post-build step, `re3_wasm.js`/`.wasm` are copied
automatically into `web/build/`, so `web/` is always a self-contained, directly
servable directory after a build. See [docs/BROWSER_RUNTIME.md](BROWSER_RUNTIME.md)
for how to serve and use it.

## Known Limitations (Phase 1 + 2)
- Threads (`pthreads`, used for `PTHREAD_POOL_SIZE=4`) require the page to be
  [cross-origin isolated](https://web.dev/articles/coop-coep) (COOP/COEP response
  headers) for `SharedArrayBuffer` to work at all -- `scripts/serve_web.py` sends
  those headers for local dev; any other deployment must too. Without them, module
  instantiation itself fails (see docs/BROWSER_RUNTIME.md).
- No asset packaging or virtual filesystem integration exists yet. `re3`'s engine
  init (`CdStreamPosix.cpp` in particular) still assumes a real native filesystem
  full of GTA III game files and currently fails an assertion / crashes once it gets
  that far, since none of that exists in the browser. This is expected at this stage
  -- `web/launcher.js` catches it and reports it instead of hiding it.
- The virtual filesystem for save states and settings is not mapped.
