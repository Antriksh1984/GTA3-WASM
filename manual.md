# re3 in the browser -- user manual

This is a WebAssembly build of [re3](https://github.com/GTAmodding/re3) (a
reverse-engineered, source-compatible GTA III) that runs entirely in a
browser tab via WebGL2 -- no native install, no plugin. This manual covers
building it, getting it running locally, and playing it. It does not cover
engine internals; see `docs/` for that (pointers at the bottom).

## 1. Prerequisites

- **Emscripten SDK (emsdk)** -- installed and activated. This repo already
  vendors one under `emsdk/`; see "Activate emsdk" below.
- **CMake** 3.8 or newer.
- **Python 3** -- used by the dev server and the asset validator.
- **A browser with WebGL2** -- current Chrome, Firefox, or Edge. Check via
  the in-page diagnostics panel (below) if unsure.
- **Your own copy of GTA III's game files.**

## 2. Building

### Activate emsdk

```bash
source emsdk/emsdk_env.sh
```

This puts `emcc`/`emcmake` on `PATH` for the current shell.

> **Windows note**: the `python`/`python3` that ends up on `PATH` after
> activating emsdk on Windows can resolve to a broken Microsoft Store alias
> stub instead of a real interpreter, which breaks the CMake build silently.
> If `cmake --build` fails in a way that looks python-related, put emsdk's
> own bundled Python first on `PATH` for that shell:
> ```bash
> export PATH="$(pwd)/emsdk/python/3.13.3_64bit:$PATH"
> ```

### Build

```bash
cmake --build build-wasm --target re3_wasm --parallel 4
```

(First time only: `emcmake cmake -B build-wasm -DCMAKE_BUILD_TYPE=Release`,
or just run `bash scripts/build_wasm.sh`, which does both steps.)

This produces `build-wasm/src/re3_wasm.js` + `.wasm` and copies them into
`web/build/` automatically -- `web/` is a self-contained, servable directory
after every build. Re-running the build command after a source change and
reloading the page is the whole edit/rebuild loop; nothing else needs
regenerating.

## 3. Running it

```bash
python scripts/serve_web.py 8080
```

Then open **http://127.0.0.1:8080** in your browser.

Don't open `web/index.html` directly from disk (`file://`) -- browsers block
fetching the `.wasm` file under that origin, so the page will fail to load
the module. Always serve it over `http://`.

`scripts/serve_web.py` is a small wrapper around Python's built-in HTTP
server; a plain static file server also works for just playing the game, but
the custom one additionally supports the dev-server asset shortcut (3B below)
and sends a couple of extra headers that don't hurt anything to have.

## 4. First launch: getting your GTA III files in

On load, the page shows a **Game assets** panel with three ways to supply
your files -- pick whichever fits:

**A. "Choose GTA III folder…" (normal use)**
Click it and pick your GTA III install folder in the browser's own folder
picker. Every file is read straight from your disk into the page's private
browser storage (IndexedDB) -- nothing is uploaded anywhere, nothing leaves
your machine. Once loaded, it's remembered across reloads, so you only do
this once per browser.

**B. Local dev-server mount (for working on this repo, not normal players)**
Start the server with an extra flag pointing at your local install, and load
the page with a query flag to reveal the button:
```bash
python scripts/serve_web.py 8080 web --dev-assets /path/to/your/gta3
```
python scripts/serve_web.py 8001 web --dev-assets gamefiles
then open `http://127.0.0.1:8080/?devAssets=1` and click **Load from dev
server**. Convenient for iterating on this repo's own code without re-picking
a folder every reload; not how an end user is meant to get assets in.

**C. Packaged build**
If a `web/build/package/` with a `manifest.json` exists, it's loaded
automatically. This repo doesn't generate one by default (so a normal build
can never accidentally ship copyrighted game data) -- you'd build it
yourself if you wanted this path. Most people will use (A).

The panel reports exactly which required files are missing, if any, using
the phrasing `Missing game asset: <path>` -- the same message the C++ engine
itself prints. You can still click Start with assets missing to see how the
engine reports that, but gameplay won't work correctly until all required
files are present.

Want to check a folder before even opening a browser?
```bash
python scripts/validate_assets.py /path/to/your/gta3
```

## 5. Playing

Click **Start engine** once your assets validate. The engine boots through
its normal startup (you'll see boot-progress steps and log lines), reaches
the main menu, and plays like any other GTA III PC build from there.

- **Keyboard**: standard GTA III PC bindings (WASD, arrows, etc. -- whatever
  your in-game control settings say).
- **Mouse look**: click the canvas to lock the pointer ("Click to enable
  mouse look"); <kbd>Esc</kbd> releases it. This is a real browser security
  gesture requirement, not a bug -- pointer lock can only ever be requested
  in direct response to a click.
- **Fullscreen**: the **Fullscreen** button in the top bar.
- **Saving and loading**: works exactly like the native game -- use the
  normal pause-menu Save/Load screens. Saves are automatically persisted to
  your browser's storage (IndexedDB) in the background after every save, and
  restored automatically the next time you load the page, before the engine
  even starts. See the **Save system** panel in the sidebar for live status
  (mounted/syncing/last result), a manual **Force Save Sync** button, and a
  confirm-gated **Clear Browser Saves…** button if you want to wipe them.

Saves, picked asset folders, and everything else this page stores all live
in *your browser's* storage for this origin -- clearing your browser's site
data for the page will remove them.

## 6. Diagnostics panel

The sidebar (toggle via the **Diagnostics** button) shows live info useful
for troubleshooting: WebGL/WebAssembly support, canvas size, current engine
state, frame timing, and a scrolling log of everything the engine and the
page shell have printed. Worth checking first if something looks wrong.

## 7. Known quirks

- **Backgrounded/hidden tabs**: browsers suspend `requestAnimationFrame` for
  tabs that aren't actually being composited (backgrounded, some embedded
  contexts). If the game seems frozen in such a context, the **Debug mode**
  checkbox in the top bar reschedules the main loop onto `setTimeout`
  instead, which keeps running even when rAF doesn't. Not needed for normal
  foreground browser tabs.
- **`WrongDocumentError` on pointer lock**: a known Chrome behavior for pages
  running in certain embedded/automated contexts (not a normal end-user
  tab); harmless, and doesn't stop the engine -- see `docs/BROWSER_INPUT.md`.
- **First real GTA III game data load can take a little while** the first
  time a level streams in, since asset reads go through the browser's
  virtual filesystem rather than a native disk.

## Further reading

This manual is the "how do I run it" version. For how any of this actually
works internally: `docs/WASM_BUILD.md`, `docs/BROWSER_RUNTIME.md`,
`docs/GAME_ASSETS.md`, `docs/BROWSER_INPUT.md`, `docs/AUDIO_WASM.md`,
`docs/SAVES_WASM.md`, `docs/WASM_GAMEPLAY_STABILITY.md`.
