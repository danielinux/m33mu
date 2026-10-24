// Logic-level buses through the worker: the bus demo firmware talks to a
// 24C02, a TMP102 and a W25Q flash, and loops USART2 back, with the wiring
// deciding success or the fault a real bus would show.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { loadWorker, pad } from './web-worker-harness.mjs';

const [workerPath, busElf] = process.argv.slice(2);
const w = await loadWorker(workerPath);
const firmware = new Uint8Array(readFileSync(busElf));

function bench({ pullups = true, eepromSda = true, flashMiso = true, loopback = true } = {}) {
  const pins = ['PB8', 'PB9', 'PA5', 'PB5', 'PG9', 'PD14', 'PD5', 'PD6'];
  const devices = [
    { kind: 'ground', id: 'g', node: 'GND' },
    { kind: 'vsource', id: 'board-3V3', p: '3V3', n: 'GND', volts: 3.3 },
    // A loopback wire merges PD5 and PD6 into one net.
    ...pins.map((p) => ({ kind: 'pad', id: `pad-${p}`, node: loopback && p === 'PD6' ? 'PD5' : p, pad: pad(p) })),
    { kind: 'resistor', id: 'eeprom.iq', a: '3V3', b: 'GND', ohms: 100e3 },
    { kind: 'resistor', id: 'temp.iq', a: '3V3', b: 'GND', ohms: 100e3 },
    { kind: 'resistor', id: 'flash.iq', a: '3V3', b: 'GND', ohms: 100e3 }
  ];
  if (pullups) {
    devices.push({ kind: 'resistor', id: 'rscl', a: '3V3', b: 'PB8', ohms: 4700 },
      { kind: 'resistor', id: 'rsda', a: '3V3', b: 'PB9', ohms: 4700 });
  }
  const power = { VCC: '3V3', GND: 'GND' };
  const logic = {
    pins: Object.fromEntries(pins.map((p) => [p, loopback && p === 'PD6' ? 'PD5' : p])),
    devices: [
      { id: 'eeprom', kind: 'i2c-eeprom', params: { addressPins: '0' },
        nets: { ...power, SCL: 'PB8', ...(eepromSda ? { SDA: 'PB9' } : {}) } },
      { id: 'temp', kind: 'i2c-temp', params: { addressPins: '0', celsius: 25 },
        nets: { ...power, SCL: 'PB8', SDA: 'PB9' } },
      { id: 'flash', kind: 'spi-flash', params: {},
        nets: { ...power, CS: 'PD14', SCK: 'PA5', MOSI: 'PB5', ...(flashMiso ? { MISO: 'PG9' } : {}) } },
      { id: 'sniff', kind: 'sniffer', params: {}, nets: { CH1: 'PB8', CH2: 'PB9' } }
    ]
  };
  return { netlist: { schemaVersion: 1, devices }, logic };
}

async function run(options) {
  w.messages.length = 0;
  assert.equal((await w.request('loadFirmware', { bytes: firmware })).ok, true);
  const { uarts } = (await w.request('uartList')).result;
  const vcp = uarts.find((u) => u.label === 'USART3').index;
  const b = bench(options);
  const created = await w.request('createBench', { ...b, options: { sampleNs: 1e6 } });
  assert.equal(created.ok, true, JSON.stringify(created.error));
  await w.runBench(1_200_000_000);
  const text = Buffer.concat(w.messages.filter((m) => m.event === 'uart' && m.payload.index === vcp)
    .map((m) => Buffer.from(m.payload.bytes))).toString('latin1');
  const log = w.messages.filter((m) => m.event === 'busLog').flatMap((m) => m.payload.records);
  await w.request('destroyBench');
  const line = text.split('\n').find((l) => l.startsWith('#01')) ?? '';
  return { text, line, log };
}

const good = await run();
assert.match(good.line, /EEPROM W:OK R:OK=01A5/, good.text);
assert.match(good.line, /TEMP:OK=1900/, good.line);
assert.match(good.line, /JEDEC=EF4018/, good.line);
assert.match(good.line, /U2:ping/, good.text);
assert.ok(good.log.some((r) => r.bus === 'I2C1' && /0x50 W \[10 01 a5\]/.test(r.summary)), JSON.stringify(good.log.slice(0, 6)));
assert.ok(good.log.some((r) => r.bus === 'SPI1' && /flash: MOSI \[9f ff ff ff\] MISO \[ff ef 40 18\]/.test(r.summary)));
assert.ok(good.log.some((r) => r.bus === 'USART2' && /ping/.test(r.summary)));

const noPullups = await run({ pullups: false });
assert.match(noPullups.line, /EEPROM W:BUSY/, noPullups.text);
assert.ok(noPullups.log.some((r) => /BUSY, SCL \(PB8\) low/.test(r.summary)));

const halfWired = await run({ eepromSda: false });
assert.match(halfWired.line, /EEPROM W:NACK/, halfWired.text);
assert.match(halfWired.line, /TEMP:OK/);
assert.ok(halfWired.log.some((r) => /eeprom: SDA not connected/.test(r.summary)));

const noMiso = await run({ flashMiso: false, loopback: false });
assert.match(noMiso.line, /JEDEC=000000/, noMiso.text);
assert.match(noMiso.line, /U2:\s*$/m);
console.log(`web buses: ok (${good.line.trim()})`);
