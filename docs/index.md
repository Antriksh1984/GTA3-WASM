# GTA3-WASM — Grand Theft Auto III in WebAssembly

**Course:** UCS503: Software Engineering (Project) — TIET Patiala  
**Authors:** Antriksh (`1024031034`), Divam (`1024031032`)

## Overview
**GTA3-WASM** is a browser-native WebAssembly port of [`re3`](https://github.com/halpz/re3), the open-source reverse-engineered engine for *Grand Theft Auto III*. Built with the Emscripten toolchain, the project runs the 3D engine directly inside a modern browser tab using WebGL 2.0, WebAudio, Pointer Lock input shims, and client-side IndexedDB (`IDBFS`) save persistence—requiring no native installation or browser plugins.

> **Disclaimer:** This repository contains engine source code, WebAssembly shims, build tooling, and the web launcher only. Users must supply their own legally obtained PC copy of *Grand Theft Auto III* data files.

## Engineering Documentation Index
- **Build & Boot:** [WASM Build Pipeline](WASM_BUILD.md) · [Game Asset Mounting](GAME_ASSETS.md) · [First Boot Guide](FIRST_BOOT.md)
- **Core Architecture:** [WASM Porting Audit](WASM_PORTING_AUDIT.md) · [Browser Runtime](BROWSER_RUNTIME.md) · [Browser Main Loop](BROWSER_MAIN_LOOP.md) · [Threading in WASM](THREADING_WASM.md)
- **Subsystems:** [WebGL2 Compatibility](WEBGL_COMPATIBILITY.md) · [WebAudio & OpenAL](AUDIO_WASM.md) · [Browser Input & Pointer Lock](BROWSER_INPUT.md) · [IndexedDB Save Persistence](SAVES_WASM.md)
- **Debugging & Stability:** [Menu-to-Gameplay Debugging](MENU_TO_GAMEPLAY_DEBUGGING.md) · [WASM Gameplay Stability](WASM_GAMEPLAY_STABILITY.md)
