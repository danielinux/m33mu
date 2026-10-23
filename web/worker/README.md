# H563 headless worker

Build the worker bundle with:

```sh
scripts/build-web-worker.sh /path/to/output
```

The output contains `worker.mjs`, `engine.mjs`, and the Emscripten `m33mu_web_api.js` / `.wasm` pair. Serve them with the worker entry as an ES module. `scripts/test-web-worker.sh` builds the same module and runs the actual worker entry under Node with a Worker-like message scope.

The handler implements the v1 worker envelope for `loadFirmware`, bounded `step`, yielding `run`, `pause`, `reset`, and current-time `gpioWrite`. A run advances in 512-instruction slices and yields to the worker event loop between slices. Worker state events serialize the 64-bit cycle count as a decimal string.

This is an initial headless P0 endpoint. The C bridge currently supports H563 ELF, stops at BKPT, and reports unsupported faults/exceptions. GPIO writes are immediate; timed event queues, full asynchronous interrupt/exception scheduling, output GPIO event streaming, UART, and the analog solver are not implemented yet.
