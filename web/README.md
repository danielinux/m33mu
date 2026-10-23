# m33mu Web Workbench

A browser bench where real STM32H563 firmware, running in the m33mu emulator compiled to WebAssembly, drives an analog circuit on the same virtual clock. You place parts on a to-scale NUCLEO-H563ZI drawing and a breadboard, wire them to the board's two verified contacts — **CN7 pin 10 (PA5)** and **CN9 pin 1 (PA6)** — load an ELF and run. The worker solves the circuit (`sim/`), feeds solved pad voltages back to the firmware through Schmitt thresholds, and streams waveforms to a two-channel scope.

- Parts: breadboard, resistor, capacitor, LED, diode, push button, NPN, N-MOSFET, DC supply, ground.
- Instruments: inspector (terminal voltages, branch currents, pad mode/pull/drive, LED brightness from current), scope with cursors, sampled trigger, CSV export and SIGC voltage capture preview/import/export, virtual time and real-time ratio. See `docs/scope-capture.md` for SIGC limits.
- Projects: v2 JSON with the circuit, probes, solver settings, view and an embedded ELF checked by SHA-256; undo/redo; examples in `public/examples/`.
- Not modelled: ADC, PWM/timer outputs and other alternate functions, UART/SPI/I²C, other board contacts and other boards. The UI says so rather than approximating. See `docs/current-capabilities.md`.

Design and evidence: `docs/adr-001-solver.md` (solver choice and tolerances), `docs/electrical-model.md` (pad model), `docs/acceptance-matrix.md` (CLI/session/worker parity), `docs/p0-progress.md` (tracker), `boards/nucleo-h563zi-pins.json` (contact evidence).

## Build and run

From the repository root, configure CMake once and start the server:

```sh
cmake -S . -B build
cmake --build build --target webserver
```

With the Makefiles generator, `make -C build webserver` does the same thing. The target installs the pinned npm dependencies, builds the Emscripten worker and app, then starts Vite at `http://127.0.0.1:5173/`. Press Ctrl-C to stop it. `cmake --build build --target web-app` builds `web/dist/` without starting the server.

The web UI is optional. CMake enables the `web-app` and `webserver` targets, and the WASM/worker tests, only when `emcc`, `emcmake`, `node` and `npm` are all found; otherwise the configuration summary reports `web UI: not found (missing ...)` and the native emulator builds as usual. Pass `-DM33MU_ENABLE_WEB_UI=OFF` to skip detection entirely.

The equivalent manual commands are:

```sh
scripts/build-web-app.sh --install   # runs npm ci first
npm --prefix web run dev
```

The app build compiles the Emscripten worker into `web/public/worker/` and builds the static app into `web/dist/`. The worker and board assets are served locally. No server-side emulator, CDN font, or network service is required at runtime.

The development toolchain requires Node.js/npm, CMake, and Emscripten. The worker runs H563 firmware only, through the production session bridge, in slices that yield to browser messages; a coupled bench runs in 12 ms slices and can be capped at real time.

## Tests

- `node tests/run-unit.mjs` — solver, scheduler, project model, net graph and example unit tests (bundled with Vite's esbuild; CTest `web_sim_unit_test`).
- `scripts/test-web-bench.sh`, `scripts/test-web-demos.sh` — worker integration with real firmware (RC/EXTI feedback, LED blink, RC button, lifecycle).
- `scripts/test-web-acceptance-matrix.sh` — CLI vs native session vs Node/WASM vs Chromium worker.
- `scripts/test-web-browser.sh [screenshot-dir]` — Chromium interaction tests against `dist/` with external network blocked (needs `chromium`; `M33MU_CHROMIUM` overrides the path).
