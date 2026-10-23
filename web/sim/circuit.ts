// Modified nodal analysis transient solver for the workbench.
//
// All dynamic state lives in solver-owned typed arrays so a run can be
// snapshotted and rolled back exactly; the MCU coupling scheduler relies on
// that to retract analog predictions when firmware changes a pad.

import { DenseSystem } from './linalg.ts';
import {
  type DeviceSpec, type Netlist, type PadDrive, type PadElectrical, type PadPull,
  GROUND_NAMES, validateNetlist
} from './netlist.ts';

// Thermal voltage at 300.15 K (27 C), the SPICE nominal temperature.
const VT = 1.380649e-23 * 300.15 / 1.602176634e-19;
const GMIN = 1e-12;

export type SolverOptions = {
  reltol: number;
  vntol: number;
  abstol: number;
  /** LTE overestimation allowance, as SPICE TRTOL. */
  trtol: number;
  hMin: number;
  hMax: number;
  /** First step after start or a discontinuity. */
  hInit: number;
  maxNewton: number;
  /** Threshold-crossing localization tolerance, seconds. */
  eventTol: number;
};

export const DEFAULT_OPTIONS: SolverOptions = {
  reltol: 1e-4, vntol: 1e-6, abstol: 1e-12, trtol: 1,
  hMin: 1e-15, hMax: 1e-5, hInit: 1e-9, maxNewton: 100, eventTol: 2.5e-10
};

export class SolverError extends Error {
  code: string;
  constructor(code: string, message: string) {
    super(message);
    this.code = code;
  }
}

/** A watched node with Schmitt thresholds, e.g. an MCU input pad. */
export type Crossing = { watch: string; level: 0 | 1; t: number; volts: number };

type Method = 'be' | 'trap';

type Ctx = {
  dc: boolean;
  h: number;
  method: Method;
  /** Source scale for DC source stepping. */
  srcScale: number;
  gmin: number;
};

type Compiled = {
  kind: DeviceSpec['kind'];
  spec: DeviceSpec;
  n: number[];        // unknown index per terminal (-1 = ground)
  branch: number;     // branch-current unknown, or -1
  state: number;      // first state slot
};

// State slot layout per device kind (all in Circuit.state):
//  capacitor: [vPrev, iPrev]
//  inductor:  [vPrev] (branch current is an unknown)
//  diode/led: [vdIter]
//  bjt:       [vbeIter, vbcIter]
//  mos:       [vgsIter, vdsIter]
//  pad:       [vCinPrev, iCinPrev, driveCode, pullCode, digital]
const STATE_SLOTS: Record<DeviceSpec['kind'], number> = {
  ground: 0, resistor: 0, capacitor: 2, inductor: 1, vsource: 0, isource: 0,
  diode: 1, led: 1, switch: 1, npn: 2, pnp: 2, nmos: 2, pmos: 2, pad: 5
};

const DRIVE_CODE: Record<PadDrive, number> = { z: 0, low: 1, high: 2 };
const PULL_CODE: Record<PadPull, number> = { none: 0, up: 1, down: 2 };

export type Snapshot = {
  t: number;
  x: Float64Array;
  state: Float64Array;
  hist: Float64Array;
  histT: Float64Array;
  histCount: number;
  hNext: number;
  stepsSinceBreak: number;
  stepCount: number;
  rejected: number;
  newtonIterations: number;
};

function pnjlim(vnew: number, vold: number, vt: number, vcrit: number): number {
  if (vnew > vcrit && Math.abs(vnew - vold) > 2 * vt) {
    if (vold > 0) {
      const arg = 1 + (vnew - vold) / vt;
      return arg > 0 ? vold + vt * Math.log(arg) : vcrit;
    }
    return vt * Math.log(vnew / vt);
  }
  return vnew;
}

function fetlim(vnew: number, vold: number): number {
  const d = vnew - vold;
  if (d > 0.5) return vold + 0.5;
  if (d < -0.5) return vold - 0.5;
  return vnew;
}

function limvds(vnew: number, vold: number): number {
  if (vold >= 3.5) {
    if (vnew > vold) return Math.min(vnew, 3 * vold + 2);
    if (vnew < 3.5) return Math.max(vnew, 2);
    return vnew;
  }
  if (vnew > vold) return Math.min(vnew, 4);
  return Math.max(vnew, -0.5);
}

export class Circuit {
  readonly netNames: string[];          // unknown index -> net name
  readonly netIndex: Map<string, number>;
  readonly devices: Compiled[];
  readonly byId: Map<string, Compiled>;
  readonly size: number;
  readonly opts: SolverOptions;
  t = 0;
  x: Float64Array;
  state: Float64Array;
  stepCount = 0;
  rejected = 0;
  newtonIterations = 0;
  private sys: DenseSystem;
  private xIter: Float64Array;
  private xTrial: Float64Array;
  private stateTrial: Float64Array;
  // Accepted-point history for LTE: rows of reactive values, newest first.
  private reactive: { index: number; kind: 'v' | 'i'; a: number; b: number }[];
  private hist: Float64Array;
  private histT: Float64Array;
  private histCount = 0;
  private hNext: number;
  private stepsSinceBreak = 0;
  private padByName: Map<string, Compiled>;
  private converged = true;
  /** Called after every accepted step, including crossing-truncated steps. */
  onCommit: ((c: Circuit) => void) | null = null;

  constructor(netlist: Netlist, options: Partial<SolverOptions> = {}) {
    const problems = validateNetlist(netlist);
    if (problems.length) throw new SolverError('INVALID_NETLIST', problems.join('; '));
    this.opts = { ...DEFAULT_OPTIONS, ...options };
    const ground = new Set<string>(GROUND_NAMES);
    for (const d of netlist.devices) if (d.kind === 'ground') ground.add(d.node);
    this.netIndex = new Map();
    this.netNames = [];
    const net = (name: string) => {
      if (ground.has(name)) return -1;
      let i = this.netIndex.get(name);
      if (i === undefined) {
        i = this.netNames.length;
        this.netIndex.set(name, i);
        this.netNames.push(name);
      }
      return i;
    };
    const pending: Omit<Compiled, 'branch'>[] = [];
    let stateSize = 0;
    for (const spec of netlist.devices) {
      if (spec.kind === 'ground') continue;
      let terms: string[];
      switch (spec.kind) {
      case 'resistor': case 'capacitor': case 'inductor': case 'switch': terms = [spec.a, spec.b]; break;
      case 'vsource': case 'isource': terms = [spec.p, spec.n]; break;
      case 'diode': case 'led': terms = [spec.a, spec.k]; break;
      case 'npn': case 'pnp': terms = [spec.c, spec.b, spec.e]; break;
      case 'nmos': case 'pmos': terms = [spec.d, spec.g, spec.s]; break;
      case 'pad': terms = [spec.node]; break;
      }
      pending.push({ kind: spec.kind, spec, n: terms.map(net), state: stateSize });
      stateSize += STATE_SLOTS[spec.kind];
    }
    const nodeCount = this.netNames.length;
    let branches = 0;
    this.devices = pending.map((p) => ({
      ...p, branch: p.kind === 'vsource' || p.kind === 'inductor' ? nodeCount + branches++ : -1
    }));
    this.size = nodeCount + branches;
    this.byId = new Map(this.devices.map((d) => [d.spec.id, d]));
    this.padByName = new Map();
    for (const d of this.devices) {
      if (d.spec.kind === 'pad') this.padByName.set(d.spec.pad.socPad, d);
    }
    this.sys = new DenseSystem(Math.max(this.size, 1));
    this.x = new Float64Array(this.size);
    this.xIter = new Float64Array(this.size);
    this.xTrial = new Float64Array(this.size);
    this.state = new Float64Array(stateSize);
    this.stateTrial = new Float64Array(stateSize);
    this.reactive = [];
    for (const d of this.devices) {
      if (d.kind === 'capacitor') this.reactive.push({ index: -1, kind: 'v', a: d.n[0], b: d.n[1] });
      else if (d.kind === 'pad' && d.spec.kind === 'pad' && d.spec.pad.cIn > 0) {
        this.reactive.push({ index: -1, kind: 'v', a: d.n[0], b: -1 });
      } else if (d.kind === 'inductor') this.reactive.push({ index: d.branch, kind: 'i', a: -1, b: -1 });
      if (d.spec.kind === 'pad') {
        this.state[d.state + 2] = DRIVE_CODE[d.spec.pad.drive];
        this.state[d.state + 3] = PULL_CODE[d.spec.pad.pull];
      }
      if (d.spec.kind === 'switch') this.state[d.state] = d.spec.closed ? 1 : 0;
    }
    this.hist = new Float64Array(4 * this.reactive.length);
    this.histT = new Float64Array(4);
    this.hNext = this.opts.hInit;
  }

  // ---------------------------------------------------------------- access

  nodeVoltage(name: string): number {
    if (GROUND_NAMES.has(name)) return 0;
    const i = this.netIndex.get(name);
    if (i === undefined) return 0;
    return this.x[i];
  }

  private v(i: number, x: Float64Array = this.x): number {
    return i < 0 ? 0 : x[i];
  }

  /** Current through a two-terminal device from its first to second terminal,
   * or out of an MCU pad into the circuit. */
  deviceCurrent(id: string): number {
    const d = this.byId.get(id);
    if (!d) return 0;
    const x = this.x;
    const s = d.spec;
    switch (s.kind) {
    case 'resistor': return (this.v(d.n[0]) - this.v(d.n[1])) / s.ohms;
    case 'switch': return (this.v(d.n[0]) - this.v(d.n[1])) * this.switchG(d);
    case 'capacitor': return this.state[d.state + 1];
    case 'inductor': return x[d.branch];
    case 'vsource': return -x[d.branch];
    case 'isource': return s.amps;
    case 'diode': case 'led': {
      const vd = this.v(d.n[0]) - this.v(d.n[1]);
      return this.diodeCurrent(s.model.is, s.model.n, s.model.bv, vd)[0];
    }
    case 'pad': return this.padDriveCurrent(d, this.v(d.n[0]));
    case 'npn': case 'pnp': {
      const p = s.kind === 'npn' ? 1 : -1;
      const vbe = p * (this.v(d.n[1]) - this.v(d.n[2]));
      const vbc = p * (this.v(d.n[1]) - this.v(d.n[0]));
      return p * this.bjt(s.model.is, s.model.bf, s.model.br, vbe, vbc).ic;
    }
    case 'nmos': case 'pmos': {
      const p = s.kind === 'nmos' ? 1 : -1;
      const vgsN = p * (this.v(d.n[1]) - this.v(d.n[2]));
      const vdsN = p * (this.v(d.n[0]) - this.v(d.n[2]));
      const sign = vdsN < 0 ? -1 : 1;
      const vgs = vdsN < 0 ? vgsN - vdsN : vgsN;
      return sign * p * this.mos(s.model.vto * p, s.model.kp, s.model.lambda, vgs, Math.abs(vdsN)).id;
    }
    default: return 0;
    }
  }

  hasPad(socPad: string): boolean {
    return this.padByName.has(socPad);
  }

  padSocPads(): string[] {
    return [...this.padByName.keys()];
  }

  padNodeVoltage(socPad: string): number {
    const d = this.padByName.get(socPad);
    return d ? this.v(d.n[0]) : 0;
  }

  padDigital(socPad: string): 0 | 1 {
    const d = this.padByName.get(socPad);
    return d && this.state[d.state + 4] ? 1 : 0;
  }

  padSpec(socPad: string): PadElectrical | null {
    const d = this.padByName.get(socPad);
    return d && d.spec.kind === 'pad' ? d.spec.pad : null;
  }

  /** Change an MCU pad's drive. This is a discontinuity: the next step
   * restarts with backward Euler at hInit. */
  setPad(socPad: string, drive: PadDrive, pull: PadPull): boolean {
    const d = this.padByName.get(socPad);
    if (!d) return false;
    const dc = DRIVE_CODE[drive];
    const pc = PULL_CODE[pull];
    if (this.state[d.state + 2] === dc && this.state[d.state + 3] === pc) return false;
    this.state[d.state + 2] = dc;
    this.state[d.state + 3] = pc;
    this.breakpoint();
    return true;
  }

  setSwitch(id: string, closed: boolean): boolean {
    const d = this.byId.get(id);
    if (!d || d.kind !== 'switch') return false;
    const v = closed ? 1 : 0;
    if (this.state[d.state] === v) return false;
    this.state[d.state] = v;
    this.breakpoint();
    return true;
  }

  breakpoint(): void {
    this.histCount = 0;
    this.stepsSinceBreak = 0;
    this.hNext = this.opts.hInit;
  }

  snapshot(): Snapshot {
    return {
      t: this.t, x: this.x.slice(), state: this.state.slice(), hist: this.hist.slice(),
      histT: this.histT.slice(), histCount: this.histCount, hNext: this.hNext,
      stepsSinceBreak: this.stepsSinceBreak, stepCount: this.stepCount,
      rejected: this.rejected, newtonIterations: this.newtonIterations
    };
  }

  restore(s: Snapshot): void {
    this.t = s.t;
    this.x.set(s.x);
    this.state.set(s.state);
    this.hist.set(s.hist);
    this.histT.set(s.histT);
    this.histCount = s.histCount;
    this.hNext = s.hNext;
    this.stepsSinceBreak = s.stepsSinceBreak;
    this.stepCount = s.stepCount;
    this.rejected = s.rejected;
    this.newtonIterations = s.newtonIterations;
  }

  // ---------------------------------------------------------------- models

  private switchG(d: Compiled): number {
    const s = d.spec;
    if (s.kind !== 'switch') return 0;
    return this.state[d.state] ? 1 / (s.ron ?? 0.01) : 1 / (s.roff ?? 1e12);
  }

  private diodeCurrent(is: number, n: number, bv: number | undefined, vd: number): [number, number] {
    const nvt = n * VT;
    const e = Math.exp(Math.min(vd / nvt, 80));
    let id = is * (e - 1);
    let gd = is * e / nvt;
    if (bv !== undefined && vd < -bv + 50 * nvt) {
      const r = Math.exp(Math.min((-bv - vd) / nvt, 80));
      id -= is * r;
      gd += is * r / nvt;
    }
    return [id, gd + GMIN];
  }

  private bjt(is: number, bf: number, br: number, vbe: number, vbc: number) {
    const ebe = Math.exp(Math.min(vbe / VT, 80));
    const ebc = Math.exp(Math.min(vbc / VT, 80));
    const iF = is * (ebe - 1);
    const iR = is * (ebc - 1);
    const gF = is * ebe / VT + GMIN;
    const gR = is * ebc / VT + GMIN;
    return {
      ic: iF - iR - iR / br,
      ib: iF / bf + iR / br,
      dIcDvbe: gF, dIcDvbc: -gR * (1 + 1 / br),
      dIbDvbe: gF / bf, dIbDvbc: gR / br
    };
  }

  /** Level-1 drain current for vds >= 0 (caller handles reversal). */
  private mos(vto: number, kp: number, lambda: number, vgs: number, vds: number) {
    const vov = vgs - vto;
    if (vov <= 0) return { id: 0, gm: 0, gds: 0 };
    const clm = 1 + lambda * vds;
    if (vds < vov) {
      const core = vov * vds - vds * vds / 2;
      return { id: kp * core * clm, gm: kp * vds * clm,
        gds: kp * (vov - vds) * clm + kp * core * lambda };
    }
    return { id: kp / 2 * vov * vov * clm, gm: kp * vov * clm, gds: kp / 2 * vov * vov * lambda };
  }

  private padDriveCurrent(d: Compiled, vp: number): number {
    const s = d.spec;
    if (s.kind !== 'pad') return 0;
    const p = s.pad;
    let i = 0;
    const drive = this.state[d.state + 2];
    const pull = this.state[d.state + 3];
    if (drive === 2) i += (p.vdd - vp) / p.rOn;
    else if (drive === 1) i += -vp / p.rOn;
    if (pull === 1) i += (p.vdd - vp) / p.rPull;
    else if (pull === 2) i += -vp / p.rPull;
    return i;
  }

  // ---------------------------------------------------------------- stamping

  /** Linearized nonlinear current from terminal set: I_k(v) into the device. */
  private stampNonlinear(nodes: number[], current: number[], jac: number[][], vop: number[]): void {
    const sys = this.sys;
    for (let k = 0; k < nodes.length; k++) {
      const row = nodes[k];
      if (row < 0) continue;
      let lin = current[k];
      for (let j = 0; j < nodes.length; j++) {
        const g = jac[k][j];
        if (g === 0) continue;
        sys.add(row, nodes[j], g);
        lin -= g * vop[j];
      }
      sys.addRhs(row, -lin);
    }
  }

  private conductance(a: number, b: number, g: number): void {
    const sys = this.sys;
    sys.add(a, a, g); sys.add(b, b, g); sys.add(a, b, -g); sys.add(b, a, -g);
  }

  /** Current source pushing `i` from node a through the device into node b. */
  private currentFlow(a: number, b: number, i: number): void {
    this.sys.addRhs(a, -i);
    this.sys.addRhs(b, i);
  }

  private stamp(x: Float64Array, st: Float64Array, ctx: Ctx): void {
    const sys = this.sys;
    sys.clear();
    for (let i = 0; i < this.netNames.length; i++) sys.add(i, i, ctx.gmin);
    this.converged = true;
    for (const d of this.devices) {
      const s = d.spec;
      const n = d.n;
      switch (s.kind) {
      case 'resistor': this.conductance(n[0], n[1], 1 / s.ohms); break;
      case 'switch': this.conductance(n[0], n[1], this.switchG(d)); break;
      case 'capacitor': case 'pad': {
        let a = n[0];
        let b = n[1] ?? -1;
        let c = 0;
        if (s.kind === 'capacitor') c = s.farads;
        else {
          c = s.pad.cIn;
          b = -1;
          a = n[0];
          const p = s.pad;
          const drive = st[d.state + 2];
          const pull = st[d.state + 3];
          let g = 0;
          let gv = 0;
          if (drive === 2) { g += 1 / p.rOn; gv += p.vdd * ctx.srcScale / p.rOn; }
          else if (drive === 1) g += 1 / p.rOn;
          if (pull === 1) { g += 1 / p.rPull; gv += p.vdd * ctx.srcScale / p.rPull; }
          else if (pull === 2) g += 1 / p.rPull;
          sys.add(a, a, g);
          sys.addRhs(a, gv);
        }
        if (ctx.dc || c <= 0) break;
        const geq = (ctx.method === 'trap' ? 2 : 1) * c / ctx.h;
        const vPrev = st[d.state];
        const iPrev = st[d.state + 1];
        const ieq = ctx.method === 'trap' ? geq * vPrev + iPrev : geq * vPrev;
        this.conductance(a, b, geq);
        // i = geq*v - ieq flows a->b; the -ieq part is a source b->a.
        this.currentFlow(b, a, ieq);
        break;
      }
      case 'inductor': {
        const br = d.branch;
        sys.add(n[0], br, 1); sys.add(n[1], br, -1);
        sys.add(br, n[0], 1); sys.add(br, n[1], -1);
        if (ctx.dc) break;
        const req = (ctx.method === 'trap' ? 2 : 1) * s.henries / ctx.h;
        sys.add(br, br, -req);
        const iPrev = this.x[br];
        const vPrev = st[d.state];
        sys.addRhs(br, ctx.method === 'trap' ? -req * iPrev - vPrev : -req * iPrev);
        break;
      }
      case 'vsource': {
        const br = d.branch;
        sys.add(n[0], br, 1); sys.add(n[1], br, -1);
        sys.add(br, n[0], 1); sys.add(br, n[1], -1);
        sys.addRhs(br, s.volts * ctx.srcScale);
        break;
      }
      case 'isource': this.currentFlow(n[1], n[0], s.amps * ctx.srcScale); break;
      case 'diode': case 'led': {
        const nvt = s.model.n * VT;
        const vcrit = nvt * Math.log(nvt / (Math.SQRT2 * s.model.is));
        const raw = this.v(n[0], x) - this.v(n[1], x);
        let vd = pnjlim(raw, st[d.state], nvt, vcrit);
        if (s.model.bv !== undefined && vd < -s.model.bv + 10 * nvt) {
          // Mirror limiting for the breakdown branch.
          vd = -pnjlim(-vd - s.model.bv, -st[d.state] - s.model.bv, nvt, vcrit) - s.model.bv;
        }
        if (Math.abs(vd - raw) > 1e-9) this.converged = false;
        st[d.state] = vd;
        const [id, gd] = this.diodeCurrent(s.model.is, s.model.n, s.model.bv, vd);
        this.conductance(n[0], n[1], gd);
        this.currentFlow(n[0], n[1], id - gd * vd);
        break;
      }
      case 'npn': case 'pnp': {
        const p = s.kind === 'npn' ? 1 : -1;
        const vcrit = VT * Math.log(VT / (Math.SQRT2 * s.model.is));
        const rawBe = p * (this.v(n[1], x) - this.v(n[2], x));
        const rawBc = p * (this.v(n[1], x) - this.v(n[0], x));
        const vbe = pnjlim(rawBe, st[d.state], VT, vcrit);
        const vbc = pnjlim(rawBc, st[d.state + 1], VT, vcrit);
        if (Math.abs(vbe - rawBe) > 1e-9 || Math.abs(vbc - rawBc) > 1e-9) this.converged = false;
        st[d.state] = vbe; st[d.state + 1] = vbc;
        const m = this.bjt(s.model.is, s.model.bf, s.model.br, vbe, vbc);
        // Terminal order c, b, e; currents into the device.
        const ic = p * m.ic;
        const ib = p * m.ib;
        // d/dV_term: vbe = p(vb - ve), vbc = p(vb - vc)
        const dIc = [-m.dIcDvbc, m.dIcDvbe + m.dIcDvbc, -m.dIcDvbe];
        const dIb = [-m.dIbDvbc, m.dIbDvbe + m.dIbDvbc, -m.dIbDvbe];
        const dIe = dIc.map((g, j) => -(g + dIb[j]));
        // Junction voltages are the limited ones: reconstruct terminal
        // operating voltages consistent with them (emitter as reference).
        const ve = this.v(n[2], x);
        const vb = ve + p * vbe;
        const vc = vb - p * vbc;
        this.stampNonlinear(n, [ic, ib, -(ic + ib)], [dIc, dIb, dIe], [vc, vb, ve]);
        break;
      }
      case 'nmos': case 'pmos': {
        const p = s.kind === 'nmos' ? 1 : -1;
        // Limit in the netlist orientation (SPICE fetlim/limvds), then
        // swap drain and source for a negative vds.
        const rawVgs = p * (this.v(n[1], x) - this.v(n[2], x));
        const rawVds = p * (this.v(n[0], x) - this.v(n[2], x));
        const vgsN = fetlim(rawVgs, st[d.state]);
        const vdsN = limvds(rawVds, st[d.state + 1]);
        if (Math.abs(vgsN - rawVgs) > 1e-9 || Math.abs(vdsN - rawVds) > 1e-9) this.converged = false;
        st[d.state] = vgsN; st[d.state + 1] = vdsN;
        const reversed = vdsN < 0;
        const dI = reversed ? n[2] : n[0];
        const sI = reversed ? n[0] : n[2];
        const vgs = reversed ? vgsN - vdsN : vgsN;
        const vds = reversed ? -vdsN : vdsN;
        const m = this.mos(s.model.vto * p, s.model.kp, s.model.lambda, vgs, vds);
        // Drain current p*id flows d->s; terminals [d, g, s] after reversal.
        const nodes = [dI, n[1], sI];
        const id = p * m.id;
        const jd = [m.gds, m.gm, -(m.gm + m.gds)];
        const vs = this.v(sI, x);
        const vg = vs + p * vgs;
        const vd = vs + p * vds;
        this.stampNonlinear(nodes, [id, 0, -id], [jd, [0, 0, 0], jd.map((g) => -g)], [vd, vg, vs]);
        this.conductance(dI, sI, GMIN);
        break;
      }
      default: break;
      }
    }
  }

  // ---------------------------------------------------------------- solving

  /** Newton iteration from xIter; result in xIter. Returns iterations or -1. */
  private newton(ctx: Ctx, st: Float64Array): number {
    const x = this.xIter;
    const next = this.xTrial;
    const { reltol, vntol, abstol, maxNewton } = this.opts;
    const nodes = this.netNames.length;
    for (let it = 1; it <= maxNewton; it++) {
      this.stamp(x, st, ctx);
      const bad = this.sys.solve(next);
      if (bad >= 0) {
        const what = bad < nodes ? `node ${this.netNames[bad]}` : 'a voltage source or inductor loop';
        throw new SolverError('SINGULAR', `Circuit matrix is singular at ${what}`);
      }
      let ok = this.converged;
      for (let i = 0; i < this.size; i++) {
        const tol = reltol * Math.max(Math.abs(next[i]), Math.abs(x[i])) + (i < nodes ? vntol : abstol);
        if (Math.abs(next[i] - x[i]) > tol) ok = false;
        x[i] = next[i];
      }
      this.newtonIterations++;
      if (ok && it > 1) return it;
    }
    return -1;
  }

  /** DC operating point with gmin and source stepping fallbacks. */
  operatingPoint(): void {
    const st = this.stateTrial;
    const tryDc = (gmin: number, srcScale: number, from: Float64Array | null) => {
      if (from) this.xIter.set(from); else this.xIter.fill(0);
      st.set(this.state);
      return this.newton({ dc: true, h: 0, method: 'be', srcScale, gmin }, st) >= 0;
    };
    let ok = tryDc(GMIN, 1, this.x);
    if (!ok) {
      ok = true;
      let prev: Float64Array | null = null;
      for (let g = 1e-2; g >= GMIN * 0.99; g /= 10) {
        if (!tryDc(g, 1, prev)) { ok = false; break; }
        prev = this.xIter.slice();
      }
    }
    if (!ok) {
      ok = true;
      let prev: Float64Array | null = null;
      for (let k = 1; k <= 20; k++) {
        if (!tryDc(GMIN, k / 20, prev)) { ok = false; break; }
        prev = this.xIter.slice();
      }
    }
    if (!ok) throw new SolverError('NONCONVERGENT', 'DC operating point did not converge');
    this.x.set(this.xIter);
    this.state.set(st);
    for (const d of this.devices) {
      if (d.kind === 'capacitor' || d.kind === 'pad') {
        this.state[d.state] = this.v(d.n[0]) - (d.kind === 'capacitor' ? this.v(d.n[1]) : 0);
        this.state[d.state + 1] = 0;
      } else if (d.kind === 'inductor') {
        this.state[d.state] = 0;
      }
    }
    this.initDigital();
    this.breakpoint();
  }

  /** Zero initial conditions: every node at 0 V, as at power-on. */
  zeroState(): void {
    this.x.fill(0);
    for (const d of this.devices) {
      if (d.kind === 'capacitor' || d.kind === 'pad') {
        this.state[d.state] = 0;
        this.state[d.state + 1] = 0;
      }
    }
    this.initDigital();
    this.breakpoint();
  }

  private initDigital(): void {
    for (const d of this.padByName.values()) {
      if (d.spec.kind !== 'pad') continue;
      const v = this.v(d.n[0]);
      this.state[d.state + 4] = v >= d.spec.pad.vRise ? 1 : 0;
    }
  }

  /** Solve one step of size h from the committed state into xTrial/stateTrial. */
  private trial(h: number, method: Method): boolean {
    this.xIter.set(this.x);
    this.stateTrial.set(this.state);
    const it = this.newton({ dc: false, h, method, srcScale: 1, gmin: GMIN }, this.stateTrial);
    if (it < 0) return false;
    // Update reactive histories in the trial state.
    const xs = this.xIter;
    const st = this.stateTrial;
    for (const d of this.devices) {
      const s = d.spec;
      if (s.kind === 'capacitor' || (s.kind === 'pad' && s.pad.cIn > 0)) {
        const c = s.kind === 'capacitor' ? s.farads : s.pad.cIn;
        const b = s.kind === 'capacitor' ? d.n[1] : -1;
        const vNew = this.v(d.n[0], xs) - this.v(b, xs);
        const geq = (method === 'trap' ? 2 : 1) * c / h;
        const vPrev = this.state[d.state];
        const iPrev = this.state[d.state + 1];
        st[d.state] = vNew;
        st[d.state + 1] = method === 'trap' ? geq * (vNew - vPrev) - iPrev : geq * (vNew - vPrev);
      } else if (s.kind === 'inductor') {
        st[d.state] = this.v(d.n[0], xs) - this.v(d.n[1], xs);
      }
    }
    return true;
  }

  private reactiveValue(r: { index: number; kind: 'v' | 'i'; a: number; b: number }, x: Float64Array): number {
    return r.kind === 'i' ? x[r.index] : this.v(r.a, x) - this.v(r.b, x);
  }

  /** Local truncation error ratio (error / tolerance) for a trial step; <= 1 accepts. */
  private lteRatio(h: number, method: Method): number {
    const n = this.reactive.length;
    if (n === 0) return 0;
    const order = method === 'trap' ? 3 : 2;
    if (this.histCount < order) return 0;
    const ts = [this.t + h, this.histT[0], this.histT[1], this.histT[2]];
    let worst = 0;
    for (let r = 0; r < n; r++) {
      const val = [this.reactiveValue(this.reactive[r], this.xIter),
        this.hist[r * 4], this.hist[r * 4 + 1], this.hist[r * 4 + 2]];
      // Divided differences.
      const d1 = [0, 1, 2].map((k) => (val[k] - val[k + 1]) / (ts[k] - ts[k + 1]));
      const d2 = [0, 1].map((k) => (d1[k] - d1[k + 1]) / (ts[k] - ts[k + 2]));
      let err: number;
      if (order === 3) {
        const d3 = (d2[0] - d2[1]) / (ts[0] - ts[3]);
        err = Math.abs(h * h * h / 12 * 6 * d3);
      } else {
        err = Math.abs(h * h / 2 * 2 * d2[0]);
      }
      const abs = this.reactive[r].kind === 'v' ? this.opts.vntol * 10 : 1e-9;
      const tol = this.opts.trtol * (this.opts.reltol * Math.max(Math.abs(val[0]), Math.abs(val[1])) + abs);
      worst = Math.max(worst, err / tol);
    }
    return worst;
  }

  private commit(h: number): void {
    this.t += h;
    this.x.set(this.xIter);
    this.state.set(this.stateTrial);
    const n = this.reactive.length;
    for (let r = 0; r < n; r++) {
      this.hist[r * 4 + 3] = this.hist[r * 4 + 2];
      this.hist[r * 4 + 2] = this.hist[r * 4 + 1];
      this.hist[r * 4 + 1] = this.hist[r * 4];
      this.hist[r * 4] = this.reactiveValue(this.reactive[r], this.x);
    }
    this.histT[3] = this.histT[2]; this.histT[2] = this.histT[1];
    this.histT[1] = this.histT[0]; this.histT[0] = this.t;
    this.histCount = Math.min(this.histCount + 1, 4);
    this.stepsSinceBreak++;
    this.stepCount++;
    if (this.onCommit) this.onCommit(this);
  }

  /** Trial-state digital transition for a pad, if its Schmitt threshold was crossed. */
  private crossingAt(d: Compiled, x: Float64Array): 0 | 1 | -1 {
    if (d.spec.kind !== 'pad') return -1;
    const v = this.v(d.n[0], x);
    const level = this.state[d.state + 4];
    if (!level && v >= d.spec.pad.vRise) return 1;
    if (level && v <= d.spec.pad.vFall) return 0;
    return -1;
  }

  private firstCrossing(): { d: Compiled; level: 0 | 1 } | null {
    for (const d of this.padByName.values()) {
      const c = this.crossingAt(d, this.xIter);
      if (c >= 0) return { d, level: c as 0 | 1 };
    }
    return null;
  }

  /**
   * Advance to tEnd. Stops early, with the circuit exactly at the crossing,
   * after `maxCrossings` pad threshold crossings. Returns crossings in order.
   */
  advanceTo(tEnd: number, maxCrossings = Infinity, stepBudget = Infinity): Crossing[] {
    const out: Crossing[] = [];
    const o = this.opts;
    let steps = 0;
    while (tEnd - this.t > o.hMin && out.length < maxCrossings && steps < stepBudget) {
      const method: Method = this.stepsSinceBreak < 2 ? 'be' : 'trap';
      let h = Math.min(this.hNext, o.hMax, tEnd - this.t);
      // Avoid leaving a sliver before tEnd.
      if (tEnd - this.t - h < o.hMin * 10) h = tEnd - this.t;
      let ok = this.trial(h, method);
      let ratio = ok ? this.lteRatio(h, method) : Infinity;
      while (!ok || ratio > 1) {
        this.rejected++;
        const shrink = ok ? Math.max(0.1, 0.9 * Math.pow(ratio, -1 / (method === 'trap' ? 3 : 2))) : 0.125;
        h *= shrink;
        if (h < o.hMin) {
          throw new SolverError(ok ? 'TIMESTEP_TOO_SMALL' : 'NONCONVERGENT',
            `Transient step fell below ${o.hMin} s at t=${this.t}`);
        }
        ok = this.trial(h, method);
        ratio = ok ? this.lteRatio(h, method) : Infinity;
      }
      steps++;
      const crossing = this.firstCrossing();
      if (crossing) {
        // Illinois regula falsi on the step size; keep the side past the threshold.
        const d = crossing.d;
        const spec = d.spec.kind === 'pad' ? d.spec.pad : null;
        const thr = crossing.level ? spec!.vRise : spec!.vFall;
        const f = (x: Float64Array) => (this.v(d.n[0], x) - thr) * (crossing.level ? 1 : -1);
        let lo = 0;
        let flo = f(this.x);
        let hi = h;
        let fhi = f(this.xIter);
        let side = 0;
        while (hi - lo > o.eventTol) {
          let m = lo + (hi - lo) * (-flo / (fhi - flo));
          if (!(m > lo + (hi - lo) * 1e-3 && m < hi - (hi - lo) * 1e-3)) m = (lo + hi) / 2;
          if (!this.trial(m, method)) { m = (lo + hi) / 2; if (!this.trial(m, method)) break; }
          const fm = f(this.xIter);
          if (fm >= 0) {
            hi = m; fhi = fm;
            if (side === 1) flo /= 2;
            side = 1;
          } else {
            lo = m; flo = fm;
            if (side === -1) fhi /= 2;
            side = -1;
          }
        }
        this.trial(hi, method);
        this.commit(hi);
        this.state[d.state + 4] = crossing.level;
        out.push({ watch: spec!.socPad, level: crossing.level, t: this.t, volts: this.v(d.n[0]) });
        // A crossing is not a discontinuity; keep the step size.
        this.hNext = Math.max(h, o.hInit);
        continue;
      }
      this.commit(h);
      const order = method === 'trap' ? 3 : 2;
      const grow = ratio > 0 ? Math.min(2, 0.9 * Math.pow(ratio, -1 / order)) : 2;
      this.hNext = Math.max(o.hMin, Math.min(o.hMax, h * Math.max(grow, 0.5)));
    }
    if (Math.abs(tEnd - this.t) <= o.hMin) this.t = tEnd;
    return out;
  }
}
