import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { emptyProject, parseProject, serializeProject, type Project } from '../../src/project/model.ts';
import { compileCircuit, hitTerminal, allTerminals, type BoardLayout } from '../../src/project/netgraph.ts';
import { commit, createHistory, HISTORY_LIMIT, redo, undo } from '../../src/project/history.ts';
import { PITCH, parseSi, formatSi } from '../../src/project/catalog.ts';
import { Circuit } from '../../sim/circuit.ts';

const root = process.env.M33MU_WEB_ROOT!;
const layout = JSON.parse(readFileSync(join(root, 'public/boards/layout/stm32h563.json'), 'utf8')) as BoardLayout;
const layouts = new Map([['stm32h563', layout]]);

function blinkProject(): Project {
  const p = emptyProject();
  p.parts.push(
    { id: 'r1', kind: 'resistor', x: 900, y: 300, rotation: 0, params: { ohms: 330 } },
    { id: 'led1', kind: 'led', x: 900, y: 500, rotation: 0, params: { color: 'red' } },
    { id: 'gnd1', kind: 'ground', x: 900, y: 700, rotation: 0, params: {} }
  );
  p.wires.push(
    { id: 'w1', from: { kind: 'board', board: 'board1', contact: 'CN7-10' }, to: { kind: 'part', part: 'r1', terminal: 'a' }, color: '#d33' },
    { id: 'w2', from: { kind: 'part', part: 'r1', terminal: 'b' }, to: { kind: 'part', part: 'led1', terminal: 'a' }, color: '#d33' },
    { id: 'w3', from: { kind: 'part', part: 'led1', terminal: 'k' }, to: { kind: 'part', part: 'gnd1', terminal: 'g' }, color: '#333' }
  );
  return p;
}

test('LED circuit compiles to a pad, resistor, LED and ground', () => {
  const c = compileCircuit(blinkProject(), layouts);
  assert.deepEqual(c.diagnostics.filter((d) => d.severity === 'error'), []);
  assert.deepEqual(c.pads, ['PA5']);
  const kinds = c.netlist.devices.map((d) => d.kind).sort();
  assert.deepEqual(kinds, ['ground', 'led', 'pad', 'resistor']);
  const pad = c.netlist.devices.find((d) => d.kind === 'pad')!;
  assert.equal(pad.kind === 'pad' && pad.node, 'PA5');
  // The compiled netlist is accepted by the solver.
  const circuit = new Circuit(c.netlist);
  circuit.setPad('PA5', 'high', 'none');
  circuit.operatingPoint();
  const i = circuit.deviceCurrent('led1');
  assert.ok(i > 3e-3 && i < 5e-3, `LED current ${i}`);
});

test('wiring a visual-only contact is an error and is not connected', () => {
  const p = blinkProject();
  p.wires[0].from = { kind: 'board', board: 'board1', contact: 'CN7-9' };
  const c = compileCircuit(p, layouts);
  assert.ok(c.diagnostics.some((d) => d.severity === 'error' && /CN7-9 is visual-only/.test(d.message)));
  assert.deepEqual(c.pads, []);
});

test('missing ground reference is reported', () => {
  const p = blinkProject();
  p.parts = p.parts.filter((x) => x.kind !== 'ground');
  p.wires = p.wires.filter((w) => w.id !== 'w3');
  const c = compileCircuit(p, layouts);
  assert.ok(c.diagnostics.some((d) => /ground reference/.test(d.message)));
});

test('breadboard strips connect leads placed in the same column', () => {
  const p = emptyProject();
  p.parts.push(
    { id: 'bb1', kind: 'breadboard', x: 1000, y: 0, rotation: 0, params: {} },
    // r1 leads in a1 and e5 (rows a..e at y = 3..7 pitches): a1 -> column 1 top strip.
    { id: 'r1', kind: 'resistor', x: 1000 + PITCH, y: 3 * PITCH, rotation: 0, params: { ohms: 1000 } },
    // r2 from c5 (same strip as r1.b at a5) to c9.
    { id: 'r2', kind: 'resistor', x: 1000 + 5 * PITCH, y: 5 * PITCH, rotation: 0, params: { ohms: 1000 } }
  );
  const c = compileCircuit(p, layouts);
  const net = (part: string, t: string) => c.terminalNet.get(`part:${part}:${t}`);
  assert.equal(net('r1', 'b'), net('r2', 'a'));
  assert.notEqual(net('r1', 'a'), net('r2', 'b'));
  assert.equal(net('r1', 'b'), net('bb1', 'e5'));
  assert.notEqual(net('bb1', 'e5'), net('bb1', 'f5'));
});

test('contact hit testing is unambiguous at several zoom levels', () => {
  const terms = allTerminals(emptyProject(), layouts);
  const pa5 = terms.find((t) => t.key === 'board:board1:CN7-10')!;
  for (const zoom of [0.25, 0.5, 1, 2, 4]) {
    // The UI hit radius is 8 screen pixels, converted to world units.
    const radius = Math.min(8 / zoom, 0.45 * PITCH);
    assert.equal(hitTerminal(terms, pa5.x + radius * 0.6, pa5.y, radius)?.key, pa5.key);
    assert.equal(hitTerminal(terms, pa5.x + PITCH * 0.5, pa5.y + PITCH * 0.5, radius), null);
  }
  const ids = terms.map((t) => t.key);
  assert.equal(new Set(ids).size, ids.length);
});

test('project round trip is stable and v1 empty projects migrate', () => {
  const p = blinkProject();
  const text = serializeProject(p);
  assert.equal(serializeProject(parseProject(text)), text);
  const v1 = { schemaVersion: 1, projectId: 'old', name: 'Old', simulator: { cpu: 'stm32h563', firmwareFormat: 'elf',
    firmwareSha256: null, firmwareName: '' }, boards: [], contacts: [], nets: [], components: [], instruments: [] };
  const migrated = parseProject(JSON.stringify(v1));
  assert.equal(migrated.schemaVersion, 2);
  assert.equal(migrated.projectId, 'old');
});

test('malformed and unsupported projects are rejected with actionable messages', () => {
  const p = blinkProject();
  assert.throws(() => parseProject('{'), /not valid JSON/);
  assert.throws(() => parseProject(JSON.stringify({ ...p, schemaVersion: 9 })), /Unsupported project schemaVersion 9/);
  const bad = JSON.parse(serializeProject(p));
  bad.parts[0].kind = 'transformer';
  assert.throws(() => parseProject(JSON.stringify(bad)), /unsupported component "transformer"/);
  const neg = JSON.parse(serializeProject(p));
  neg.parts[0].params.ohms = -5;
  assert.throws(() => parseProject(JSON.stringify(neg)), /Resistance must be between/);
  const v1 = { schemaVersion: 1, projectId: 'x', name: 'x', components: [{ id: 'c' }] };
  assert.throws(() => parseProject(JSON.stringify(v1)), /cannot be migrated/);
  const dangling = JSON.parse(serializeProject(p));
  dangling.wires[1].to.part = 'nope';
  assert.throws(() => parseProject(JSON.stringify(dangling)), /unknown part nope/);
});

test('undo/redo keeps at least 100 operations', () => {
  let h = createHistory(0);
  for (let i = 1; i <= 150; i++) h = commit(h, i);
  for (let i = 0; i < 120; i++) h = undo(h);
  assert.equal(h.present, 30);
  for (let i = 0; i < 5; i++) h = redo(h);
  assert.equal(h.present, 35);
  assert.ok(HISTORY_LIMIT >= 100);
  h = commit(h, 99);
  assert.equal(h.future.length, 0);
});

test('SI parsing and formatting', () => {
  assert.equal(parseSi('4.7k'), 4700);
  assert.equal(parseSi('10n'), 10e-9);
  assert.equal(parseSi('330 Ω'), 330);
  assert.equal(parseSi('abc'), null);
  assert.equal(formatSi(0.0123, 'A'), '12.3 mA');
});
