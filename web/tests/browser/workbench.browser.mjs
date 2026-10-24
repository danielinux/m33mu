// Browser interaction tests for the built workbench (web/dist) in Chromium.
// Usage: node web/tests/browser/workbench.browser.mjs [screenshot-dir]
// Env: M33MU_CHROMIUM (default /usr/bin/chromium).
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { mkdirSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { extname, join, normalize } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright-core';

const webRoot = fileURLToPath(new URL('../..', import.meta.url));
const dist = join(webRoot, 'dist');
const shots = process.argv[2] ?? null;
if (shots) mkdirSync(shots, { recursive: true });
const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css',
  '.json': 'application/json', '.svg': 'image/svg+xml', '.wasm': 'application/wasm', '.elf': 'application/octet-stream' };

const server = createServer((req, res) => {
  const path = normalize(decodeURIComponent(new URL(req.url, 'http://x').pathname)).replace(/^\/+/, '');
  let file = join(dist, path || 'index.html');
  try { if (statSync(file).isDirectory()) file = join(file, 'index.html'); } catch { /* 404 below */ }
  try {
    const body = readFileSync(file);
    res.writeHead(200, { 'content-type': types[extname(file)] ?? 'application/octet-stream' });
    res.end(body);
  } catch {
    res.writeHead(404); res.end();
  }
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const origin = `http://127.0.0.1:${server.address().port}`;

const browser = await chromium.launch({ executablePath: process.env.M33MU_CHROMIUM ?? '/usr/bin/chromium',
  args: ['--no-sandbox'] });
const context = await browser.newContext({ viewport: { width: 1440, height: 900 }, acceptDownloads: true });
const external = [];
await context.route('**/*', (route) => {
  const url = route.request().url();
  if (url.startsWith(origin) || url.startsWith('blob:') || url.startsWith('data:')) return route.continue();
  external.push(url);
  return route.abort();
});
const page = await context.newPage();
const consoleErrors = [];
page.on('console', (m) => { if (m.type() === 'error') consoleErrors.push(m.text()); });
page.on('pageerror', (e) => consoleErrors.push(String(e)));
const results = [];
async function check(name, fn) {
  const t0 = Date.now();
  await fn();
  results.push(`${name} (${Date.now() - t0} ms)`);
  console.log(`ok - ${name}`);
}
const center = async (locator) => {
  const b = await locator.boundingBox();
  assert.ok(b, 'element is visible');
  return { x: b.x + b.width / 2, y: b.y + b.height / 2 };
};
const status = () => page.getByTestId('status-message').textContent();
const shot = async (name) => { if (shots) await page.screenshot({ path: join(shots, `${name}.png`) }); };

try {
  await check('loads offline with board artwork', async () => {
    await page.goto(origin + '/');
    await page.waitForSelector('circle[data-contact="CN7-10"]');
    const enabled = await page.locator('circle.contact.enabled').count();
    assert.equal(enabled, 25, 'reviewed electrical contacts only');
    assert.equal(await page.locator('circle[data-contact="CN9-1"].enabled').count(), 1);
    await shot('01-empty-bench');
  });

  await check('place a resistor', async () => {
    await page.getByTestId('place-resistor').click();
    const canvas = await page.getByTestId('bench-canvas').boundingBox();
    await page.mouse.click(canvas.x + canvas.width * 0.72, canvas.y + 160);
    await page.waitForSelector('g[data-part="r1"]');
    await page.keyboard.press('Escape');
  });

  await check('wire CN7-10 to the resistor at several zoom levels', async () => {
    await page.getByTestId('wire-tool').click();
    for (const steps of [0, -3, 4]) {
      const canvas = await page.getByTestId('bench-canvas').boundingBox();
      await page.mouse.move(canvas.x + canvas.width / 2, canvas.y + canvas.height / 2);
      for (let i = 0; i < Math.abs(steps); i++) await page.mouse.wheel(0, steps < 0 ? 120 : -120);
      await page.waitForTimeout(150);
      const from = await center(page.locator('circle[data-contact="CN7-10"]'));
      await page.mouse.move(from.x, from.y);
      assert.match(await page.locator('.canvas-status').textContent(), /CN7-10 \(PA5\)/);
    }
    // Visual-only neighbours are not wiring targets.
    const neighbour = await center(page.locator('circle[data-contact="CN7-9"]'));
    await page.mouse.move(neighbour.x, neighbour.y);
    assert.doesNotMatch(await page.locator('.canvas-status').textContent(), /CN7-9/);
    const from = await center(page.locator('circle[data-contact="CN7-10"]'));
    const to = await center(page.locator('g[data-part="r1"] circle.lead').first());
    await page.mouse.move(from.x, from.y);
    await page.mouse.down();
    await page.mouse.move((from.x + to.x) / 2, to.y, { steps: 5 });
    await page.mouse.move(to.x, to.y, { steps: 5 });
    await page.mouse.up();
    assert.equal(await page.locator('g[data-wire]').count(), 1);
    await page.getByTestId('diagnostics').getByText(/ground reference/).waitFor();
    await shot('02-wired');
  });

  await check('undo and redo the wire', async () => {
    await page.keyboard.press('Escape');
    await page.keyboard.press('Control+z');
    assert.equal(await page.locator('g[data-wire]').count(), 0);
    await page.keyboard.press('Control+y');
    assert.equal(await page.locator('g[data-wire]').count(), 1);
  });

  await check('a wire to a visual-only contact is refused', async () => {
    await page.getByTestId('wire-tool').click();
    const bad = await center(page.locator('circle[data-contact="CN7-9"]'));
    const to = await center(page.locator('g[data-part="r1"] circle.lead').nth(1));
    await page.mouse.move(bad.x, bad.y);
    await page.mouse.down();
    await page.mouse.move(to.x, to.y, { steps: 5 });
    await page.mouse.up();
    assert.equal(await page.locator('g[data-wire]').count(), 1);
    await page.keyboard.press('Escape');
  });

  await check('save and reopen the project', async () => {
    const [download] = await Promise.all([page.waitForEvent('download'), page.getByRole('button', { name: 'Save' }).click()]);
    const file = await download.path();
    const saved = JSON.parse(readFileSync(file, 'utf8'));
    assert.equal(saved.schemaVersion, 2);
    assert.equal(saved.wires.length, 1);
    assert.deepEqual(saved.wires[0].from, { kind: 'board', board: 'board1', contact: 'CN7-10' });
    await page.keyboard.press('Control+z');
    await page.keyboard.press('Control+z');
    assert.equal(await page.locator('g[data-part="r1"]').count(), 0);
    await page.getByTestId('open-input').setInputFiles(file);
    await page.waitForSelector('g[data-part="r1"]');
    assert.equal(await page.locator('g[data-wire]').count(), 1);
    const bad = join(tmpdir(), `m33mu-bad-project-${process.pid}.json`);
    writeFileSync(bad, JSON.stringify({ ...saved, parts: [{ ...saved.parts[0], kind: 'transformer' }] }));
    await page.getByTestId('open-input').setInputFiles(bad);
    await page.waitForFunction(() => /unsupported component/.test(document.querySelector('[data-testid=status-message]')?.textContent ?? ''));
    assert.equal(await page.locator('g[data-part="r1"]').count(), 1, 'rejected project leaves the bench untouched');
    await page.getByTestId('diagnostics').getByText(/unsupported component/).waitFor();
    const [reportDownload] = await Promise.all([page.waitForEvent('download'),
      page.getByRole('button', { name: 'Export report' }).click()]);
    const report = JSON.parse(readFileSync(await reportDownload.path(), 'utf8'));
    assert.equal(report.schemaVersion, 1);
    assert.ok(report.runtime.some((entry) => entry.source === 'workbench' && /unsupported component/.test(entry.message)));
    await page.getByRole('button', { name: 'Clear history' }).click();
    assert.ok(await page.getByTestId('diagnostics').getByText('No runtime diagnostics.').isVisible());
    const badElf = join(tmpdir(), `m33mu-bad-firmware-${process.pid}.elf`);
    writeFileSync(badElf, Buffer.from('not an ELF'));
    await page.getByTestId('elf-input').setInputFiles(badElf);
    await page.waitForFunction(() => /worker/.test(document.querySelector('[data-testid=diagnostics] ol')?.textContent ?? ''));
    const [workerReportDownload] = await Promise.all([page.waitForEvent('download'),
      page.getByRole('button', { name: 'Export report' }).click()]);
    const workerReport = JSON.parse(readFileSync(await workerReportDownload.path(), 'utf8'));
    assert.ok(workerReport.runtime.some((entry) => entry.source === 'worker' && entry.severity === 'error'));
  });

  await check('H563 blink example runs with a glowing LED and scope trace', async () => {
    await page.getByTestId('examples').selectOption('h563-blink');
    await page.waitForFunction(() => /H563 LED blink/.test(document.querySelector('[data-testid=status-message]')?.textContent ?? ''));
    await page.getByTestId('real-time').uncheck();
    await page.getByTestId('run').click();
    const t0 = Date.now();
    await page.waitForFunction(() => {
      const t = document.querySelector('[data-testid=virtual-time]')?.textContent ?? '';
      const m = /([\d.]+) (m?)s/.exec(t);
      return m && (m[2] === '' ? Number(m[1]) * 1000 : Number(m[1])) > 150;
    }, null, { timeout: 120000 });
    const wall = Date.now() - t0;
    await page.waitForSelector('g[data-part="led1"] circle.glow', { timeout: 60000 });
    assert.ok(await page.getByRole('button', { name: 'Export CSV' }).isEnabled());
    await shot('03-blink-running');
    await page.getByRole('button', { name: 'Pause' }).click();
    await page.getByTestId('scope-hold').click();
    assert.equal(await page.getByTestId('scope-hold').getAttribute('aria-pressed'), 'true');
    assert.ok(await page.getByTestId('scope-held-status').isVisible());
    const [heldCsv] = await Promise.all([page.waitForEvent('download'),
      page.getByRole('button', { name: 'Export CSV' }).click()]);
    const heldBytes = readFileSync(await heldCsv.path());
    await page.getByTestId('run').click();
    await page.waitForFunction(() => {
      const m = /([\d.]+) (m?)s/.exec(document.querySelector('[data-testid=virtual-time]')?.textContent ?? '');
      return m && (m[2] === '' ? Number(m[1]) * 1000 : Number(m[1])) > 250;
    }, null, { timeout: 120000 });
    await page.getByRole('button', { name: 'Pause' }).click();
    const [stillHeldCsv] = await Promise.all([page.waitForEvent('download'),
      page.getByRole('button', { name: 'Export CSV' }).click()]);
    assert.deepEqual(readFileSync(await stillHeldCsv.path()), heldBytes, 'hold freezes exported capture');
    await page.getByTestId('scope-hold').click();
    assert.equal(await page.getByTestId('scope-hold').getAttribute('aria-pressed'), 'false');
    const [liveCsv] = await Promise.all([page.waitForEvent('download'),
      page.getByRole('button', { name: 'Export CSV' }).click()]);
    const liveBytes = readFileSync(await liveCsv.path());
    assert.notDeepEqual(liveBytes, heldBytes, 'release shows later live samples');
    await page.getByTestId('real-time').check();
    results.push(`blink: 150 ms simulated in ${wall} ms wall (unpaced)`);
  });

  await check('RC button example: pressing the switch lights the LED through EXTI', async () => {
    await page.getByTestId('examples').selectOption('h563-button');
    await page.waitForFunction(() => /RC button/.test(document.querySelector('[data-testid=status-message]')?.textContent ?? ''));
    await page.getByTestId('run').click();
    await page.waitForFunction(() => /coupled/.test(document.querySelector('[data-testid=status-message]')?.textContent ?? ''));
    const sw = await center(page.locator('g[data-part="sw1"]'));
    await page.mouse.move(sw.x, sw.y);
    await page.mouse.down();
    await page.waitForSelector('g[data-part="led1"] circle.glow', { timeout: 60000 });
    await shot('04-button-pressed');
    await page.mouse.up();
    await page.waitForSelector('g[data-part="led1"] circle.glow', { state: 'detached', timeout: 60000 });
    await page.getByRole('button', { name: 'Pause' }).click();
  });

  await check('analog-only divider example and inspector readout', async () => {
    await page.getByTestId('examples').selectOption('divider');
    await page.getByTestId('run').click();
    await page.locator('g[data-part="r2"]').click();
    await page.waitForFunction(() => /3 V/.test(document.querySelector('.inspector')?.textContent ?? ''));
    await shot('05-divider-inspector');
  });

  await check('op-amp example runs and reports its loaded output', async () => {
    await page.getByTestId('examples').selectOption('opamp-gain');
    await page.getByTestId('run').click();
    await page.locator('g[data-part="u1"]').click();
    await page.waitForFunction(() => /OUT/.test(document.querySelector('.inspector')?.textContent ?? '') &&
      /2 V/.test(document.querySelector('.inspector')?.textContent ?? ''));
    await page.getByTestId('scope-trigger').selectOption('0-rising');
    await page.getByLabel('Trigger level').fill('1.5');
    const [download] = await Promise.all([page.waitForEvent('download'), page.getByRole('button', { name: 'Save' }).click()]);
    const saved = JSON.parse(readFileSync(await download.path(), 'utf8'));
    assert.deepEqual(saved.instruments.scopes[0].trigger, { channel: 0, edge: 'rising', level: 1.5 });
    await page.getByRole('button', { name: 'Pause' }).click();
    await page.getByTestId('run').waitFor();
    const [sigc] = await Promise.all([page.waitForEvent('download'),
      page.getByRole('button', { name: 'Export .sigc' }).click()]);
    const sigcFile = await sigc.path();
    assert.equal(readFileSync(sigcFile).subarray(0, 4).toString(), 'SIGC');
    await page.getByTestId('sigc-input').setInputFiles(sigcFile);
    await page.waitForFunction(() => /Imported 2 voltage trace/.test(document.querySelector('[data-testid=status-message]')?.textContent ?? ''));
  });

  await check('normal scope waits for an RC crossing', async () => {
    await page.getByTestId('examples').selectOption('rc-switch');
    await page.waitForFunction(() => /RC charge through a switch/.test(document.querySelector('[data-testid=status-message]')?.textContent ?? ''));
    await page.waitForSelector('g[data-part="sw1"]');
    await page.getByTestId('scope-trigger').selectOption('0-rising');
    await page.getByLabel('Trigger level').fill('2.5');
    await page.getByTestId('scope-acquisition').selectOption('normal');
    assert.equal(await page.getByTestId('scope-acquisition').inputValue(), 'normal');
    await page.getByTestId('run').click();
    await page.getByRole('button', { name: 'Pause' }).waitFor();
    await page.waitForFunction(() => /Waiting for edge/.test(document.querySelector('[data-testid=trigger-status]')?.textContent ?? ''));
    const sw = await center(page.locator('g[data-part="sw1"]'));
    await page.mouse.move(sw.x, sw.y);
    await page.mouse.down();
    await page.waitForFunction(() => /Triggered at/.test(document.querySelector('[data-testid=trigger-status]')?.textContent ?? ''),
      null, { timeout: 60000 });
    await page.mouse.up();
    await page.getByRole('button', { name: 'Pause' }).click();
  });

  await check('single scope captures one RC rising crossing and rearms', async () => {
    await page.getByTestId('examples').selectOption('divider');
    await page.waitForFunction(() => /DC divider/.test(document.querySelector('[data-testid=status-message]')?.textContent ?? ''));
    await page.getByTestId('examples').selectOption('rc-switch');
    await page.waitForFunction(() => /RC charge through a switch/.test(document.querySelector('[data-testid=status-message]')?.textContent ?? ''));
    await page.waitForSelector('g[data-part="sw1"]');
    await page.getByTestId('scope-trigger').selectOption('0-rising');
    await page.getByLabel('Trigger level').fill('2.5');
    await page.getByTestId('scope-acquisition').selectOption('single');
    const [modeProject] = await Promise.all([page.waitForEvent('download'),
      page.getByRole('button', { name: 'Save' }).click()]);
    assert.equal(JSON.parse(readFileSync(await modeProject.path(), 'utf8')).instruments.scopes[0].acquisition, 'single');
    await page.getByTestId('run').click();
    await page.getByRole('button', { name: 'Pause' }).waitFor();
    const sw = await center(page.locator('g[data-part="sw1"]'));
    await page.mouse.move(sw.x, sw.y);
    await page.mouse.down();
    await page.waitForFunction(() => /Triggered at/.test(document.querySelector('[data-testid=trigger-status]')?.textContent ?? ''),
      null, { timeout: 60000 });
    await page.waitForFunction(() => /Single capture/.test(document.querySelector('[data-testid=scope-held-status]')?.textContent ?? ''),
      null, { timeout: 60000 });
    assert.equal(await page.getByTestId('scope-hold').textContent(), 'Rearm');
    await page.mouse.up();
    await page.getByTestId('scope-hold').click();
    assert.equal(await page.getByTestId('scope-hold').getAttribute('aria-pressed'), 'false');
    await page.getByRole('button', { name: 'Pause' }).click();
    await page.getByTestId('run').waitFor();
    await page.getByRole('button', { name: 'Export .sigc' }).click();
    await page.waitForFunction(() => /SIGC stores millivolts only/.test(document.querySelector('[data-testid=status-message]')?.textContent ?? ''));
  });

  await check('UART terminals: pick UARTs, one tab each, echo through USART3', async () => {
    const echo = join(webRoot, '..', 'tests', 'firmware', 'test-stm32h563-uart-echo', 'app.elf');
    await page.getByTestId('elf-input').setInputFiles(echo);
    await page.getByTestId('uart-picker').click();
    await page.getByTestId('uart-USART1').check();
    assert.ok(await page.getByTestId('uart-USART3').isChecked(), 'USART3 open by default');
    await page.getByTestId('uart-picker').click();
    assert.equal(await page.getByTestId('tab-USART1').count(), 1);
    await page.getByTestId('tab-USART3').click();
    await page.getByTestId('run').click();
    const term = page.getByTestId('terminal-USART3');
    await term.locator('pre').getByText(/STM32H5 echo repro/).waitFor({ timeout: 30000 });
    await term.getByRole('textbox').fill('browser says hi');
    await term.getByRole('button', { name: 'Send' }).click();
    await page.waitForFunction(() => (document.querySelector('[data-testid=terminal-USART3] pre')?.textContent ?? '')
      .split('browser says hi').length > 1, null, { timeout: 30000 });
    await shot('06-uart-terminal');
    await page.getByRole('button', { name: 'Pause' }).click();
    await page.getByTestId('uart-picker').click();
    await page.getByTestId('uart-USART1').uncheck();
    await page.getByTestId('uart-picker').click();
    assert.equal(await page.getByTestId('tab-USART1').count(), 0);
  });

  await check('dock: several scopes, hide/show, CRT terminal look', async () => {
    await page.getByTestId('tab-USART3').click();
    assert.equal(await page.locator('.terminal.crt').count(), 1, 'CRT look on by default');
    await page.getByTestId('crt').uncheck();
    assert.equal(await page.locator('.terminal.crt').count(), 0);
    await page.getByTestId('crt').check();
    await page.getByTestId('add-scope').click();
    await page.getByTestId('tab-scope2').waitFor();
    assert.ok(await page.getByTestId('scope-scope2').isVisible());
    await shot('07-second-scope');
    await page.getByTestId('dock-toggle').click();
    assert.equal(await page.getByTestId('scope-scope2').isVisible(), false, 'dock hidden');
    assert.equal(await page.locator('.dock.collapsed').count(), 1);
    await page.getByTestId('dock-toggle').click();
    assert.ok(await page.getByTestId('scope-scope2').isVisible(), 'dock shown again');
    await page.getByRole('button', { name: 'Remove Scope 2' }).click();
    assert.equal(await page.getByTestId('tab-scope2').count(), 0);
  });

  await check('worker destroy and recreate in the browser', async () => {
    const r = await page.evaluate(async () => {
      const bytes = new Uint8Array(await (await fetch('examples/h563-blink.elf')).arrayBuffer());
      const call = (worker, requestId, operation, payload) => new Promise((resolve) => {
        const prev = worker.onmessage;
        worker.onmessage = ({ data }) => {
          if (data.kind === 'response' && data.requestId === requestId) { worker.onmessage = prev; resolve(data); }
        };
        worker.postMessage({ schemaVersion: 1, kind: 'request', requestId, operation, payload });
      });
      const out = [];
      for (let i = 0; i < 3; i++) {
        const worker = new Worker('worker/worker.mjs', { type: 'module' });
        out.push((await call(worker, 1, 'loadFirmware', { bytes: bytes.slice() })).ok);
        out.push((await call(worker, 2, 'step', { instructionBudget: 100 })).result.instructions);
        out.push((await call(worker, 3, 'diagnostics', {})).result.wasmHeapBytes);
        worker.terminate();
      }
      return out;
    });
    assert.deepEqual([r[0], r[3], r[6]], [true, true, true]);
    // The firmware reaches WFI within the budget; every instance must agree.
    assert.ok(r[1] > 0);
    assert.deepEqual([r[4], r[7]], [r[1], r[1]]);
    assert.equal(r[2], r[5]);
    assert.equal(r[5], r[8]);
  });

  assert.deepEqual(external, [], 'no network requests outside the local origin');
  assert.deepEqual(consoleErrors.filter((e) => !/favicon/.test(e)), []);
  console.log(`browser: chromium ${browser.version()}; ${results.join('; ')}`);
} finally {
  await browser.close();
  server.close();
}
