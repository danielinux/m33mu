import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

const messages = [];
const responseWaiters = new Map();
const scope = {
  postMessage(message) {
    messages.push(message);
    if (message.kind === 'response' && responseWaiters.has(message.requestId)) {
      responseWaiters.get(message.requestId)(message);
      responseWaiters.delete(message.requestId);
    }
  },
  setTimeout
};
globalThis.self = scope;
await import(pathToFileURL(process.argv[2]).href);
assert.equal(typeof scope.onmessage, 'function');

function put16(view, offset, value) { view.setUint16(offset, value, true); }
function put32(view, offset, value) { view.setUint32(offset, value, true); }

function h563Elf(instructions) {
  const bytes = new Uint8Array(84 + 8 + instructions.length * 2);
  const view = new DataView(bytes.buffer);
  bytes.set([0x7f, 0x45, 0x4c, 0x46, 1, 1, 1]);
  put16(view, 18, 40);
  put32(view, 24, 0x0c000000);
  put32(view, 28, 52);
  put16(view, 40, 52);
  put16(view, 42, 32);
  put16(view, 44, 1);
  put32(view, 52, 1);
  put32(view, 56, 84);
  put32(view, 60, 0x0c000000);
  put32(view, 64, 0x0c000000);
  put32(view, 68, 8 + instructions.length * 2);
  put32(view, 72, 8 + instructions.length * 2);
  put32(view, 76, 4);
  put32(view, 84, 0x30080000);
  put32(view, 88, 0x0c000009);
  instructions.forEach((instruction, index) =>
    put16(view, 92 + index * 2, instruction));
  return bytes;
}

let nextRequestId = 1;
function request(operation, payload = {}) {
  const requestId = nextRequestId++;
  const response = new Promise(resolve => responseWaiters.set(requestId, resolve));
  scope.onmessage({ data: {
    schemaVersion: 1, kind: 'request', requestId, operation, payload
  } });
  return response;
}

function waitForEvent(predicate) {
  const existing = messages.find(message =>
    message.kind === 'event' && predicate(message));
  if (existing) return Promise.resolve(existing);
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      clearInterval(timer);
      reject(new Error('Timed out waiting for worker event'));
    }, 10000);
    const timer = setInterval(() => {
      const found = messages.find(message =>
        message.kind === 'event' && predicate(message));
      if (found) {
        clearTimeout(timeout);
        clearInterval(timer);
        resolve(found);
      }
    }, 1);
  });
}

const beforeBoot = await request('step', { instructionBudget: 1 });
assert.equal(beforeBoot.ok, false);
assert.equal(beforeBoot.error.code, 'NOT_BOOTED');

const loaded = await request('loadFirmware', {
  bytes: h563Elf([0x2001, 0xbeab])
});
assert.equal(loaded.ok, true);
assert.equal(loaded.result.format, 'elf32-arm');

const stepped = await request('step', { instructionBudget: 1 });
assert.equal(stepped.ok, true);
assert.equal(stepped.result.instructions, 1);
assert.equal(stepped.result.r0, 1);
assert.equal(stepped.result.registers[0], 1);
assert.equal(stepped.result.pc, 0x0c00000b);
assert.equal(stepped.result.cycleTotal, '1');
assert.equal(stepped.result.stacks.mspSecure, 0x30080000);

const parity = JSON.parse(readFileSync(process.env.M33MU_CLI_PARITY, 'utf8'));
const parityFirmware = h563Elf([0x2001, 0xbeab]);
assert.equal(parity.sha256, createHash('sha256').update(parityFirmware).digest('hex'));
assert.equal(parity.pc, stepped.result.pc);
assert.equal(parity.r0, stepped.result.r0);
assert.equal(parity.mspSecure, stepped.result.stacks.mspSecure);

const runAck = await request('run');
assert.equal(runAck.ok, true);
const stopped = await waitForEvent(message =>
  message.event === 'state' && message.payload.stopped === true);
assert.equal(stopped.payload.r0, 1);

const reset = await request('reset');
assert.equal(reset.ok, true);
const resetState = await waitForEvent(message =>
  message.event === 'state' && message.payload.requestId === reset.requestId);
assert.equal(resetState.payload.r0, 0);
assert.equal(resetState.payload.pc, 0x0c000009);

const loopLoad = await request('loadFirmware', { bytes: h563Elf([0xe7fe]) });
assert.equal(loopLoad.ok, true);
const runLoop = await request('run');
assert.equal(runLoop.ok, true);
const firstRunSlice = await waitForEvent(message =>
  message.event === 'state' && message.payload.requestId === runLoop.requestId);
assert.equal(firstRunSlice.payload.cycleTotal, '512');
const secondRunSlice = await waitForEvent(message =>
  message.event === 'state' && message.payload.requestId === runLoop.requestId &&
  BigInt(message.payload.cycleTotal) >= 1024n);
assert.equal(secondRunSlice.payload.cycleTotal, '1024');
const paused = await request('pause');
assert.equal(paused.ok, true);
const pauseState = await waitForEvent(message =>
  message.event === 'state' && message.payload.requestId === paused.requestId);
assert.equal(pauseState.payload.paused, true);
const runEventsBeforeWait = messages.filter(message =>
  message.kind === 'event' && message.payload.requestId === runLoop.requestId).length;
await new Promise(resolve => setTimeout(resolve, 20));
const runEventsAfterWait = messages.filter(message =>
  message.kind === 'event' && message.payload.requestId === runLoop.requestId).length;
assert.equal(runEventsAfterWait, runEventsBeforeWait);

const queuedInput = await request('gpioWrite', {
  socPad: 'PA5', level: true, eventTimeNs: '1000000000'
});
assert.equal(queuedInput.ok, true);
assert.equal(queuedInput.result.accepted, true);
assert.equal(queuedInput.result.eventTimeNs, '1000000000');

const pastInput = await request('gpioWrite', {
  socPad: 'PA5', level: false, eventTimeNs: '1'
});
assert.equal(pastInput.ok, false);
assert.equal(pastInput.error.code, 'EVENT_IN_PAST');

const badPad = await request('gpioWrite', {
  socPad: 'PA99', level: true, eventTimeNs: '0'
});
assert.equal(badPad.ok, false);
assert.equal(badPad.error.code, 'INVALID_ARGUMENT');

const wfiLoad = await request('loadFirmware', {
  bytes: h563Elf([0xbf30, 0xbeab])
});
assert.equal(wfiLoad.ok, true);
const idleRun = await request('run');
assert.equal(idleRun.ok, true);
const idleState = await waitForEvent(message =>
  message.event === 'state' && message.payload.requestId === idleRun.requestId);
assert.equal(idleState.payload.sleeping, true);
assert.equal(idleState.payload.paused, true);

const wakeInput = await request('gpioWrite', {
  socPad: 'PA5', level: true, eventTimeNs: '1000000'
});
assert.equal(wakeInput.ok, true);
const wakeRun = await request('run');
assert.equal(wakeRun.ok, true);
const yieldingSleep = await waitForEvent(message =>
  message.event === 'state' && message.payload.requestId === wakeRun.requestId &&
  message.payload.sleeping && !message.payload.paused);
assert.ok(BigInt(yieldingSleep.payload.virtualTimeNs) < 1000000n);
const wakeState = await waitForEvent(message =>
  message.event === 'state' && message.payload.requestId === wakeRun.requestId &&
  message.payload.sleeping && message.payload.paused);
assert.ok(BigInt(wakeState.payload.virtualTimeNs) >= 1000000n);

if (process.env.M33MU_WFI_FIRMWARE) {
  const firmware = new Uint8Array(readFileSync(process.env.M33MU_WFI_FIRMWARE));
  const wfiExtiLoad = await request('loadFirmware', { bytes: firmware });
  assert.equal(wfiExtiLoad.ok, true);
  const pa6Edge = await request('gpioWrite', {
    socPad: 'PA6', level: true, eventTimeNs: '1000000'
  });
  assert.equal(pa6Edge.ok, true);
  const wfiExtiRun = await request('run');
  assert.equal(wfiExtiRun.ok, true);
  const wfiExtiResult = await waitForEvent(message =>
    message.payload.requestId === wfiExtiRun.requestId &&
    (message.event === 'fault' ||
      (message.event === 'state' && message.payload.stopped)));
  assert.equal(wfiExtiResult.event, 'state');
  assert.ok(BigInt(wfiExtiResult.payload.virtualTimeNs) >= 1000000n);
  assert.equal(wfiExtiResult.payload.sleeping, false);
}

if (process.env.M33MU_ACTIVE_EXTI_FIRMWARE) {
  const firmware = new Uint8Array(readFileSync(
    process.env.M33MU_ACTIVE_EXTI_FIRMWARE));
  const loopPc = Number(process.env.M33MU_ACTIVE_EXTI_LOOP_PC);
  assert.ok(Number.isInteger(loopPc) && loopPc > 0);
  const activeLoad = await request('loadFirmware', { bytes: firmware });
  assert.equal(activeLoad.ok, true);

  let loopState;
  for (let count = 0; count < 512; count++) {
    const steppedLoop = await request('step', { instructionBudget: 1 });
    assert.equal(steppedLoop.ok, true);
    if (steppedLoop.result.pc === (loopPc | 1)) {
      loopState = steppedLoop.result;
      break;
    }
  }
  assert.ok(loopState, 'firmware did not reach its active EXTI loop');
  const targetCycle = BigInt(loopState.cycleTotal) + 1n;
  const targetNs = targetCycle * 1000000000n / BigInt(loopState.cpuHz);
  const edge = await request('gpioWrite', {
    socPad: 'PA6', level: true, eventTimeNs: targetNs.toString()
  });
  assert.equal(edge.ok, true);

  const boundaryStep = await request('step', { instructionBudget: 1 });
  assert.equal(boundaryStep.ok, true);
  assert.equal(boundaryStep.result.registers[4],
    (loopState.registers[4] + 1) >>> 0,
    'instruction at the GPIO/EXTI boundary must execute before IRQ entry');

  const handler = await request('step', { instructionBudget: 32 });
  assert.equal(handler.ok, true);
  assert.equal(handler.result.stopped, true,
    'active-core timestamped EXTI did not reach the handler BKPT');
}

console.log('test-web-worker: ok');
