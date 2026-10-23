// External linear-solver acceptance probe. This file is copied into a pinned Perun checkout by
// run-web-perun-acceptance-probe.sh; Perun source is not imported into m33mu.
import { writeFileSync } from 'node:fs';
import { readCircuit } from '@perun/format';
import { CapacitorElm, DiodeElm, ResistorElm, TransistorElm, VoltageElm } from '../../../packages/elements/src/index.ts';
import { describe, expect, it } from 'vitest';

const resultPath = process.env.M33MU_PERUN_ACCEPTANCE_RESULT!;
const tau = 1000 * 10e-6;
const dividerText = `$ 1 5.0E-6 10 50 5.0 50\nv 96 320 96 96 0 0 40.0 5.0 0.0\nw 96 96 256 96 0\nr 256 96 256 208 0 2000.0\nr 256 208 256 320 0 3000.0\nw 96 320 256 320 0\ng 96 320 96 352 0\n`;
const rcText = `$ 1 1.0E-6 10 50 5.0 50\nv 96 320 96 96 0 0 40.0 1.0 0.0\nr 96 96 256 96 0 1000.0\nc 256 96 256 320 0 1.0E-5 0.0\nw 96 320 256 320 0\ng 96 320 96 352 0\n`;
const diodeText = `$ 1 5.0E-6 10 50 5.0 50\nv 96 320 96 96 0 0 40.0 5.0 0.0\nd 96 96 256 96 2 default\nr 256 96 256 320 0 1000.0\nw 96 320 256 320 0\ng 96 320 96 352 0\n`;
const bjtText = `$ 1 5.0E-6 10 50 5.0 50\nR 352 96 352 64 0 0 40.0 12.0 0.0 0.0 0.5\nr 352 96 352 224 0 1000.0\nw 352 96 208 96 0\nr 208 96 208 240 0 220000.0\nw 208 240 304 240 0\nt 304 240 352 240 0 1 0 0 100 default\ng 352 256 352 288 0\n`;

describe('pinned Perun linear acceptance', () => {
  it('meets the DC divider and 1 us RC analytic criteria', () => {
    const divider = readCircuit(dividerText);
    expect(divider.warnings).toEqual([]);
    const lower = divider.elements.find((e): e is ResistorElm =>
      e instanceof ResistorElm && e.resistance === 3000);
    expect(lower).toBeDefined();
    expect(divider.sim.step(1)).toBe(1);
    const dividerVoltage = lower!.volts[0]!;
    const dividerRelativeError = Math.abs(dividerVoltage - 3) / 3;
    expect(dividerRelativeError).toBeLessThanOrEqual(1e-6);

    const rc = readCircuit(rcText);
    expect(rc.warnings).toEqual([]);
    rc.sim.timeStep = 1e-6;
    rc.sim.maxTimeStep = 1e-6;
    rc.sim.minTimeStep = 1e-6;
    rc.sim.adjustTimeStep = false;
    const source = rc.elements.find((e): e is VoltageElm => e instanceof VoltageElm)!;
    const capacitor = rc.elements.find((e): e is CapacitorElm => e instanceof CapacitorElm)!;
    let maxAbsoluteError = 0;
    let referencePeak = 0;
    for (let i = 0; i < 500; i++) {
      expect(rc.sim.step(1)).toBe(1);
      const reference = 1 - Math.exp(-rc.sim.t / tau);
      maxAbsoluteError = Math.max(maxAbsoluteError, Math.abs(capacitor.voltdiff - reference));
      referencePeak = Math.max(referencePeak, Math.abs(reference));
    }
    const changeTime = rc.sim.t;
    source.maxVoltage = 0;
    source.bias = 0;
    rc.sim.analyzeFlag = true;
    for (let i = 0; i < 500; i++) {
      expect(rc.sim.step(1)).toBe(1);
      const reference = (1 - Math.exp(-changeTime / tau)) *
        Math.exp(-(rc.sim.t - changeTime) / tau);
      maxAbsoluteError = Math.max(maxAbsoluteError, Math.abs(capacitor.voltdiff - reference));
      referencePeak = Math.max(referencePeak, Math.abs(reference));
    }
    const normalizedRcError = maxAbsoluteError / referencePeak;
    expect(Math.abs(changeTime - 0.5e-3)).toBeLessThan(1e-9);
    expect(Math.abs(rc.sim.t - 1e-3)).toBeLessThan(1e-9);
    expect(normalizedRcError).toBeLessThanOrEqual(0.01);

    const diodeCircuit = readCircuit(diodeText);
    expect(diodeCircuit.warnings).toEqual([]);
    const diode = diodeCircuit.elements.find((e): e is DiodeElm => e instanceof DiodeElm)!;
    const load = diodeCircuit.elements.find((e): e is ResistorElm =>
      e instanceof ResistorElm && e.resistance === 1000)!;
    expect(diode.modelName).toBe('default');
    expect(diodeCircuit.sim.step(10)).toBe(10);
    const diodeForwardV = diode.volts[0]! - diode.volts[1]!;
    const diodeCurrentA = load.current;
    // Reference values measured by the paired, checksum-pinned ngspice 47+ds WASM smoke.
    const ngspiceDiodeForwardV = 0.526073126081;
    const ngspiceDiodeCurrentA = 0.00447392687392;
    const diodeVoltageRelativeError =
      Math.abs(diodeForwardV - ngspiceDiodeForwardV) / ngspiceDiodeForwardV;
    const diodeCurrentRelativeError =
      Math.abs(diodeCurrentA - ngspiceDiodeCurrentA) / ngspiceDiodeCurrentA;
    expect(diodeVoltageRelativeError).toBeLessThanOrEqual(1e-5);
    expect(diodeCurrentRelativeError).toBeLessThanOrEqual(1e-5);

    const bjtCircuit = readCircuit(bjtText);
    expect(bjtCircuit.warnings).toEqual([]);
    const transistor = bjtCircuit.elements.find((e): e is TransistorElm =>
      e instanceof TransistorElm)!;
    const collectorLoad = bjtCircuit.elements.find((e): e is ResistorElm =>
      e instanceof ResistorElm && e.resistance === 1000)!;
    const baseBias = bjtCircuit.elements.find((e): e is ResistorElm =>
      e instanceof ResistorElm && e.resistance === 220000)!;
    expect(transistor.modelName).toBe('default');
    expect(bjtCircuit.sim.step(10)).toBe(10);
    const bjtCollectorV = collectorLoad.volts[1]!;
    const bjtBaseV = baseBias.volts[1]!;
    const ngspiceBjtCollectorV = 6.83546662765;
    const ngspiceBjtBaseV = 0.638027855019;
    const bjtCollectorRelativeError =
      Math.abs(bjtCollectorV - ngspiceBjtCollectorV) / ngspiceBjtCollectorV;
    const bjtBaseRelativeError = Math.abs(bjtBaseV - ngspiceBjtBaseV) / ngspiceBjtBaseV;
    expect(bjtCollectorRelativeError).toBeLessThanOrEqual(1e-5);
    expect(bjtBaseRelativeError).toBeLessThanOrEqual(1e-5);

    writeFileSync(resultPath, JSON.stringify({
      perunRevision: '067d1c9c59e27f72f6498ec224f3735d0da6364a',
      divider: { sourceV: 5, upperOhm: 2000, lowerOhm: 3000,
        measuredV: dividerVoltage, relativeError: dividerRelativeError },
      rc: { resistanceOhm: 1000, capacitanceF: 10e-6, timestepS: 1e-6,
        sourceChangeTimeS: changeTime, finalTimeS: rc.sim.t,
        maxAbsoluteErrorV: maxAbsoluteError, referencePeakV: referencePeak,
        normalizedMaxError: normalizedRcError },
      defaultDiode: { sourceV: 5, loadOhm: 1000, perunModel: diode.modelName,
        perunForwardV: diodeForwardV, ngspiceForwardV: ngspiceDiodeForwardV,
        forwardRelativeError: diodeVoltageRelativeError, perunCurrentA: diodeCurrentA,
        ngspiceCurrentA: ngspiceDiodeCurrentA,
        currentRelativeError: diodeCurrentRelativeError, tolerance: 1e-5 },
      defaultNpnBias: { supplyV: 12, collectorOhm: 1000, baseBiasOhm: 220000,
        perunModel: transistor.modelName, beta: transistor.beta,
        perunCollectorV: bjtCollectorV, ngspiceCollectorV: ngspiceBjtCollectorV,
        collectorRelativeError: bjtCollectorRelativeError, perunBaseV: bjtBaseV,
        ngspiceBaseV: ngspiceBjtBaseV, baseRelativeError: bjtBaseRelativeError,
        tolerance: 1e-5 },
    }, null, 2));
  });
});
