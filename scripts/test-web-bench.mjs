// Worker integration: real H563 firmware (PA6 EXTI toggles PA5) coupled to an
// RC circuit through createBench/runUntil. Writes a trace fixture when
// M33MU_BENCH_TRACE is set.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

const [workerPath, firmwarePath, passPcHex] = process.argv.slice(2);
const messages = [];
const waiters = new Map();
const eventWaiters = [];
const scope = {
  postMessage(message) {
    messages.push(message);
    if (message.kind === 'response' && waiters.has(message.requestId)) {
      waiters.get(message.requestId)(message);
      waiters.delete(message.requestId);
    }
    for (const w of [...eventWaiters]) {
      if (message.kind === 'event' && w.match(message)) {
        eventWaiters.splice(eventWaiters.indexOf(w), 1);
        w.resolve(message);
      }
    }
  },
  setTimeout
};
globalThis.self = scope;
await import(pathToFileURL(workerPath).href);

let nextId = 1;
function request(operation, payload = {}) {
  const requestId = nextId++;
  const response = new Promise((resolve) => waiters.set(requestId, resolve));
  scope.onmessage({ data: { schemaVersion: 1, kind: 'request', requestId, operation, payload } });
  return response;
}
function waitEvent(match, ms = 60000) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('timeout waiting for worker event')), ms);
    eventWaiters.push({ match, resolve: (m) => { clearTimeout(timer); resolve(m); } });
  });
}

const firmware = new Uint8Array(readFileSync(firmwarePath));
const firmwareSha256 = createHash('sha256').update(firmware).digest('hex');
const pad = (socPad) => ({ socPad, vdd: 3.3, rOn: 40, rPull: 40e3, cIn: 5e-12,
  vRise: 1.775, vFall: 1.525, drive: 'z', pull: 'none' });
const netlist = { schemaVersion: 1, devices: [
  { kind: 'pad', id: 'pa5', node: 'drive', pad: pad('PA5') },
  { kind: 'resistor', id: 'r1', a: 'drive', b: 'rc', ohms: 1000 },
  { kind: 'capacitor', id: 'c1', a: 'rc', b: 'gnd', farads: 10e-9 },
  { kind: 'ground', id: 'g', node: 'gnd' },
  { kind: 'pad', id: 'pa6', node: 'rc', pad: pad('PA6') }
] };
const probes = [{ id: 'v_rc', kind: 'voltage', net: 'rc' }, { id: 'i_r1', kind: 'current', device: 'r1' }];

// Validation rejects before allocation.
const bad = await request('compileNetlist', { netlist: { schemaVersion: 1, devices: [
  { kind: 'resistor', id: 'r', a: 'x', b: 'gnd', ohms: 0 }] } });
assert.equal(bad.ok, false);
assert.equal(bad.error.code, 'INVALID_NETLIST');
const noFw = await request('createBench', { netlist, probes });
assert.equal(noFw.ok, false);
assert.equal(noFw.error.code, 'NOT_BOOTED');

async function runOnce() {
  const loaded = await request('loadFirmware', { bytes: firmware });
  assert.equal(loaded.ok, true);
  const created = await request('createBench', { netlist, probes, options: { sampleNs: 100 } });
  assert.equal(created.ok, true, JSON.stringify(created.error));
  const stepWhileCoupled = await request('step', { instructionBudget: 1 });
  assert.equal(stepWhileCoupled.error.code, 'BENCH_ACTIVE');
  const batches = [];
  const done = waitEvent((m) => m.event === 'fault' ||
    (m.event === 'bench' && (m.payload.mcuStopped || m.payload.reachedTarget) && m.payload.requestId > 0 &&
     m.payload.timeNs !== '0'));
  const startIndex = messages.length;
  const run = await request('runUntil', { targetNs: '2000000' });
  assert.equal(run.ok, true);
  const last = await done;
  assert.notEqual(last.event, 'fault', JSON.stringify(last.payload));
  for (const m of messages.slice(startIndex)) if (m.event === 'bench') batches.push(m.payload);
  const state = messages.slice(startIndex).filter((m) => m.event === 'state').at(-1).payload;
  const events = batches.flatMap((b) => b.events);
  const seqs = batches.map((b) => b.benchSequence);
  for (let i = 1; i < seqs.length; i++) assert.equal(seqs[i], seqs[i - 1] + 1);
  const samples = [];
  for (const b of batches) {
    for (let i = 0; i < b.samples.length; i += 3) samples.push([b.samples[i], b.samples[i + 1], b.samples[i + 2]]);
  }
  return { last: last.payload, state, events, samples };
}

const first = await runOnce();
assert.equal(first.last.mcuStopped, true, 'firmware should reach BKPT');
const passPc = Number.parseInt(passPcHex, 16);
const stopPc = first.state.pc & ~1;
assert.ok(stopPc === passPc || stopPc === passPc + 2,
  `pc ${first.state.pc.toString(16)} is not the pass BKPT ${passPcHex}`);

const crossings = first.events.filter((e) => e.kind === 'crossing' && e.socPad === 'PA6');
const drives = first.events.filter((e) => e.kind === 'pad' && e.socPad === 'PA5');
assert.ok(crossings.length >= 4, `crossings ${crossings.length}`);
// Piecewise analytic response of the 1040 ohm (R + pad rOn) / 10.005 nF RC.
const tau = 1040 * (10e-9 + 5e-12);
let v = 0;
let tPrev = 0;
let vs = 0;
let di = 0;
const errors = [];
for (const x of crossings.slice(0, 4)) {
  while (di < drives.length && drives[di].timeNs <= x.timeNs) {
    v = vs + (v - vs) * Math.exp(-(drives[di].timeNs - tPrev) * 1e-9 / tau);
    tPrev = drives[di].timeNs;
    vs = drives[di].drive === 'high' ? 3.3 : 0;
    di++;
  }
  const target = x.level ? 1.775 : 1.525;
  const expected = tPrev - tau * Math.log((vs - target) / (vs - v)) * 1e9;
  errors.push(x.timeNs - expected);
}
for (const e of errors) assert.ok(Math.abs(e) < 1e-3 * tau * 1e9, `crossing error ${e} ns`);
// Each PA6 crossing is answered by a PA5 toggle within the handler latency.
for (const x of crossings.slice(0, 4)) {
  const response = drives.find((d) => d.timeNs > x.timeNs);
  assert.ok(response, 'feedback edge');
  assert.ok(response.timeNs - x.timeNs < 2000, `latency ${response.timeNs - x.timeNs} ns`);
  assert.equal(response.drive, x.level ? 'low' : 'high');
}
// Waveform within the rails and monotonic in time.
for (let i = 1; i < first.samples.length; i++) assert.ok(first.samples[i][0] > first.samples[i - 1][0]);
for (const s of first.samples) assert.ok(s[1] > -0.05 && s[1] < 3.35);

// Reset/replay determinism: a fresh load reproduces the trace exactly.
const second = await runOnce();
assert.deepEqual(second.events, first.events);
assert.deepEqual(second.samples, first.samples);

// Cancellation mid-run: pause stops further batches.
await request('loadFirmware', { bytes: firmware });
await request('createBench', { netlist, probes });
await request('runUntil', { targetNs: '2000000' });
await new Promise((r) => setTimeout(r, 0));
const paused = await request('pause');
assert.equal(paused.ok, true);
const afterPause = messages.length;
await new Promise((r) => setTimeout(r, 50));
assert.equal(messages.slice(afterPause).filter((m) => m.event === 'bench').length, 0);
await request('destroyBench');

if (process.env.M33MU_BENCH_TRACE) {
  writeFileSync(process.env.M33MU_BENCH_TRACE, JSON.stringify({
    description: 'H563 PA5 -> 1 kOhm -> 10 nF -> PA6 EXTI -> PA5 feedback through the worker bench',
    firmware: 'tests/firmware/test-stm32h563-signal-analog-loop/app.elf', firmwareSha256,
    solver: 'm33mu web/sim MNA (trapezoidal, LTE-controlled)', netlist, probes,
    tauSeconds: tau, toleranceNs: 1e-3 * tau * 1e9, crossingErrorsNs: errors,
    crossings: crossings.slice(0, 4), drives: drives.slice(0, 5),
    finalTimeNs: first.last.timeNs, passPc: passPcHex
  }, null, 2) + '\n');
}
console.log(`web bench worker test: ok (crossing errors ns: ${errors.map((e) => e.toFixed(3)).join(', ')})`);
