// Persistent save storage for re3 (browser build). See docs/SAVES_WASM.md.
//
// re3's own save system (src/save/PCSave.cpp, src/save/GenericGameStorage.cpp)
// is completely unmodified in *what* it does: it still opens/reads/writes
// plain files with CFileMgr, under a directory it computes itself
// (_psGetUserFilesFolder() -> "userfiles", src/skel/glfw/glfw.cpp). This file
// only makes that one directory persist across page reloads, reusing the
// exact same IDBFS/IndexedDB mechanism web/vfs.js already uses for game
// assets -- but as its OWN, separate IDBFS mount, so a save (tens of KB)
// never needs to sync the entire multi-hundred-MB asset tree, and vice versa.
//
// Two entry points:
//   - mountAndRestore(instance, assetMountPoint, logFn): called once from
//     web/launcher.js's runEngineMain(), BEFORE re3's own callMain() runs.
//     Mounts IDBFS at <assetMountPoint>/userfiles and restores whatever was
//     persisted last time (FS.syncfs(true)) -- awaited, so re3's own startup
//     save-slot scan (C_PcSave::PopulateSlotInfo(), called from the frontend
//     menu once the engine is running) never sees a directory the browser
//     hasn't finished restoring into yet.
//   - window.Re3Saves.onSaveDirChanged(reason): called directly from C++
//     (src/save/PCSave.cpp's re3_NotifySaveDirChangedJS, right after
//     SaveSlot()/DeleteSlot() finish touching a file) to queue a persist
//     (FS.syncfs(false)) back to IndexedDB.

(() => {
	"use strict";

	const SAVE_SUBDIR = "userfiles";

	let FS = null;
	let saveDir = null;
	let saveMount = null; // the IDBFS mount descriptor for saveDir specifically (FS.mount()'s return value's .mount)
	let logFn = () => {};

	// --- sync state machine ---------------------------------------------------
	// IDLE -> (change) -> SYNCING -> (another change arrived while syncing?)
	//   no  -> IDLE
	//   yes -> sync again, then re-evaluate
	// This guarantees FS.syncfs(false) calls never overlap, and that the
	// *last* change made before the sync queue drains is always the one that
	// ends up persisted -- never silently dropped.
	const IDLE = "idle";
	const SYNCING = "syncing";
	let state = IDLE;
	let pending = false;
	let lastSyncResult = null; // "success" | "failure" | null (never synced yet)
	let lastSyncError = null;
	let lastSyncAt = null;
	let onStateChange = () => {};

	function setState(next) {
		state = next;
		try { onStateChange(); } catch { /* diagnostics-panel errors must not break saving */ }
	}

	// Deliberately NOT the top-level FS.syncfs(): that call walks *every* mounted
	// filesystem from root (FS.getMounts(FS.root.mount)), and saveDir is mounted
	// *inside* the asset root (assetMountPoint/userfiles is a child of the /game
	// IDBFS mount, per the game's own relative-path expectations -- see
	// mountAndRestore() below). Emscripten's IDBFS.getLocalSet() walks its whole
	// subtree including nested mount points, so a global FS.syncfs() call ends up
	// asking the *asset* mount to reconcile a child directory that is actually a
	// different IDBFS mount underneath it -- and when that diff says "this local
	// entry isn't in my remote set, remove it", it calls FS.rmdir() on a live
	// mountpoint, which throws (ErrnoError errno 10). Calling the save mount's own
	// `type.syncfs` directly scopes the operation to exactly this one mount, so it
	// can never touch (or be touched by) the asset tree's own sync.
	function syncfsAsync(populate) {
		return new Promise((resolve, reject) => {
			saveMount.type.syncfs(saveMount, populate, (err) => (err ? reject(err) : resolve()));
		});
	}

	async function runSync() {
		setState(SYNCING);
		try {
			await syncfsAsync(false);
			lastSyncResult = "success";
			lastSyncError = null;
			logFn("[Save] Sync complete", "info");
		} catch (err) {
			lastSyncResult = "failure";
			lastSyncError = String((err && err.message) || err);
			logFn(`[Save] ERROR: IndexedDB sync failed: ${lastSyncError}`, "stderr");
		}
		lastSyncAt = new Date();
		if (pending) {
			pending = false;
			await runSync(); // a newer change arrived mid-sync -- run once more
			return;
		}
		setState(IDLE);
	}

	function requestSync() {
		if (!FS || !saveDir) return; // not mounted (never mounted, or IDBFS unavailable)
		if (state === SYNCING) {
			pending = true; // coalesce -- never start a second overlapping syncfs()
			return;
		}
		runSync();
	}

	/** Called directly from C++ (src/save/PCSave.cpp) right after a save/delete. */
	function onSaveDirChanged(reason, filename) {
		logFn(reason === "delete" ? `[Save] Save deleted: ${filename}` : `[Save] Save written: ${filename}`, "info");
		logFn("[Save] Syncing to IndexedDB…", "info");
		requestSync();
	}

	/**
	 * Mount a dedicated IDBFS at <assetMountPoint>/userfiles and restore
	 * whatever was persisted last session. Must be awaited by the caller --
	 * FS.syncfs() is asynchronous, and re3's startup save-slot scan must never
	 * run before this resolves. On any failure (IDBFS unavailable, e.g. some
	 * private-browsing modes), falls back to a plain in-memory directory: the
	 * game can still save normally for the current session, it just won't
	 * persist across a reload.
	 */
	async function mountAndRestore(instance, assetMountPoint, log) {
		logFn = log || logFn;
		FS = instance.FS;
		saveDir = `${assetMountPoint.replace(/\/+$/, "")}/${SAVE_SUBDIR}`;

		try {
			FS.mkdirTree(saveDir);
		} catch (e) {
			if (!FS.analyzePath(saveDir).exists) throw e;
		}

		try {
			if (!FS.filesystems || !FS.filesystems.IDBFS) {
				throw new Error("IDBFS is not available in this build (missing -lidbfs.js?)");
			}
			const mountRoot = FS.mount(FS.filesystems.IDBFS, {}, saveDir);
			saveMount = mountRoot.mount;
			logFn(`[Save] Persistent filesystem mounted at ${saveDir}`, "info");
		} catch (err) {
			logFn(`[Save] ERROR: IDBFS unavailable (${err}); saves will not persist across reloads this session`, "stderr");
			saveDir = null; // disables further sync attempts; the plain MEMFS
			// directory from mkdirTree() above still lets the game save normally
			onStateChange(); // the diagnostics panel's own render() only runs on
			// setState() transitions, and mounting/restoring here happens entirely
			// outside that state machine (it's a one-time setup step, not a save
			// sync) -- nudge it once so "Backend"/"Save directory" reflect reality
			// instead of showing whatever setupSaveDiagnosticsPanel() saw at its own
			// (pre-mount) initial render.
			return;
		}

		logFn("[Save] Restoring saves from IndexedDB…", "info");
		try {
			await syncfsAsync(true);
			logFn("[Save] Restore complete", "info");
		} catch (err) {
			logFn(`[Save] ERROR: could not restore saves from IndexedDB: ${err}`, "stderr");
		}
		onStateChange(); // see comment in the failure branch above
	}

	/** Developer control: sync now instead of waiting for the next save. */
	function forceSync() {
		if (!FS || !saveDir) return;
		requestSync();
	}

	/** Developer control: destructive -- caller (launcher.js) must confirm first. */
	async function clearSaves() {
		if (!FS || !saveDir) throw new Error("save directory not mounted");
		for (const name of FS.readdir(saveDir)) {
			if (name === "." || name === "..") continue;
			try {
				FS.unlink(`${saveDir}/${name}`);
			} catch {
				/* not a plain file (shouldn't happen under userfiles/) -- skip it */
			}
		}
		await syncfsAsync(false);
		logFn("[Save] Browser saves cleared", "info");
	}

	function getStatus() {
		return {
			backend: FS && saveDir ? "IDBFS" : (FS ? "MEMFS (session-only)" : "not mounted"),
			saveDir,
			state,
			lastSyncResult,
			lastSyncError,
			lastSyncAt,
		};
	}

	function setOnStateChange(fn) {
		onStateChange = fn || (() => {});
	}

	window.Re3Saves = {
		mountAndRestore,
		onSaveDirChanged,
		forceSync,
		clearSaves,
		getStatus,
		setOnStateChange,
	};
})();
