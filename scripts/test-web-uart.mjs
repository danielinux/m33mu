// Worker UART terminals: list H563 UARTs, receive firmware output and echo
// host input through USART3 with the uart-echo fixture.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { loadWorker } from './web-worker-harness.mjs';

const [workerPath, echoElf] = process.argv.slice(2);
const w = await loadWorker(workerPath);
assert.equal((await w.request('loadFirmware', { bytes: new Uint8Array(readFileSync(echoElf)) })).ok, true);
const { uarts } = (await w.request('uartList')).result;
const usart3 = uarts.find((u) => u.label === 'USART3');
assert.ok(usart3, JSON.stringify(uarts));
assert.equal(usart3.base, 0x40004800);
const received = () => Buffer.concat(w.messages.filter((m) => m.event === 'uart' && m.payload.index === usart3.index)
  .map((m) => Buffer.from(m.payload.bytes))).toString('latin1');
const until = async (pred, ms = 20000) => {
  const t0 = Date.now();
  while (!pred()) {
    if (Date.now() - t0 > ms) throw new Error(`timeout; USART3 got ${JSON.stringify(received())}`);
    await new Promise((r) => setTimeout(r, 20));
  }
};
assert.equal((await w.request('run')).ok, true);
await new Promise((r) => setTimeout(r, 300));
const before = received().length;
const sent = await w.request('uartWrite', { index: usart3.index, text: 'hello web\r' });
assert.equal(sent.result.accepted, 10);
await until(() => received().slice(before).includes('hello web'));
await w.request('pause');
const bad = await w.request('uartWrite', { index: 99, text: 'x' });
assert.equal(bad.error.code, 'INVALID_ARGUMENT');
console.log(`web uart: ok (${uarts.length} UARTs; USART3 output ${JSON.stringify(received().slice(0, 60))})`);
