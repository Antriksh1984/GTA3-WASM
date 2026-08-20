# Game Assets (Phase 3)

## Legal requirement

**This repository never contains, downloads, packages, or distributes GTA III game
data.** Everything in this document assumes *you* already own a legal copy of GTA III
and are pointing this engine at your own files. Nothing here automates acquiring the
game. `gamefiles/` (conventionally where a local installation lives -- see
`.gitignore`) is excluded from version control except for the handful of files that
belong to re3 itself, not Take-Two/Rockstar (`gta3.ini`, `gamecontrollerdb.txt`,
`ReadMe/`, `Website/`, `installscript.vdf`).

## Architecture

```
Your local GTA III install (gamefiles/, or anywhere you like)
        |
Asset preparation / import      <- you choose one of three paths, see "Getting
        |                          assets into the browser" below
Emscripten virtual filesystem   <- web/vfs.js (AssetVFS), mounted at a configurable
        |                          root (default /game)
re3 filesystem abstraction      <- src/core/FileMgr.cpp + src/skel/crossplatform.cpp
        |                          (casepath()) -- unmodified, see "Why re3 needed
        |                          zero path-handling changes" below
Game engine                     <- unmodified re3 C++, reads relative paths exactly
                                    as it does on native Linux/Windows
```

## Where the WASM module looks for assets

The mount point is `/game` by default and is **configurable** (task 5): pass
`?assetRoot=/whatever` in the page URL, or call `AssetVFS` functions with a different
mount point directly. Whatever it's set to, `web/launcher.js` calls
`FS.chdir(mountPoint)` right before handing off to re3's `main()`
(`AssetVFS.chdirToRoot()` in `web/vfs.js`) -- from that point on, the engine sees it
as its current working directory, exactly like it would `cd` into an install
directory on native Linux.

### Why re3 needed zero path-handling changes

Every asset path re3 opens is **relative**, resolved against whatever the process's
current working directory happens to be:

- `src/core/FileMgr.cpp`'s `CFileMgr::Initialise()` captures `getcwd()` once at
  startup as the "root"; `SetDir()`/`ChangeDir()` build further relative paths
  (`DATA\`, `TEXT`, `data\paths`, etc.) and `chdir()` into them.
- `src/skel/crossplatform.cpp`'s `casepath()` does the actual case-insensitive
  directory walk (GTA III's file references are mixed-case Windows paths; real
  installs -- and this repo's own tooling, see below -- are often not) and handles
  `/` and `\` interchangeably.
- `src/core/CdStreamPosix.cpp`'s `CdStreamInit()` opens `models/gta3.img` with a
  plain relative path and raw POSIX `open()`/`read()`/`lseek()`.

Emscripten's virtual filesystem (MEMFS/IDBFS) implements `chdir`/`getcwd`/`opendir`/
`readdir`/`open`/`stat`/`statvfs` faithfully enough that all of this works completely
unmodified once real files exist at the expected relative paths and the working
directory is set correctly before `main()` runs. That's the whole abstraction: get
bytes into the VFS, `chdir` there, call `main()`. No engine code needed to change to
support a configurable asset root -- see docs/BROWSER_RUNTIME.md's "abstraction
boundary" section for why that's deliberate.

(Two diagnostic messages *were* improved for clarity -- see "How missing assets are
diagnosed" below -- but neither changes any control flow or path resolution.)

## Path categories (tasks 1 & 2)

Full detail with source references lives in `web/asset-manifest.json`; summary:

| Category | Examples | Where it lives | Notes |
|---|---|---|---|
| **Engine files** | `gamecontrollerdb.txt` | asset root (ships with re3, not GTA III content) | Loading is compiled out under `__EMSCRIPTEN__` (see docs/BROWSER_RUNTIME.md) since Emscripten's GLFW port doesn't implement gamepad mapping DB APIs; kept for native-build parity. |
| **Config files** | `gta3.ini`, `gta3.set` | `gta3.ini`: asset root. `gta3.set`: **user files directory**, not the asset root. | Not required to pre-exist; the engine writes defaults on first run. |
| **Game assets** | `data/gta3.dat`, `models/gta3.img`, `audio/sfx.SDT`, `TEXT/*.gxt`, `data/*.dat`/`.cfg` | asset root | The actual copyrighted GTA III data. See `web/asset-manifest.json` for the full required/optional split with source-line justification for each required file. |
| **Save files** | `GTA3sf<slot>.b` | user files directory | Written by `C_PcSave` (`src/save/PCSave.cpp`). |
| **Log files** | `SCRDBG.LOG` | current directory when opened | Debug-only, opened directly with `fopen()` (bypasses `CFileMgr`), never read back. |
| **Temp files** | none found | -- | re3 doesn't currently write scratch/cache files beyond saves and the log above. |

The **user files directory** (`_psGetUserFilesFolder()` in `src/skel/glfw/glfw.cpp`)
is a separate relative directory (`userfiles/`, created with `mkdir` on first run) from
the asset root -- saves and settings are never mixed with read-only game data, and
never need to be "supplied" the way assets do.

## Getting assets into the browser

Three mechanisms, all implemented in `web/vfs.js` (`AssetVFS`), matching task 6:

### A. Packaged filesystem (task 7) -- `AssetVFS.mountFromPackage`

Fetches `<baseUrl>/manifest.json` (a JSON array of relative paths) plus each file
from `web/build/package/` and writes them into the VFS. **This repo does not
generate a package by default** -- there is no build step that copies `gamefiles/`
into `web/build/package/`, deliberately, so a normal `cmake --build` can never ship
GTA III bytes. If you want this locally, build the package yourself (e.g. `cp` your
`gamefiles/` into `web/build/package/` and write a `manifest.json` listing the
relative paths) and keep it out of version control (`.gitignore` already excludes
`web/build/` entirely). `launcher.js` tries this mount on every load; a 404 is the
normal, expected outcome when no package exists and is logged as informational, not
an error.

### B. User-provided asset directory (task 6B) -- `AssetVFS.mountFromFileList`

The "Choose GTA III folder…" button in the page uses
`<input type="file" webkitdirectory multiple>` to let the user pick their local GTA
III folder through the browser's own file picker. Every file's bytes go straight from
the user's disk into the mounted VFS in their own browser -- never through a server,
never written anywhere but that browser's own storage. This is the mechanism a real
end user is meant to use.

### C. Development-only dev-server mount (task 8) -- `AssetVFS.mountFromDevServer`

For iterating on this repo without re-picking a folder every reload:

```bash
python scripts/serve_web.py 8080 web --dev-assets gamefiles
# then open http://127.0.0.1:8080/?devAssets=1
```

Two separate opt-ins are required before any local file touches the network: the
server must be started with `--dev-assets <path>` (off by default, no default path),
*and* the page must be loaded with `?devAssets=1` (the "Load from dev server" button
is otherwise not even shown). Even then, `--dev-assets` only binds to `127.0.0.1` and
serves the directory read-only, straight off disk, on every request -- nothing is
cached, copied, or written into this repository. See `scripts/serve_web.py`'s module
docstring for the full reasoning. **Do not enable this flag in any deployment** --
it's a local development convenience, not a hosting mechanism.

### Persistence (task 6C) -- IDBFS

Whichever mechanism populates it, the asset root is mounted on IndexedDB
(`AssetVFS.mountIDBFS` + `loadFromIDB`/`persistToIDB`) when available, so a returning
visitor doesn't need to re-supply their folder on every page load. If IndexedDB is
unavailable (e.g. some private-browsing modes), the launcher falls back to a
session-only in-memory mount and says so in the log.

## How missing assets are diagnosed (task 9)

Every missing-file report in this project -- from the C++ engine, the in-browser
validator, and the CLI tool -- uses the same specific phrasing:

```
Missing game asset: <path>
```

never a bare "File failed". Three layers report it:

1. **`src/skel/crossplatform.cpp`'s `casepath()`** -- the engine's own case-insensitive
   path resolver -- prints `Missing game asset: <path>` (with the specific path
   component that couldn't be found) whenever it can't resolve a path, to the same
   place all of re3's debug output goes (visible in the browser via
   `web/launcher.js`'s `print`/`printErr` hooks, in the on-page log panel and
   devtools console). `src/core/CdStreamPosix.cpp`'s `CdStreamInit()` has its own copy
   of this message for `models/gta3.img` specifically, since that file is opened
   directly rather than through `CFileMgr`.
2. **`web/vfs.js`'s `AssetVFS.validate()`** -- runs *before* the engine ever starts,
   checking `web/asset-manifest.json`'s required/optional lists and (if present)
   every path referenced by the mounted `data/gta3.dat`, against a case-insensitive
   index of what's actually mounted (MEMFS/IDBFS are case-sensitive; re3's own
   `casepath()` isn't, and GTA III installs vary in on-disk casing -- see the comment
   above `buildCaseInsensitiveIndex` in `web/vfs.js` for why this matters). Results
   render in the page's "Game assets" panel and log with the same message format.
3. **`scripts/validate_assets.py`** -- the same check, run from the command line
   against a local directory, before you ever open a browser. See below.

## Asset validation tool (task 10)

```bash
python scripts/validate_assets.py [path-to-your-gta3-install]
```

Defaults to `gamefiles/`. Checks the manifest's required/optional files, then (if
`data/gta3.dat` is present) parses it and checks every `IDE`/`IPL`/`COLFILE`/etc. path
it references too -- the same two-tier check `web/vfs.js` does in-browser. Prints
`Missing game asset: <path>` for anything absent, matching in both places by design
(the exact keyword-parsing rules are kept in sync between
`scripts/validate_assets.py`'s `GTA3_DAT_PATH_KEYWORDS`/`parse_gta3_dat` and
`web/vfs.js`'s `GTA3_DAT_PATH_KEYWORDS`/`parseGta3Dat` -- update both if
`src/core/FileLoader.cpp`'s `LoadLevel()` keyword set ever changes). Exits 0 if every
required asset is present, 1 otherwise, so it can gate a CI step or a pre-flight
script. Never reads asset *content*, only file names -- it doesn't touch, copy, or
print anything about what's inside your GTA III files.

## What was verified against a real installation

This mechanism was tested end-to-end against a real, locally supplied GTA III
installation (via the dev-server mount above) as part of building it, not just read
through:

- `scripts/validate_assets.py` and the in-browser validator both correctly report a
  complete installation as complete (all 15 curated required files + all 46 files
  referenced by that install's `data/gta3.dat`).
- With the asset root mounted and `chdir`'d into, `main()` gets **past**
  `CdStreamInit`'s `models/gta3.img` check -- the exact point it failed at in Phase 2
  with no assets mounted -- and continues into the engine's own startup state
  machine, returning cleanly from `main()`.
- It then hits a **different, unrelated** failure: `CdStreamInitThread` (still in
  `src/core/CdStreamPosix.cpp`) fails to create a named POSIX semaphore
  (`sem_open`/`RE3_SEM_OPEN`) under Emscripten's pthread emulation. This is a
  threading/POSIX-API gap, not an asset-loading one -- it's next-phase work, not
  patched here, consistent with the "clean abstraction boundaries, not workarounds"
  guidance carried over from Phase 2.
- A smaller, separate observation: on first run (no `userfiles/` yet), `casepath()`
  reports `Missing game asset: userfiles` and `Missing game asset: gta3.set`. These
  aren't GTA III assets at all (see the category table above) -- they're the
  save/config directory that's supposed to be created on demand
  (`_psCreateFolder()` in `src/skel/glfw/glfw.cpp`). The engine tolerates this fine
  (it's not what causes the semaphore failure above), but `_psCreateFolder()`'s use of
  `realpath()` on a path that doesn't exist yet looks like a pre-existing, unrelated
  correctness issue worth a closer look in a later phase -- noted here rather than
  changed, since it's not asset-loading and native-build behavior shouldn't be risked
  chasing it now.

## Do not proceed to full gameplay yet

Per the Phase 3 brief: this phase is the asset-loading *mechanism*, validated as
working. It does not mean re3 fully initializes or renders anything yet -- see
"What was verified" above for exactly where it currently stops.
