// Minimal launcher for web/audiotest.html (Phase 6 audio diagnostic).
//
// Mirrors rendertest-launcher.js's structure but for audio: instantiate the
// module immediately (safe -- audiotest/main.cpp's main() does nothing
// audio-related, see its header comment), then wait for a real click on
// "Click to Start Audio Test" before calling audiotest_init(), which is the
// first point anything here touches OpenAL/AudioContext. Every subsequent
// control (play/pause/resume/volume/pan) is itself a button click, so nothing
// in this page ever calls into audio outside a user gesture.

(() => {
	"use strict";

	const els = {
		statusDot: document.getElementById("status-dot"),
		statusText: document.getElementById("status-text"),
		loadingOverlay: document.getElementById("loading-overlay"),
		errorBanner: document.getElementById("error-banner"),
		errorDetail: document.getElementById("error-detail"),
		logOutput: document.getElementById("log-output"),
		diagnosticsBtn: document.getElementById("diagnostics-btn"),
		sidepanel: document.getElementById("sidepanel"),
		startBtn: document.getElementById("start-audio-btn"),
		initState: document.getElementById("init-state"),
		controls: document.getElementById("audio-controls"),
		playSfxBtn: document.getElementById("play-sfx-btn"),
		sfxVolume: document.getElementById("sfx-volume"),
		sfxState: document.getElementById("sfx-state"),
		playMusicBtn: document.getElementById("play-music-btn"),
		pauseMusicBtn: document.getElementById("pause-music-btn"),
		resumeMusicBtn: document.getElementById("resume-music-btn"),
		stopMusicBtn: document.getElementById("stop-music-btn"),
		musicVolume: document.getElementById("music-volume"),
		musicState: document.getElementById("music-state"),
		panSlider: document.getElementById("pan-slider"),
		playPanBtn: document.getElementById("play-pan-btn"),
		panState: document.getElementById("pan-state"),
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

	// AL_SOURCE_STATE enum values (AL/al.h) -- decoded here purely for the log
	// panel; audiotest_get_state() returns the raw int.
	const AL_STATE_NAMES = { 4113: "INITIAL", 4114: "PLAYING", 4115: "PAUSED", 4116: "STOPPED" };
	function describeState(code) {
		return AL_STATE_NAMES[code] || `unknown(${code})`;
	}

	async function main() {
		if (typeof createAudioTestModule !== "function") {
			showFatalError(
				"build/audiotest.js did not load",
				new Error("createAudioTestModule() is not defined. Build the target first: " +
					"cmake --build build-wasm --target audiotest")
			);
			return;
		}

		setStatus("loading", "loading audio diagnostic module…");

		let instance;
		try {
			instance = await createAudioTestModule({
				print: (text) => log(text, text.includes("FAIL") ? "stderr" : "info"),
				printErr: (text) => log(text, "stderr"),
				onAbort: (reason) => showFatalError("Audio diagnostic module aborted", reason),
			});
		} catch (err) {
			showFatalError("Failed to instantiate the audio diagnostic module", err);
			return;
		}

		els.loadingOverlay.classList.add("hidden");
		log("[module] instantiated (no audio touched yet)", "info");

		try {
			instance.callMain([]);
		} catch (err) {
			showFatalError("Module main() crashed", err);
			return;
		}

		const init = instance.cwrap("audiotest_init", "number", []);
		const playSfx = instance.cwrap("audiotest_play_sfx", "number", []);
		const playMusic = instance.cwrap("audiotest_play_music", "number", []);
		const pauseMusic = instance.cwrap("audiotest_pause_music", "number", []);
		const resumeMusic = instance.cwrap("audiotest_resume_music", "number", []);
		const stopMusic = instance.cwrap("audiotest_stop_music", "number", []);
		const setMusicVolume = instance.cwrap("audiotest_set_music_volume", "number", ["number"]);
		const setSfxVolume = instance.cwrap("audiotest_set_sfx_volume", "number", ["number"]);
		const playPan = instance.cwrap("audiotest_play_pan_test", "number", ["number"]);
		const getState = instance.cwrap("audiotest_get_state", "number", ["number"]);

		setStatus("ok", "module ready — waiting for user interaction");
		els.startBtn.disabled = false;
		els.initState.textContent = "ready to start";

		function pollStates() {
			els.sfxState.textContent = describeState(getState(0));
			els.musicState.textContent = describeState(getState(1));
			els.panState.textContent = describeState(getState(2));
			setTimeout(pollStates, 250);
		}

		els.startBtn.addEventListener("click", () => {
			// This click handler's call stack is the user gesture -- audiotest_init()
			// is the first line of code in this whole page that touches OpenAL.
			const ok = init();
			if (!ok) {
				els.initState.textContent = "init FAILED — see log";
				setStatus("error", "Audio init failed");
				return;
			}
			els.initState.textContent = "audio initialised";
			els.startBtn.disabled = true;
			els.controls.dataset.active = "true";
			log("[audiotest] AudioContext/OpenAL context created from user gesture", "info");
			pollStates();
		}, { once: true });

		els.playSfxBtn.addEventListener("click", () => playSfx());
		els.sfxVolume.addEventListener("input", () => setSfxVolume(parseFloat(els.sfxVolume.value)));

		els.playMusicBtn.addEventListener("click", () => playMusic());
		els.pauseMusicBtn.addEventListener("click", () => pauseMusic());
		els.resumeMusicBtn.addEventListener("click", () => resumeMusic());
		els.stopMusicBtn.addEventListener("click", () => stopMusic());
		els.musicVolume.addEventListener("input", () => setMusicVolume(parseFloat(els.musicVolume.value)));

		els.playPanBtn.addEventListener("click", () => playPan(parseFloat(els.panSlider.value)));
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
