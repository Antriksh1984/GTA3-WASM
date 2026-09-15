// Minimal launcher for web/rendertest.html (Phase 4 rendering diagnostic).
//
// Deliberately much simpler than web/launcher.js: this module has no game assets,
// no threading (rendertest/CMakeLists.txt doesn't set USE_PTHREADS), and nothing to
// validate except "did the WebGL2 context come up and did the test scene render
// correctly" -- which rendertest/main.cpp itself checks via framebuffer readback and
// reports as PASS/FAIL lines through stdout. This file's job is just to get those
// lines on screen and fail visibly if the module itself won't load.

(() => {
	"use strict";

	const els = {
		statusDot: document.getElementById("status-dot"),
		statusText: document.getElementById("status-text"),
		loadingOverlay: document.getElementById("loading-overlay"),
		loadingStatus: document.getElementById("loading-status"),
		errorBanner: document.getElementById("error-banner"),
		errorDetail: document.getElementById("error-detail"),
		canvas: document.getElementById("canvas"),
		logOutput: document.getElementById("log-output"),
		diagnosticsBtn: document.getElementById("diagnostics-btn"),
		sidepanel: document.getElementById("sidepanel"),
	};

	function setStatus(state, text) {
		els.statusDot.dataset.state = state;
		els.statusText.textContent = text;
	}

	function log(text, cls) {
		const line = document.createElement("div");
		line.className = "log-line" + (cls ? ` ${cls}` : "");
		line.textContent = text;
		els.logOutput.appendChild(line);
		els.logOutput.parentElement.scrollTop = els.logOutput.parentElement.scrollHeight;
	}

	function showFatalError(title, err) {
		const detail = err instanceof Error ? (err.stack || err.message) : String(err);
		els.errorDetail.textContent = detail;
		els.errorBanner.querySelector(".error-title").textContent = title;
		els.errorBanner.classList.add("visible");
		els.loadingOverlay.classList.add("hidden");
		setStatus("error", title);
		log(`[fatal] ${title}: ${detail}`, "stderr");
		console.error(title, err);
	}

	els.diagnosticsBtn.addEventListener("click", () => els.sidepanel.classList.toggle("open"));

	// Must match rendertest/main.cpp's Init() (sk::globals.width/height) -- GLFW
	// captures the canvas's *current* backing size at window-creation time, so if
	// that size doesn't match what the C++ side believes it created (960x540), the
	// GL viewport and the Raster/Camera dimensions disagree and reads/draws land in
	// the wrong place. Rather than derive the initial size from CSS layout (which
	// may not have run yet this early -- e.g. a still-hidden/prerendering tab -- and
	// would otherwise size the canvas to 0 or 1x1), set it explicitly up front and
	// only switch to layout-driven sizing for *subsequent* resizes.
	const DEFAULT_WIDTH = 960, DEFAULT_HEIGHT = 540;
	els.canvas.width = DEFAULT_WIDTH;
	els.canvas.height = DEFAULT_HEIGHT;

	function resizeCanvas() {
		const dpr = window.devicePixelRatio || 1;
		const rect = els.canvas.parentElement.getBoundingClientRect();
		if (rect.width < 1 || rect.height < 1) return; // layout not ready; keep current size
		els.canvas.width = Math.max(1, Math.round(rect.width * dpr));
		els.canvas.height = Math.max(1, Math.round(rect.height * dpr));
	}
	window.addEventListener("resize", resizeCanvas);
	if (typeof ResizeObserver !== "undefined") {
		new ResizeObserver(resizeCanvas).observe(els.canvas.parentElement);
	}

	let sawFailure = false;

	async function main() {
		if (typeof createRenderTestModule !== "function") {
			showFatalError(
				"build/rendertest.js did not load",
				new Error("createRenderTestModule() is not defined. Build the target first: " +
					"cmake --build build-wasm --target rendertest")
			);
			return;
		}

		setStatus("loading", "loading WebGL2 rendering module…");

		let instance;
		try {
			instance = await createRenderTestModule({
				canvas: els.canvas,
				print: (text) => {
					log(text, text.includes("FAIL") ? "stderr" : "info");
					if (text.includes("FAIL")) sawFailure = true;
				},
				printErr: (text) => log(text, "stderr"),
				onAbort: (reason) => showFatalError("WebGL rendering module aborted", reason),
			});
		} catch (err) {
			showFatalError("Failed to instantiate the rendering module", err);
			return;
		}

		els.loadingOverlay.classList.add("hidden");
		log("[module] instantiated, starting render diagnostic…", "info");

		try {
			instance.callMain([]);
			// rendertest/main.cpp runs its validation on the first rendered frame and
			// keeps rendering afterwards (emscripten_set_main_loop) -- give it a beat.
			setTimeout(() => {
				setStatus(sawFailure ? "error" : "ok", sawFailure ? "Some checks failed -- see log" : "All rendering checks passed");
			}, 500);
		} catch (err) {
			showFatalError("Rendering diagnostic crashed", err);
		}
	}

	window.addEventListener("error", (event) => {
		if (els.errorBanner.classList.contains("visible")) return;
		showFatalError("Unhandled script error", event.error || event.message);
	});
	window.addEventListener("unhandledrejection", (event) => {
		if (els.errorBanner.classList.contains("visible")) return;
		showFatalError("Unhandled promise rejection", event.reason);
	});

	main().catch((err) => showFatalError("Launcher failed unexpectedly", err));
})();
