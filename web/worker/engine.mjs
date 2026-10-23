const ERROR_NAMES = [
  'OK', 'INVALID_ARGUMENT', 'UNSUPPORTED_TARGET', 'SESSION_BUSY',
  'OUT_OF_MEMORY', 'INITIALIZATION_FAILED', 'IMAGE_REJECTED', 'NOT_BOOTED',
  'EXECUTION_FAULT', 'UNSUPPORTED_INSTRUCTION', 'EVENT_IN_PAST',
  'EVENT_QUEUE_FULL'
];

const MAX_FIRMWARE_BYTES = 32 * 1024 * 1024;
const MAX_STEP_BUDGET = 100000;
const RUN_SLICE = 512;

function requestError(message) {
  return message && message.kind === 'request' &&
    message.schemaVersion === 1 && Number.isSafeInteger(message.requestId) &&
    message.requestId > 0 && typeof message.operation === 'string' &&
    message.payload && typeof message.payload === 'object';
}

export function attachM33muWorker(scope, createModule) {
  let modulePromise;
  let api;
  let session = 0;
  let firmwareLoaded = false;
  let sequence = 0;
  let runGeneration = 0;

  function postResponse(requestId, result) {
    scope.postMessage({
      schemaVersion: 1, kind: 'response', requestId, ok: true, result
    });
  }

  function postError(requestId, code, message) {
    scope.postMessage({
      schemaVersion: 1, kind: 'response', requestId, ok: false,
      error: { code, message }
    });
  }

  function postEvent(event, payload) {
    scope.postMessage({
      schemaVersion: 1, kind: 'event', sequence: ++sequence, event, payload
    });
  }

  async function loadApi() {
    if (!modulePromise) {
      modulePromise = createModule();
    }
    const wasm = await modulePromise;
    if (!api) {
      const wrap = (name, result, args) => wasm.cwrap(name, result, args);
      api = {
        create: wrap('mm_web_abi_create', 'number', []),
        load: wrap('mm_web_abi_load', 'number', ['number', 'number', 'number']),
        step: wrap('mm_web_abi_step', 'number', ['number', 'number']),
        reset: wrap('mm_web_abi_reset', 'number', ['number']),
        gpio: wrap('mm_web_abi_gpio_input', 'number',
          ['number', 'string', 'number', 'number', 'number']),
        lastError: wrap('mm_web_abi_last_error', 'number', []),
        instructionCount: wrap('mm_web_abi_last_instruction_count', 'number', []),
        stopped: wrap('mm_web_abi_last_stopped', 'number', []),
        reg: wrap('mm_web_abi_cpu_reg', 'number', ['number', 'number']),
        cycles: wrap('mm_web_abi_cycle_low', 'number', ['number']),
        cyclesHigh: wrap('mm_web_abi_cycle_high', 'number', ['number']),
        xpsr: wrap('mm_web_abi_xpsr', 'number', ['number']),
        mspS: wrap('mm_web_abi_msp_s', 'number', ['number']),
        mspNs: wrap('mm_web_abi_msp_ns', 'number', ['number']),
        pspS: wrap('mm_web_abi_psp_s', 'number', ['number']),
        pspNs: wrap('mm_web_abi_psp_ns', 'number', ['number']),
        securityState: wrap('mm_web_abi_security_state', 'number', ['number']),
        mode: wrap('mm_web_abi_mode', 'number', ['number']),
        sleeping: wrap('mm_web_abi_sleeping', 'number', ['number']),
        cpuHz: wrap('mm_web_abi_cpu_hz', 'number', ['number']),
        virtualTimeLow: wrap('mm_web_abi_virtual_time_low', 'number', ['number']),
        virtualTimeHigh: wrap('mm_web_abi_virtual_time_high', 'number', ['number']),
        destroy: wrap('mm_web_abi_destroy', null, ['number'])
      };
    }
    return wasm;
  }

  async function ensureSession() {
    const wasm = await loadApi();
    if (!session) {
      session = api.create();
      if (!session) {
        const code = ERROR_NAMES[api.lastError()] || 'INITIALIZATION_FAILED';
        throw Object.assign(new Error(`Could not create H563 session (${code})`), { code });
      }
    }
    return wasm;
  }

  function requireFirmware() {
    if (!firmwareLoaded) {
      throw Object.assign(new Error('Load firmware before running the MCU'),
        { code: 'NOT_BOOTED' });
    }
  }

  function errorFromCode(code) {
    const name = ERROR_NAMES[code] || 'INITIALIZATION_FAILED';
    return Object.assign(new Error(`m33mu operation failed (${name})`), { code: name });
  }

  function snapshot() {
    const registers = Array.from({ length: 16 }, (_, index) =>
      api.reg(session, index));
    return {
      registers,
      r0: registers[0],
      pc: registers[15],
      xpsr: api.xpsr(session),
      stacks: {
        mspSecure: api.mspS(session),
        mspNonsecure: api.mspNs(session),
        pspSecure: api.pspS(session),
        pspNonsecure: api.pspNs(session)
      },
      securityState: api.securityState(session),
      mode: api.mode(session),
      sleeping: api.sleeping(session) !== 0,
      cpuHz: api.cpuHz(session),
      cycleTotal: ((BigInt(api.cyclesHigh(session)) << 32n) |
        BigInt(api.cycles(session))).toString(),
      virtualTimeNs: ((BigInt(api.virtualTimeHigh(session)) << 32n) |
        BigInt(api.virtualTimeLow(session))).toString()
    };
  }

  function emitState(requestId, stopped = false, extra = {}) {
    postEvent('state', { requestId, ...snapshot(), stopped, ...extra });
  }

  function runSlice(generation, requestId) {
    if (generation !== runGeneration || !session) return;
    const status = api.step(session, RUN_SLICE);
    if (status !== 0) {
      const error = errorFromCode(status);
      postEvent('fault', { requestId, code: error.code, message: error.message });
      return;
    }
    const stopped = api.stopped() !== 0;
    emitState(requestId, stopped);
    if (!stopped && generation === runGeneration) {
      scope.setTimeout(() => runSlice(generation, requestId), 0);
    }
  }

  async function dispatch(message) {
    if (!requestError(message)) {
      postError(message && Number.isSafeInteger(message.requestId) ?
        message.requestId : 0, 'INVALID_REQUEST', 'Malformed worker request');
      return;
    }
    const { requestId, operation, payload } = message;
    try {
      switch (operation) {
      case 'loadFirmware': {
        let bytes = payload.bytes;
        if (bytes instanceof ArrayBuffer) bytes = new Uint8Array(bytes);
        if (!(bytes instanceof Uint8Array) || bytes.byteLength === 0 ||
            bytes.byteLength > MAX_FIRMWARE_BYTES) {
          throw Object.assign(new Error('Firmware must be a non-empty ELF up to 32 MiB'),
            { code: 'IMAGE_REJECTED' });
        }
        const wasm = await ensureSession();
        ++runGeneration;
        const ptr = wasm._malloc(bytes.byteLength);
        if (!ptr) throw Object.assign(new Error('WASM memory allocation failed'),
          { code: 'OUT_OF_MEMORY' });
        try {
          wasm.HEAPU8.set(bytes, ptr);
          const status = api.load(session, ptr, bytes.byteLength);
          if (status !== 0) throw errorFromCode(status);
          firmwareLoaded = true;
        } finally {
          wasm._free(ptr);
        }
        postResponse(requestId, { loaded: true, format: 'elf32-arm' });
        emitState(requestId);
        return;
      }
      case 'step': {
        await ensureSession();
        requireFirmware();
        const budget = payload.instructionBudget;
        if (!Number.isInteger(budget) || budget < 1 || budget > MAX_STEP_BUDGET) {
          throw Object.assign(new Error('instructionBudget must be 1..100000'),
            { code: 'INVALID_ARGUMENT' });
        }
        ++runGeneration;
        const status = api.step(session, budget);
        if (status !== 0) throw errorFromCode(status);
        const result = {
          instructions: api.instructionCount(),
          stopped: api.stopped() !== 0,
          ...snapshot()
        };
        postResponse(requestId, result);
        emitState(requestId, result.stopped);
        return;
      }
      case 'run': {
        await ensureSession();
        requireFirmware();
        const generation = ++runGeneration;
        postResponse(requestId, { running: true, sliceInstructions: RUN_SLICE });
        scope.setTimeout(() => runSlice(generation, requestId), 0);
        return;
      }
      case 'pause':
        ++runGeneration;
        postResponse(requestId, { paused: true });
        emitState(requestId, false, { paused: true });
        return;
      case 'reset': {
        await ensureSession();
        requireFirmware();
        ++runGeneration;
        const status = api.reset(session);
        if (status !== 0) throw errorFromCode(status);
        postResponse(requestId, { reset: true });
        emitState(requestId);
        return;
      }
      case 'gpioWrite': {
        await ensureSession();
        requireFirmware();
        if (typeof payload.socPad !== 'string' ||
            typeof payload.eventTimeNs !== 'string' ||
            !/^[0-9]+$/.test(payload.eventTimeNs) ||
            typeof payload.level !== 'boolean') {
          throw Object.assign(new Error(
            'gpioWrite requires socPad, decimal eventTimeNs, and boolean level'),
            { code: 'INVALID_ARGUMENT' });
        }
        const eventTime = BigInt(payload.eventTimeNs);
        if (eventTime > 0xffffffffffffffffn) {
          throw Object.assign(new Error('eventTimeNs exceeds uint64'),
            { code: 'INVALID_ARGUMENT' });
        }
        const status = api.gpio(
          session, payload.socPad,
          Number((eventTime >> 32n) & 0xffffffffn),
          Number(eventTime & 0xffffffffn), payload.level ? 1 : 0);
        if (status !== 0) throw errorFromCode(status);
        postResponse(requestId, {
          accepted: true, socPad: payload.socPad,
          eventTimeNs: eventTime.toString()
        });
        return;
      }
      default:
        throw Object.assign(new Error(`Unsupported operation: ${operation}`),
          { code: 'UNSUPPORTED_OPERATION' });
      }
    } catch (error) {
      postError(requestId, error.code || 'INITIALIZATION_FAILED', error.message);
    }
  }

  scope.onmessage = event => { void dispatch(event.data); };
  return {
    dispose() {
      ++runGeneration;
      if (session) api.destroy(session);
      session = 0;
    }
  };
}
