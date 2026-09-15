// Browser input layer for re3 (Phase 5, see docs/BROWSER_INPUT.md).
//
// re3's own input abstraction (src/core/Pad.h/.cpp, src/core/ControllerConfig.cpp,
// src/skel/events.cpp) is untouched by this file and is not even aware it exists.
// Keyboard and mouse already reach it correctly through the existing GLFW3 layer
// (src/skel/glfw/glfw.cpp's keypressCB/CapturePad, Emscripten's libglfw.js) --
// nothing here re-implements or bypasses that pipeline. This file only handles
// things that live *above* GLFW, at the browser/DOM level, which nothing in the
// engine can see or control:
//   - preventing disruptive default browser actions for game keys (arrow-key page
//     scroll, Tab stealing focus, accidental page reload)
//   - pointer lock, which can only ever be *requested* in response to a genuine
//     user gesture (a browser security requirement no amount of C++ can work
//     around)
//   - detecting browser-tab focus loss, which -- per investigation -- does not
//     reliably reach re3's own windowFocusCB under Emscripten (see
//     docs/BROWSER_INPUT.md for why), and triggering the same CPad::Clear() the
//     engine already calls at startup, via the re3_OnBrowserFocusLost() export in
//     src/skel/glfw/glfw.cpp
//   - a debug overlay reading raw browser key/mouse/gamepad events directly, kept
//     completely independent of the engine so it stays useful for verifying what
//     the *browser* thinks is happening, before or after the engine gets a look

(() => {
	"use strict";

	// ---------------------------------------------------------------------
	// preventDefault for game keys (task 5)
	// ---------------------------------------------------------------------

	// Keys whose default browser action (page scroll, focus change, reload) would
	// be actively disruptive during gameplay. Deliberately excludes F11 (fullscreen)
	// and F12 (devtools) -- browsers do not allow scripts to suppress those anyway,
	// so there is nothing to gain by trying.
	const PREVENTABLE_CODES = new Set([
		"Space", "Tab",
		"ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight",
		"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10",
	]);

	function isInteractiveElement(el) {
		if (!el) return false;
		const tag = el.tagName;
		return tag === "BUTTON" || tag === "INPUT" || tag === "SELECT" || tag === "TEXTAREA" || tag === "A";
	}

	function setupPreventDefaults() {
		window.addEventListener("keydown", (event) => {
			if (!PREVENTABLE_CODES.has(event.code)) return;
			// Respect normal keyboard use of this page's *own* UI (e.g. tabbing to
			// and activating the "Start engine" button with Space/Enter) -- only
			// suppress the browser default when nothing on our own page wants the key.
			if (isInteractiveElement(document.activeElement) && (event.code === "Space" || event.code === "Tab")) return;
			event.preventDefault();
		}, { capture: true });
	}

	// ---------------------------------------------------------------------
	// Focus-loss recovery (task 8)
	// ---------------------------------------------------------------------

	function setupFocusLossRecovery(instance, log) {
		let armed = false; // don't fire before the engine has actually started (see
		// re3_OnBrowserFocusLost's own RsGlobal.ps guard in glfw.cpp -- this is an
		// extra belt-and-suspenders check on the JS side, not a substitute for it)

		function onFocusLost(reason) {
			if (!armed) return;
			try {
				instance.ccall("re3_OnBrowserFocusLost", null, [], []);
				log?.(`[input] browser focus lost (${reason}) -- released keys/mouse/pad state`, "info");
			} catch (err) {
				log?.(`[input] re3_OnBrowserFocusLost failed: ${err}`, "stderr");
			}
			if (document.pointerLockElement) document.exitPointerLock();
		}

		window.addEventListener("blur", () => onFocusLost("window blur"));
		document.addEventListener("visibilitychange", () => {
			if (document.hidden) onFocusLost("tab hidden");
		});

		return {
			arm() { armed = true; },
		};
	}

	// ---------------------------------------------------------------------
	// Pointer lock (task 6)
	// ---------------------------------------------------------------------

	function setupPointerLock(canvas, hintEl, log) {
		function updateHint() {
			const locked = document.pointerLockElement === canvas;
			hintEl.textContent = locked
				? "Mouse look active — click or Esc to release"
				: "Click to enable mouse look — Esc to release";
			hintEl.classList.toggle("locked", locked);
		}

		hintEl.classList.remove("hidden");
		updateHint();

		hintEl.addEventListener("click", () => {
			if (document.pointerLockElement === canvas) {
				document.exitPointerLock?.();
			} else {
				// Pointer Lock can only be requested from within a real user gesture's
				// call stack -- this click handler is exactly that. re3's own C++ side
				// never touches the Pointer Lock API at all (see the comment in
				// CPad::UpdateMouse(), src/core/Pad.cpp) -- engaging it is entirely a
				// browser-layer concern, kept out of the engine.
				canvas.requestPointerLock?.();
			}
		});

		document.addEventListener("pointerlockchange", () => {
			updateHint();
			log?.(`[input] pointer lock ${document.pointerLockElement === canvas ? "engaged" : "released"}`, "info");
		});
		document.addEventListener("pointerlockerror", () => {
			log?.("[input] pointer lock request failed (browser denied it)", "stderr");
		});
	}

	// Menu mouse navigation needs absolute cursor position; pointer lock only ever
	// reports relative deltas. re3_IsMenuActive() is a tiny read-only C++ getter
	// (src/skel/glfw/glfw.cpp) -- polled here, at the browser layer, rather than
	// having engine code reach into browser-only pointer-lock APIs itself.
	function setupMenuAwarePointerLockRelease(instance, canvas) {
		let isMenuActive;
		try {
			isMenuActive = instance.cwrap("re3_IsMenuActive", "number", []);
		} catch {
			return; // export not present (e.g. an older/rebuilt-without-it module); skip
		}
		function poll() {
			if (document.pointerLockElement === canvas && isMenuActive()) {
				document.exitPointerLock();
			}
			requestAnimationFrame(poll);
		}
		requestAnimationFrame(poll);
	}

	// ---------------------------------------------------------------------
	// Debug overlay (task 10) -- entirely independent of the engine; reads raw
	// browser events directly so it's meaningful even if re3 itself never starts.
	// ---------------------------------------------------------------------

	function setKV(table, rowsMap, key, value) {
		let row = rowsMap.get(key);
		if (!row) {
			row = document.createElement("tr");
			const k = document.createElement("td");
			k.className = "key";
			k.textContent = key;
			const v = document.createElement("td");
			v.className = "val";
			row.append(k, v);
			table.appendChild(row);
			rowsMap.set(key, row);
		}
		row.children[1].textContent = value;
	}

	const MOUSE_BUTTON_NAMES = ["Left", "Middle", "Right", "Back", "Forward"];

	function describeButtons(mask) {
		const held = MOUSE_BUTTON_NAMES.filter((_, i) => mask & (1 << i));
		return held.length ? held.join(", ") : "(none)";
	}

	function describeGamepad(pad) {
		const buttons = pad.buttons
			.map((b, i) => (b.pressed ? `#${i}=${b.value.toFixed(2)}` : null))
			.filter(Boolean);
		const axes = pad.axes.map((a) => a.toFixed(2)).join(", ");
		return (
			`[${pad.index}] ${pad.id}\n` +
			`  mapping: ${pad.mapping || "(none)"}\n` +
			`  buttons held: ${buttons.length ? buttons.join(" ") : "(none)"}\n` +
			`  axes: ${axes}`
		);
	}

	function setupDebugOverlay(els) {
		const heldKeys = new Set();
		const summaryRows = new Map();
		const mouseRows = new Map();
		const mouseState = { x: 0, y: 0, dx: 0, dy: 0, buttons: 0 };

		function renderKeys() {
			els.keysList.innerHTML = "";
			els.keysList.classList.toggle("empty", heldKeys.size === 0);
			for (const code of heldKeys) {
				const chip = document.createElement("span");
				chip.className = "input-key-chip";
				chip.textContent = code;
				els.keysList.appendChild(chip);
			}
		}

		window.addEventListener("keydown", (e) => {
			heldKeys.add(e.code);
			renderKeys();
		}, { capture: true });
		window.addEventListener("keyup", (e) => {
			heldKeys.delete(e.code);
			renderKeys();
		}, { capture: true });
		window.addEventListener("blur", () => {
			heldKeys.clear();
			renderKeys();
		});

		function renderMouse() {
			setKV(els.mouseTable, mouseRows, "Position", `${Math.round(mouseState.x)}, ${Math.round(mouseState.y)}`);
			setKV(els.mouseTable, mouseRows, "Movement (locked delta)", `${mouseState.dx.toFixed(1)}, ${mouseState.dy.toFixed(1)}`);
			setKV(els.mouseTable, mouseRows, "Buttons held", describeButtons(mouseState.buttons));
		}

		window.addEventListener("mousemove", (e) => {
			mouseState.x = e.clientX;
			mouseState.y = e.clientY;
			mouseState.dx = e.movementX || 0;
			mouseState.dy = e.movementY || 0;
			renderMouse();
		});
		window.addEventListener("mousedown", (e) => {
			mouseState.buttons |= 1 << e.button;
			renderMouse();
		});
		window.addEventListener("mouseup", (e) => {
			mouseState.buttons &= ~(1 << e.button);
			renderMouse();
		});
		window.addEventListener("wheel", (e) => {
			setKV(els.mouseTable, mouseRows, "Last wheel deltaY", e.deltaY.toFixed(1));
		}, { passive: true });
		window.addEventListener("blur", () => {
			mouseState.buttons = 0;
			renderMouse();
		});

		function renderSummary() {
			setKV(els.summaryTable, summaryRows, "Pointer lock", document.pointerLockElement ? "engaged" : "inactive");
			setKV(els.summaryTable, summaryRows, "Fullscreen", document.fullscreenElement ? "active" : "inactive");
		}
		document.addEventListener("pointerlockchange", renderSummary);
		document.addEventListener("fullscreenchange", renderSummary);

		// setTimeout, not requestAnimationFrame: this is a text-content refresh, not
		// a paint-aligned animation -- see web/perf.js's setupFrameTimingPanel for
		// why that distinction matters (found validating Phase 9, docs/FIRST_BOOT.md).
		function pollGamepads() {
			const pads = navigator.getGamepads ? navigator.getGamepads() : [];
			const connected = Array.from(pads).filter(Boolean);
			els.gamepadInfo.textContent = connected.length
				? connected.map(describeGamepad).join("\n\n")
				: "No gamepad connected. Press a button on a connected controller to activate it.";
			setTimeout(pollGamepads, 250);
		}
		setTimeout(pollGamepads, 250);

		renderKeys();
		renderMouse();
		renderSummary();
	}

	window.Re3Input = {
		setupPreventDefaults,
		setupFocusLossRecovery,
		setupPointerLock,
		setupMenuAwarePointerLockRelease,
		setupDebugOverlay,
	};
})();
