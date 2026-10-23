// External live shared-clock probe: m33mu WASM H563 <-> Perun RC.
import { readFileSync, writeFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { pathToFileURL } from 'node:url';
import { readCircuit } from '@perun/format';
import { CapacitorElm, VoltageElm } from '../../../packages/elements/src/index.ts';

const HZ = 64_000_000;
const RISE_MV = 1775;
const FALL_MV = 1525;
const wasmModulePath = process.env.M33MU_WASM_MODULE!;
const firmwarePath = process.env.M33MU_FIRMWARE!;
const resultPath = process.env.M33MU_LIVE_RESULT!;

describe('live m33mu WASM and Perun shared-clock RC feedback', () => {
  it('returns analog crossings to H563 EXTI while preserving cycle order', async () => {
    const factory = (await import(pathToFileURL(wasmModulePath).href)).default;
    const wasm = await factory();
    const wrap = (name: string, result: string | null, args: string[]) =>
      wasm.cwrap(name, result, args);
    const api = {
      create: wrap('mm_web_abi_create', 'number', []),
      load: wrap('mm_web_abi_load', 'number', ['number', 'number', 'number']),
      step: wrap('mm_web_abi_step', 'number', ['number', 'number']),
      gpio: wrap('mm_web_abi_gpio_input', 'number', ['number', 'string', 'number', 'number', 'number']),
      next: wrap('mm_web_abi_gpio_output_next', 'number', ['number']),
      pad: wrap('mm_web_abi_gpio_output_pad', 'string', []),
      level: wrap('mm_web_abi_gpio_output_level', 'number', []),
      outputTimeLo: wrap('mm_web_abi_gpio_output_time_low', 'number', []),
      outputTimeHi: wrap('mm_web_abi_gpio_output_time_high', 'number', []),
      stopped: wrap('mm_web_abi_last_stopped', 'number', []),
      cycleLo: wrap('mm_web_abi_cycle_low', 'number', ['number']),
      cycleHi: wrap('mm_web_abi_cycle_high', 'number', ['number']),
      destroy: wrap('mm_web_abi_destroy', null, ['number']),
    };
    const session = api.create();
    expect(session).toBeTruthy();
    const firmware = readFileSync(firmwarePath);
    const ptr = wasm._malloc(firmware.length);
    expect(ptr).toBeTruthy();
    wasm.HEAPU8.set(firmware, ptr);
    expect(api.load(session, ptr, firmware.length)).toBe(0);
    wasm._free(ptr);

    const circuitText = readFileSync(new URL('../circuits/rc-charge.txt', import.meta.url), 'utf8')
      .replace('1.0E-5', '1.0E-8');
    const circuit = readCircuit(circuitText);
    const source = circuit.elements.find((e): e is VoltageElm => e instanceof VoltageElm)!;
    const capacitor = circuit.elements.find((e): e is CapacitorElm => e instanceof CapacitorElm)!;
    circuit.sim.dcAnalysisFlag = false;
    circuit.sim.timeStep = 1 / HZ;
    circuit.sim.maxTimeStep = 1 / HZ;
    circuit.sim.minTimeStep = 1 / HZ;
    circuit.sim.adjustTimeStep = false;
    source.maxVoltage = 0;
    source.bias = 0;

    const cycles = () => (BigInt(api.cycleHi(session)) << 32n) | BigInt(api.cycleLo(session));
    const edges: { cycle: number; timeNs: string; level: number }[] = [];
    const pendingDriveEvents: { cycle: number; level: number }[] = [];
    const drives: { cycle: number; level: number; initialVoltage: number }[] = [];
    const crossings: { cycle: number; fractionalCycle: number; level: number; mv: number }[] = [];
    let digital = 0;
    let simCycle = 0;
    let previousMv = 0;
    const drain = () => {
      for (;;) {
        const rc = api.next(session);
        if (rc === 0) break;
        expect(rc).toBe(1);
        expect(api.pad()).toBe('PA5');
        const timeNs = (BigInt(api.outputTimeHi()) << 32n) |
          BigInt(api.outputTimeLo());
        const eventCycle = Number((timeNs * BigInt(HZ) + 999_999_999n) /
          1_000_000_000n);
        edges.push({ cycle: eventCycle, timeNs: timeNs.toString(), level: api.level() });
        pendingDriveEvents.push({ cycle: eventCycle, level: api.level() });
      }
    };
    const applyDrivesAtBoundary = () => {
      while (pendingDriveEvents.length > 0 &&
             pendingDriveEvents[0]!.cycle <= simCycle) {
        const event = pendingDriveEvents.shift()!;
        expect(event.cycle).toBe(simCycle);
        source.maxVoltage = 0;
        source.bias = event.level ? 3.3 : 0;
        circuit.sim.analyzeFlag = true;
        drives.push({ cycle: simCycle, level: event.level,
          initialVoltage: capacitor.voltdiff });
      }
    };
    const advanceSolverTo = (target: number) => {
      while (simCycle < target) {
        expect(circuit.sim.step(1)).toBe(1);
        simCycle++;
        const mv = capacitor.voltdiff * 1000;
        const threshold = digital ? FALL_MV : RISE_MV;
        const didCross = digital ? previousMv > threshold && mv <= threshold : previousMv < threshold && mv >= threshold;
        if (didCross) {
          digital = digital ? 0 : 1;
          const fraction = (threshold - previousMv) / (mv - previousMv);
          const fractionalCycle = simCycle - 1 + fraction;
          const cycle = Math.ceil(fractionalCycle);
          crossings.push({ cycle, fractionalCycle, level: digital, mv });
          const ns = BigInt(Math.floor(cycle * 1e9 / HZ));
          expect(api.gpio(session, 'PA6', Number(ns >> 32n), Number(ns & 0xffffffffn), digital)).toBe(0);
        }
        previousMv = mv;
        applyDrivesAtBoundary();
      }
    };

    try {
      drain();
      for (let guard = 0; guard < 250_000 && !api.stopped(); guard++) {
        const before = Number(cycles());
        advanceSolverTo(before + 1);
        expect(api.step(session, 1)).toBe(0);
        drain();
        const after = Number(cycles());
        expect(after).toBeGreaterThanOrEqual(before);
        applyDrivesAtBoundary();
        advanceSolverTo(after);
      }
      expect(api.stopped()).toBe(1);
      expect(crossings.length).toBeGreaterThanOrEqual(4);
      expect(edges.filter((e) => e.level === 1).length).toBeGreaterThanOrEqual(3);
      expect(edges.length).toBeGreaterThanOrEqual(5);
      expect(pendingDriveEvents).toHaveLength(0);
      const crossingErrors = crossings.map((crossing) => {
        const drive = drives.filter((event) => event.cycle <= crossing.fractionalCycle).at(-1)!;
        const targetV = (crossing.level ? RISE_MV : FALL_MV) / 1000;
        const sourceV = drive.level ? 3.3 : 0;
        const tauSeconds = 1000 * 1e-8;
        const elapsedSeconds = drive.level ?
          -tauSeconds * Math.log((sourceV - targetV) / (sourceV - drive.initialVoltage)) :
          -tauSeconds * Math.log(targetV / drive.initialVoltage);
        const expectedCycle = drive.cycle + elapsedSeconds * HZ;
        return { cycle: crossing.fractionalCycle, expectedCycle,
          errorCycles: crossing.fractionalCycle - expectedCycle };
      });
      for (const error of crossingErrors) expect(Math.abs(error.errorCycles)).toBeLessThan(1);
      for (const [index, crossing] of crossings.slice(0, 4).entries()) {
        const response = edges.find((edge, edgeIndex) =>
          edgeIndex >= 2 && edge.cycle > crossing.cycle);
        expect(response).toBeDefined();
        expect(response!.level).toBe(1 - crossing.level);
        expect(response!.cycle - crossing.cycle).toBeGreaterThan(0);
        expect(response!.cycle - crossing.cycle).toBeLessThanOrEqual(16);
        if (index > 0) expect(response!.cycle).toBeGreaterThan(edges[index + 1]!.cycle);
      }
      expect(Math.abs(Number(cycles()) - simCycle)).toBeLessThanOrEqual(1);
      writeFileSync(resultPath, JSON.stringify({ solver: 'Perun', hz: HZ, quantumNs: 1e9 / HZ,
        thresholdsMv: { rise: RISE_MV, fall: FALL_MV }, drives, crossings, crossingErrors, outputEdges: edges,
        finalCycle: Number(cycles()), finalSolverCycle: simCycle }, null, 2));
    } finally {
      api.destroy(session);
    }
  }, 60_000);
});
