# WASM execution extraction notes

**Status:** investigation; no browser execution API implemented. Reviewed at local `master` HEAD `cf73f9e`.

## Current paths

- `src/main.c::load_elf_segments()` is private and accepts a path, uses `FILE*` and libelf (`elf_begin`/`gelf_getphdr`), and copies PT_LOAD bytes into private flash/RAM buffers. The loader is not an in-memory API and the Emscripten toolchain cannot use the system libelf installation.
- `src/main.c::step_core_simple()` handles a limited subset of timing/exception logic for a secondary path. `src/runloop.c::mm_step()` executes only NOP and branch placeholders. Neither is the production H563 path.
- The main execution loop assembles `mm_execute_ctx` and calls `mm_execute_decoded()`, but the surrounding fetch/decode, interrupt dispatch, cycle accounting, timers, SCS, signal hooks, TrustZone state, faults, WFI/WFE and core-system state are managed inline in `main.c`.
- Startup also composes target MMIO, memory regions, flash/RAM aliases, code cache, SoC reset, timer, USART/SPI, protection, boot resolution, and peripheral reset. A thin wrapper around `mm_execute_decoded()` alone would omit these production behaviors.
- Process-wide state in memory-map and signal facilities, and static target drivers, means the planned first API must own and enforce one live session. Concurrent session isolation is not an incidental worker feature.

## Extraction requirements

1. Move CPU stepping and its owned scheduler state behind a typed session API used by both CLI and web builds. Keep instruction execution, interrupt ordering, exception semantics, virtual-cycle accounting and signal hooks shared; do not fork an approximate browser-only loop.
2. Move image loading behind a memory-buffer API. The browser contract starts with bounded ELF32 little-endian ARM PT_LOAD records; validate header/program-table bounds, address ranges and all integer additions before copying.
3. Give session creation the selected target and owned storage, initialize the same target/MMIO/memory/cache peripherals as CLI startup, and return explicit unsupported-feature errors for native-only host backends.
4. Add native tests that load a real H563 ELF from bytes and compare boot PC/register/memory and bounded stepping against the CLI path. Add an Emscripten worker smoke test only after native behavior matches.

## Known implementation risk

The current full loop has intertwined options and globals (GDB/TUI/recording/fault injection/multicore). Split optional CLI policy from shared execution in narrow pieces and keep the native suite green after each extraction. Do not route production browser runs through `step_core_simple()` or `mm_step()` as a shortcut.
