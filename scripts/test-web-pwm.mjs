// Timer PWM through the worker: TIM4_CH4 on PD15 (1 kHz, 25 %) drives an RC
// filter whose output settles at the duty-weighted average, and TIM3_CH3 on
// PB0 (on-board LD1) ramps its duty cycle.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { loadWorker, pad } from './web-worker-harness.mjs';

const [workerPath, pwmElf] = process.argv.slice(2);
const w = await loadWorker(workerPath);
assert.equal((await w.request('loadFirmware', { bytes: new Uint8Array(readFileSync(pwmElf)) })).ok, true);
const netlist = { schemaVersion: 1, devices: [
  { kind: 'pad', id: 'pad-PD15', node: 'PD15', pad: pad('PD15') },
  { kind: 'resistor', id: 'r1', a: 'PD15', b: 'f', ohms: 10e3 },
  { kind: 'capacitor', id: 'c1', a: 'f', b: 'gnd', farads: 1e-6 },
  { kind: 'ground', id: 'gnd1', node: 'gnd' }
] };
const created = await w.request('createBench', { netlist, options: { sampleNs: 10_000 },
  probes: [{ id: 'pwm', kind: 'voltage', net: 'PD15' }, { id: 'rc', kind: 'voltage', net: 'f' }] });
assert.equal(created.ok, true, JSON.stringify(created.error));
const batches = await w.runBench(120_000_000);

// Pad edges: period 1 ms, high 250 us.
const drives = batches.flatMap((b) => b.events).filter((e) => e.kind === 'pad' && e.socPad === 'PD15');
const highs = drives.filter((e) => e.drive === 'high').map((e) => e.timeNs);
const lows = drives.filter((e) => e.drive === 'low').map((e) => e.timeNs);
assert.ok(highs.length > 100, `only ${highs.length} rising edges`);
for (let k = 2; k < highs.length; k++) {
  assert.ok(Math.abs(highs[k] - highs[k - 1] - 1e6) < 50, `period ${highs[k] - highs[k - 1]} ns`);
}
const width = lows.find((t) => t > highs[2]) - highs[2];
assert.ok(Math.abs(width - 250e3) < 50, `high time ${width} ns`);
const unsupported = batches.flatMap((b) => b.events).filter((e) => e.kind === 'unsupported' && e.socPad === 'PD15');
assert.equal(unsupported.length, 0, JSON.stringify(unsupported));

// RC output after 10 tau: mean 3.3 V * 25 %, ripple a few mV.
const rows = [];
for (const b of batches) for (let i = 0; i < b.samples.length; i += 3) rows.push([b.samples[i], b.samples[i + 1], b.samples[i + 2]]);
const tail = rows.filter((r) => r[0] > 100e6);
const mean = tail.reduce((s, r) => s + r[2], 0) / tail.length;
assert.ok(Math.abs(mean - 0.825) < 0.03, `RC mean ${mean} V`);
const vmax = Math.max(...tail.map((r) => r[1]));
assert.ok(vmax > 3.2, `PWM high ${vmax} V`);

// LD1: duty ramps by 2 % per 20 ms, reported by padDuty.
const duty = batches.map((b) => [Number(b.timeNs), b.padDuty?.PB0]).filter(([, d]) => d !== undefined);
const late = await w.runBench(520_000_000);
const lateDuty = late.map((b) => b.padDuty?.PB0).filter((d) => d !== undefined).at(-1);
assert.ok(duty.length > 0);
assert.ok(lateDuty > 0.4 && lateDuty < 0.6, `LD1 duty at 0.5 s: ${lateDuty}`);
await w.request('destroyBench');
console.log(`web pwm: ok (period 1 ms, high ${width / 1e3} us, RC mean ${mean.toFixed(3)} V, LD1 duty at 0.5 s ${lateDuty.toFixed(2)})`);
process.exit(0);
