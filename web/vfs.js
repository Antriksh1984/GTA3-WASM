// AssetVFS: browser-compatible filesystem abstraction for re3's game-asset root.
//
// re3 itself needs *zero* changes to work with this: the engine already resolves
// every asset path relative to its current working directory (CFileMgr::Initialise
// captures getcwd() once at startup; see src/core/FileMgr.cpp and
// src/skel/crossplatform.cpp's casepath()). So the whole abstraction here is: get
// real files into Emscripten's virtual filesystem under a configurable mount point,
// then FS.chdir() there before main() runs. That's the clean boundary called for in
// docs/BROWSER_RUNTIME.md's Phase 2 section, carried into Phase 3 -- this file knows
// nothing about re3/librw internals, only about Emscripten's FS API and the shape of
// web/asset-manifest.json.
//
// This module never contains, downloads, or ships any GTA III asset data itself. It
// only moves bytes the *user* already has (via a folder picker or their own local
// dev server) into the running module's virtual filesystem.

(() => {
	"use strict";

	const DEFAULT_MOUNT_POINT = "/game";

	// COLFILE <level-number> <path> is the one gta3.dat keyword whose path isn't
	// the first token after the keyword -- keep this in sync with
	// scripts/validate_assets.py's GTA3_DAT_PATH_KEYWORDS/parse_gta3_dat, and with
	// src/core/FileLoader.cpp's LoadLevel() if that ever changes.
	const GTA3_DAT_PATH_KEYWORDS = new Set(["IDE", "IPL", "MODELFILE", "HIERFILE", "TEXDICTION", "CDIMAGE"]);

	function normalize(path) {
		return path.replace(/\\/g, "/").replace(/^\/+/, "");
	}

	function joinPath(mountPoint, relPath) {
		return `${mountPoint.replace(/\/+$/, "")}/${normalize(relPath)}`;
	}

	// --- low-level FS helpers ------------------------------------------------

	function ensureMountPoint(FS, mountPoint) {
		try {
			FS.mkdirTree(mountPoint);
		} catch (e) {
			if (e?.errno !== 20 /* EEXIST-ish, Emscripten FS uses its own ERRNO_CODES */) {
				// mkdirTree throws if the leaf already exists as a directory in some
				// versions; only re-throw if it's something else going on.
				if (!(FS.analyzePath(mountPoint).exists)) throw e;
			}
		}
	}

	function writeFileDeep(FS, absPath, data) {
		const dir = absPath.slice(0, absPath.lastIndexOf("/"));
		if (dir) FS.mkdirTree(dir);
		FS.writeFile(absPath, data);
	}

	// --- mounting strategies ---------------------------------------------------

	/** Bare MEMFS directory at mountPoint, nothing persisted. Always safe, always
	 * available; the baseline every other strategy builds on. */
	function mountEmpty(FS, mountPoint) {
		ensureMountPoint(FS, mountPoint);
		return mountPoint;
	}

	/** Mount IDBFS at mountPoint for persistence across page loads (task 6C).
	 * Must be called before any files are written there. Returns the mount point;
	 * call loadFromIDB() afterwards to pull in whatever was persisted last time. */
	function mountIDBFS(FS, mountPoint) {
		if (!FS.filesystems || !FS.filesystems.IDBFS) {
			throw new Error("IDBFS is not available in this build (missing -lidbfs.js?)");
		}
		ensureMountPoint(FS, mountPoint);
		FS.mount(FS.filesystems.IDBFS, {}, mountPoint);
		return mountPoint;
	}

	function syncfs(FS, populate) {
		return new Promise((resolve, reject) => {
			FS.syncfs(populate, (err) => (err ? reject(err) : resolve()));
		});
	}

	/** Pull persisted files from IndexedDB into the mounted IDBFS directory. */
	function loadFromIDB(FS, _mountPoint) {
		return syncfs(FS, true);
	}

	/** Push the current contents of the mounted IDBFS directory into IndexedDB. */
	function persistToIDB(FS, _mountPoint) {
		return syncfs(FS, false);
	}

	/**
	 * Task 6B: mount from a user-provided asset directory, e.g. the FileList from
	 * <input type="file" webkitdirectory multiple> or a drag-and-drop of a folder.
	 * Each File must carry a relative path (webkitRelativePath, or `.relativePath`
	 * if the caller set one up itself via the File System Access API).
	 *
	 * This is the only place actual GTA III bytes flow through this file, and they
	 * flow from the user's own machine straight into the in-memory/IDBFS-backed
	 * VFS -- never through a server, never persisted anywhere but the user's own
	 * browser storage.
	 */
	async function mountFromFileList(FS, mountPoint, fileList, { onProgress } = {}) {
		ensureMountPoint(FS, mountPoint);
		const files = Array.from(fileList);
		let done = 0;
		for (const file of files) {
			const rel = file.relativePath || file.webkitRelativePath || file.name;
			// webkitRelativePath includes the picked folder's own name as the first
			// segment (e.g. "gamefiles/data/gta3.dat"); strip it so paths line up
			// with the manifest, which is relative to the *contents* of that folder.
			const trimmed = rel.includes("/") ? rel.slice(rel.indexOf("/") + 1) : rel;
			if (!trimmed) continue;
			const buf = new Uint8Array(await file.arrayBuffer());
			writeFileDeep(FS, joinPath(mountPoint, trimmed), buf);
			done++;
			onProgress?.(done, files.length, trimmed);
		}
		return done;
	}

	/**
	 * Task 8: development-only asset mounting. Fetches a file listing + raw bytes
	 * from the *local dev server* (scripts/serve_web.py --dev-assets <dir>), never
	 * from anywhere else. This code path only runs when explicitly requested (see
	 * launcher.js: the ?devAssets=1 query flag), and scripts/serve_web.py refuses
	 * to serve --dev-assets at all unless you pass that flag on the command line
	 * too -- so there are two separate opt-ins before any local game files ever
	 * touch the network, and even then only to localhost. Nothing under this path
	 * is reachable in a normal (non-dev) run of the page, and web/build/ (the
	 * copied build output) never contains asset bytes.
	 */
	async function mountFromDevServer(FS, mountPoint, { baseUrl = "/__dev_assets", onProgress } = {}) {
		const listResp = await fetch(`${baseUrl}/manifest.json`);
		if (!listResp.ok) {
			throw new Error(
				`Dev asset server did not respond at ${baseUrl}/manifest.json (${listResp.status}). ` +
				`Is scripts/serve_web.py running with --dev-assets <path-to-your-gamefiles>?`
			);
		}
		const files = await listResp.json(); // array of relative paths
		ensureMountPoint(FS, mountPoint);
		let done = 0;
		for (const rel of files) {
			const resp = await fetch(`${baseUrl}/file/${rel.split("/").map(encodeURIComponent).join("/")}`);
			if (!resp.ok) throw new Error(`Failed to fetch dev asset "${rel}": HTTP ${resp.status}`);
			const buf = new Uint8Array(await resp.arrayBuffer());
			writeFileDeep(FS, joinPath(mountPoint, rel), buf);
			done++;
			onProgress?.(done, files.length, rel);
		}
		return done;
	}

	/**
	 * Task 7: packaged filesystem. Loads a pre-built manifest (array of relative
	 * paths) + fetches each file from `baseUrl` (relative to the page), for a
	 * "package once, mount every load" flow -- e.g. assets copied into
	 * web/build/package/ by a separate, explicit packaging step (never part of the
	 * default `cmake --build` -- see docs/GAME_ASSETS.md). Structurally identical
	 * to mountFromDevServer, kept separate because its intent (a prepared package
	 * vs. a developer's live local folder) and trust boundary are different.
	 */
	async function mountFromPackage(FS, mountPoint, { baseUrl = "build/package", onProgress } = {}) {
		const listResp = await fetch(`${baseUrl}/manifest.json`);
		if (!listResp.ok) {
			throw new Error(`No packaged asset manifest at ${baseUrl}/manifest.json (HTTP ${listResp.status})`);
		}
		const files = await listResp.json();
		ensureMountPoint(FS, mountPoint);
		let done = 0;
		for (const rel of files) {
			const resp = await fetch(`${baseUrl}/${rel.split("/").map(encodeURIComponent).join("/")}`);
			if (!resp.ok) throw new Error(`Failed to fetch packaged asset "${rel}": HTTP ${resp.status}`);
			const buf = new Uint8Array(await resp.arrayBuffer());
			writeFileDeep(FS, joinPath(mountPoint, rel), buf);
			done++;
			onProgress?.(done, files.length, rel);
		}
		return done;
	}

	/** Point the engine's cwd at the mounted asset root. Call this once, after
	 * mounting/populating is complete, before instance.callMain(). */
	function chdirToRoot(FS, mountPoint) {
		FS.chdir(mountPoint);
	}

	// --- validation (tasks 9 & 10, browser-side counterpart to
	// scripts/validate_assets.py) --------------------------------------------

	function readTextFile(FS, absPath) {
		try {
			return FS.readFile(absPath, { encoding: "utf8" });
		} catch {
			return null;
		}
	}

	/**
	 * re3 resolves paths case-insensitively at runtime (casepath() in
	 * src/skel/crossplatform.cpp does a manual, case-insensitive directory scan),
	 * but Emscripten's MEMFS/IDBFS are plain case-sensitive filesystems. A GTA III
	 * install's on-disk casing varies (installers, mixed-case CD dumps, etc.), and
	 * the manifest/gta3.dat paths are written in whatever case the original Windows
	 * game files used -- so a naive case-sensitive FS.analyzePath() check produces
	 * false "missing" reports for files that are actually there and that the engine
	 * would find just fine. Build a lowercased-path index once (same approach as
	 * scripts/validate_assets.py's build_case_insensitive_index) and validate
	 * against that instead, mapping back to the real on-disk relative path so
	 * callers (e.g. reading data/gta3.dat itself) can open the file that's
	 * actually there.
	 */
	function buildCaseInsensitiveIndex(FS, mountPoint) {
		const index = new Map(); // lowercased relative path -> real relative path
		function walk(absDir, relDir) {
			let entries;
			try {
				entries = FS.readdir(absDir);
			} catch {
				return;
			}
			for (const name of entries) {
				if (name === "." || name === "..") continue;
				const absPath = `${absDir}/${name}`;
				const relPath = relDir ? `${relDir}/${name}` : name;
				index.set(relPath.toLowerCase(), relPath);
				let st;
				try {
					st = FS.stat(absPath);
				} catch {
					continue;
				}
				if (FS.isDir(st.mode)) walk(absPath, relPath);
			}
		}
		walk(mountPoint, "");
		return index;
	}

	/** Returns the real on-disk relative path for a case-insensitive match, or null. */
	function resolveCI(index, relPath) {
		return index.get(normalize(relPath).toLowerCase()) ?? null;
	}

	/** Mirrors scripts/validate_assets.py's parse_gta3_dat(). */
	function parseGta3Dat(text) {
		const paths = [];
		for (let line of text.split("\n")) {
			line = line.trim();
			if (!line || line.startsWith("#")) continue;
			const firstSpace = line.search(/\s/);
			if (firstSpace === -1) continue;
			const keyword = line.slice(0, firstSpace).toUpperCase();
			const rest = line.slice(firstSpace + 1).trim();
			if (keyword === "COLFILE") {
				const fields = rest.split(/\s+/);
				if (fields.length >= 2) paths.push(normalize(fields[1]));
			} else if (GTA3_DAT_PATH_KEYWORDS.has(keyword)) {
				paths.push(normalize(rest.split(/\s+/)[0]));
			}
		}
		return paths;
	}

	/**
	 * Checks the manifest's required/optional game assets (and, if data/gta3.dat
	 * is present under mountPoint, every path it references) against what's
	 * actually mounted. Returns missing paths as "Missing game asset: <path>"
	 * strings -- the same phrasing scripts/validate_assets.py uses -- ready to
	 * hand straight to the log panel, never a bare "File failed".
	 */
	function validate(FS, mountPoint, manifest) {
		const result = {
			missingRequired: [],
			missingOptional: [],
			presentRequired: [],
			presentOptional: [],
			gta3DatChecked: false,
			missingFromGta3Dat: [],
		};

		const index = buildCaseInsensitiveIndex(FS, mountPoint);

		for (const entry of manifest.gameAssets.required) {
			(resolveCI(index, entry.path) ? result.presentRequired : result.missingRequired).push(entry.path);
		}
		for (const entry of manifest.gameAssets.optional) {
			(resolveCI(index, entry.path) ? result.presentOptional : result.missingOptional).push(entry.path);
		}

		const gta3DatReal = resolveCI(index, "data/gta3.dat");
		const datText = gta3DatReal ? readTextFile(FS, joinPath(mountPoint, gta3DatReal)) : null;
		if (datText != null) {
			result.gta3DatChecked = true;
			for (const path of parseGta3Dat(datText)) {
				if (!resolveCI(index, path)) result.missingFromGta3Dat.push(path);
			}
		}

		result.ok = result.missingRequired.length === 0 && result.missingFromGta3Dat.length === 0;
		result.messages = [
			...result.missingRequired.map((p) => `Missing game asset: ${p}`),
			...result.missingFromGta3Dat.map((p) => `Missing game asset: ${p} (referenced by data/gta3.dat)`),
		];
		return result;
	}

	window.AssetVFS = {
		DEFAULT_MOUNT_POINT,
		mountEmpty,
		mountIDBFS,
		loadFromIDB,
		persistToIDB,
		mountFromFileList,
		mountFromDevServer,
		mountFromPackage,
		chdirToRoot,
		validate,
	};
})();
