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

test('scope trigger is saved and invalid settings are rejected', () => {
  const p = emptyProject();
  p.instruments.scope.trigger = { channel: 1, edge: 'falling', level: 1.25 };
  assert.deepEqual(parseProject(serializeProject(p)).instruments.scope.trigger, p.instruments.scope.trigger);
  const bad = JSON.parse(serializeProject(p));
  bad.instruments.scope.trigger.level = Infinity;
  assert.throws(() => parseProject(JSON.stringify(bad)), /Scope trigger needs/);
});
