import { Circuit, SolverError } from '../sim/circuit.ts';
import { Bench } from '../sim/bench.ts';
import { validateNetlist } from '../sim/netlist.ts';
import { createMcuPort } from './mcu-port.mjs';

const ERROR_NAMES = [
  'OK', 'INVALID_ARGUMENT', 'UNSUPPORTED_TARGET', 'SESSION_BUSY',
  'OUT_OF_MEMORY', 'INITIALIZATION_FAILED', 'IMAGE_REJECTED', 'NOT_BOOTED',
  'EXECUTION_FAULT', 'UNSUPPORTED_INSTRUCTION', 'EVENT_IN_PAST',
  'EVENT_QUEUE_FULL'
];

const MAX_FIRMWARE_BYTES = 32 * 1024 * 1024;
const MAX_STEP_BUDGET = 100000;
const RUN_SLICE = 512;
const MAX_NETLIST_JSON = 1024 * 1024;
const MAX_PROBES = 32;
const BENCH_SLICE_MS = 12;
const BENCH_BATCH_SAMPLES = 4096;

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
  let bench = null;
  let benchSequence = 0;
  let benchFault = null;

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
        runSlice: wrap('mm_web_abi_run_slice', 'number',
          ['number', 'number', 'number']),
        runUntil: wrap('mm_web_abi_run_until', 'number',
          ['number', 'number', 'number', 'number', 'number', 'number']),
        deadlineReached: wrap('mm_web_abi_last_deadline_reached', 'number', []),
        padChanged: wrap('mm_web_abi_last_pad_changed', 'number', []),
        cancelInputs: wrap('mm_web_abi_cancel_gpio_inputs', 'number',
          ['number', 'string', 'number', 'number']),
        padState: wrap('mm_web_abi_pad_state', 'number', ['number', 'string']),
        gpioOutputConfig: wrap('mm_web_abi_gpio_output_config', 'number', []),
        hasPendingDeadline: wrap('mm_web_abi_has_pending_deadline', 'number',
          ['number']),
        reset: wrap('mm_web_abi_reset', 'number', ['number']),
        gpio: wrap('mm_web_abi_gpio_input', 'number',
          ['number', 'string', 'number', 'number', 'number']),
        gpioOutputNext: wrap('mm_web_abi_gpio_output_next', 'number', ['number']),
        gpioOutputPad: wrap('mm_web_abi_gpio_output_pad', 'string', []),
        gpioOutputLevel: wrap('mm_web_abi_gpio_output_level', 'number', []),
        gpioOutputTimeLow: wrap('mm_web_abi_gpio_output_time_low', 'number', []),
        gpioOutputTimeHigh: wrap('mm_web_abi_gpio_output_time_high', 'number', []),
        lastError: wrap('mm_web_abi_last_error', 'number', []),
        instructionCount: wrap('mm_web_abi_last_instruction_count', 'number', []),
        stopped: wrap('mm_web_abi_last_stopped', 'number', []),
        systemReset: wrap('mm_web_abi_last_system_reset', 'number', []),
        lockup: wrap('mm_web_abi_last_lockup', 'number', []),
        lockupPc: wrap('mm_web_abi_last_lockup_pc', 'number', []),
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

  function requireNoBench() {
    if (bench && bench.mcu) {
      throw Object.assign(new Error(
        'The MCU is coupled to a circuit; use runUntil or destroyBench'), { code: 'BENCH_ACTIVE' });
    }
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
      cycleTotal: ((BigInt(api.cyclesHigh(session) >>> 0) << 32n) |
        BigInt(api.cycles(session) >>> 0)).toString(),
      virtualTimeNs: ((BigInt(api.virtualTimeHigh(session) >>> 0) << 32n) |
        BigInt(api.virtualTimeLow(session) >>> 0)).toString()
    };
  }

  function reportLockup(requestId) {
    if (api.lockup() === 0) return false;
    const pc = (api.lockupPc() >>> 0).toString(16).padStart(8, '0');
    postEvent('fault', { requestId, code: 'LOCKUP',
      message: `Core locked up at PC 0x${pc}: a fault could not be taken; reset or reload to continue` });
    return true;
  }

  function emitState(requestId, stopped = false, extra = {}) {
    postEvent('state', { requestId, ...snapshot(), stopped, ...extra });
  }

  function drainGpioOutput(requestId) {
    for (;;) {
      const status = api.gpioOutputNext(session);
      if (status === 0) return true;
      if (status < 0) {
        const error = errorFromCode(api.lastError());
        postEvent('fault', { requestId, code: error.code, message: error.message });
        return false;
      }
      postEvent('gpio', {
        requestId, socPad: api.gpioOutputPad(), level: api.gpioOutputLevel(),
        eventTimeNs: ((BigInt(api.gpioOutputTimeHigh() >>> 0) << 32n) |
          BigInt(api.gpioOutputTimeLow() >>> 0)).toString()
      });
    }
  }

  function runSlice(generation, requestId) {
    if (generation !== runGeneration || !session) return;
    const status = api.runSlice(session, RUN_SLICE, RUN_SLICE);
    if (!drainGpioOutput(requestId)) return;
    if (status !== 0) {
      const error = errorFromCode(status);
      postEvent('fault', { requestId, code: error.code, message: error.message });
      return;
    }
    const stopped = api.stopped() !== 0;
    const systemReset = api.systemReset() !== 0;
    reportLockup(requestId);
    const state = snapshot();
    const sleepDeadlinePending = state.sleeping &&
      api.hasPendingDeadline(session) !== 0;
    emitState(requestId, stopped, {
      paused: state.sleeping && !sleepDeadlinePending, systemReset
    });
    if (!stopped && (!state.sleeping || sleepDeadlinePending) &&
        generation === runGeneration) {
      scope.setTimeout(() => runSlice(generation, requestId), 0);
    }
  }

  function invalid(message) {
    return Object.assign(new Error(message), { code: 'INVALID_ARGUMENT' });
  }

  function checkNetlist(netlist) {
    let size;
    try { size = JSON.stringify(netlist).length; } catch { size = Infinity; }
    if (size > MAX_NETLIST_JSON) throw invalid(`Netlist exceeds ${MAX_NETLIST_JSON} bytes`);
    const problems = validateNetlist(netlist);
    if (problems.length) {
      throw Object.assign(new Error(problems.join('; ')), { code: 'INVALID_NETLIST', problems });
    }
    return netlist.devices.filter((d) => d.kind === 'pad').map((d) => d.pad.socPad);
  }

  function parseTimeNs(value, name) {
    if (typeof value !== 'string' || !/^[0-9]{1,16}$/.test(value)) {
      throw invalid(`${name} must be a decimal nanosecond string`);
    }
    return Number(value);
  }

  function benchFlush(requestId, report) {
    const samples = bench.takeSamples();
    const events = bench.takeEvents();
    const width = bench.probes.length + 1;
    for (let start = 0; start < samples.length || start === 0; start += BENCH_BATCH_SAMPLES) {
      const chunk = samples.slice(start, start + BENCH_BATCH_SAMPLES);
      const flat = new Float64Array(chunk.length * width);
      chunk.forEach((row, i) => flat.set(row, i * width));
      scope.postMessage({
        schemaVersion: 1, kind: 'event', sequence: ++sequence, event: 'bench',
        payload: {
          requestId, benchSequence: ++benchSequence, probes: bench.probes.map((p) => p.id),
          samples: flat, sampleOverflow: bench.sampleOverflow,
          events: start === 0 ? events : [], observation: start === 0 ? bench.observe() : null,
          ...report
        }
      }, [flat.buffer]);
      if (samples.length === 0) break;
    }
  }

  function benchSlice(generation, requestId, targetNs, started) {
    if (generation !== runGeneration || !bench) return;
    const wall0 = Date.now();
    const sim0 = bench.timeNs;
    let report;
    benchFault = null;
    // Real-time pacing: never run ahead of maxRatio x wall-clock time.
    let sliceTarget = targetNs;
    if (started.maxRatio) {
      const allowed = started.sim + (wall0 - started.wall + BENCH_SLICE_MS) * 1e6 * started.maxRatio;
      if (allowed <= sim0) {
        scope.setTimeout(() => benchSlice(generation, requestId, targetNs, started), BENCH_SLICE_MS);
        return;
      }
      sliceTarget = Math.min(targetNs, allowed);
    }
    try {
      do {
        report = bench.runUntil(sliceTarget, 8);
        if (benchFault) throw benchFault;
      } while (!report.reachedTarget && !report.mcuStopped &&
               Date.now() - wall0 < BENCH_SLICE_MS);
      if (report.reachedTarget && report.timeNs < targetNs) report = { ...report, reachedTarget: false };
    } catch (error) {
      const code = error instanceof SolverError ? error.code : (error.code || 'BENCH_FAILED');
      postEvent('fault', { requestId, code, message: error.message });
      ++runGeneration;
      return;
    }
    const wallMs = Math.max(Date.now() - started.wall, 1);
    benchFlush(requestId, {
      timeNs: Math.round(report.timeNs).toString(),
      reachedTarget: report.reachedTarget, mcuStopped: report.mcuStopped,
      systemReset: report.systemReset, rollbacks: report.rollbacks,
      solverSteps: bench.circuit.stepCount, rejectedSteps: bench.circuit.rejected,
      simulatedPerWall: (report.timeNs - started.sim) * 1e-9 / (wallMs * 1e-3),
      sliceSimulatedNs: report.timeNs - sim0
    });
    if (bench.mcu) {
      if (report.mcuStopped) reportLockup(requestId);
      emitState(requestId, report.mcuStopped, { paused: report.reachedTarget });
    }
    if (!report.reachedTarget && !report.mcuStopped && generation === runGeneration) {
      scope.setTimeout(() => benchSlice(generation, requestId, targetNs, started), 0);
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
        bench = null;
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
        if (!drainGpioOutput(requestId)) return;
        emitState(requestId);
        return;
      }
      case 'step': {
        await ensureSession();
        requireFirmware();
        requireNoBench();
        const budget = payload.instructionBudget;
        if (!Number.isInteger(budget) || budget < 1 || budget > MAX_STEP_BUDGET) {
          throw Object.assign(new Error('instructionBudget must be 1..100000'),
            { code: 'INVALID_ARGUMENT' });
        }
        ++runGeneration;
        const status = api.step(session, budget);
        if (!drainGpioOutput(requestId)) return;
        if (status !== 0) throw errorFromCode(status);
        const lockup = reportLockup(requestId);
        const result = {
          instructions: api.instructionCount(),
          stopped: api.stopped() !== 0,
          lockup,
          systemReset: api.systemReset() !== 0,
          ...snapshot()
        };
        postResponse(requestId, result);
        emitState(requestId, result.stopped);
        return;
      }
      case 'run': {
        await ensureSession();
        requireFirmware();
        requireNoBench();
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
        bench = null;
        const status = api.reset(session);
        if (status !== 0) throw errorFromCode(status);
        postResponse(requestId, { reset: true });
        if (!drainGpioOutput(requestId)) return;
        emitState(requestId);
        return;
      }
      case 'gpioWrite': {
        await ensureSession();
        requireFirmware();
        requireNoBench();
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
        if (!drainGpioOutput(requestId)) return;
        return;
      }
      case 'compileNetlist': {
        const pads = checkNetlist(payload.netlist);
        postResponse(requestId, { valid: true, devices: payload.netlist.devices.length, pads });
        return;
      }
      case 'createBench': {
        const pads = checkNetlist(payload.netlist);
        const probes = payload.probes ?? [];
        if (!Array.isArray(probes) || probes.length > MAX_PROBES) {
          throw invalid(`probes must be an array of at most ${MAX_PROBES}`);
        }
        const options = payload.options ?? {};
        for (const key of Object.keys(options)) {
          if (key === 'initial' && (options.initial === 'dc' || options.initial === 'zero')) continue;
          if (!['lookaheadNs', 'sampleNs', 'maxSamples'].includes(key) ||
              !(Number.isFinite(options[key]) && options[key] > 0)) {
            throw invalid(`Unsupported or invalid bench option ${key}`);
          }
        }
        const solver = payload.solver ?? {};
        for (const key of Object.keys(solver)) {
          if (!['hMax', 'reltol', 'eventTol'].includes(key) ||
              !(Number.isFinite(solver[key]) && solver[key] > 0)) {
            throw invalid(`Unsupported or invalid solver option ${key}`);
          }
        }
        ++runGeneration;
        bench = null;
        let port = null;
        if (pads.length > 0) {
          await ensureSession();
          requireFirmware();
          port = createMcuPort(api, session, (status) => {
            benchFault = benchFault || errorFromCode(status);
          });
          // GPIO events already emitted belong to the MCU-only timeline.
          drainGpioOutput(requestId);
        }
        const circuit = new Circuit(payload.netlist, solver);
        bench = new Bench(circuit, port, probes, options);
        if (benchFault) { const f = benchFault; bench = null; throw f; }
        postResponse(requestId, {
          created: true, nets: circuit.netNames, pads, probes: probes.map((p) => p.id),
          timeNs: Math.round(bench.timeNs).toString()
        });
        benchFlush(requestId, { timeNs: Math.round(bench.timeNs).toString(), reachedTarget: true,
          mcuStopped: false, systemReset: false, rollbacks: 0, solverSteps: 0, rejectedSteps: 0,
          simulatedPerWall: 0, sliceSimulatedNs: 0 });
        return;
      }
      case 'runUntil': {
        if (!bench) throw Object.assign(new Error('Create a bench first'), { code: 'NO_BENCH' });
        const targetNs = payload.targetNs === undefined ? Number.MAX_SAFE_INTEGER :
          parseTimeNs(payload.targetNs, 'targetNs');
        const generation = ++runGeneration;
        postResponse(requestId, { running: true });
        let maxRatio = 0;
        if (payload.maxRatio !== undefined) {
          if (!(Number.isFinite(payload.maxRatio) && payload.maxRatio > 0)) throw invalid('maxRatio must be > 0');
          maxRatio = payload.maxRatio;
        }
        const started = { wall: Date.now(), sim: bench.timeNs, maxRatio };
        scope.setTimeout(() => benchSlice(generation, requestId, targetNs, started), 0);
        return;
      }
      case 'setSwitch': {
        if (!bench) throw Object.assign(new Error('Create a bench first'), { code: 'NO_BENCH' });
        if (typeof payload.device !== 'string' || typeof payload.closed !== 'boolean') {
          throw invalid('setSwitch requires device and boolean closed');
        }
        const dev = bench.circuit.byId.get(payload.device);
        if (!dev || dev.kind !== 'switch') throw invalid(`No switch ${payload.device}`);
        const changed = bench.setSwitch(payload.device, payload.closed);
        postResponse(requestId, { changed, timeNs: Math.round(bench.timeNs).toString() });
        return;
      }
      case 'destroyBench':
        ++runGeneration;
        bench = null;
        postResponse(requestId, { destroyed: true });
        return;
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
      bench = null;
      if (session) api.destroy(session);
      session = 0;
    }
  };
}
