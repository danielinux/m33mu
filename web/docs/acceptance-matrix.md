# Web acceptance matrix

Columns are the four ways firmware runs: the native CLI (`build/m33mu`), the native web session API (`include/m33mu/web.h`, via `build/web_parity_runner`), the shipped worker under Node/WASM, and the same worker in Chromium. "=" means a byte/state/event comparison against the CLI passes in CTest; a test name means behaviour is checked in that column only.

| Behaviour | CLI | Native session | Node/WASM worker | Chromium worker |
|---|---|---|---|---|
| ELF load, reset vector, initial SP | `cli_elf_loader_test` | = (`web_session_test`) | = (`wasm_worker_api_test`, CLI parity file) | `web_browser_chromium_test` |
| SysTick + WFI wake ordering, PendSV, SVC, handled UsageFault, CPU clock change | reference | = | = | = (`web_acceptance_matrix_test`) |
| Final r0-r3, cycle count, CPU Hz on the parity fixture | reference | = | = | = |
| Firmware `SYSRESETREQ` | `system_reset_test` | `web_session_test` | `wasm_worker_api_test` | — |
| Data/fetch fault routing (MemManage/HardFault, enabled/disabled) | — | `web_session_test` | `wasm_execution_state_test` (`web_session_wasm_smoke`) | — |
| Lockup (fault entry fails) | reference: PC 0xffffffff after 16385 cycles | `web_session_test`; a manual `web_parity_runner` run matched the CLI PC and cycles | worker reports `LOCKUP` | — |
| IRQ boundary (edge on next cycle does not preempt it) | reference | `web_session_test` | `wasm_worker_active_exti_boundary_test` | — |
| WFI with timestamped GPIO wake through EXTI | — | `web_session_test` | `wasm_worker_wfi_exti_test` | — |
| Timestamped GPIO input/output ordering, queue limits | `signal_test` | `web_session_test` | `wasm_worker_api_test` | — |
| Clock change rebases queued inputs | — | `web_session_test` | `wasm_execution_state_test` (`web_session_wasm_smoke`) | — |
| Pad mode/pull/drive reporting, `run_until` deadline and stop-on-pad-change, input cancellation | — | `web_session_test` | `wasm_worker_bench_rc_exti_test` | `web_browser_chromium_test` |
| Target device polling quantum (64 cycles), flash-stall charging | shared `mm_execution_state_*` | shared | shared | shared |
| Coupled RC/EXTI feedback timing | — | — | `wasm_worker_bench_rc_exti_test` (±3 ns of analytic) | — |
| LED blink and RC button demos, golden replay | — | — | `wasm_worker_demos_test` | `web_browser_chromium_test` |
| Load/reload/destroy, failure cleanup, heap growth | — | `web_session_test` | `wasm_worker_demos_test` (50 cycles, heap stable) | `web_browser_chromium_test` (3 worker instances) |

Run the differential columns with `scripts/test-web-acceptance-matrix.sh [result.json]`; it rebuilds `tests/firmware/test-stm32h563-web-parity`, the app and the worker, then compares all columns and writes `web/fixtures/acceptance/parity.json`.

## Known deviations

- **CLI translated blocks.** With translated blocks (the CLI default) the parity fixture stops 14 cycles earlier than with `M33MU_DISABLE_TB=1`, because interrupts are taken at block boundaries. The web scheduler steps instructions and matches the interpreter path; the matrix reports the TB column as informational.
- **Timing model.** All columns charge one virtual cycle per instruction plus flash stalls when flash timing is enabled; no pipeline, bus contention or wait-state model.
- **Single session.** One emulator session per process/worker; a second `mm_web_create` fails with `MM_WEB_SESSION_BUSY` (`web_session_test`).
