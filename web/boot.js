// Boot-progress, debug mode, and WebGL error capture for re3 (Phase 9, see
// docs/FIRST_BOOT.md).
//
// Three independent pieces, all optional/best-effort (never block boot if an
// export is missing from an older build):
//   - setupBootProgress(): renders the [1..7] stage list task 11 asks for.
//     Stages 1-2 (WASM loaded, filesystem initialized) are pure JS-side
//     concerns tracked by launcher.js itself; stages 3-7 come from the C++
//     side (re3_GetBootStage(), src/skel/glfw/glfw.cpp) once the engine
//     starts running, polled the same way web/perf.js polls frame timing.
//   - setupDebugMode(): wires the topbar "Debug mode" checkbox to
//     re3_SetDebugTiming() -- reschedules the main loop onto setTimeout
//     instead of requestAnimationFrame, for environments (a backgrounded tab,
//     or a host that isn't compositing the page at all) where rAF never
//     fires. Off by default; this trades away vsync alignment, so it's not
//     what a normal player wants.
//   - setupWebGLErrorCapture(): the browser-level half of "WebGL errors"
//     (task 7) that a C++ glGetError() check can't see -- context loss,
//     context creation failure.

(() => {
	"use strict";

	function setupBootProgress(listEl) {
		const items = {};
		listEl.querySelectorAll("li[data-stage]").forEach((li) => {
			items[li.dataset.stage] = li;
		});
		let maxStage = 0;

		function applyStage(n) {
			if (n < maxStage) return; // never go backwards
			maxStage = n;
			for (let i = 1; i <= 7; i++) {
				const li = items[i];
				if (!li) continue;
				if (i < n) li.dataset.state = "done";
				else if (i === n) li.dataset.state = n >= 7 ? "done" : "active";
				else delete li.dataset.state;
			}
		}

		function markFailed(n) {
			const li = items[n];
			if (li) li.dataset.state = "failed";
		}

		// Stages 1-2: called directly by launcher.js at the JS-side milestones
		// that correspond to them (module instantiated, assets validated).
		function setStage(n) {
			applyStage(n);
		}

		// Stages 3-7: polled from the C++ side once the engine is running. Uses
		// setTimeout, not requestAnimationFrame -- see web/perf.js's
		// setupFrameTimingPanel for why (same reasoning applies here).
		function pollFromEngine(instance) {
			let getStage;
			try {
				getStage = instance.cwrap("re3_GetBootStage", "number", []);
			} catch {
				return; // export not present (e.g. an older build); leave stages 1-2 as-is
			}
			function poll() {
				applyStage(getStage());
				setTimeout(poll, 250);
			}
			setTimeout(poll, 250);
		}

		return { setStage, markFailed, pollFromEngine };
	}

	function setupDebugMode(checkbox, instance, log) {
		let setDebugTiming;
		try {
			setDebugTiming = instance.cwrap("re3_SetDebugTiming", null, ["number"]);
		} catch {
			checkbox.disabled = true;
			checkbox.title = "re3_SetDebugTiming export not present in this build";
			return;
		}
		checkbox.addEventListener("change", () => {
			setDebugTiming(checkbox.checked ? 1 : 0);
			log(
				`[debug] main loop timing -> ${checkbox.checked ? "setTimeout(16ms), bypasses requestAnimationFrame" : "requestAnimationFrame (default)"}`,
				"info"
			);
		});
	}

	function setupWebGLErrorCapture(canvas, log) {
		canvas.addEventListener("webglcontextlost", (event) => {
			event.preventDefault(); // allow context restoration attempts
			log(`[webgl] context lost${event.statusMessage ? `: ${event.statusMessage}` : ""}`, "stderr");
		});
		canvas.addEventListener("webglcontextrestored", () => {
			log("[webgl] context restored", "info");
		});
		canvas.addEventListener("webglcontextcreationerror", (event) => {
			log(`[webgl] context creation error${event.statusMessage ? `: ${event.statusMessage}` : ""}`, "stderr");
		});
	}

	window.Re3Boot = {
		setupBootProgress,
		setupDebugMode,
		setupWebGLErrorCapture,
	};
})();
