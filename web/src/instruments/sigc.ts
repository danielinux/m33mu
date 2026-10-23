// Browser reader/writer for the CLI's SIGC v1 container. See scripts/sigc.py.
// Scope import is display-only: this module does not bind traces to MCU pads.

export const SIGC_MAX_BYTES = 8 * 1024 * 1024;
export const SIGC_MAX_SAMPLES = 400_000;

export type SigcTrace = { name: string; fullScaleMv: number; samplesMv: Int16Array };
export type SigcCapture = { timesNs: number[]; traces: SigcTrace[] };

export class SigcError extends Error {}

function checkedName(name: string): Uint8Array {
  if (!name || name.length >= 32 || [...name].some((c) => c.charCodeAt(0) > 127 || c === '\0')) {
    throw new SigcError('SIGC trace names must be 1–31 ASCII bytes');
  }
  return new TextEncoder().encode(name);
}

function checkCapture(capture: SigcCapture): void {
  const { timesNs, traces } = capture;
  if (timesNs.length < 1 || timesNs.length > SIGC_MAX_SAMPLES) {
    throw new SigcError(`SIGC scope capture needs 1–${SIGC_MAX_SAMPLES} samples`);
  }
  if (traces.length < 1 || traces.length > 2) throw new SigcError('The scope supports one or two SIGC traces');
  const names = new Set<string>();
  let previous = -1;
  for (const t of timesNs) {
    if (!Number.isSafeInteger(t) || t < 0 || t < previous) {
      throw new SigcError('SIGC timestamps must be nondecreasing, safe whole nanoseconds');
    }
    previous = t;
  }
  for (const tr of traces) {
    checkedName(tr.name);
    if (names.has(tr.name)) throw new SigcError(`Duplicate SIGC trace name ${tr.name}`);
    names.add(tr.name);
    if (tr.samplesMv.length !== timesNs.length) throw new SigcError(`Trace ${tr.name} has the wrong sample count`);
    if (!Number.isInteger(tr.fullScaleMv) || tr.fullScaleMv < -0x80000000 || tr.fullScaleMv > 0x7fffffff) {
      throw new SigcError(`Trace ${tr.name} has an invalid full-scale field`);
    }
  }
}

export function encodeSigc(capture: SigcCapture): Uint8Array {
  checkCapture(capture);
  const { timesNs, traces } = capture;
  const length = 16 + 12 + timesNs.length * 8 + traces.length * (44 + timesNs.length * 2);
  if (length > SIGC_MAX_BYTES) throw new SigcError('SIGC scope capture exceeds 8 MiB');
  const bytes = new Uint8Array(length);
  const view = new DataView(bytes.buffer);
  bytes.set([83, 73, 71, 67], 0); // SIGC
  view.setUint32(4, 1, true); // version
  view.setUint32(8, 1, true); // one explicit timebase
  view.setUint32(12, traces.length, true);
  view.setUint32(16, 1, true); // timebase id
  view.setUint32(20, timesNs.length, true);
  view.setUint32(24, 0, true); // period 0 means explicit timestamps
  let at = 28;
  for (const t of timesNs) { view.setBigUint64(at, BigInt(t), true); at += 8; }
  traces.forEach((tr, i) => {
    view.setUint32(at, i + 1, true);
    bytes.set(checkedName(tr.name), at + 4);
    view.setUint32(at + 36, 1, true);
    view.setInt32(at + 40, tr.fullScaleMv, true);
    at += 44;
    for (const mv of tr.samplesMv) { view.setInt16(at, mv, true); at += 2; }
  });
  return bytes;
}

export function decodeSigc(bytes: Uint8Array): SigcCapture {
  if (bytes.length > SIGC_MAX_BYTES) throw new SigcError('SIGC input exceeds 8 MiB');
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let at = 0;
  const need = (n: number) => { if (at + n > bytes.length) throw new SigcError('Truncated SIGC container'); };
  const u32 = () => { need(4); const v = view.getUint32(at, true); at += 4; return v; };
  need(4);
  if (bytes[0] !== 83 || bytes[1] !== 73 || bytes[2] !== 71 || bytes[3] !== 67) throw new SigcError('Bad SIGC magic');
  at = 4;
  if (u32() !== 1) throw new SigcError('Unsupported SIGC version');
  const nTimebases = u32();
  const nTraces = u32();
  if (nTimebases !== 1 || nTraces < 1 || nTraces > 2) {
    throw new SigcError('The scope imports one SIGC timebase and one or two voltage traces');
  }
  const tbId = u32();
  const count = u32();
  const period = u32();
  if (count < 1 || count > SIGC_MAX_SAMPLES) throw new SigcError('SIGC sample count exceeds the scope limit');
  const timesNs: number[] = [];
  if (period === 0) {
    need(count * 8);
    for (let i = 0; i < count; i++) {
      const t = view.getBigUint64(at, true);
      if (t > BigInt(Number.MAX_SAFE_INTEGER)) throw new SigcError('SIGC timestamp exceeds safe nanoseconds');
      timesNs.push(Number(t)); at += 8;
    }
  } else {
    if ((count - 1) * period > Number.MAX_SAFE_INTEGER) throw new SigcError('SIGC timebase exceeds safe nanoseconds');
    for (let i = 0; i < count; i++) timesNs.push(i * period);
  }
  const traces: SigcTrace[] = [];
  for (let i = 0; i < nTraces; i++) {
    const id = u32();
    need(32);
    const nameBytes = bytes.subarray(at, at + 32);
    const nul = nameBytes.indexOf(0);
    if (nul < 1 || nameBytes.subarray(0, nul).some((b) => b > 127)) {
      throw new SigcError(`SIGC trace ${id} has an invalid name`);
    }
    const name = new TextDecoder('ascii').decode(nameBytes.subarray(0, nul));
    at += 32;
    if (u32() !== tbId) throw new SigcError(`SIGC trace ${name} uses another timebase`);
    need(4 + count * 2);
    const fullScaleMv = view.getInt32(at, true); at += 4;
    const samplesMv = new Int16Array(count);
    for (let j = 0; j < count; j++) { samplesMv[j] = view.getInt16(at, true); at += 2; }
    traces.push({ name, fullScaleMv, samplesMv });
  }
  if (at !== bytes.length) throw new SigcError('Trailing data after SIGC traces');
  checkCapture({ timesNs, traces });
  return { timesNs, traces };
}

export function roundMillivolts(volts: number): number {
  const scaled = volts * 1000;
  if (!Number.isFinite(scaled) || scaled < -32768 || scaled > 32767) {
    throw new SigcError('SIGC voltage sample must fit signed 16-bit millivolts');
  }
  const floor = Math.floor(scaled);
  const fraction = scaled - floor;
  return fraction < 0.5 ? floor : fraction > 0.5 ? floor + 1 : floor % 2 === 0 ? floor : floor + 1;
}
