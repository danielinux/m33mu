// H563 electrical demos through the worker bench: LED blink on CN7-10 (PA5)
// and RC button input on CN9-1 (PA6) mirrored to the LED by EXTI.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync } from 'node:fs';
import { loadWorker, pad } from './web-worker-harness.mjs';

const [workerPath, blinkElf, buttonElf, goldenPath] = process.argv.slice(2);
const w = await loadWorker(workerPath);
const sha = (b) => createHash('sha256').update(b).digest('hex');
const ledRed = { is: 5.3e-11, n: 4 };
const golden = {};

// --- LED blink -------------------------------------------------------------
{
  const fw = new Uint8Array(readFileSync(blinkElf));
  assert.equal((await w.request('loadFirmware', { bytes: fw })).ok, true);
  const netlist = { schemaVersion: 1, devices: [
    { kind: 'pad', id: 'pad-PA5', node: 'PA5', pad: pad('PA5') },
    { kind: 'resistor', id: 'r1', a: 'PA5', b: 'n1', ohms: 330 },
    { kind: 'led', id: 'led1', a: 'n1', k: 'gnd', model: ledRed, color: 'red', ratedAmps: 0.02 },
    { kind: 'ground', id: 'gnd1', node: 'gnd' }
  ] };
  const created = await w.request('createBench', { netlist, options: { sampleNs: 100_000 },
    probes: [{ id: 'v', kind: 'voltage', net: 'PA5' }, { id: 'i', kind: 'current', device: 'led1' }] });
  assert.equal(created.ok, true, JSON.stringify(created.error));
  const batches = await w.runBench(350_000_000);
  const state = w.messages.filter((m) => m.event === 'state').at(-1).payload;
  const period = 100 * 64000 / state.cpuHz * 1e9;
  const events = batches.flatMap((b) => b.events);
  const drives = events.filter((e) => e.kind === 'pad' && e.socPad === 'PA5');
  const highs = drives.filter((e) => e.drive === 'high').map((e) => e.timeNs);
  const lows = drives.filter((e) => e.drive === 'low').map((e) => e.timeNs);
  assert.ok(highs.length >= 2 && lows.length >= 2, JSON.stringify(drives));
  // Toggle edges every 100 SysTick periods (first edge after boot latency).
  for (let k = 1; k < highs.length; k++) assert.ok(Math.abs(highs[k] - highs[k - 1] - 2 * period) < 2000);
  assert.ok(Math.abs(highs[0] - period) < 5000, `first toggle ${highs[0]} vs ${period}`);
  const obsOn = batches.map((b) => b.observation).filter(Boolean).at(-1);
  // Samples: LED current at mid-on and mid-off times.
  const rows = [];
  for (const b of batches) for (let i = 0; i < b.samples.length; i += 3) rows.push([b.samples[i], b.samples[i + 1], b.samples[i + 2]]);
  const at = (t) => rows.find((r) => r[0] >= t);
  const on = at(highs[0] + period / 2);
  const off = at(lows.find((t) => t > highs[0]) + period / 2);
  // Independent KCL check on the solved operating point: I = (Vpad - Vf) / 330.
  const vf = 4 * 1.380649e-23 * 300.15 / 1.602176634e-19 * Math.log(on[2] / ledRed.is + 1);
  assert.ok(Math.abs((on[1] - vf) / 330 - on[2]) / on[2] < 1e-6, `KCL ${on}`);
  assert.ok(on[2] > 3e-3 && on[2] < 4.5e-3, `LED on current ${on[2]}`);
  assert.ok(Math.abs(off[2]) < 1e-9, `LED off current ${off[2]}`);
  // Pad driving 0..3.3 V through rOn: Vpad = 3.3 - 40 * I when high.
  assert.ok(Math.abs(on[1] - (3.3 - 40 * on[2])) < 1e-6);
  // Continue past 2^31 ns: 64-bit virtual time must survive the 32-bit ABI halves.
  const late = await w.runBench(2_350_000_000);
  const lateHighs = late.flatMap((b) => b.events).filter((e) => e.kind === 'pad' && e.socPad === 'PA5' && e.drive === 'high');
  assert.ok(lateHighs.some((e) => Math.abs(e.timeNs - (highs[0] + 10 * 2 * period)) < 5000), JSON.stringify(lateHighs));
  assert.ok(Number(late.at(-1).timeNs) >= 2_350_000_000);
  golden.blink = { firmwareSha256: sha(fw), cpuHz: state.cpuHz, togglePeriodNs: period,
    highs: highs.slice(0, 3), lows: lows.slice(0, 3), ledOnAmps: on[2], padOnVolts: on[1], ledOffAmps: off[2],
    brightnessFraction: on[2] / 0.02, lastObservationTimeNs: obsOn?.timeNs };
  await w.request('destroyBench');
}

// --- RC button input -------------------------------------------------------
{
  const fw = new Uint8Array(readFileSync(buttonElf));
  assert.equal((await w.request('loadFirmware', { bytes: fw })).ok, true);
  const netlist = { schemaVersion: 1, devices: [
    { kind: 'vsource', id: 'v1', p: 'vcc', n: 'gnd', volts: 3.3 },
    { kind: 'switch', id: 'sw1', a: 'vcc', b: 'sw', closed: false, ron: 0.05, roff: 1e10 },
    { kind: 'resistor', id: 'r1', a: 'sw', b: 'PA6', ohms: 10e3 },
    { kind: 'resistor', id: 'r2', a: 'PA6', b: 'gnd', ohms: 100e3 },
    { kind: 'capacitor', id: 'c1', a: 'PA6', b: 'gnd', farads: 100e-9 },
    { kind: 'pad', id: 'pad-PA6', node: 'PA6', pad: pad('PA6') },
    { kind: 'pad', id: 'pad-PA5', node: 'PA5', pad: pad('PA5') },
    { kind: 'resistor', id: 'r3', a: 'PA5', b: 'n1', ohms: 330 },
    { kind: 'led', id: 'led1', a: 'n1', k: 'gnd', model: ledRed, color: 'red', ratedAmps: 0.02 },
    { kind: 'ground', id: 'gnd1', node: 'gnd' }
  ] };
  assert.equal((await w.request('createBench', { netlist, options: { sampleNs: 10_000 },
    probes: [{ id: 'vin', kind: 'voltage', net: 'PA6' }, { id: 'iled', kind: 'current', device: 'led1' }] })).ok, true);
  const all = [];
  all.push(...await w.runBench(1_000_000));
  const press = await w.request('setSwitch', { device: 'sw1', closed: true });
  assert.equal(press.ok, true);
  const tPress = Number(press.result.timeNs);
  all.push(...await w.runBench(8_000_000));
  const release = await w.request('setSwitch', { device: 'sw1', closed: false });
  const tRelease = Number(release.result.timeNs);
  all.push(...await w.runBench(40_000_000));
  const events = all.flatMap((b) => b.events);
  const crossings = events.filter((e) => e.kind === 'crossing' && e.socPad === 'PA6');
  const leds = events.filter((e) => e.kind === 'pad' && e.socPad === 'PA5');
  assert.equal(crossings.length, 2, JSON.stringify(crossings));
  const c = 100e-9 + 5e-12;
  const rth = 10e3 * 100e3 / 110e3;
  const vth = 3.3 * 100e3 / (110e3 + 0.05);
  const tauOn = rth * c;
  const expectRise = tPress - tauOn * Math.log(1 - 1.775 / vth) * 1e9;
  const vAtRelease = vth * (1 - Math.exp(-(tRelease - tPress) * 1e-9 / tauOn));
  const tauOff = 100e3 * c;
  const expectFall = tRelease - tauOff * Math.log(1.525 / vAtRelease) * 1e9;
  assert.equal(crossings[0].level, 1);
  assert.equal(crossings[1].level, 0);
  assert.ok(Math.abs(crossings[0].timeNs - expectRise) < 1e-3 * tauOn * 1e9, `rise ${crossings[0].timeNs} vs ${expectRise}`);
  assert.ok(Math.abs(crossings[1].timeNs - expectFall) < 1e-3 * tauOff * 1e9, `fall ${crossings[1].timeNs} vs ${expectFall}`);
  const ledOn = leds.find((e) => e.drive === 'high' && e.timeNs > crossings[0].timeNs);
  const ledOff = leds.find((e) => e.drive === 'low' && e.timeNs > crossings[1].timeNs);
  assert.ok(ledOn && ledOn.timeNs - crossings[0].timeNs < 2000, 'EXTI handler drove PA5 high');
  assert.ok(ledOff && ledOff.timeNs - crossings[1].timeNs < 2000, 'EXTI handler drove PA5 low');
  golden.button = { firmwareSha256: sha(fw), pressNs: tPress, releaseNs: tRelease,
    riseCrossingNs: crossings[0].timeNs, riseExpectedNs: expectRise,
    fallCrossingNs: crossings[1].timeNs, fallExpectedNs: expectFall,
    ledOnNs: ledOn.timeNs, ledOffNs: ledOff.timeNs, toleranceNs: [1e-3 * tauOn * 1e9, 1e-3 * tauOff * 1e9] };
  await w.request('destroyBench');
}

if (goldenPath && process.env.M33MU_UPDATE_GOLDEN) {
  writeFileSync(goldenPath, JSON.stringify(golden, null, 2) + '\n');
} else if (goldenPath) {
  const want = JSON.parse(readFileSync(goldenPath, 'utf8'));
  // Deterministic replay: event times must match the recorded golden exactly
  // when the firmware is unchanged.
  for (const demo of ['blink', 'button']) {
    if (want[demo].firmwareSha256 !== golden[demo].firmwareSha256) {
      console.log(`${demo}: firmware differs from golden (rebuilt toolchain?); analytic checks only`);
      continue;
    }
    assert.deepEqual(golden[demo], want[demo], `${demo} drifted from ${goldenPath}`);
  }
}
console.log('web demos: ok', JSON.stringify({ ledOnAmps: golden.blink.ledOnAmps,
  riseErrNs: golden.button.riseCrossingNs - golden.button.riseExpectedNs,
  fallErrNs: golden.button.fallCrossingNs - golden.button.fallExpectedNs }));
