# Web workbench P0 tracker

Updated 2026-10-23. Keep this checklist current as P0 work advances.

## Completed

- [x] Fast-forward local `master` to `34cc86d`; record PR #15 head `012eaf5`.
- [x] Fix stale PUF zeroize test and commit `7188fb9` locally; no push. Full native suite: 88/88 enabled tests pass.
- [x] Build PR #15 from a clean archive with optional integrations disabled; its native build succeeds and CTest passes 90/90 enabled tests (one disabled).
- [x] Build and run representative PR #15 H563 EXTI and timer capture firmware fixtures, plus H5F4 GPIO input fixture; all report expected breakpoint success.
- [x] Merge the tested PR #15 head locally as `5799dd5`. Signal input remains digital-level injection and output remains `0/1/Z` observation; ADC is still a design, not implemented.
- [x] Rebuild merged local `master` and run serial CTest: 93/93 enabled tests pass (one disabled). An earlier parallel harness run hit sandbox socket denial; the isolated and serial runs pass.
- [x] Confirm Emscripten 6.0.5 and Node 24 are available.
- [x] Attempt full CMake Emscripten build with tests and optional integrations disabled. It fails at `src/m33mu/eth_backend.c:16` due to missing Linux-only `linux/if.h`, confirming CMake's broad source set needs a dedicated web target.
- [x] Verify H563 electrical signal mappings from ST's pinned CubeH5 example: PA5 is CN7 pin 10; PA6 is CN9 pin 1. Record the evidence in `../boards/nucleo-h563zi-pins.json`.
- [x] Parse the supplied 15-board artwork manifest and confirm its policy leaves electrical wiring disabled; H563 has zero artwork pins marked verified.
- [x] Record current implementation limits and per-target board/GPIO status in `current-capabilities.md`.
- [x] Add the v1 project JSON Schema and H563 pin evidence override. Validate the 15 supplied board layouts, coordinate bounds, contact counts and electrical-enable policy with `scripts/validate-web-boards.py`; 15 layouts and 2 H563 mappings pass.

## In progress / next

- [ ] Build an explicit Emscripten source target without Linux network, terminal, debugger socket and optional native plugins. Initial full-build probe failed at `eth_backend.c` on `linux/if.h`.
- [ ] Extract the production firmware execution path and one-session, in-memory H563 ELF API; do not use `src/runloop.c` as a substitute.
- [ ] Reconcile verified CN7/10 and CN9/1 signal mappings with the sprite's provisional ZIO contact anchors; record exact schematic revision and keep wiring disabled until the geometry match is confirmed.
- [ ] Run ngspice WASM and CircuitJS TypeScript incremental transient spikes, or record a concrete toolchain/license blocker. Test changes during a transient, pause/resume, state preservation, reproducibility and worker yielding.
- [ ] Demonstrate causal firmware-output changes and analog threshold crossings with stated time tolerance. The PR #15 block-delivery timing alone does not meet the analog coupling contract.
- [x] Define versioned board-pad, capability and worker-message JSON Schemas. The H563 board override follows the board-pad contract; full schema-level valid/invalid fixture validation remains open because no JSON Schema validator is installed in the environment.
- [x] Inventory currently supplied artwork and upstream pin evidence in `THIRD_PARTY.md`; no solver code, device models, artwork files, or runtime packages have been imported. License audit at actual solver adoption remains a gate.
- [ ] Run project-schema positive/negative fixture validation and finish the third-party license audit.

## Not started

- [ ] P1 browser UI and minimal verified H563 workbench.
- [ ] P2 analog device breadth, ADC and instruments.
- [ ] P3 broad board mappings and developer workflows.
