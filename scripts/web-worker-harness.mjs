// Drive the bundled worker entry under Node with a Worker-like scope.
import { pathToFileURL } from 'node:url';

export async function loadWorker(workerPath) {
  const messages = [];
  const waiters = new Map();
  const eventWaiters = [];
  const scope = {
    postMessage(message) {
      messages.push(message);
      if (message.kind === 'response' && waiters.has(message.requestId)) {
        waiters.get(message.requestId)(message);
        waiters.delete(message.requestId);
      }
      for (const w of [...eventWaiters]) {
        if (message.kind === 'event' && w.match(message)) {
          eventWaiters.splice(eventWaiters.indexOf(w), 1);
          w.resolve(message);
        }
      }
    },
    setTimeout
  };
  globalThis.self = scope;
  await import(pathToFileURL(workerPath).href);
  let nextId = 1;
  const request = (operation, payload = {}) => {
    const requestId = nextId++;
    const response = new Promise((resolve) => waiters.set(requestId, resolve));
    scope.onmessage({ data: { schemaVersion: 1, kind: 'request', requestId, operation, payload } });
    return response;
  };
  const waitEvent = (match, ms = 120000) => new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('timeout waiting for worker event')), ms);
    eventWaiters.push({ match, resolve: (m) => { clearTimeout(timer); resolve(m); } });
  });
  /** runUntil target; resolves with all bench payloads of the run. */
  const runBench = async (targetNs) => {
    const start = messages.length;
    const done = waitEvent((m) => m.event === 'fault' ||
      (m.event === 'bench' && (m.payload.reachedTarget || m.payload.mcuStopped) && m.payload.solverSteps > 0 &&
       BigInt(m.payload.timeNs) >= BigInt(targetNs)));
    const r = await request('runUntil', { targetNs: String(targetNs) });
    if (!r.ok) throw new Error(`runUntil failed: ${JSON.stringify(r.error)}`);
    const last = await done;
    if (last.event === 'fault') throw new Error(`bench fault: ${JSON.stringify(last.payload)}`);
    return messages.slice(start).filter((m) => m.event === 'bench').map((m) => m.payload);
  };
  return { messages, request, waitEvent, runBench };
}

export const pad = (socPad) => ({ socPad, vdd: 3.3, rOn: 40, rPull: 40e3, cIn: 5e-12,
  vRise: 1.775, vFall: 1.525, drive: 'z', pull: 'none' });
