<img src="assets/logomob.webp" alt="TIET Logo" width="60">
UCS503: Software Engineering (Project) TIET Patiala

# GTA3-WASM
[ 📄 Download Final Report (PDF)](project-report-final/main.pdf)
**Author(s)**:

`(A)` Antriksh -- Roll No. `1024031034`
`(D)` Divam -- Roll No. `1024031032`

GTA3-WASM is a WebAssembly port of [re3](https://github.com/Hezkore/hez-gta-re3)
-- a reverse-engineered, source-compatible build of Grand Theft Auto III --
so that it runs entirely in a browser tab via WebGL2, with no native
install and no plugin.

**This repository does not contain, download, or distribute any GTA III
game data.** You need your own legally obtained copy of GTA III (Steam,
GOG, a retail disc, etc.) -- this port only reads the same data files the
original PC game ships with, from wherever you point it.

## Repository layout

This repository follows the [UCS503P project
template](https://github.com/tiet-ucs503/ucs503p-202627odd-template):

- `project-proposal/`, `project-report-prototype-stage/`,
  `project-report-final/` -- the three LaTeX reports for the course.
- `journals/` -- one folder per team member, with weekly journal entries.
- `docs/` -- documentation, built and deployed as a website via `mkdocs`
  (see below). Includes the engineering write-ups for the WASM port
  (audio, input, threading, saves, WebGL compatibility, etc.).
- The engine/game source code, build scripts, and web frontend
  (`src/`, `vendor/`, `cmake/`, `web/`, `gamefiles/`, `CMakeLists.txt`,
  `premake5.lua`, ...) are kept at the repository root rather than under a
  `code/` folder, since this is a fork of the upstream `re3` build tree and
  moving it would break existing build tooling.

## Building & running

See [manual.md](manual.md) for the full build and run guide (prerequisites,
activating `emsdk`, building the WASM module, packaging game assets, and
running the local dev server).

## Docs

All technical documentation, subsystem porting notes, and weekly progress logs are maintained directly in this repository:

- **[`docs/`](docs/)** — Complete engineering write-ups covering the Emscripten build pipeline, WebGL2 compatibility, WebAudio porting, browser input shims, IndexedDB save persistence, and gameplay stability.
- **[`manual.md`](manual.md)** — Step-by-step setup guide for compiling the WebAssembly module, packaging local game assets, and running the local development server.
- **[`journals/`](journals/)** — Weekly development logs for both team members (`1024031032-divam` and `1024031034-antriksh`).

To preview the documentation site locally with MkDocs:

```bash
pip install -r pyproject.toml
make docs

## Credits & Upstream

This project builds directly on top of the
[re3](https://github.com/GTAmodding/re3) reverse-engineering project. All
credit for the reverse-engineered engine itself belongs to the `re3`/
GTAmodding contributors; this repository's original contribution is the
WebAssembly/browser port (build tooling, browser runtime shims, input,
audio, saves, and the web frontend under `web/`).
