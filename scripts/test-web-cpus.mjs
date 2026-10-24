// Every --cpu profile through the browser worker: load the target's test
// firmware as ELF, run it and look for its UART banner. Firmware that is not
// built in this tree is skipped; at least the STM32H563 image must exist.
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { loadWorker } from './web-worker-harness.mjs';

const [workerPath, firmwareRoot] = process.argv.slice(2);
const w = await loadWorker(workerPath);
// cpu, ELF under tests/firmware, banner the UART must show.
const CASES = [
  ['stm32h563', 'test-stm32h563-uart-echo/app.elf', /STM32H5 echo repro/],
  ['stm32h5f4', 'test-stm32h5f4-uart-echo/app.elf', /STM32H5 echo repro/],
  ['stm32u585', 'test-stm32u585/app.elf', /Test started\./],
  ['stm32l552', 'test-stm32l552/app.elf', /Timers OK/],
  ['mcxn947', 'test-mcxn947/app.elf', /MCXN947 test start/],
  ['mcxw71c', 'test-mcxw/app.elf', /MCXW test done/],
  ['nrf5340', 'test-nrf5340/app.elf', /nRF5340 test start/],
  ['nrf54lm20', 'test-nrf54lm20/app.elf', /nRF54LM20 test start/],
  ['rp2350', 'test-rp2350-pio/app.elf', /PASS: rx_not_empty/],
  ['pic32ck', 'test-pic32ck/test_uart.elf', /=== ALL PASS ===/],
  ['m2354', 'test-m2354/test_uart.elf', /M2354 uart\/clock test/],
  ['rw612', 'test-rw612-sdk/test_uart.elf', /=== ALL PASS ===/],
  // Reaches its HardFault-free BKPT exit; prints nothing on a UART.
  ['lpc55s69', 'test-lpc55s69/app.elf', null]
];
const ran = [];
for (const [cpu, elf, banner] of CASES) {
  const path = join(firmwareRoot, elf);
  if (!existsSync(path)) { console.log(`web cpus: skip ${cpu} (${elf} not built)`); continue; }
  w.messages.length = 0;
  const loaded = await w.request('loadFirmware', { bytes: new Uint8Array(readFileSync(path)), cpu });
  assert.equal(loaded.ok, true, `${cpu}: ${JSON.stringify(loaded.error)}`);
  assert.equal(loaded.result.cpu, cpu);
  let text = '';
  for (let i = 0; i < 100; i++) {
    const s = await w.request('step', { instructionBudget: 100000 });
    assert.equal(s.ok, true, `${cpu}: ${JSON.stringify(s.error)}`);
    text = Buffer.concat(w.messages.filter((m) => m.event === 'uart').map((m) => Buffer.from(m.payload.bytes))).toString('latin1');
    if (s.result.stopped || (banner && banner.test(text))) break;
  }
  assert.equal(w.messages.filter((m) => m.event === 'fault').length, 0, `${cpu} faulted`);
  const state = w.messages.filter((m) => m.event === 'state').at(-1).payload;
  assert.equal(state.cpu, cpu);
  if (banner) assert.match(text, banner, `${cpu} UART: ${JSON.stringify(text.slice(0, 200))}`);
  ran.push(cpu);
}
assert.ok(ran.includes('stm32h563'), 'the H563 firmware must be built');
console.log(`web cpus: ok (${ran.join(', ')})`);
process.exit(0);
