// Browser main-loop / frame-timing layer for re3 (Phase 7, see
// docs/BROWSER_MAIN_LOOP.md).
//
// re3's own main loop (src/skel/glfw/glfw.cpp) already drives itself through
// emscripten_set_main_loop(tick, 0, 1) -- non-blocking, requestAnimationFrame-paced,
// set up before this phase. Nothing here reimplements or wraps that loop; this file
// only does two things neither C++ nor the engine's own game-logic code can do from
// inside the browser sandbox:
//
//   - react to the Page Visibility API (task 9): rAF, and therefore the engine's
//     entire tick() loop, simply stops being called while the tab is hidden --
//     nothing spins or races, but the *next* CTimer::Update() once the tab comes
//     back would otherwise measure the full hidden duration as one frame's elapsed
//     time. re3_OnBrowserTabHidden()/re3_OnBrowserTabVisible() (src/skel/glfw/
//     glfw.cpp) wrap the engine's own CTimer::Suspend()/Resume() -- already used
//     natively for exactly this "this real time gap must not count" purpose (see
//     the comment above those exports) -- called here in matched pairs so a
//     mis-ordered or repeated visibilitychange event can never leave CTimer's
//     suspend depth unbalanced.
//   - surface frame timing (task 10) that only exists as raw doubles on the C++
//     side (re3_GetFPS/GetFrameTimeMs/GetSimTimeMs/GetRenderTimeMs, updated once
//     per rendered frame at the end of Idle()) into the page's diagnostics panel.

(() => {
	"use strict";

	function setupTabThrottling(instance, log) {
		let armed = false; // mirrors Re3Input.setupFocusLossRecovery's own gate --
		// don't touch CTimer before the engine has actually started running.
		let suspended = false; // tracks *our own* belief about CTimer's suspend
		// state so re3_OnBrowserTabHidden/Visible are always called in matched
		// pairs, however many visibilitychange events the browser fires.

		let onHidden, onVisible;
		try {
			onHidden = instance.cwrap("re3_OnBrowserTabHidden", null, []);
			onVisible = instance.cwrap("re3_OnBrowserTabVisible", null, []);
		} catch {
			return { arm() {} }; // export not present (e.g. an older build); no-op
		}

		document.addEventListener("visibilitychange", () => {
			if (!armed) return;
			if (document.hidden && !suspended) {
				suspended = true;
				onHidden();
				log?.("[perf] tab hidden -- CTimer::Suspend() (elapsed time won't count as a frame)", "info");
			} else if (!document.hidden && suspended) {
				suspended = false;
				onVisible();
				log?.("[perf] tab visible again -- CTimer::Resume()", "info");
			}
		});

		return {
			arm() { armed = true; },
		};
	}

	// Polls via setTimeout, not requestAnimationFrame: this is a text-content
	// update, not a paint-aligned animation, so it doesn't need vsync timing --
	// and, discovered validating Phase 9 (docs/FIRST_BOOT.md), rAF-based polling
	// is suspended by the exact same browser/host states (a hidden tab, a pane
	// that isn't being composited) that motivated the "Debug mode" main-loop
	// timing toggle in the first place. Using setTimeout here means this panel
	// keeps working in those states even without Debug mode enabled.
	function setupFrameTimingPanel(instance, table, badgeEl) {
		let getFPS, getFrameTime, getSimTime, getRenderTime;
		try {
			getFPS = instance.cwrap("re3_GetFPS", "number", []);
			getFrameTime = instance.cwrap("re3_GetFrameTimeMs", "number", []);
			getSimTime = instance.cwrap("re3_GetSimTimeMs", "number", []);
			getRenderTime = instance.cwrap("re3_GetRenderTimeMs", "number", []);
		} catch {
			if (badgeEl) badgeEl.textContent = "FPS: n/a";
			return; // export not present; leave the panel showing its static placeholder
		}

		const rows = new Map();
		function setRow(key, value) {
			let row = rows.get(key);
			if (!row) {
				row = document.createElement("tr");
				const k = document.createElement("td");
				k.className = "key";
				k.textContent = key;
				const v = document.createElement("td");
				v.className = "val";
				row.append(k, v);
				table.appendChild(row);
				rows.set(key, row);
			}
			row.children[1].textContent = value;
		}

		function poll() {
			const fps = getFPS();
			const frameMs = getFrameTime();
			const simMs = getSimTime();
			const renderMs = getRenderTime();
			if (frameMs > 0) {
				setRow("FPS", fps.toFixed(1));
				setRow("Frame time", `${frameMs.toFixed(2)} ms`);
				setRow("Simulation time", `${simMs.toFixed(2)} ms`);
				setRow("Render time", `${renderMs.toFixed(2)} ms`);
				if (badgeEl) badgeEl.textContent = `FPS: ${fps.toFixed(0)}`;
			} else {
				setRow("FPS", "(not in-game yet)");
				if (badgeEl) badgeEl.textContent = "FPS: -";
			}
			setTimeout(poll, 250);
		}
		setTimeout(poll, 250);
	}

	window.Re3Perf = {
		setupTabThrottling,
		setupFrameTimingPanel,
	};
})();
