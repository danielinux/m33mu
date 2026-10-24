import { test } from 'node:test';
import assert from 'node:assert/strict';
import { appendDiagnostic, DIAGNOSTIC_LIMIT } from '../../src/diagnostics.ts';

test('diagnostics fold repeated unsupported events and retain their time span', () => {
  const first = appendDiagnostic([], { source: 'capability', severity: 'warning', code: 'UNSUPPORTED',
    message: 'PA5: alternate function unavailable', timeNs: 10 });
  const repeated = appendDiagnostic(first, { source: 'capability', severity: 'warning', code: 'UNSUPPORTED',
    message: 'PA5: alternate function unavailable', timeNs: 30 });
  assert.equal(repeated.length, 1);
  assert.equal(repeated[0].count, 2);
  assert.equal(repeated[0].firstTimeNs, 10);
  assert.equal(repeated[0].lastTimeNs, 30);
  assert.equal(appendDiagnostic(repeated, { source: 'worker', severity: 'error', code: 'LOCKUP',
    message: 'Core locked up' }).length, 2);
});

test('diagnostics bound unique failures and reject unusable timestamps', () => {
  let entries = [] as ReturnType<typeof appendDiagnostic>;
  for (let i = 0; i < DIAGNOSTIC_LIMIT + 7; i++) {
    entries = appendDiagnostic(entries, { source: 'workbench', severity: 'error', code: `E${i}`,
      message: 'x'.repeat(1200), timeNs: NaN });
  }
  assert.equal(entries.length, DIAGNOSTIC_LIMIT);
  assert.equal(entries[0].code, 'E7');
  assert.equal(entries.at(-1)?.code, `E${DIAGNOSTIC_LIMIT + 6}`);
  assert.equal(entries[0].firstTimeNs, null);
  assert.equal(entries[0].message.length, 1000);
});
