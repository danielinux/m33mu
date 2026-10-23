# H563 headless worker

Build the worker bundle with:

```sh
scripts/build-web-worker.sh /path/to/output
```

The output contains `worker.mjs`, `engine.mjs`, and the Emscripten `m33mu_web_api.js` / `.wasm` pair. Serve them with the worker entry as an ES module. `scripts/test-web-worker.sh` builds the same module and runs the actual worker entry under Node with a Worker-like message scope.

When `arm-none-eabi-gcc` is available, CTest also runs `scripts/test-web-worker-wfi.sh`. It builds a real H563 firmware fixture that configures PA6 rising-edge EXTI, enters WFI, receives a timestamped worker GPIO input, wakes through the interrupt, and stops at its success BKPT.

The handler implements the v1 worker envelope for `loadFirmware`, bounded `step`, yielding `run`, `pause`, `reset`, and timestamped `gpioWrite`. A run advances in slices bounded to 512 scheduler work units, including sleep-time advancement, and yields to the worker event loop between slices. It advances through queued GPIO events and future SysTick/signal deadlines while a core sleeps; after the deadline, it reports paused if no enabled interrupt wakes the core. An idle WFI/WFE core with no pending deadline reports paused. Worker state events serialize cycle count and virtual nanoseconds as decimal strings and include the configured CPU clock in hertz. Output pad transitions are emitted as ordered `gpio` events after each slice with their virtual nanosecond timestamp and level `0`, `1`, or `2` (`Z`).

This is an initial headless P0 endpoint. The C bridge currently supports H563 ELF, stops at BKPT, and reports unsupported faults/exceptions. `gpioWrite` requires `{ socPad, level, eventTimeNs }`; `eventTimeNs` is an unsigned decimal string in the session virtual-time domain. Inputs are queued (up to 64 pending events), ordered by cycle and request order for ties, and applied before instruction fetch at the first instruction boundary at or after the timestamp. A pad stimulus is accepted even when the current GPIO mode hides it from IDR. Events older than current virtual time are rejected. Submit future inputs before starting a synchronous `step` or `run` slice; the worker cannot receive messages until that slice yields. Output events use a 4096-transition queue; overflow produces a fault rather than silently presenting a complete trace. Full asynchronous interrupt/exception scheduling, UART, and the analog solver are not implemented yet.
