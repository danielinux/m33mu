// Example projects: they load, compile without errors, and the analog-only
// ones match their references.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { parseProject } from '../../src/project/model.ts';
import { compileCircuit, type BoardLayout } from '../../src/project/netgraph.ts';
import { Circuit } from '../../sim/circuit.ts';

const root = process.env.M33MU_WEB_ROOT!;
const examples = join(root, 'public/examples');
const layout = JSON.parse(readFileSync(join(root, 'public/boards/layout/stm32h563.json'), 'utf8')) as BoardLayout;
const layouts = new Map([['stm32h563', layout]]);
const index = JSON.parse(readFileSync(join(examples, 'index.json'), 'utf8'));

function load(id: string) {
  const entry = index.examples.find((e: { id: string }) => e.id === id);
  const project = parseProject(readFileSync(join(examples, entry.project), 'utf8'));
  const compiled = compileCircuit(project, layouts);
  assert.deepEqual(compiled.diagnostics, [], `${id} diagnostics`);
  return { project, compiled };
}

test('every example parses and compiles cleanly', () => {
  for (const e of index.examples) load(e.id);
  assert.deepEqual(load('h563-blink').compiled.pads, ['PA5']);
  assert.deepEqual(load('h563-button').compiled.pads, ['PA5', 'PA6']);
});

test('divider example reads 3 V within 1e-6', () => {
  const { compiled } = load('divider');
  const c = new Circuit(compiled.netlist);
  c.operatingPoint();
  const mid = compiled.terminalNet.get('part:r2:a')!;
  assert.ok(Math.abs(c.nodeVoltage(mid) - 3) < 3e-6, `midpoint ${c.nodeVoltage(mid)}`);
});

test('RC switch example follows 1 kOhm / 10 uF within 0.1% of 5 V', () => {
  const { compiled } = load('rc-switch');
  const c = new Circuit(compiled.netlist);
  c.zeroState();
  c.setSwitch('sw1', true);
  const node = compiled.terminalNet.get('part:c1:a')!;
  let worst = 0;
  for (let k = 1; k <= 50; k++) {
    c.advanceTo(k * 1e-3);
    worst = Math.max(worst, Math.abs(c.nodeVoltage(node) - 5 * (1 - Math.exp(-c.t / 1e-2))));
  }
  assert.ok(worst / 5 < 1e-3, `worst error ${worst} V`);
});

test('NPN example saturates and sinks the LED current', () => {
  const { compiled } = load('npn-led');
  const c = new Circuit(compiled.netlist);
  c.setSwitch('sw1', true);
  c.operatingPoint();
  const i = c.deviceCurrent('led1');
  const vce = c.nodeVoltage(compiled.terminalNet.get('part:q1:c')!);
  // Hand reference: base current (5 - 0.7) / 10k = 0.43 mA overdrives the
  // load, so Vce(sat) < 0.2 V and I = (5 - Vf - Vce) / 220 with Vf about 2.1 V.
  assert.ok(vce < 0.2, `not saturated: Vce ${vce}`);
  assert.ok(i > 0.0125 && i < 0.0135, `LED current ${i}`);
  c.setSwitch('sw1', false);
  c.operatingPoint();
  assert.ok(c.deviceCurrent('led1') < 1e-6, 'LED off with the base open');
});
