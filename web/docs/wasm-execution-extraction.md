# WASM execution extraction notes

**Status:** in progress; shared in-memory ELF32/ARM loader implemented, production session/stepping API still absent. Reviewed at local `master` HEAD `278b14e`.

## Current paths

- `cmake/wasm` now builds a static core library with `scripts/build-web-core.sh`, excluding `main.c`, GDB sockets, POSIX target HAL, Linux Ethernet/USB host modules, optional TPM/capstone sources and TUI. CTest runs this compile smoke when `emcmake` is found. Static archive creation does not yet prove a linked/runnable session because host-facing callbacks and a session entry point are still absent.
- `mm_elf_load_memory()` accepts a byte buffer, validates ELF32 little-endian ARM program-header and segment bounds, maps PT_LOAD data into caller-owned flash/RAM/serial-flash buffers, and reports loaded address range. CLI path loading reads the image and delegates to this shared parser. `scripts/test-web-elf-loader.sh` links the loader test as WASM and runs it in Node; CTest registers this smoke when `emcc` and Node are found.
- `scripts/test_cli_elf_loader.py` generates a minimal H563 ELF with a reset vector and BKPT, then verifies normal CLI loading, reset, and instruction execution. This confirms the path wrapper and in-memory parser work together.
- Remaining loader gaps: session-owned target storage and reset lifecycle, configured input size limit, and browser worker integration. The new parser test currently covers a flash PT_LOAD, linked-at-zero flash offset, and rejection of a malformed later segment without partial writes.
- `src/main.c::step_core_simple()` handles a limited subset of timing/exception logic for a secondary path. `src/runloop.c::mm_step()` executes only NOP and branch placeholders. Neither is the production H563 path.
- The main execution loop assembles `mm_execute_ctx` and calls `mm_execute_decoded()`, but the surrounding fetch/decode, interrupt dispatch, cycle accounting, timers, SCS, signal hooks, TrustZone state, faults, WFI/WFE and core-system state are managed inline in `main.c`.
- Startup also composes target MMIO, memory regions, flash/RAM aliases, code cache, SoC reset, timer, USART/SPI, protection, boot resolution, and peripheral reset. A thin wrapper around `mm_execute_decoded()` alone would omit these production behaviors.
- Process-wide state in memory-map and signal facilities, and static target drivers, means the planned first API must own and enforce one live session. Concurrent session isolation is not an incidental worker feature.
- Board caveat: the supplied H563 layout declares `geometryStatus: stylized_provisional_not_manufacturing_scale` and `electricalMapStatus: UNVERIFIED_VENDOR_SCHEMATIC_REQUIRED`. ST's official CMSIS-Pack documentation identifies the MB1404 C01 schematic, but the drawing has not been reviewed and the sprite's contact coordinates/IDs cannot be reconciled to CN7/CN9. The PA5/CN7-10 and PA6/CN9-1 facts stay metadata-only until schematic and artwork anchors are established.

## Extraction requirements

1. Move CPU stepping and its owned scheduler state behind a typed session API used by both CLI and web builds. Keep instruction execution, interrupt ordering, exception semantics, virtual-cycle accounting and signal hooks shared; do not fork an approximate browser-only loop.
2. Move image loading behind a memory-buffer API. The browser contract starts with bounded ELF32 little-endian ARM PT_LOAD records; validate header/program-table bounds, address ranges and all integer additions before copying.
3. Give session creation the selected target and owned storage, initialize the same target/MMIO/memory/cache peripherals as CLI startup, and return explicit unsupported-feature errors for native-only host backends.
4. Add native tests that load a real H563 ELF from bytes and compare boot PC/register/memory and bounded stepping against the CLI path. Add an Emscripten worker smoke test only after native behavior matches.

## Known implementation risk

The current full loop has intertwined options and globals (GDB/TUI/recording/fault injection/multicore). Split optional CLI policy from shared execution in narrow pieces and keep the native suite green after each extraction. Do not route production browser runs through `step_core_simple()` or `mm_step()` as a shortcut.
