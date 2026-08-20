# Persistent saves in the browser build

re3's own save system is **completely unmodified**. This document covers the one
thing added on top of it: making the directory it already writes to survive a
page reload, by mounting that directory as its own IndexedDB-backed filesystem
(IDBFS) underneath the existing virtual filesystem. No custom save format, no
`localStorage`, no JSON -- the exact same `GTA3sf<N>.b` files re3 has always
written, just persisted.

## 1. The existing re3 save architecture (audited, not modified)

- **`C_PcSave`** (`src/save/PCSave.h`/`.cpp`, global instance `PcSaveHelper`):
  - `SaveSlot(int32 slot)` -- opens `<DefaultPCSaveFileName><slot+1>.b` for
    writing via `CFileMgr::OpenFile`, calls `DoGameSpecificStuffBeforeSave()`,
    then `GenericSave(file)` to serialize the actual game state, then closes
    the file.
  - `DeleteSlot(int32 slot)` -- `DeleteFile()` on the same path pattern.
  - `PopulateSlotInfo()` -- scans all `SLOT_COUNT` (8) slots by opening each
    file, reading its header, and validating it (`CheckDataNotCorrupt`),
    filling in the global `Slots[SLOT_COUNT+1]` array
    (`SLOT_OK`/`SLOT_EMPTY`/`SLOT_CORRUPTED`, see `PCSave.h`) that the
    frontend menu reads to decide what to show/allow.
- **`GenericSave(int file)` / `GenericLoad()`**
  (`src/save/GenericGameStorage.h`/`.cpp`) -- the actual serialization of
  player, world, script, and stats state to/from the open file handle.
  Untouched.
- **`CFileMgr`** (`src/core/FileMgr.cpp`) -- `OpenFile`/`Read`/`Write`/`CloseFile`,
  with case-insensitive path resolution for non-Windows targets. This is the
  only layer that ever touches a real path string; everything above it deals
  in slot numbers.
- **The save directory itself**: `C_PcSave::SetSaveDirectory(path)` sets
  `DefaultPCSaveFileName = "<path>\GTA3sf"`. `path` comes from
  `_psGetUserFilesFolder()` (`src/skel/glfw/glfw.cpp`), which on the
  Emscripten build returns the relative string `"userfiles"`. Since the engine
  `chdir()`s to the asset mount point (`/game`, see part 2) before `main()`
  runs, this resolves to **`/game/userfiles`** -- created via `realpath()` +
  `mkdir()` the first time it's needed.
- **Save**: reached from the pause/frontend menu's `MENUPAGE_SAVING_IN_PROGRESS`
  screen (`src/core/Frontend.cpp`), which calls
  `PcSaveHelper.SaveSlot(m_nCurrSaveSlot)` directly.
- **Load**: reached from `MENUPAGE_LOADING_IN_PROGRESS`. This does **not**
  call a `LoadSlot()` method directly -- instead it sets
  `m_nCurrSaveSlot`, `m_bWantToRestart = true`,
  `CMenuManager::RequestFrontEndShutDown()`, `m_bWantToLoad = true`, and
  `b_FoundRecentSavedGameWantToLoad = true`. An Emscripten-specific restart
  handler inside `tick()` (`src/skel/glfw/glfw.cpp`, checked every tick,
  independent of `gGameState`) sees `m_bWantToRestart` and calls
  `CGame::ShutDownForRestart()` + `CGame::InitialiseWhenRestarting()`, which
  (when `m_bWantToLoad`) calls `GenericLoad()` (`src/core/Game.cpp`).
- **Slot scan timing**: `PopulateSlotInfo()` is called from several places in
  `Frontend.cpp` as the frontend menu opens/refreshes its Save/Load/Delete
  pages -- i.e. it runs continuously through the frontend's normal lifetime,
  not once at boot. This is exactly why the save directory must be fully
  restored **before the frontend ever starts running**, not just before some
  specific "check saves" step -- there is no single such step to race against
  otherwise.

None of the above needed to change. The only C++ addition is a notification
hook (part 4).

## 2. Existing Emscripten filesystem setup (audited)

Before this work, the browser build had exactly one virtual-filesystem
persistence mechanism: `web/vfs.js`'s `AssetVFS` module, which mounts IDBFS at
the **asset** root (`/game` by default) so a user-provided GTA III install
doesn't have to be re-picked every reload. `web/launcher.js`'s `main()`
sequence is:

1. Instantiate the WASM module (`createRe3Module()` -- does **not** auto-run
   `main()`; this build uses `MODULARIZE=1` specifically so JS controls when
   the C++ entry point runs).
2. `setupAssetPanel()` -- mount IDBFS at `/game`, `await` its restore, then
   optionally load a packaged/dev-server/user-picked asset tree.
3. Wait for the user to click **Start engine**.
4. `runEngineMain()` -- `chdirToRoot()` then `instance.callMain([])`.

Step 3's "wait for an explicit click" is what makes a second, deterministic
`await`ed step insertable before `callMain()` without any timers: nothing
about the asset flow needed to change to make room for save persistence, it
just needed one more awaited call in the same sequence.

## 3. Save persistence: mount point and startup ordering

`web/saves.js` (new) mounts a **second, separate** IDBFS at
`<assetMountPoint>/userfiles` -- i.e. `/game/userfiles`, the exact directory
`_psGetUserFilesFolder()` already resolves to. This was a deliberate choice
over an arbitrary path like `/re3-saves`: the game computes this path itself
relative to its working directory, so putting the persisted directory anywhere
else would require patching `_psGetUserFilesFolder()` just to redirect it back
to where saves already needed to be mounted from.

It is a **separate IndexedDB database** from the asset mount (Emscripten's
IDBFS keys the database by mount path, so `/game` and `/game/userfiles` never
share storage) -- a save is tens of KB; the asset tree is hundreds of MB. Sync
operations on one must never touch the other. See part 3a for why that
required more than just "mount a different path".

`web/launcher.js`'s `runEngineMain()`:

```js
async function runEngineMain(instance, mountPoint, focusRecovery, tabThrottling) {
    setStatus("loading", "restoring saved games…");
    try {
        await Re3Saves.mountAndRestore(instance, mountPoint, log);
    } catch (err) {
        log(`[Save] ERROR: save persistence setup failed unexpectedly: ${err}`, "stderr");
    }
    els.assetsOverlay.classList.add("hidden");
    AssetVFS.chdirToRoot(instance.FS, mountPoint);
    ...
    instance.callMain([]);
}
```

`mountAndRestore()` mounts the IDBFS, then `await`s `FS.syncfs(true)`
(populate the in-memory tree from IndexedDB) before returning. Because this is
awaited **before** `callMain()`, re3's C++ startup -- and every later
`PopulateSlotInfo()` call from the frontend -- only ever sees `/game/userfiles`
in its final, restored state. There is no window where the engine could start
before persisted saves are in place, and nothing here uses `setTimeout` to
paper over the async gap.

### 3a. The nested-mount bug (found and fixed during validation)

The first working version of this mount used the top-level `FS.syncfs()` (the
same call `AssetVFS` uses for the asset mount). Live testing surfaced a
restore failure -- `ErrnoError { errno: 10 }` -- that did not reproduce when
calling the `/game/userfiles` mount's own sync function in isolation.

Root cause: Emscripten's `FS.syncfs(populate, cb)` does not scope itself to
one mount. It calls `FS.getMounts(FS.root.mount)`, which walks the **entire**
mount tree from `/` down, and runs every mounted filesystem's own `syncfs`.
Since `/game/userfiles` is mounted *inside* `/game`'s own tree, syncing
`/game` also asks IDBFS to reconcile `/game`'s local filesystem against its
remote (IndexedDB) snapshot -- and `IDBFS.getLocalSet()` walks `/game`
recursively, including into the `userfiles` subtree, treating it as an
ordinary child directory. When that diff decides a local-only entry (the
`userfiles` directory, absent from `/game`'s own remote snapshot) should be
removed to match the remote state, it calls `FS.rmdir()` on what is actually a
live mount point -- which Emscripten's FS layer refuses
(`FS.ErrnoError(10)`, the same code `FS.mount()` itself throws for
"already a mountpoint").

Fix: `web/saves.js` never calls the top-level `FS.syncfs()`. It captures the
mount descriptor `FS.mount(...)` returns and calls
`mount.type.syncfs(mount, populate, cb)` directly -- scoping every sync
strictly to the `/game/userfiles` mount, so it can never see (or be seen by)
the asset mount's own reconciliation.

## 4. Save-write notification (C++ -> JS)

`src/save/PCSave.cpp` gained one `EM_JS` hook, called right after
`CFileMgr::CloseFile()` succeeds in `SaveSlot()`, and at the end of
`DeleteSlot()`:

```cpp
EM_JS(void, re3_NotifySaveDirChangedJS, (const char *reason, const char *filename), {
    if (window.Re3Saves && window.Re3Saves.onSaveDirChanged)
        window.Re3Saves.onSaveDirChanged(UTF8ToString(reason), UTF8ToString(filename));
});
```

called as `re3_NotifySaveDirChangedJS("save", ValidSaveName)` /
`re3_NotifySaveDirChangedJS("delete", FileName)`. This was the natural
integration point: it's the exact moment the native save code already knows
the write succeeded, with the real filename already in hand, and it requires
no change to *when* or *what* re3 writes -- only a notification that it just
did. Both call sites are `#ifdef __EMSCRIPTEN__`-guarded; native Windows/Linux
builds are unaffected (see part 8).

## 5. Async sync queue (never overlapping, never dropped)

`web/saves.js` implements a small IDLE/SYNCING state machine:

```
IDLE --(change)--> SYNCING --(sync finishes)--> [was there another
                                                   change mid-sync?]
                                                     no  -> IDLE
                                                     yes -> sync again
```

`requestSync()`: if a sync is already in flight, it just sets a `pending`
flag and returns -- it never starts a second, overlapping `FS.syncfs(false)`
call. When the in-flight sync's callback fires, `runSync()` checks `pending`
and, if set, immediately runs one more sync before going back to `IDLE`. This
guarantees:

- saves never race each other into IndexedDB,
- the *last* write before the queue drains is always the one that ends up
  persisted (verified in part 9's multi-save test: 3 saves in immediate
  succession produced exactly 2 real `syncfs()` calls, and the final value
  survived a reload intact),
- a slow or failed sync never blocks gameplay -- `SaveSlot()` itself is
  synchronous and already returned before any of this runs.

If IndexedDB is unavailable (e.g. some private-browsing modes) or a sync
fails, `[Save] ERROR: ...` is logged and `lastSyncError` is recorded for the
diagnostics panel; the in-memory save (via `mkdirTree`'s plain-directory
fallback) still works normally for the current tab session, it just won't
survive a reload.

## 6. Diagnostics UI and logging

`web/index.html` gained a "Save system" sidebar section (table + two
buttons); `web/launcher.js`'s `setupSaveDiagnosticsPanel()` renders it,
driven entirely by `Re3Saves.setOnStateChange()` callbacks -- no polling.
Shows: **Backend** (`IDBFS` / `MEMFS (session-only)` / `not mounted`), **Save
directory**, **Storage status** (`Ready` / `Syncing…`), **Last sync**
(`Success` / `Failure: <error>` with a timestamp).

- **Force Save Sync** -- calls `Re3Saves.forceSync()` directly; harmless no-op
  if already idle with nothing pending.
- **Clear Browser Saves…** -- gated behind a `confirm()` dialog (it deletes
  every file under `/game/userfiles` and syncs the empty state to
  IndexedDB); not required for normal play.

Log lines (all prefixed `[Save]`, one line per real event, no per-frame or
per-tick noise):

```
[Save] Persistent filesystem mounted at /game/userfiles
[Save] Restoring saves from IndexedDB…
[Save] Restore complete
[Save] Save written: userfiles\GTA3sf1.b
[Save] Syncing to IndexedDB…
[Save] Sync complete
[Save] ERROR: <message>
```

## 7. Startup and save-flow diagrams

**Startup:**

```
Browser launch
      |
      v
Mount asset IDBFS at /game, await restore (existing, web/vfs.js)
      |
      v
User clicks "Start engine"
      |
      v
Mount save IDBFS at /game/userfiles  (web/saves.js: FS.mount)
      |
      v
Restore from IndexedDB: mount.type.syncfs(mount, true, cb)  -- awaited
      |
      v
Save files (if any) now present on the virtual filesystem
      |
      v
chdir to /game, instance.callMain([])  -- native re3 startup begins
      |
      v
re3 reaches the frontend menu; PopulateSlotInfo() scans /game/userfiles
and finds exactly what was restored
```

**Saving:**

```
Player saves (pause menu -> Save Game, or a fresh game's autosave path)
      |
      v
C_PcSave::SaveSlot()  -- unmodified: CFileMgr::OpenFile/GenericSave/CloseFile
      |
      v
Save file written to the virtual filesystem (/game/userfiles/GTA3sf<N>.b)
      |
      v
re3_NotifySaveDirChangedJS("save", filename)  -- EM_JS call, PCSave.cpp
      |
      v
window.Re3Saves.onSaveDirChanged()  -- logs, calls requestSync()
      |
      v
IDBFS sync: mount.type.syncfs(mount, false, cb)
  (coalesced with any other pending save -- see part 5)
      |
      v
IndexedDB updated; diagnostics panel shows "Last sync: Success"
```

## 8. Native builds are unaffected

Every change is one of:

- `#ifdef __EMSCRIPTEN__` in `src/save/PCSave.cpp` (the notification hook and
  all `re3_Debug*` test exports),
- pure browser-side JS/HTML (`web/saves.js`, `web/index.html`,
  `web/launcher.js`), or
- a local dev-server change (`scripts/serve_web.py`, part 10) that ships to
  no build at all.

Nothing in `C_PcSave`, `GenericGameStorage`, or `CFileMgr` was modified.
Windows/Linux native builds compile and run identically to before this work.

## 9. Validation (actually performed, this session)

All five scenarios were run against a real build, with proof pulled from the
live page state -- not inferred from the code.

**1. Fresh state.** Cleared both IndexedDB databases
(`indexedDB.deleteDatabase` for `/game` and `/game/userfiles`), fresh page
load, mounted assets, clicked Start engine. Log showed
`[Save] Persistent filesystem mounted...` -> `Restoring...` -> `Restore
complete` with **zero** files in `/game/userfiles`, and the boot log's
earlier `Missing game asset: userfiles` line was gone (it only appeared
before the save mount existed at all) -- confirming the directory is ready
and empty, not missing.

**2. Save creation.** Reached `GS_PLAYING_GAME` (via a debug "close the
frontend menu" hook, `re3_DebugForceExitFrontend()`, added purely to drive
this test without blind UI automation -- see part 11), then called
`re3_DebugTriggerSave(0)`, which invokes the real, unmodified
`C_PcSave::SaveSlot(0)`. Result: `GTA3sf1.b` (201,820 bytes) present in
`instance.FS` at `/game/userfiles`, log showed the exact requested sequence
(`Save written: userfiles\GTA3sf1.b` -> `Syncing to IndexedDB…` -> `Sync
complete`), and `Re3Saves.getStatus().lastSyncResult === "success"`.

**3. Reload persistence.** Reloaded the page (no IndexedDB clear). Log order
after Start engine: `Persistent filesystem mounted` -> `Restoring saves from
IndexedDB…` -> `Restore complete` -> **then** `cwd set to /game, calling
main()`. `GTA3sf1.b` (still 201,820 bytes) was present in `instance.FS`
*before* `main()` ran. Reaching the frontend and calling
`re3_DebugScanSlot(0)` (the real `PopulateSlotInfo()`) returned `SLOT_OK`.

**4. Load game.** To get a test that actually distinguishes "loaded" from "a
coincidentally-similar fresh game", a distinctive value
(`CWorld::Players[0].m_nMoney = 8675309`, verified `0` beforehand) was set
after starting a new game, then saved. After a full reload,
`re3_DebugTriggerLoad(0)` was used -- **not** a hand-rolled loader: it sets
`m_nCurrSaveSlot` and `m_nCurrScreen = MENUPAGE_LOADING_IN_PROGRESS`, i.e.
exactly the state the frontend's own slot-confirm button leaves behind, and
lets the engine's completely unmodified `CMenuManager::Process()` ->
Emscripten restart handler -> `CGame::InitialiseWhenRestarting()` ->
`GenericLoad()` chain do the rest. (An earlier attempt that set
`m_bWantToRestart`/`m_bWantToLoad` directly from JS was found to be
unreliable: `CMenuManager::Process()` unconditionally resets
`m_bWantToRestart = false` at its own top on every call, so a flag set
between ticks from outside gets silently clobbered before the restart handler
ever sees it -- driving the same menu-screen state a real click reaches was
the fix.) Result: `re3_DebugGetPlayerMoney()` returned **`8675309`** --
byte-for-byte the saved value, round-tripped through
`GenericSave()` -> IndexedDB -> `GenericLoad()`.

**5. Multiple saves.** From a loaded game, fired three saves in immediate
succession with no gap for syncs to settle: slot 0 (money=1111), slot 1
(money=2222), slot 0 again (money=3333333). All three `SaveSlot()` calls
succeeded; the log showed 3 `Save written`/`Syncing` pairs but only **2**
`Sync complete` lines -- exactly the coalescing behavior part 5 describes (the
second and third requests arrived while the first sync was still in flight,
so they collapsed into one follow-up sync rather than three sequential ones).
After a full reload: both `GTA3sf1.b` and `GTA3sf2.b` were present;
`re3_DebugScanSlot()` returned `SLOT_OK` for both; loading slot 0 via the real
Load path (`re3_DebugTriggerLoad(0)`) returned money **`3333333`** -- the
final overwrite, not a stale intermediate value. No crashes, no corrupted
slots, no lost writes.

## 10. Incidental fix: dev server HTTP caching

Mid-session, `web/launcher.js` and `web/saves.js` edits appeared not to take
effect in the browser even after rebuilding and confirming (via `curl`) that
the server was returning the updated file. Root cause: `scripts/serve_web.py`
(Python's `http.server.SimpleHTTPRequestHandler`) sends a `Last-Modified`
header but no `Cache-Control`, so Chrome applies heuristic freshness caching
and can keep serving an old cached response for minutes without even
revalidating -- and the browser's HTTP cache is shared across tabs in the
same profile, so a "fresh" tab doesn't help either. Fixed by having the dev
server always send `Cache-Control: no-cache` (this repo's `web/` is served
locally for development; this header does not affect any production
deployment story, since there isn't one -- see `scripts/serve_web.py`'s own
docstring). `web/index.html`'s script tags also carry a `?v=N` query bump for
this session's already-cached entries, which predate the header fix.

## 11. Debug/test-only exports (off by default, kept for regression testing)

Following the pattern already established elsewhere in this build
(`re3_DebugSetForceForward` for input testing), five small exports were added
to make the above validation possible without blind mouse/keyboard UI
automation. None are called by the engine itself; all require an explicit
`ccall`/`cwrap` from JS:

| Export | File | Purpose |
|---|---|---|
| `re3_DebugForceExitFrontend()` | `src/skel/glfw/glfw.cpp` | Clears `m_bMenuActive`, the same flag "New Game" clears, to reach `GS_PLAYING_GAME` without driving the title screen. |
| `re3_DebugTriggerSave(slot)` | `src/save/PCSave.cpp` | Calls the real `PcSaveHelper.SaveSlot(slot)` (only when `gGameState == GS_PLAYING_GAME`). |
| `re3_DebugTriggerLoad(slot)` | `src/save/PCSave.cpp` | Sets `m_nCurrSaveSlot`/`m_nCurrScreen` to reproduce a real "Load Game" click; the engine's own code does the rest. |
| `re3_DebugScanSlot(slot)` | `src/save/PCSave.cpp` | Calls the real `PopulateSlotInfo()` and returns `Slots[slot+1]`. |
| `re3_DebugGetPlayerMoney()` / `re3_DebugSetPlayerMoney(n)` | `src/save/PCSave.cpp` | Read/write `CWorld::Players[0].m_nMoney` -- the distinctive value used in scenario 4/5's round-trip checks. |

## Clearing saves / inspecting them during development

- **From the UI**: sidebar "Save system" -> **Clear Browser Saves…** (asks
  for confirmation first).
- **From devtools**: `await window.Re3Saves.clearSaves()`, or inspect
  directly:
  ```js
  window.re3Module.FS.readdir('/game/userfiles')
  window.re3Module.FS.readFile('/game/userfiles/GTA3sf1.b')
  await indexedDB.databases()               // lists both the asset and save DBs
  indexedDB.deleteDatabase('/game/userfiles') // wipe persisted saves entirely
  ```
- **Status snapshot**: `window.Re3Saves.getStatus()` -> `{ backend, saveDir,
  state, lastSyncResult, lastSyncError, lastSyncAt }`.
