// Executable web acceptance matrix: run the same firmware through the native
// CLI, the native web session API, the Node/WASM worker and a Chromium worker,
// and require identical final register/cycle state.
// Usage: node scripts/web-acceptance-matrix.mjs <m33mu-cli> <web_parity_runner>
//        <worker-dir-with-wasm> <web-dist> <firmware.elf> [result.json]
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { createServer } from 'node:http';
import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { createRequire } from 'node:module';
import { extname, join, normalize } from 'node:path';
import { loadWorker } from './web-worker-harness.mjs';

const [cli, runner, workerDir, dist, firmwarePath, resultPath] = process.argv.slice(2);
const firmware = readFileSync(firmwarePath);
const columns = {};

// 1. Native CLI, interpreter path (dump needs per-instruction stepping).
{
  const out = execFileSync(cli, ['--cpu', 'stm32h563', '--rng-seed', '1', '--dump', '--expect-bkpt', '0x7f',
    '--timeout', '10', firmwarePath], { env: { ...process.env, M33MU_DISABLE_TB: '1' }, encoding: 'utf8' });
  const rows = [...out.matchAll(/\[DUMP\] PC=0x([0-9a-f]+) .*?r0=0x([0-9a-f]+) r1=0x([0-9a-f]+) r2=0x([0-9a-f]+) r3=0x([0-9a-f]+).*\{clocks:(\d+)\}/g)];
  const last = rows.at(-1);
  const hz = /\[CLOCK\] CPU (\d+) Hz/g;
  const clocks = [...out.matchAll(hz)];
  columns.cli = { r0: parseInt(last[2], 16), r1: parseInt(last[3], 16), r2: parseInt(last[4], 16),
    r3: parseInt(last[5], 16), cycles: Number(last[6]), cpuHz: Number(clocks.at(-1)?.[1]) };
}
// 1b. Native CLI with translated blocks (the default): final cycles only.
{
  const out = execFileSync(cli, ['--cpu', 'stm32h563', '--rng-seed', '1', '--expect-bkpt', '0x7f',
    '--timeout', '10', firmwarePath], { encoding: 'utf8' });
  columns.cliTb = { cycles: Number(/stopped after (\d+) virtual cycles/.exec(out)[1]) };
}
// 2. Native web session.
{
  const out = execFileSync(runner, [firmwarePath], { encoding: 'utf8' }).trim().split('\n').at(-1);
  const s = JSON.parse(out);
  columns.nativeSession = { r0: s.r0, r1: s.r1, r2: s.r2, r3: s.r3, cycles: s.cycles, cpuHz: s.cpuHz };
}
// 3. Node/WASM worker (the shipped worker bundle and module).
{
  const w = await loadWorker(join(workerDir, 'worker.mjs'));
  assert.equal((await w.request('loadFirmware', { bytes: new Uint8Array(firmware) })).ok, true);
  const stopped = w.waitEvent((m) => m.event === 'state' && m.payload.stopped);
  await w.request('run');
  const s = (await stopped).payload;
  columns.nodeWasm = { r0: s.registers[0] >>> 0, r1: s.registers[1] >>> 0, r2: s.registers[2] >>> 0,
    r3: s.registers[3] >>> 0, cycles: Number(s.cycleTotal), cpuHz: s.cpuHz };
}
// 4. Chromium worker served from web/dist.
const chromiumPath = process.env.M33MU_CHROMIUM ?? '/usr/bin/chromium';
const require = createRequire(join(dist, '..', 'package.json'));
let playwright = null;
try { playwright = require('playwright-core'); } catch { /* column skipped */ }
if (playwright && existsSync(chromiumPath) && existsSync(join(dist, 'worker', 'worker.mjs'))) {
  const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm' };
  const server = createServer((req, res) => {
    const path = normalize(decodeURIComponent(new URL(req.url, 'http://x').pathname)).replace(/^\/+/, '');
    if (path === 'fw.elf') { res.writeHead(200); res.end(firmware); return; }
    try {
      const file = path || 'index.html';
      const body = readFileSync(join(dist, file));
      res.writeHead(200, { 'content-type': types[extname(file)] ?? 'application/octet-stream' });
      res.end(body);
    } catch { res.writeHead(404); res.end(); }
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const browser = await playwright.chromium.launch({ executablePath: chromiumPath, args: ['--no-sandbox'] });
  try {
    const page = await browser.newPage();
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    const s = await page.evaluate(async () => {
      const bytes = new Uint8Array(await (await fetch('fw.elf')).arrayBuffer());
      const worker = new Worker('worker/worker.mjs', { type: 'module' });
      let id = 0;
      return await new Promise((resolve, reject) => {
        worker.onmessage = ({ data }) => {
          if (data.kind === 'event' && data.event === 'state' && data.payload.stopped) resolve(data.payload);
          if (data.kind === 'event' && data.event === 'fault') reject(new Error(data.payload.message));
          if (data.kind === 'response' && data.requestId === 1 && data.ok) {
            worker.postMessage({ schemaVersion: 1, kind: 'request', requestId: ++id + 1, operation: 'run', payload: {} });
          }
        };
        worker.postMessage({ schemaVersion: 1, kind: 'request', requestId: ++id, operation: 'loadFirmware', payload: { bytes } });
      });
    });
    columns.chromiumWorker = { r0: s.registers[0] >>> 0, r1: s.registers[1] >>> 0, r2: s.registers[2] >>> 0,
      r3: s.registers[3] >>> 0, cycles: Number(s.cycleTotal), cpuHz: s.cpuHz, browser: `chromium ${browser.version()}` };
  } finally {
    await browser.close();
    server.close();
  }
} else {
  columns.chromiumWorker = { skipped: 'playwright-core, Chromium or web/dist worker not available' };
}

const reference = columns.cli;
for (const [name, col] of Object.entries(columns)) {
  // cliTb is informational: translated blocks take interrupts at block
  // boundaries, a documented CLI-internal timing deviation.
  if (col.skipped || name === 'cli' || name === 'cliTb') continue;
  for (const key of Object.keys(col)) {
    if (key === 'browser') continue;
    assert.equal(col[key], reference[key], `${name}.${key} differs from the CLI: ${col[key]} vs ${reference[key]}`);
  }
}
columns.cliTb.note = columns.cliTb.cycles === reference.cycles ? 'matches' :
  `differs by ${columns.cliTb.cycles - reference.cycles} cycles (block-granular interrupt entry)`;
const result = { firmware: firmwarePath.replace(/^.*tests\/firmware\//, 'tests/firmware/'),
  firmwareSha256: createHash('sha256').update(firmware).digest('hex'), columns };
if (resultPath) writeFileSync(resultPath, JSON.stringify(result, null, 2) + '\n');
for (const [name, col] of Object.entries(columns)) console.log(`${name.padEnd(15)} ${JSON.stringify(col)}`);
console.log('web acceptance matrix: all columns agree');
