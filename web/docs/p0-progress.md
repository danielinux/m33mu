# Web workbench P0 tracker

Updated 2026-10-23. Keep this checklist current as P0 work advances.

## Completed

- [x] Fast-forward local `master` to `34cc86d`; record PR #15 head `012eaf5`.
- [x] Fix stale PUF zeroize test and commit `7188fb9` locally; no push. Full native suite: 88/88 enabled tests pass.
- [x] Build PR #15 from a clean archive with optional integrations disabled; its native build succeeds and CTest passes 90/90 enabled tests (one disabled).
- [x] Build and run representative PR #15 H563 EXTI and timer capture firmware fixtures, plus H5F4 GPIO input fixture; all report expected breakpoint success.
- [x] Merge the tested PR #15 head locally as `5799dd5`. Signal input remains digital-level injection and output remains `0/1/Z` observation; ADC is still a design, not implemented.
- [x] Rebuild after Emscripten core and loader work and run serial CTest: current suite has 97/97 enabled tests passing (one disabled), including the core-build, Node-hosted WASM loader, and CLI ELF smokes. An earlier parallel harness run hit sandbox socket denial; isolated and serial runs pass.
- [x] Confirm Emscripten 6.0.5 and Node 24 are available.
- [x] Attempt the broad root CMake Emscripten build; it fails at `src/m33mu/eth_backend.c:16` due to Linux-only `linux/if.h`. Add `cmake/wasm` static-core target excluding host modules, with repeatable `scripts/build-web-core.sh`; its CTest smoke passes. This target is a compileable core, not a runnable browser emulator.
- [x] Verify H563 electrical signal mappings from ST's pinned CubeH5 example: PA5 is CN7 pin 10; PA6 is CN9 pin 1. Record the evidence in `web/boards/nucleo-h563zi-pins.json`. ST's official board-doc index identifies the MB1404 C01 schematic; the supplied sprite has no CN7/CN9 identifiers (it labels CN11/CN12 and ZIO_D/ZIO_A), and declares connector identities and geometry provisional. Both contacts stay electrically disabled pending a schematic and artwork revision that can actually be reconciled.
- [x] Parse the supplied 15-board artwork manifest and confirm its policy leaves electrical wiring disabled; H563 has zero artwork pins marked verified.
- [x] Record current implementation limits and per-target board/GPIO status in `current-capabilities.md`.
- [x] Add the v1 project JSON Schema and H563 pin evidence override. Validate the 15 supplied board layouts, coordinate bounds, contact counts and electrical-enable policy with `scripts/validate-web-boards.py`; 15 layouts and 2 H563 mappings pass.

## In progress / next

- [x] Build an explicit Emscripten source target without Linux network, terminal, debugger socket and optional native plugins. Initial broad-build probe failed at `eth_backend.c` on `linux/if.h`; the dedicated static-core target now builds.
- [ ] Extract the production firmware execution path into a one-session API; do not use `src/runloop.c` as a substitute. Shared in-memory ELF32/ARM loading and cycle/peripheral time advancement are implemented and build with Emscripten, but execution state, scheduling, interrupt handling and lifecycle remain in `main.c`.
- [x] Run the new in-memory ELF loader tests as a Node-hosted WASM module using `scripts/test-web-elf-loader.sh`; all loader fixtures pass.
- [x] Register the Node-hosted WASM loader smoke in CTest when both Emscripten and Node are found; focused CTest passes.
- [x] Add a generated minimal H563 ELF CLI integration fixture that boots from its vector table and reaches the expected BKPT through the shared parser.
- [ ] Obtain/review the MB1404 C01 drawing and resolve the connector-ID mismatch between verified CN7/CN9 signal evidence and sprite identifiers CN11/CN12/ZIO_D/ZIO_A. Replace or relabel only against board geometry with revision-specific evidence; keep wiring disabled until exact contact identity is established.
- [ ] Run ngspice WASM and CircuitJS TypeScript incremental transient spikes, or record a concrete toolchain/license blocker. Test changes during a transient, pause/resume, state preservation, reproducibility and worker yielding.
- [ ] Demonstrate causal firmware-output changes and analog threshold crossings with stated time tolerance. The PR #15 block-delivery timing alone does not meet the analog coupling contract.
- [x] Define versioned board-pad, capability and worker-message JSON Schemas. `scripts/validate-web-schemas.py` validates all four Draft 2020-12 schemas and passes four valid and three invalid fixtures, including rejection of an electrically enabled contact without artwork verification.
- [x] Inventory currently supplied artwork and upstream pin evidence in `THIRD_PARTY.md`; no solver code, device models, artwork files, or runtime packages have been imported. License audit at actual solver adoption remains a gate.
- [x] Inspect the public EEcircuit engine API and source: it runs the full netlist (`destroy all`, `run`, export raw data) with no public incremental stepping/source-update API, so it fails the interactive co-simulation contract as-is. CircuitJS1's `SimulationManager.runCircuit()` exposes iteration internals but is governed by wall clock, speed controls and frame limits, and calls UI/CirSim hooks; a bounded headless quantum still needs extraction and testing.
- [x] Record the solver decision as open in `adr-001-solver.md`; pin reviewed candidate revisions and state the required evidence. No solver is selected yet.
- [x] Map the production firmware execution and loader extraction requirements in `wasm-execution-extraction.md`; no approximate runloop was introduced.
- [x] Separate host-independent target callback dispatch from POSIX UART/PTY HAL code; validate callback forwarding natively and compile the runtime into the Emscripten core. This does not yet create the production session or scheduler API.
- [x] Extract production virtual-cycle accounting and cycle-driven SysTick, target timer and signal advancement into `mm_execution_advance_cycles()`; use it in production and secondary stepping paths and cover its counter/timer behavior with a native test.
- [ ] Finish the third-party license audit when selecting/importing a solver and adding runtime dependencies or models.

## Not started

- [ ] P1 browser UI and minimal verified H563 workbench.
- [ ] P2 analog device breadth, ADC and instruments.
- [ ] P3 broad board mappings and developer workflows.
