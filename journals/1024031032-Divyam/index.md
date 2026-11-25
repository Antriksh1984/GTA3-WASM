# Weekly Project Journal — Divyam (1024031032)

## Week 1: Repository Scaffolding & Requirement Analysis
- Initialized the `GTA3-WASM` GitHub repository based on the `UCS503P` course template (`docs/`, `journals/`, `project-proposal/`, `project-report-prototype-stage/`, `project-report-final/`).
- Configured `.gitignore` rules to exclude proprietary GTA III game assets (`gamefiles.zip`), build artifacts, and local environment files.
- Co-authored the project proposal scope, deliverables, and initial Gantt schedule.

## Week 2: Build Environment Verification & Branch Management
- Set up branch conventions, commit guidelines, and repository layout documentation explaining why upstream `re3` directories (`src/`, `vendor/`, `cmake/`) remain at the repository root.
- Verified local Emscripten SDK (`emsdk`) installation steps and documented build prerequisites.

## Week 3: Web Frontend Shell & Local Dev-Server Configuration
- Configured the `web/` frontend directory structure and local HTTP server headers (`Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy`) required for browser WASM execution.
- Reviewed browser runtime boot logs and organized issue tracking for main-loop and threading blockers.

## Week 4: WebGL2 Viewport & Canvas Integration
- Integrated the responsive HTML5 `<canvas>` container and loading overlay in `web/` to display compilation and asset-mounting progress.
- Tested WebGL2 context initialization across Chrome and Edge and logged shader compilation warnings.

## Week 5: Audio Diagnostics & Browser Autoplay Testing
- Verified browser audio unlock behavior (user-gesture `AudioContext` resume) using the standalone `audiotest/` diagnostic harness.
- Documented audio codec dependencies (`libsndfile`, `openal-soft`) and repository structure updates in `README.md`.

## Week 6: Asset Packaging Workflow & First-Boot QA
- Tested the client-side asset packaging and mounting workflow (`GAME_ASSETS.md`) using legally supplied GTA III data files.
- Executed end-to-end first-boot verification from landing screen to main menu and updated `manual.md` with user setup instructions.

## Week 7: Save-Data Persistence & Prototype Stage Report
- Verified browser IndexedDB (`IDBFS`) save-game persistence across page reloads (`saves.js`) and tested save slot export/import behavior.
- Compiled and uploaded the prototype-stage LaTeX report (`project-report-prototype-stage/main.tex`) and synchronized repository milestones.

## Week 8: Cross-Browser Compatibility & Gameplay Stability Testing
- Conducted structured QA testing across Chrome, Firefox, and Edge covering keyboard/mouse pointer lock, frame pacing, and mission transitions.
- Documented functional test cases (`TC-01` through `TC-03`) covering WASM boot, WebGL2/audio initialization, and IndexedDB save persistence.

## Week 9: Final Report, MkDocs Deployment & Submission Audit
- Finalized the LaTeX final report (`project-report-final/main.tex`) including architecture diagrams, sequence/activity flows, 9-week Gantt chart, functional test cases, and references.
- Audited all repository folders, updated `README.md` report download links, completed weekly journals, and verified repository readiness for evaluation.
