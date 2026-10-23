// Shared virtual-time scheduler for one MCU session and one analog circuit.
//
// The MCU and the circuit advance in alternating windows. The circuit is
// solved ahead to a horizon with the pad drives held at their current values;
// every predicted input-threshold crossing is queued in the MCU at its
// timestamp. The MCU then runs to the horizon and stops at the first pad drive
// or configuration change. If that change concerns a pad in the circuit, the
// circuit rolls back to the last synchronized snapshot, replays to the change,
// applies the new drive and continues; predicted crossings after the change are
// cancelled. The firmware therefore never runs past an unresolved analog
// event, and the circuit never uses a pad drive before the firmware set it.
//
// Rounding: a crossing at time t is delivered at the first MCU instruction
// boundary at or after ceil(t) ns. A pad change is applied to the circuit at
// the virtual nanosecond the MCU reported for it.

import { Circuit, type Crossing, type Snapshot } from './circuit.ts';
import type { PadDrive, PadPull } from './netlist.ts';

export type McuPadEvent = {
  socPad: string;
  level: 0 | 1 | 2;
  mode: number;
  openDrain: boolean;
  pull: number;
  timeNs: number;
};

export type McuRunResult = {
  deadlineReached: boolean;
  padChanged: boolean;
  stopped: boolean;
  systemReset: boolean;
  instructions: number;
};

/** The subset of the m33mu web session the bench needs. */
export interface McuPort {
  nowNs(): number;
  runUntil(deadlineNs: number, instructionBudget: number, workBudget: number): McuRunResult;
  popPadEvents(): McuPadEvent[];
  padState(socPad: string): Omit<McuPadEvent, 'timeNs' | 'socPad'>;
  /** Queue an input level; times at or before now apply at the next boundary. */
  scheduleInput(socPad: string, level: 0 | 1, timeNs: number): void;
  cancelInputs(fromNs: number): void;
}

export type Probe =
  | { id: string; kind: 'voltage'; net: string }
  | { id: string; kind: 'current'; device: string }
  | { id: string; kind: 'pad'; socPad: string };

export type BenchOptions = {
  lookaheadNs: number;
  maxCrossingsPerWindow: number;
  sampleNs: number;
  maxSamples: number;
  mcuInstructionSlice: number;
  mcuWorkSlice: number;
};

export const DEFAULT_BENCH: BenchOptions = {
  lookaheadNs: 50_000, maxCrossingsPerWindow: 32, sampleNs: 1000, maxSamples: 200_000,
  mcuInstructionSlice: 4096, mcuWorkSlice: 4096
};

export type BenchEvent =
  | { kind: 'crossing'; socPad: string; level: 0 | 1; timeNs: number; volts: number }
  | { kind: 'pad'; socPad: string; drive: PadDrive; pull: PadPull; mode: number; timeNs: number }
  | { kind: 'unsupported'; socPad: string; timeNs: number; message: string }
  | { kind: 'gpio'; socPad: string; level: 0 | 1 | 2; mode: number; pull: number; timeNs: number }
  | { kind: 'switch'; device: string; closed: boolean; timeNs: number };

export type SliceReport = {
  timeNs: number;
  reachedTarget: boolean;
  mcuStopped: boolean;
  systemReset: boolean;
  instructions: number;
  solverSteps: number;
  rollbacks: number;
};

const AF_MESSAGE = 'Alternate-function output is not modelled electrically; the pad is treated as high impedance';

export function padDriveFromMcu(level: 0 | 1 | 2, pull: number): { drive: PadDrive; pull: PadPull } {
  return {
    drive: level === 1 ? 'high' : level === 0 ? 'low' : 'z',
    pull: pull === 1 ? 'up' : pull === 2 ? 'down' : 'none'
  };
}

export class Bench {
  readonly circuit: Circuit;
  readonly mcu: McuPort | null;
  readonly opts: BenchOptions;
  readonly probes: Probe[];
  /** Final (non-speculative) samples: time ns then one value per probe. */
  samples: Float64Array[] = [];
  sampleOverflow = 0;
  events: BenchEvent[] = [];
  rollbacks = 0;
  private log: { t: number; values: Float64Array }[] = [];
  private nextSampleNs = 0;
  private sync: Snapshot;
  private syncNs = 0;
  private stopped = false;

  constructor(circuit: Circuit, mcu: McuPort | null, probes: Probe[] = [],
    options: Partial<BenchOptions> = {}) {
    this.circuit = circuit;
    this.mcu = mcu;
    this.probes = probes;
    this.opts = { ...DEFAULT_BENCH, ...options };
    for (const p of probes) {
      if (p.kind === 'voltage' && !circuit.netIndex.has(p.net) && !['0', 'gnd', 'GND'].includes(p.net)) {
        throw new Error(`Probe ${p.id}: unknown net ${p.net}`);
      }
      if (p.kind === 'current' && !circuit.byId.has(p.device)) {
        throw new Error(`Probe ${p.id}: unknown device ${p.device}`);
      }
    }
    const t0 = mcu ? mcu.nowNs() : 0;
    // Pads start in the state the firmware has them in now.
    if (mcu) {
      for (const socPad of circuit.padSocPads()) {
        const s = mcu.padState(socPad);
        const e = padDriveFromMcu(s.level, s.pull);
        circuit.setPad(socPad, e.drive, e.pull);
      }
    }
    circuit.t = t0 * 1e-9;
    circuit.operatingPoint();
    circuit.onCommit = (c) => this.record(c);
    this.nextSampleNs = t0;
    this.record(circuit);
    this.sync = circuit.snapshot();
    this.syncNs = t0;
    // Deliver the initial digital level of every input.
    if (mcu) {
      for (const socPad of circuit.padSocPads()) {
        mcu.scheduleInput(socPad, circuit.padDigital(socPad), t0);
      }
    }
    this.flush(t0);
  }

  get timeNs(): number {
    return this.mcu && !this.stopped ? this.mcu.nowNs() : this.circuit.t * 1e9;
  }

  private probeValue(p: Probe): number {
    switch (p.kind) {
    case 'voltage': return this.circuit.nodeVoltage(p.net);
    case 'current': return this.circuit.deviceCurrent(p.device);
    case 'pad': return this.circuit.padNodeVoltage(p.socPad);
    }
  }

  private record(c: Circuit): void {
    const values = new Float64Array(this.probes.length);
    this.probes.forEach((p, i) => { values[i] = this.probeValue(p); });
    this.log.push({ t: c.t * 1e9, values });
  }

  /** Resample the committed step log up to tNs onto the sample grid. */
  private flush(tNs: number): void {
    const log = this.log;
    let i = 0;
    while (this.nextSampleNs <= tNs + 1e-6 && log.length > 0) {
      while (i + 1 < log.length && log[i + 1].t < this.nextSampleNs) i++;
      const a = log[i];
      const b = log[Math.min(i + 1, log.length - 1)];
      if (b.t < this.nextSampleNs - 1e-6 && b === log[log.length - 1]) break;
      const row = new Float64Array(this.probes.length + 1);
      row[0] = this.nextSampleNs;
      const span = b.t - a.t;
      const f = span > 0 ? Math.min(1, Math.max(0, (this.nextSampleNs - a.t) / span)) : 0;
      for (let k = 0; k < this.probes.length; k++) {
        row[k + 1] = a.values[k] + (b.values[k] - a.values[k]) * f;
      }
      if (this.samples.length >= this.opts.maxSamples) {
        this.samples.shift();
        this.sampleOverflow++;
      }
      this.samples.push(row);
      this.nextSampleNs += this.opts.sampleNs;
    }
    // Keep the last entry at or before the next sample as the interpolation base.
    let keep = 0;
    while (keep + 1 < log.length && log[keep + 1].t <= this.nextSampleNs) keep++;
    if (keep > 0) log.splice(0, keep);
  }

  private truncateLog(tNs: number): void {
    while (this.log.length > 1 && this.log[this.log.length - 1].t > tNs + 1e-6) this.log.pop();
  }

  private deliver(crossings: Crossing[], earliestNs: number): void {
    for (const c of crossings) {
      const t = Math.max(Math.ceil(c.t * 1e9 - 1e-6), earliestNs);
      this.events.push({ kind: 'crossing', socPad: c.watch, level: c.level, timeNs: c.t * 1e9, volts: c.volts });
      this.mcu?.scheduleInput(c.watch, c.level, t);
    }
  }

  takeSamples(): Float64Array[] {
    const out = this.samples;
    this.samples = [];
    return out;
  }

  /** Events at or before the last synchronized time; later ones may still be retracted. */
  takeEvents(): BenchEvent[] {
    const limit = this.mcu && !this.stopped ? this.syncNs : Infinity;
    const out = this.events.filter((e) => e.timeNs <= limit);
    this.events = this.events.filter((e) => e.timeNs > limit);
    return out;
  }

  /** Advance toward targetNs, returning after at most `windows` scheduling windows. */
  runUntil(targetNs: number, windows = 64): SliceReport {
    const c = this.circuit;
    const report: SliceReport = { timeNs: this.timeNs, reachedTarget: false, mcuStopped: this.stopped,
      systemReset: false, instructions: 0, solverSteps: 0, rollbacks: 0 };
    const steps0 = c.stepCount;
    const mcu = this.mcu;
    if (!mcu || this.stopped) {
      // Analog-only: no consumer for crossings beyond the event log.
      const crossings = c.advanceTo(targetNs * 1e-9);
      for (const x of crossings) {
        this.events.push({ kind: 'crossing', socPad: x.watch, level: x.level, timeNs: x.t * 1e9, volts: x.volts });
      }
      this.flush(c.t * 1e9);
      report.timeNs = c.t * 1e9;
      report.reachedTarget = report.timeNs >= targetNs - 1e-6;
      report.solverSteps = c.stepCount - steps0;
      return report;
    }
    for (let w = 0; w < windows; w++) {
      let tm = mcu.nowNs();
      if (tm >= targetNs) { report.reachedTarget = true; break; }
      let ts = c.t * 1e9;
      if (ts < tm) {
        // The MCU overshot the horizon inside one instruction; drives are
        // unchanged, so crossings in between land on the current boundary.
        this.deliver(c.advanceTo(tm * 1e-9), tm);
        ts = c.t * 1e9;
      }
      if (Math.abs(ts - tm) < 1e-6) {
        this.flush(tm);
        this.sync = c.snapshot();
        this.syncNs = tm;
        const horizon = Math.min(targetNs, tm + this.opts.lookaheadNs);
        this.deliver(c.advanceTo(horizon * 1e-9, this.opts.maxCrossingsPerWindow), tm);
        ts = c.t * 1e9;
      }
      const r = mcu.runUntil(Math.ceil(ts - 1e-6), this.opts.mcuInstructionSlice, this.opts.mcuWorkSlice);
      report.instructions += r.instructions;
      if (r.systemReset) report.systemReset = true;
      tm = mcu.nowNs();
      const events = mcu.popPadEvents();
      for (const e of events) {
        this.events.push({ kind: 'gpio', socPad: e.socPad, level: e.level, mode: e.mode, pull: e.pull, timeNs: e.timeNs });
      }
      const relevant = events.filter((e) => c.hasPad(e.socPad));
      if (relevant.length > 0) this.applyPadEvents(relevant, tm);
      if (r.stopped) {
        this.stopped = true;
        report.mcuStopped = true;
        if (c.t * 1e9 > tm) this.rollbackTo(tm);
        else this.deliver(c.advanceTo(tm * 1e-9), tm);
        this.flush(tm);
        break;
      }
    }
    report.timeNs = this.timeNs;
    if (report.timeNs >= targetNs) report.reachedTarget = true;
    report.solverSteps = c.stepCount - steps0;
    report.rollbacks = this.rollbacks;
    return report;
  }

  /** Operate a switch at the current bench time. Speculative circuit state
   * beyond that time is rolled back and its predicted crossings cancelled. */
  setSwitch(id: string, closed: boolean): boolean {
    const c = this.circuit;
    const now = this.timeNs;
    if (this.mcu && !this.stopped) {
      this.mcu.cancelInputs(now + 1);
      if (c.t * 1e9 > now + 1e-6) this.rollbackTo(now);
      else if (c.t * 1e9 < now - 1e-6) this.deliver(c.advanceTo(now * 1e-9), now);
    }
    const changed = c.setSwitch(id, closed);
    if (changed) {
      this.events.push({ kind: 'switch', device: id, closed, timeNs: now });
      if (this.mcu && !this.stopped) {
        this.flush(now);
        this.sync = c.snapshot();
        this.syncNs = now;
      }
    }
    return changed;
  }

  private rollbackTo(tNs: number): void {
    const c = this.circuit;
    c.restore(this.sync);
    this.truncateLog(this.syncNs);
    this.rollbacks++;
    // Replayed crossings up to tNs were already delivered.
    const replayed = c.advanceTo(tNs * 1e-9);
    void replayed;
    // Drop crossing events recorded for the retracted interval.
    this.events = this.events.filter((e) => e.kind !== 'crossing' || e.timeNs <= tNs);
  }

  private applyPadEvents(events: McuPadEvent[], tm: number): void {
    const c = this.circuit;
    const mcu = this.mcu!;
    const first = events[0].timeNs;
    mcu.cancelInputs(first + 1);
    if (c.t * 1e9 > first) this.rollbackTo(first);
    else this.deliver(c.advanceTo(first * 1e-9), tm);
    for (const e of events) {
      if (c.t * 1e9 < e.timeNs) this.deliver(c.advanceTo(e.timeNs * 1e-9), tm);
      const { drive, pull } = padDriveFromMcu(e.level, e.pull);
      if (e.mode === 2) {
        this.events.push({ kind: 'unsupported', socPad: e.socPad, timeNs: e.timeNs, message: AF_MESSAGE });
      }
      if (c.setPad(e.socPad, drive, pull)) {
        this.events.push({ kind: 'pad', socPad: e.socPad, drive, pull, mode: e.mode, timeNs: e.timeNs });
      }
    }
    if (c.t * 1e9 < tm) this.deliver(c.advanceTo(tm * 1e-9), tm);
  }
}
