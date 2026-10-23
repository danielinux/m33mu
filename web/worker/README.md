# H563 headless worker

Build the worker bundle with:

```sh
scripts/build-web-worker.sh /path/to/output
```

The output contains `worker.mjs` (the worker entry bundled with `engine.mjs`, `mcu-port.mjs` and the TypeScript circuit solver in `web/sim/` by `scripts/bundle-web-worker.mjs`, which uses the esbuild that ships with Vite) and the Emscripten `m33mu_web_api.js` / `.wasm` pair. Serve them with the worker entry as an ES module. `scripts/test-web-worker.sh` builds the same module and runs the actual worker entry under Node with a Worker-like message scope.

When `arm-none-eabi-gcc` is available, CTest also runs `scripts/test-web-worker-wfi.sh`. It builds a real H563 firmware fixture that configures PA6 rising-edge EXTI, enters WFI, receives a timestamped worker GPIO input, wakes through the interrupt, and stops at its success BKPT.

The handler implements the v1 worker envelope for `loadFirmware`, bounded `step`, yielding `run`, `pause`, `reset`, and timestamped `gpioWrite`. A run advances in slices bounded to 512 scheduler work units, including sleep-time advancement, and yields to the worker event loop between slices. It advances through queued GPIO events and future SysTick/signal deadlines while a core sleeps; after the deadline, it reports paused if no enabled interrupt wakes the core. An idle WFI/WFE core with no pending deadline reports paused. Firmware `SYSRESETREQ` restarts the H563 session at its reset vector while preserving virtual signal time and queued future GPIO events; the triggering step result and run state event carry `systemReset: true`. The explicit worker `reset` operation starts a fresh external timeline. Worker state events serialize cycle count and virtual nanoseconds as decimal strings and include the configured CPU clock in hertz. Output pad transitions are emitted as ordered `gpio` events after each slice with their virtual nanosecond timestamp and level `0`, `1`, or `2` (`Z`).

This is an initial headless P0 endpoint. The C bridge currently supports H563 ELF, stops at BKPT, and reports unsupported faults/exceptions. `gpioWrite` requires `{ socPad, level, eventTimeNs }`; `eventTimeNs` is an unsigned decimal string in the session virtual-time domain. Inputs are queued (up to 64 pending events), ordered by cycle and request order for ties, and applied before instruction fetch at the first instruction boundary at or after the timestamp. A pad stimulus is accepted even when the current GPIO mode hides it from IDR. Events older than current virtual time are rejected. Submit future inputs before starting a synchronous `step` or `run` slice; the worker cannot receive messages until that slice yields. Output events use a 4096-transition queue; overflow produces a fault rather than silently presenting a complete trace. Full asynchronous interrupt/exception scheduling, UART, and the analog solver are not implemented yet.

## Circuit bench

The worker also owns the electrical bench: one MCU session, one analog circuit, the authoritative virtual clock and bounded waveform buffers.

- `compileNetlist { netlist }` validates a v1 netlist (`web/sim/netlist.ts`) without allocating a solver; errors come back as `INVALID_NETLIST` with every problem listed. Netlists over 1 MiB of JSON are rejected.
- `createBench { netlist, probes?, options?, solver? }` compiles the circuit, takes the MCU pad states, solves the DC operating point and delivers initial input levels. A netlist with `pad` devices needs loaded firmware (`NOT_BOOTED` otherwise). Up to 32 probes: `voltage` (net), `current` (device) or `pad`. Options: `lookaheadNs`, `sampleNs`, `maxSamples`.
- `runUntil { targetNs? }` advances the coupled bench in 12 ms wall-clock slices, yielding between slices; omit `targetNs` to run until paused. Each slice posts a `bench` event with a monotonic `benchSequence`, transferable `samples` (time ns followed by one value per probe), `sampleOverflow`, `events` (pad drives, crossings, GPIO, switch operations and `unsupported` notices), solver step/rejection/rollback counts and the simulated-per-wall-second ratio. Events are released only once they can no longer be retracted.
- `setSwitch { device, closed }` operates a switch at the current bench time.
- `destroyBench` drops the bench. `loadFirmware` and `reset` also drop it; MCU-only `step`, `run` and `gpioWrite` are rejected with `BENCH_ACTIVE` while a coupled bench exists.

See `web/sim/bench.ts` for the scheduling and rounding rules and `web/docs/electrical-model.md` for the pad model.
