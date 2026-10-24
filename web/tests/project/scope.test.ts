import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ScopeAcquisitionEngine, ScopeBuffer } from '../../src/instruments/Scope.tsx';
import { emptyProject, newScope, parseProject, serializeProject } from '../../src/project/model.ts';

test('scope locates rising and falling edges between samples', () => {
  const b = new ScopeBuffer();
  b.append(new Float64Array([
    0, -1, 1,
    10, -1, 1,
    20, 1, -1,
    30, 1, -1,
    40, -1, 1
  ]), 3, [0, 1]);
  assert.equal(b.latestTrigger({ channel: 0, edge: 'rising', level: 0 }, 0, 40), 15);
  assert.equal(b.latestTrigger({ channel: 0, edge: 'falling', level: 0 }, 0, 40), 35);
  assert.equal(b.latestTrigger({ channel: 1, edge: 'rising', level: 0 }, 0, 40), 35);
  assert.equal(b.latestTrigger({ channel: 0, edge: 'rising', level: 0 }, 20, 40), null);
  assert.equal(b.latestTrigger({ channel: 0, edge: 'falling', level: 0 }, 0, 30), null);
  assert.equal(b.firstTrigger({ channel: 0, edge: 'rising', level: 0 }, 0, 40), 15);
  assert.equal(b.firstTrigger({ channel: 0, edge: 'falling', level: 0 }, 20, 40), 35);
  assert.equal(b.firstTrigger({ channel: 0, edge: 'rising', level: 0 }, 15, 40), null);
  assert.equal(b.firstTrigger({ channel: 0, edge: 'rising', level: 0 }, 0, 14), null);
});

test('held scope snapshot survives live writes and ring wrap', () => {
  const b = new ScopeBuffer();
  b.append(new Float64Array([0, 1, 2, 10, 3, 4]), 3, [0, 1]);
  const held = b.snapshot();
  b.append(new Float64Array([20, 5, 6]), 3, [0, 1]);
  assert.equal(held.toCsv(['ch1', 'ch2']), 'time_ns,ch1,ch2\n0,1,2\n10,3,4\n');
  assert.equal(b.length, 3);
  b.start = b.t.length - 1;
  b.length = 2;
  b.t[b.start] = 50; b.v[0][b.start] = 7; b.v[1][b.start] = 8;
  b.t[0] = 60; b.v[0][0] = 9; b.v[1][0] = 10;
  const wrapped = b.snapshot();
  assert.equal(wrapped.toCsv(['ch1', 'ch2']), 'time_ns,ch1,ch2\n50,7,8\n60,9,10\n');
  assert.equal(b.snapshotRange(55, 65).toCsv(['ch1', 'ch2']), 'time_ns,ch1,ch2\n60,9,10\n');
});

test('scope trigger is saved and invalid settings are rejected', () => {
  const p = emptyProject();
  p.instruments.scopes[0].trigger = { channel: 1, edge: 'falling', level: 1.25 };
  assert.deepEqual(parseProject(serializeProject(p)).instruments.scopes[0].trigger, p.instruments.scopes[0].trigger);
  const bad = JSON.parse(serializeProject(p));
  bad.instruments.scopes[0].trigger.level = Infinity;
  assert.throws(() => parseProject(JSON.stringify(bad)), /Scope trigger needs/);
  p.instruments.scopes[0].acquisition = 'single';
  assert.equal(parseProject(serializeProject(p)).instruments.scopes[0].acquisition, 'single');
  const invalidMode = JSON.parse(serializeProject(p));
  invalidMode.instruments.scopes[0].acquisition = 'loop';
  assert.throws(() => parseProject(JSON.stringify(invalidMode)), /Scope acquisition must/);
  p.instruments.scopes[0].trigger = null;
  assert.throws(() => serializeProject(p), /need a trigger/);
});

test('normal acquisition waits for posttrigger data and can capture another edge', () => {
  const b = new ScopeBuffer();
  const engine = new ScopeAcquisitionEngine();
  engine.arm('normal', { channel: 0, edge: 'rising', level: 0 }, 100, 0);
  b.append(new Float64Array([0, -1, 0]), 3, [0, 1]);
  assert.equal(engine.observe(b), null);
  b.append(new Float64Array([10, 1, 0]), 3, [0, 1]);
  assert.equal(engine.observe(b), null);
  b.append(new Float64Array([85, 1, 0, 90, 1, 0]), 3, [0, 1]);
  assert.deepEqual(engine.observe(b), { mode: 'normal', timeNs: 5, fromNs: 0, throughNs: 85 });
  b.append(new Float64Array([100, -1, 0, 110, 1, 0, 190, 1, 0]), 3, [0, 1]);
  assert.deepEqual(engine.observe(b), { mode: 'normal', timeNs: 105, fromNs: 85, throughNs: 185 });
});

test('single acquisition captures the first armed edge only until rearmed', () => {
  const b = new ScopeBuffer();
  const engine = new ScopeAcquisitionEngine();
  engine.arm('single', { channel: 0, edge: 'rising', level: 0 }, 100, 0);
  b.append(new Float64Array([0, -1, 0, 10, 1, 0, 90, 1, 0]), 3, [0, 1]);
  assert.deepEqual(engine.observe(b), { mode: 'single', timeNs: 5, fromNs: 0, throughNs: 85 });
  b.append(new Float64Array([100, -1, 0, 110, 1, 0, 190, 1, 0]), 3, [0, 1]);
  assert.equal(engine.observe(b), null);
  engine.arm('single', { channel: 0, edge: 'rising', level: 0 }, 100, 90);
  assert.deepEqual(engine.observe(b), { mode: 'single', timeNs: 105, fromNs: 85, throughNs: 185 });
});

test('acquisition refuses a window after its pretrigger samples were lost', () => {
  const b = new ScopeBuffer();
  const engine = new ScopeAcquisitionEngine();
  engine.arm('single', { channel: 0, edge: 'rising', level: 0 }, 100, 0);
  b.append(new Float64Array([0, -1, 0, 10, 1, 0]), 3, [0, 1]);
  assert.equal(engine.observe(b), null);
  b.append(new Float64Array([90, 1, 0]), 3, [0, 1]);
  b.start = 1; b.length = 2; b.dropped = 1;
  assert.equal(engine.observe(b), null);
});

test('legacy single scope migrates and multiple scopes round trip', () => {
  const p = emptyProject();
  const legacy = JSON.parse(serializeProject(p));
  legacy.instruments = { scope: { channels: [null, null], windowNs: 5000, trigger: null, acquisition: 'auto' } };
  const migrated = parseProject(JSON.stringify(legacy));
  assert.equal(migrated.instruments.scopes.length, 1);
  assert.equal(migrated.instruments.scopes[0].id, 'scope1');
  assert.equal(migrated.instruments.scopes[0].windowNs, 5000);
  p.instruments.scopes.push({ ...newScope(p), name: 'Bus' });
  p.instruments.terminals = ['USART3', 'USART1'];
  const back = parseProject(serializeProject(p));
  assert.deepEqual(back.instruments.scopes.map((s) => s.id), ['scope1', 'scope2']);
  assert.deepEqual(back.instruments.terminals, ['USART3', 'USART1']);
  const dup = JSON.parse(serializeProject(p));
  dup.instruments.scopes[1].id = 'scope1';
  assert.throws(() => parseProject(JSON.stringify(dup)), /Duplicate id scope1/);
});
