// External linear-solver acceptance probe. This file is copied into a pinned Perun checkout by
// run-web-perun-linear-acceptance-probe.sh; Perun source is not imported into m33mu.
import { writeFileSync } from 'node:fs';
import { readCircuit } from '@perun/format';
import { CapacitorElm, ResistorElm, VoltageElm } from '../../../packages/elements/src/index.ts';
import { describe, expect, it } from 'vitest';

const resultPath = process.env.M33MU_PERUN_LINEAR_RESULT!;
const tau = 1000 * 10e-6;
const dividerText = `$ 1 5.0E-6 10 50 5.0 50\nv 96 320 96 96 0 0 40.0 5.0 0.0\nw 96 96 256 96 0\nr 256 96 256 208 0 2000.0\nr 256 208 256 320 0 3000.0\nw 96 320 256 320 0\ng 96 320 96 352 0\n`;
const rcText = `$ 1 1.0E-6 10 50 5.0 50\nv 96 320 96 96 0 0 40.0 1.0 0.0\nr 96 96 256 96 0 1000.0\nc 256 96 256 320 0 1.0E-5 0.0\nw 96 320 256 320 0\ng 96 320 96 352 0\n`;

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

    writeFileSync(resultPath, JSON.stringify({
      perunRevision: '067d1c9c59e27f72f6498ec224f3735d0da6364a',
      divider: { sourceV: 5, upperOhm: 2000, lowerOhm: 3000,
        measuredV: dividerVoltage, relativeError: dividerRelativeError },
      rc: { resistanceOhm: 1000, capacitanceF: 10e-6, timestepS: 1e-6,
        sourceChangeTimeS: changeTime, finalTimeS: rc.sim.t,
        maxAbsoluteErrorV: maxAbsoluteError, referencePeakV: referencePeak,
        normalizedMaxError: normalizedRcError },
    }, null, 2));
  });
});
