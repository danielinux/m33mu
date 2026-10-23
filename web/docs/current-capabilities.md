# Web workbench: current capabilities

Reviewed on 2026-10-24 at local `master` (see `git log`; tracker: `p0-progress.md`). Earlier revisions of this page described a pre-solver state and are in git history.

## What works

| Area | State | Evidence |
|---|---|---|
| Build | Optional: CMake enables the `web-app` and `webserver` targets and all web tests only when `emcc`, `emcmake`, `node` and `npm` are found (`-DM33MU_ENABLE_WEB_UI=OFF` skips detection). | Configuration summary line `web UI:` |
| Firmware | STM32H563 ELF32/ARM up to 32 MiB through the one-session C API (`include/m33mu/web.h`) and the ES-module worker. Run, pause, one-instruction step, reset, CPU snapshot, firmware `SYSRESETREQ`, WFI/WFE, PendSV/SVC/SysTick/EXTI, handled faults, and CLI-style lockup when a fault cannot be taken. | `web_session_test` (native and Node/WASM), worker smokes, `web_acceptance_matrix_test` |
| Scheduler parity | One differential fixture (SysTick, WFI wake, PendSV, SVC, handled UsageFault, CPU clock change; DWT CYCCNT log digest) gives identical r0-r3, cycles and clock across the CLI interpreter, native session, Node/WASM worker and Chromium worker. See `acceptance-matrix.md`. | `web/fixtures/acceptance/parity.json` |
| Analog solver | In-repository MNA transient solver (`web/sim/`): R, C, L, V/I sources, switch, diode (with breakdown), LED, NPN/PNP, NMOS/PMOS, rail-limited op-amp macro model, MCU pad. Snapshot/restore, threshold localization. Decision and tolerances: `adr-001-solver.md`. | `web_sim_unit_test`, op-amp gain/transient fixtures |
| Coupling | Firmware and circuit share one virtual clock in the worker (`createBench`/`runUntil`); pad drive/mode/pull changes come from the GPIO registers; input levels come from solved voltage with Schmitt thresholds. Pad model: `electrical-model.md`. | `wasm_worker_bench_rc_exti_test`, `wasm_worker_demos_test` |
| Board | NUCLEO-H563ZI (MB1404 H563ZI-C01) redrawn to scale. **CN7 pin 10 (PA5)** and **CN9 pin 1 (PA6)** are the only electrical contacts; all others are visual locators and the wiring tool refuses them. | `web/boards/nucleo-h563zi-pins.json`, `scripts/validate-web-boards.py` |
| Editor | Place/move/rotate/delete parts, breadboard strips, wiring with snap and hit-testing, pan/zoom, undo/redo (200 steps), keyboard shortcuts, inspector (parameters, terminal voltages, branch current, pad mode/pull/drive/voltage/current, LED brightness), two-channel scope with cursors, sampled rising/falling threshold trigger, manual hold/release, CSV export and SIGC voltage import/export preview, v2 project save/open with embedded verified ELF and v1 migration, examples menu. | `web_browser_chromium_test`, scope and SIGC unit fixtures |
| Examples | H563 LED blink, H563 RC button → EXTI → LED, DC divider, RC switch, NPN LED switch, op-amp gain ×2. | `web/public/examples/`, `examples.test.ts`, demo golden `web/fixtures/demos/golden.json` |
| Offline | All assets, the worker and WASM are served from the app; the browser test blocks every non-local request. | `web_browser_chromium_test` |

## Not supported (shown as such in the UI)

- ADC, DAC, PWM/timer outputs and every other alternate function on a pad: an AF-mode pad is high impedance and the bench raises an `unsupported` notice.
- UART, SPI, I²C; serial console in the browser.
- Board contacts other than CN7-10 and CN9-1; the other 14 boards (display assets exist, no reviewed mapping, no browser firmware profile).
- More than one MCU per bench; more than one emulator session per worker (process-global target state).
- Op-amp frequency response, slew rate, common-mode range and input offset; regulators, transformers, junction/gate capacitance, temperature. The available op-amp is a rail-limited macro model, not a device-specific IC.
- Scope trigger is a display alignment on sampled values, with linear crossing interpolation. It does not stop the emulator or provide pre-sample event localization.
- Manual scope hold copies the current sample ring and freezes its display and exports while the simulation continues. Automatic normal/single acquisition modes are not implemented.
- SIGC import previews up to two voltage traces; it does not drive MCU inputs or persist in project JSON. See `scope-capture.md`.
- Timing: one virtual cycle per instruction (no pipeline/bus model); the CLI's default translated-block mode takes interrupts at block boundaries and differs from the instruction-stepped web scheduler (14 cycles on the parity fixture).
- Browsers: only Chromium is tested (Firefox and WebKit drivers were not available in the build environment).
