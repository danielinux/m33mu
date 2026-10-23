// Worker lifecycle: repeated load/reload, bench create/destroy, failure
// cleanup and bounded WASM heap growth.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { loadWorker, pad } from './web-worker-harness.mjs';

const [workerPath, blinkElf] = process.argv.slice(2);
const w = await loadWorker(workerPath);
const fw = new Uint8Array(readFileSync(blinkElf));
const diag = async () => (await w.request('diagnostics')).result;
const netlist = { schemaVersion: 1, devices: [
  { kind: 'pad', id: 'pad-PA5', node: 'PA5', pad: pad('PA5') },
  { kind: 'resistor', id: 'r1', a: 'PA5', b: 'n1', ohms: 330 },
  { kind: 'led', id: 'led1', a: 'n1', k: 'gnd', model: { is: 5.3e-11, n: 4 }, color: 'red', ratedAmps: 0.02 },
  { kind: 'ground', id: 'g', node: 'gnd' }] };

// Failure cleanup: a rejected image leaves no booted firmware behind.
assert.equal((await diag()).sessionActive, false);
const junk = await w.request('loadFirmware', { bytes: new Uint8Array([1, 2, 3, 4]) });
assert.equal(junk.error.code, 'IMAGE_REJECTED');
assert.equal((await w.request('run')).error.code, 'NOT_BOOTED');
const tooBig = await w.request('loadFirmware', { bytes: new Uint8Array(33 * 1024 * 1024) });
assert.equal(tooBig.error.code, 'IMAGE_REJECTED');

// A failing createBench (singular circuit) leaves no bench behind.
assert.equal((await w.request('loadFirmware', { bytes: fw })).ok, true);
const singular = await w.request('createBench', { netlist: { schemaVersion: 1, devices: [
  { kind: 'vsource', id: 'a', p: 'x', n: 'gnd', volts: 1 }, { kind: 'vsource', id: 'b', p: 'x', n: 'gnd', volts: 2 },
  { kind: 'ground', id: 'g', node: 'gnd' }] } });
assert.equal(singular.error.code, 'SINGULAR');
assert.equal((await diag()).benchActive, false);
assert.equal((await w.request('runUntil', {})).error.code, 'NO_BENCH');

// Warm up, then measure heap across bounded repeated runs.
async function cycle() {
  assert.equal((await w.request('loadFirmware', { bytes: fw })).ok, true);
  assert.equal((await w.request('createBench', { netlist, options: { sampleNs: 1e6 } })).ok, true);
  await w.runBench(20_000_000);
  assert.equal((await w.request('destroyBench')).ok, true);
  assert.equal((await w.request('reset')).ok, true);
}
await cycle();
const baseline = (await diag()).wasmHeapBytes;
for (let i = 0; i < 25; i++) await cycle();
const after = await diag();
assert.equal(after.benchActive, false);
assert.ok(after.wasmHeapBytes <= baseline, `WASM heap grew from ${baseline} to ${after.wasmHeapBytes} bytes`);
const js = process.memoryUsage().heapUsed;
for (let i = 0; i < 25; i++) await cycle();
const js2 = process.memoryUsage().heapUsed;
assert.ok(js2 - js < 64 * 1024 * 1024, `JS heap grew by ${js2 - js} bytes over 25 runs`);
console.log(`web worker lifecycle: ok (WASM heap ${baseline} bytes stable over 50 load/bench/destroy/reset cycles)`);
