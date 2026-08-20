# WebGL Compatibility (Phase 4)

## Rendering trace: game → RenderWare → GL backend → WebGL2

re3 never calls OpenGL directly. All rendering goes through RenderWare's platform
abstraction (librw, vendored in `vendor/librw/`):

```
re3 game code (src/render/*, src/rw/*)
    |  RpAtomicRender, CSprite2d, CFont, particles, HUD, ...
    v
librw public API (vendor/librw/src/rw*.h)
    |  rw::im2d::*, rw::im3d::*, RpAtomic/Camera/Raster/Texture/Shader
    v
rw::Device function table (vendor/librw/src/rwengine.h)
    |  a platform-filled struct of function pointers: beginUpdate, endUpdate,
    |  clearCamera, showRaster, im2DRenderPrimitive, im3DRenderPrimitive, ...
    |  engine->device = gl3::renderdevice  (vendor/librw/src/engine.cpp:263,
    |  selected because CMakeLists.txt sets LIBRW_PLATFORM=GL3)
    v
librw's GL3 backend (vendor/librw/src/gl/gl3*.cpp)
    |  gl3device.cpp: device/context lifecycle, render state, shader selection
    |  gl3raster.cpp: Raster create/lock/unlock (texture + framebuffer I/O)
    |  gl3immed.cpp:  Im2D/Im3D immediate-mode vertex/index buffers + draw calls
    |  gl3shader.cpp: GLSL compilation/linking
    v
Real GL calls (glDrawElements, glTexImage2D, glCompileShader, glReadPixels, ...)
    |  resolved through GLFW's Emscripten port (-sUSE_GLFW=3) and WebGL2
    |  (-sUSE_WEBGL2=1), see src/CMakeLists.txt
    v
Browser WebGL2 context (backed by the <canvas> in web/index.html /
web/rendertest.html -- see docs/BROWSER_RUNTIME.md)
```

**Answering task 2 up front: yes**, the existing OpenGL path (librw's GL3 backend)
compiles for and runs on WebGL2 -- confirmed by actually running it (see
"Validation" below), not just by inspection. GL3 already targets a modern,
core-profile-shaped subset of desktop GL close to GLES3/WebGL2's shape (explicit
attribute locations, VAOs, UButham-style state caching), so **no new renderer was
written**. Every change in this phase is either (a) a compile-time shim for a
symbol/constant WebGL doesn't provide, or (b) a runtime bug fix so the *existing*
GL3 code takes the *existing* GLES/WebGL-shaped branch it already has (several of
these branches -- `gl3Caps.gles` checks -- existed in the code before this port but
were never actually reached correctly under Emscripten; see the table).

## Browser graphics initialization layer (tasks 4 & 5)

There isn't a separate "browser graphics init" module -- the existing
`vendor/librw/src/gl/gl3device.cpp` device-open path (`startGLFW()`) *is* that
layer, reused as-is:

1. `glfwInit()` / `glfwCreateWindow()` -- under Emscripten, GLFW's port creates a
   WebGL2 context on the page's `<canvas>` (`Module['canvas']`, wired up by
   `web/launcher.js`/`web/rendertest-launcher.js`) via
   `canvas.getContext('webgl2', contextAttributes)`.
2. **Viewport**: set every frame in `gl3::beginUpdate()` from the camera's
   framebuffer raster size (`glViewport(x, y, w, h)`).
3. **Framebuffer**: the on-screen camera raster (`Raster::CAMERA`) is bound to GL
   framebuffer 0 (the canvas's own default framebuffer) -- confirmed by inspection
   (`rasterCreateCamera()` sets `natras->fbo = 0`) and by runtime readback (see
   Validation).
4. **Depth buffer**: `Raster::ZBUFFER`, created alongside the camera raster in
   `sk::CameraCreate()` (`vendor/librw/skeleton/skeleton.cpp`) /
   `CameraCreate()` (`src/skel/glfw/glfw.cpp`), attached via
   `glFramebufferTexture2D`/`glFramebufferRenderbuffer` depending on platform (see
   table row below).
5. **Stencil buffer**: combined with depth via `GL_DEPTH_STENCIL_ATTACHMENT`
   (standard GL3/GLES3 practice); re3 uses stencil render states
   (`STENCILENABLE`/`STENCILFUNCTION`/etc. in `rwrender.h`) for effects like mirrors
   and shadows, unmodified.

## Compatibility table (task 6)

| Feature | Used by re3 | WebGL support | Action |
|---|---|---|---|
| Core GL3-shaped rendering (explicit attrib locations, VAOs, UBO-style state caching, `glDrawElements`/`glDrawArrays`) | Everywhere -- this is the whole GL3 backend | Yes (WebGL2 = GLES 3.0-equivalent) | None -- compiles and runs unmodified. |
| `GLFW_CLIENT_API`-based context/version detection (`gl3device.cpp`'s `profiles[]` trial loop) | Once, at device open, to set `gl3Caps.gles`/`glversion` | Emscripten's GLFW port creates a WebGL context for **any** `GLFW_CLIENT_API` value (desktop or ES) -- it doesn't actually distinguish them, so the loop's first entry (`{GLFW_OPENGL_API, 3, 3}`) always "succeeds" and reports a **desktop** GL 3.3 context that was never actually created | **Fixed**: force `gl3Caps.gles=true; gl3Caps.glversion=30` directly after context creation under `__EMSCRIPTEN__`, bypassing the (here-unreliable) detection loop's result. This is load-bearing -- see the next two rows. |
| GLSL shader `#version` pragma | Every shader (`vendor/librw/src/gl/shaders/`) | WebGL2 requires **exactly** `#version 300 es` (GLSL ES 3.00). `#version 310 es` (what the code picked once `gles` was detected correctly-ish) is GLES 3.1-only and is rejected; `#version 330` (desktop, what it picked *before* the fix above) is rejected outright | **Fixed**: added `shaderDecl300es` (a copy of the existing `shaderDecl310es` with the version line changed) used only under `__EMSCRIPTEN__`; the native GLES 3.1 path (`shaderDecl310es`) is untouched. None of librw's GL3 shaders use anything beyond GLSL ES 3.00 (checked: no compute/image-load-store/etc), so nothing is lost. |
| Framebuffer depth/stencil attachment | Every frame, camera setup | Desktop GL attaches a depth-stencil **texture** (`glFramebufferTexture2D`); GLES/WebGL expects a **renderbuffer** (`glFramebufferRenderbuffer`) for this | Already branched on `gl3Caps.gles` in the existing code (`gl3device.cpp`, two call sites) -- **no code change needed**, it just needed `gl3Caps.gles` to actually be `true` (see above) to take the branch that already existed for this. |
| `glReadPixels` arbitrary format conversion | Reading the on-screen framebuffer back to a CPU-side `Image` (`Raster::CAMERA`'s `rasterLock()`, used by `Raster::toImage()`) | Desktop GL allows requesting e.g. `GL_RGB` and lets the driver convert; WebGL2 only guarantees `GL_RGBA`/`GL_UNSIGNED_BYTE` (plus one implementation-defined combo) -- requesting `GL_RGB` fails with `GL_INVALID_OPERATION` | **Fixed**: under `__EMSCRIPTEN__`, `rasterCreateCamera()`/`rasterLock()` now read back as `GL_RGBA`/`GL_UNSIGNED_BYTE` (4 bytes/pixel) instead of `GL_RGB` (3 bytes/pixel), with `raster->format`/`natras->bpp` updated to match so `Raster::toImage()`'s format-based pixel converter (`raster.cpp`) interprets the bytes correctly. Found and fixed via this phase's rendertest tool -- see "How this was found" below. |
| GLAD (desktop GL function-pointer loader) | Loading every non-1.1 GL entry point, on desktop | Doesn't apply to WebGL -- Emscripten provides the GL/GLES/WebGL2 function bindings directly, no loader needed (and GLAD's own loader code doesn't build against WebGL headers) | **Fixed in Phase 1**: `glad.c` excluded from the Emscripten build (`CMakeLists.txt`), all `#include "glad/glad.h"` replaced with `#include <GLES3/gl3.h>` under `__EMSCRIPTEN__` (`rwgl3.h`, `wdgl.cpp`). |
| `GL_CLAMP_TO_BORDER` texture wrap mode | Optional per-texture addressing mode (`Texture::Addressing::BORDER`) | Not in GLES3/WebGL2 core | **Fixed in Phase 1**: compatibility `#define` added under `__EMSCRIPTEN__` (`rwgl3.h`) so the enum value exists at compile time. Not reachable via any texture created by the current asset-loading path, so no runtime fallback was needed yet. |
| Anisotropic filtering (`GL_TEXTURE_MAX_ANISOTROPY_EXT`) | Optional texture quality setting | Extension-gated in both desktop GL and WebGL2 (`EXT_texture_filter_anisotropic`); without GLAD there's no extension-flag query under Emscripten | **Fixed in Phase 1**: treated as unsupported (`gl3Caps.maxAnisotropy = 1.0f`) under `__EMSCRIPTEN__`; compile-time constant added so the code that *would* use it still builds. |
| S3TC / ASTC compressed texture formats (`GL_COMPRESSED_*_S3TC_*`, ASTC) | Optional, for compressed `.txd` textures | Both are real WebGL2 extensions (`WEBGL_compressed_texture_s3tc`, `_astc`) but require an explicit extension query, which the code has no path for without GLAD | **Fixed in Phase 1**: treated as unsupported (`gl3Caps.dxtSupported = gl3Caps.astcSupported = false`) under `__EMSCRIPTEN__`. Querying the real WebGL extensions is future work if compressed textures turn out to be needed (most desktop GTA III installs ship uncompressed TXDs, so this hasn't blocked anything so far). |
| `glGetTexImage` | Reading a texture's pixels back to CPU (non-GLES desktop path) | Does not exist in GLES/WebGL at all, at any version | **Fixed in Phase 1**: guarded out under `__EMSCRIPTEN__` (`gl3raster.cpp`) with an `assert(0)` -- dead code in practice, since `gl3Caps.gles` being `true` (now correctly, see above) means the sibling `glReadPixels`-via-FBO branch is what actually runs. |
| `glGetCompressedTexImage` | Reading a compressed texture's pixels back to CPU | Does not exist in GLES/WebGL | **Fixed in Phase 1**: same treatment as `glGetTexImage` above. |
| `GL_KHR_debug` (`glPushDebugGroup`/`glPopDebugGroup`) | Optional profiler/RenderDoc markers (`src/rw/RwHelper.cpp`, re3 code, not librw) | Real WebGL2 extension (`KHR_debug`... actually exposed as part of core in some browsers) but, again, no GLAD extension-flag query under Emscripten | **Fixed in Phase 1**: calls compiled out under `__EMSCRIPTEN__` -- purely a profiling nicety, `bDebugRenderGroups` already gates it at runtime too. |
| Paletted textures (`PAL4`/`PAL8`, `GL_COLOR_INDEX`-style) | Legacy 8-bit/4-bit TXD textures | Not supported by *any* GL3-era API, desktop or WebGL -- palette expansion happens upstream (`Image::unpalettize()`) before a texture ever reaches the GL3 backend | **Not WASM-specific** -- `rasterCreateTexture()`/`rasterCreateCameraTexture()` already reject `PAL4`/`PAL8` identically on native GL3 builds. No action needed. |
| Named POSIX semaphores (`sem_open`) | `CdStreamPosix.cpp`'s asset-streaming thread pool (not part of the GL3 rendering path at all) | Out of scope for this document -- not a GL/WebGL feature -- but noted because it's the current blocker to reaching in-game rendering through the full `re3_wasm` boot path (see docs/GAME_ASSETS.md's "What was verified against a real installation") | Not fixed here; `rendertest` (below) validates the actual rendering path independently of this, so it isn't a blocker for Phase 4's rendering validation. |

## How the `glReadPixels`/`GL_RGB` bug was found

Worth calling out because it's a good example of why task 10's diagnostic mode
matters: the `gl3Caps.gles` misdetection and `#version` fixes were found by reading
the code and cross-referencing the WebGL2 spec, but the `glReadPixels(..., GL_RGB,
...)` failure was **not** obvious from reading alone -- `rendertest`'s framebuffer
readback validation (see below) first showed all-black/all-zero pixels where real
colors were expected. Adding temporary `glGetError()` instrumentation at the actual
call site (not guessing from documentation) showed `GL_INVALID_OPERATION` on the
`glReadPixels` call itself, which is what led to the fix. Reasoning about WebGL
compatibility from source alone would very plausibly have missed this one.

## Rendering diagnostic mode / test scene (tasks 10 & 11)

`rendertest/main.cpp` + `web/rendertest.html` (open it directly, or via the "WebGL
rendering diagnostic" link from the main runtime page). It links directly against
`librw` + `librw_skeleton` (the same GL3 renderer re3 itself uses, via the same
lightweight harness librw's own `imguitest` tool uses) -- **not** through re3 or any
GTA III asset, so it validates the rendering path completely independently of
Phase 3's asset-loading work or the CdStream threading issue above. See
`rendertest/main.cpp`'s header comment for the full rationale.

It renders, then **reads the actual framebuffer pixels back and checks them**
(not just "did it crash"):

1. Clear screen to a solid color -- sampled and checked.
2. A flat-colored triangle via `rw::im2d::RenderPrimitive` -- exercises shader
   compilation, the Im2D vertex buffer, and a basic (untextured) draw call.
3. A textured checkerboard quad via `rw::im2d::RenderIndexedPrimitive` -- exercises
   index buffers, texture upload (`Raster::createFromImage`), and texture
   sampling. The checkerboard is generated procedurally at runtime; no asset files
   are loaded (consistent with Phase 3's "no GTA III assets required" boundary).
4. Two overlapping quads at different depths with `ZTESTENABLE`/`ZWRITEENABLE` on
   -- the nearer one is drawn first; if depth testing is genuinely rejecting
   occluded fragments (not just "last draw wins"), it stays on top. This is a real
   test of the fix in the table above, not just an assumption that it works.

Every stage prints `PASS`/`FAIL` (visible in the page's log panel and stdout) --
this is a genuine diagnostic, not a demo: a shader that failed to compile, a
texture that didn't upload, or a depth test that silently no-ops would all show up
as a specific, named `FAIL` line rather than a blank canvas.

### Validation result

All 6 checks pass, confirmed by reading the rendered pixels back and comparing
against expected values (not just visual inspection, which this environment's
browser-automation harness can't reliably do -- see docs/BROWSER_RUNTIME.md's notes
on `document.hidden`):

```
[rendertest] PASS: RW engine + GL3/WebGL2 device + camera initialized
[rendertest] PASS: checker texture created
[rendertest] PASS: clear screen -- (4,4)=rgba(20,24,32,255)
[rendertest] PASS: 2D triangle -- (160,260)=rgba(220,40,40,255)
[rendertest] PASS: textured quad (checker pattern visible) -- (380,120)=rgb(255,210,60) (500,120)=rgb(30,40,200)
[rendertest] PASS: depth test (near quad occludes far quad) -- (800,260)=rgba(60,90,230,255)
```

The reported `OpenGL version` string at startup --
`OpenGL ES 3.0 (WebGL 2.0 (OpenGL ES 3.0 Chromium))` -- itself confirms the
`gl3Caps.gles`/`glversion` fix above took effect (before the fix, this device-open
path believed and reported a desktop "OpenGL 3.3" context).

Because WebGL initialization was stable throughout this phase (no context-creation
failures, no unstable/flaky behavior across repeated runs), this phase proceeded
per the validation requirement. The `re3_wasm` target (Phase 3's full game boot
path) was also rebuilt and re-verified against the same real GTA III install used
in Phase 3 after every change in this phase, confirming no regression: `main()`
still returns cleanly, reaching the same (unrelated, documented) `CdStreamPosix.cpp`
semaphore blocker as before, not a new failure.

## What was and wasn't touched (tasks 3, 7, 9)

**Preserved unmodified**: the `rw::Device`/`rw::Driver` abstraction, the Im2D/Im3D
immediate-mode API, the shader/uniform infrastructure, the raster/texture object
model, all vertex/index buffer handling, all render-state plumbing
(`SetRenderState`/blending/depth/stencil enums in `rwrender.h`), and every bit of
re3's own rendering code (`src/render/`, `src/rw/`) -- none of it needed to change.

**Changed, always `#ifdef __EMSCRIPTEN__`-guarded so native output is
byte-for-byte identical**: `vendor/librw/src/gl/gl3device.cpp`,
`vendor/librw/src/gl/gl3raster.cpp` (table above), plus
`vendor/librw/skeleton/glfw.cpp`'s main loop, which had the same
"blocking `while()` never yields to the browser" problem as `src/skel/glfw/glfw.cpp`
(fixed in Phase 1) -- switched to `emscripten_set_main_loop()` under
`__EMSCRIPTEN__`, same pattern, needed so `rendertest` (and anything else built on
librw's own skeleton) actually renders instead of freezing the tab.

Native build verification is subject to the same pre-existing limitation noted in
earlier phases: this environment has no native C/C++ toolchain installed at all, so
"native still builds" is verified by code review (every change is
`#ifdef __EMSCRIPTEN__`-scoped) rather than an actual native compile.
