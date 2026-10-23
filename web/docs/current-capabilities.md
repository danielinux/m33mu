# Web workbench: current capabilities

Reviewed against `master` `34cc86d550b4a471be9bde3f397f56507bdb1d3b` plus local test fix `7188fb945b7c91963499931aa124a40bbc3c9823`, and PR #15 head `012eaf51e57c4803956952ee5dc1f38ddfd54699` merged locally as `5799dd587dcba1768367ad6dd29af64ac36eff3a`.

This is a development status page, not a claim that the browser workbench exists yet.

## Emulator and build

| Area | Current state | Browser consequence |
|---|---|---|
| CPU profiles | 15 SoC profiles listed in `docs/supported-cpus.md` | Profiles identify MCU models, not verified dev-board wiring. |
| Firmware formats | CLI loads ELF, BIN, Intel HEX and UF2. ELF32/ARM PT_LOAD parsing is now in shared `mm_elf_load_memory()`; other format dispatch remains in `src/main.c`. | A bounded in-memory ELF loader runs its native and Node-hosted WASM fixtures; the browser session API and worker are not implemented. Start with H563 ELF. |
| Production execution | Full dispatch, timers, sleep and exception scheduling are in the large CLI translation unit `src/main.c` | `src/runloop.c` is a NOP/branch placeholder; it is not a firmware backend. |
| Native library | `m33mu_lib` is assembled from a broad source glob and links optional host libraries | A dedicated Emscripten source target and host-backend exclusions are needed. |
| Emscripten | `emcc 6.0.5` and Node 24 are installed in the reviewed environment | Toolchain is present. A full CMake cross-build with optional integrations disabled currently fails compiling `src/m33mu/eth_backend.c` because Emscripten lacks `linux/if.h`. |
| Session isolation | GPIO readers, memory-map lookup and multiple peripheral models use global/static state | First browser version supports one active emulator session per worker. Parallel sessions require a later state-ownership refactor. |
| Native regression | PUF zeroize expectation was updated in local commit `7188fb9`; after local PR #15 merge `5799dd5`, serial CTest passed 93/93 enabled tests (one disabled) | The update tests START reactivation after zeroize and confirms recovery survives reboot. |

## GPIO and board connectivity

| Target / board | Emulator GPIO signal support | Board signal mapping in supplied artwork | Browser state |
|---|---|---|---|
| STM32H533 / NUCLEO-H533RE | PR #15 digital GPIO input/output, EXTI; timer capture | 0 verified contacts | Display only until mappings are audited |
| STM32H563 / NUCLEO-H563ZI | PR #15 digital GPIO input/output, EXTI; timer capture | 0 verified contacts | Initial demo candidate; verify exact header contacts first |
| STM32H5F4 / Nucleo-style proxy | PR #15 digital GPIO input/output, EXTI; timer capture | 0 verified contacts; proxy artwork | Display only until board and mappings are verified |
| STM32U585 / U585 Nucleo-style proxy | PR #15 digital GPIO input/output and EXTI | 0 verified contacts; proxy artwork | Display only until board and mappings are verified |
| STM32L552 / NUCLEO-L552ZE-Q | PR #15 digital GPIO input/output and EXTI | 0 verified contacts | Display only until mappings are audited |
| Other ten profiles | No PR #15 signal pad adapter | 0 verified contacts except Pico 2 package pin signals | CPU firmware may be emulated; circuit wiring to MCU is disabled |
| Raspberry Pi Pico 2 package pads | No emulator GPIO signal adapter | 40 side-pad signals checked against official pinout; SWD provisional | Package metadata only; no firmware signal coupling |

The signal hooks provide digital `0/1/Z` output observation and digital level injection. They do not provide solved analog pad voltage/current, configurable output resistance, a generic analog input, ADC behavior, PWM waveforms, or a browser streaming API. The GPIO signal state and callbacks are process-wide. H563 timing uses one instruction per virtual cycle and output observations may be delayed to a translation-block boundary.

## Other browser-facing capabilities

- CPU, TrustZone and UART state are available through native emulator internals, but there is no supported typed browser API.
- No production C session API, WASM firmware smoke test, project schema implementation, web canvas, analog solver, circuit instrument, or browser regression fixture exists yet.
- No ngspice executable or development headers are installed. A system `libngspice.so.0` runtime is present, but it is native Linux only and cannot establish a WASM build path.
- The public EEcircuit `eecircuit-engine` API inspected at upstream `eelab-dev/EEcircuit-engine` exposes `setNetList()` and whole-run `runSim()`. Its implementation runs `source test.cir`, `destroy all`, `run`, then exports the completed `out.raw`; that interface does not expose incremental transient stepping or mid-run source updates. It is not suitable as-is for a coupled live worker. The package is MIT, but its ngspice WASM build and bundled model licenses still need separate audit before reuse.
- CircuitJS1 TypeScript is GPL-2.0-or-later. Its `CircuitMatrix` is a promising computational component, while `CirSim` imports menus, mouse handling, scopes, serializers, and model/UI classes. No standalone interactive kernel extraction or license combination review has passed.
- Board artwork in `../m33mu-ui-plan/m33mu_board_sprites.zip` is provisional original CC0 geometry. Import it when implementing the catalog; preserve disabled electrical mappings until vendor sources are checked.

## Immediate gates

1. Create an explicit Emscripten core target that excludes Linux networking and optional host integrations, then extract production H563 stepping and an in-memory ELF loader.
2. Verify the actual H563 pins selected for LED and RC/EXTI demos against the exact NUCLEO-H563ZI schematic and SoC pin table.
3. Benchmark controllable incremental stepping for ngspice WASM and CircuitJS TypeScript. Neither candidate has passed the required coordinated firmware/analog timing spike yet.
4. Document timing error and event ordering before making the RC/EXTI demo a correctness claim.
