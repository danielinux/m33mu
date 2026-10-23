import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ScopeBuffer } from '../../src/instruments/Scope.tsx';
import { emptyProject, parseProject, serializeProject } from '../../src/project/model.ts';

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
});

test('scope trigger is saved and invalid settings are rejected', () => {
  const p = emptyProject();
  p.instruments.scope.trigger = { channel: 1, edge: 'falling', level: 1.25 };
  assert.deepEqual(parseProject(serializeProject(p)).instruments.scope.trigger, p.instruments.scope.trigger);
  const bad = JSON.parse(serializeProject(p));
  bad.instruments.scope.trigger.level = Infinity;
  assert.throws(() => parseProject(JSON.stringify(bad)), /Scope trigger needs/);
});
