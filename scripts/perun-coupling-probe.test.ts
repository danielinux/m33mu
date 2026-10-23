// Isolated staged m33mu GPIO -> Perun RC -> m33mu EXTI coupling probe.
import { readFileSync, writeFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { readCircuit } from '@perun/format';
import { CapacitorElm, VoltageElm } from '../../../packages/elements/src/index.ts';

type OutputEvent = { t: number; level: 0 | 1 };
const recordPath = process.env.M33MU_GPIO_RECORD;
const resultPath = process.env.M33MU_COSIM_JSON;

function readOutputEvents(): OutputEvent[] {
  if (!recordPath) throw new Error('M33MU_GPIO_RECORD is required');
  const rows = readFileSync(recordPath, 'utf8').trim().split(/\r?\n/).slice(1);
  const events: OutputEvent[] = [];
  for (const row of rows) {
    const [time, pin, level] = row.split(',');
    if (pin !== 'PA5' || (level !== '0' && level !== '1')) continue;
    events.push({ t: Number(time), level: level === '1' ? 1 : 0 });
  }
  return events;
}

describe('staged firmware GPIO -> analog RC -> firmware EXTI coupling', () => {
  it('preserves capacitor state across a mid-run source change and pause/resume', () => {
    const circuit = readCircuit(readFileSync(new URL('../circuits/rc-charge.txt', import.meta.url), 'utf8'));
    const source = circuit.elements.find((e): e is VoltageElm => e instanceof VoltageElm)!;
    const capacitor = circuit.elements.find((e): e is CapacitorElm => e instanceof CapacitorElm)!;
    expect(circuit.sim.step(200)).toBe(200);
    const before = capacitor.voltdiff;
    const timeBefore = circuit.sim.t;
    expect(before).toBeGreaterThan(0.1);

    source.maxVoltage = 0;
    source.bias = 0;
    circuit.sim.analyzeFlag = true;
    expect(circuit.sim.step(1)).toBe(1);
    expect(circuit.sim.t).toBeGreaterThan(timeBefore);
    expect(Math.abs(capacitor.voltdiff - before)).toBeLessThan(0.1);
    expect(capacitor.voltdiff).toBeGreaterThan(0.1);
    expect(circuit.sim.step(2000)).toBe(2000);
    expect(capacitor.voltdiff).toBeLessThan(before - 0.2);

    circuit.sim.pauseRequested = true;
    const pausedAt = circuit.sim.t;
    expect(circuit.sim.step(20)).toBe(1);
    expect(circuit.sim.t).toBeGreaterThan(pausedAt);
    const oneStepLater = circuit.sim.t;
    circuit.sim.pauseRequested = false;
    expect(circuit.sim.step(1)).toBe(1);
    expect(circuit.sim.t).toBeGreaterThan(oneStepLater);
  });

  it('localizes RC crossings from recorded GPIO transitions within one solver quantum', () => {
    const circuitText = readFileSync(new URL('../circuits/rc-charge.txt', import.meta.url), 'utf8')
      .replace('1.0E-5', '1.0E-8'); // R=1 kOhm, C=10 nF, tau=10 us.
    const circuit = readCircuit(circuitText);
    const source = circuit.elements.find((e): e is VoltageElm => e instanceof VoltageElm)!;

    const capacitor = circuit.elements.find((e): e is CapacitorElm => e instanceof CapacitorElm)!;
    circuit.sim.dcAnalysisFlag = false;
    const events = readOutputEvents();
    expect(events.length).toBeGreaterThanOrEqual(4);
    const updateAt = new Map(events.filter(e => e.t > 0).map(e => [e.t, e.level]));
    source.maxVoltage = 0;
    source.bias = 0;
    const samples: { t: number; mv: number }[] = [{ t: 0, mv: 0 }];
    const quantumNs = Math.round(circuit.sim.timeStep * 1e9);
    const advanceToOrPast = (targetNs: number) => {
      while (circuit.sim.t * 1e9 < targetNs) {
        expect(circuit.sim.step(1)).toBe(1);
        samples.push({ t: Math.round(circuit.sim.t * 1e9), mv: Math.round(capacitor.voltdiff * 1000) });
      }
      expect(circuit.sim.t * 1e9 - targetNs).toBeLessThanOrEqual(quantumNs + 1);
    };
    for (const event of events) {
      if (event.t <= 0) continue;
      // Advance through the edge timestamp, then apply it at the next solver boundary.
      while (circuit.sim.t * 1e9 < event.t) {
        expect(circuit.sim.step(1)).toBe(1);
        samples.push({ t: Math.round(circuit.sim.t * 1e9), mv: Math.round(capacitor.voltdiff * 1000) });
      }
      expect(circuit.sim.t * 1e9 - event.t).toBeLessThanOrEqual(quantumNs + 1);
      const level = updateAt.get(event.t)!;
      source.maxVoltage = 0;
      source.bias = level ? 3.3 : 0;
      circuit.sim.analyzeFlag = true;
      expect(circuit.sim.step(1)).toBe(1);
      samples.push({ t: Math.round(circuit.sim.t * 1e9), mv: Math.round(capacitor.voltdiff * 1000) });
    }
    advanceToOrPast(events.at(-1)!.t + 40000);

    const risingThreshold = 1775;
    const fallingThreshold = 1525;
    const crossings: { t: number; level: 0 | 1 }[] = [];
    let digital = 0;
    for (let i = 1; i < samples.length; i++) {
      const a = samples[i - 1]!;
      const b = samples[i]!;
      const threshold = digital ? fallingThreshold : risingThreshold;
      const crossed = digital ? a.mv > threshold && b.mv <= threshold : a.mv < threshold && b.mv >= threshold;
      if (crossed) {
        const t = a.t + (threshold - a.mv) * (b.t - a.t) / (b.mv - a.mv);
        digital = digital ? 0 : 1;
        crossings.push({ t: Math.round(t), level: digital as 0 | 1 });
      }
    }
    expect(crossings.length).toBeGreaterThanOrEqual(4);
    expect(resultPath).toBeDefined();
    writeFileSync(resultPath!, JSON.stringify({ quantumNs, sourceEvents: events, samples, crossings }, null, 2));
  });
});
