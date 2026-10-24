// Versioned electrical netlist accepted by the solver and the worker.
// Terminals name nets; any net touched by a `ground` device is node 0.

export type DiodeModel = {
  /** Saturation current, A. */
  is: number;
  /** Emission coefficient. */
  n: number;
  /** Reverse breakdown voltage, V (positive). Omit for no breakdown. */
  bv?: number;
};

export type BjtModel = { is: number; bf: number; br: number };
export type MosModel = { vto: number; kp: number; lambda: number };
/** Five-terminal, rail-limited op-amp macro model. */
export type OpampModel = { gain: number; rOut: number; rIn: number; headroom: number };

/** Behavioural low-dropout regulator: output follows min(vset, vin - dropout)
 * through rOut, sources only, and draws iq from the input. */
export type RegulatorModel = { vset: number; dropout: number; rOut: number; iq: number };

export type PadDrive = 'high' | 'low' | 'z';
export type PadPull = 'up' | 'down' | 'none';

export type DeviceSpec =
  | { kind: 'ground'; id: string; node: string }
  | { kind: 'resistor'; id: string; a: string; b: string; ohms: number }
  | { kind: 'capacitor'; id: string; a: string; b: string; farads: number }
  | { kind: 'inductor'; id: string; a: string; b: string; henries: number }
  | { kind: 'vsource'; id: string; p: string; n: string; volts: number }
  | { kind: 'isource'; id: string; p: string; n: string; amps: number }
  | { kind: 'diode'; id: string; a: string; k: string; model: DiodeModel }
  | { kind: 'led'; id: string; a: string; k: string; model: DiodeModel;
      color: string; ratedAmps: number }
  | { kind: 'switch'; id: string; a: string; b: string; closed: boolean;
      ron?: number; roff?: number }
  | { kind: 'npn' | 'pnp'; id: string; c: string; b: string; e: string;
      model: BjtModel }
  | { kind: 'nmos' | 'pmos'; id: string; d: string; g: string; s: string;
      model: MosModel }
  | { kind: 'opamp'; id: string; plus: string; minus: string; out: string;
      vcc: string; vee: string; model: OpampModel }
  | { kind: 'regulator'; id: string; in: string; out: string; gnd: string;
      model: RegulatorModel }
  | { kind: 'pad'; id: string; node: string; pad: PadElectrical };

/** Finite electrical model of an MCU pad. Values are documented
 * approximations, see web/docs/electrical-model.md. */
export type PadElectrical = {
  socPad: string;
  vdd: number;
  /** Output driver resistance to the selected rail, ohms. */
  rOn: number;
  /** Pull resistor, ohms. */
  rPull: number;
  /** Pad input capacitance to ground, farads. */
  cIn: number;
  /** Input Schmitt thresholds, volts. */
  vRise: number;
  vFall: number;
  drive: PadDrive;
  pull: PadPull;
};

export type Netlist = {
  schemaVersion: 1;
  devices: DeviceSpec[];
};

export const GROUND_NAMES = new Set(['0', 'gnd', 'GND']);

export const MAX_DEVICES = 2000;

/** Generic models with stated provenance. */
export const MODELS = {
  diode1n4148: { is: 2.52e-9, n: 1.752, bv: 100 } as DiodeModel,
  ledRed: { is: 5.3e-11, n: 4 } as DiodeModel,
  ledGreen: { is: 1.445e-10, n: 4.5 } as DiodeModel,
  npn2n3904: { is: 6.734e-15, bf: 416.4, br: 0.7371 } as BjtModel,
  pnp2n3906: { is: 1.41e-15, bf: 180.7, br: 4.977 } as BjtModel,
  nmosGeneric: { vto: 1.5, kp: 0.02, lambda: 0 } as MosModel,
  pmosGeneric: { vto: -1.5, kp: 0.02, lambda: 0 } as MosModel,
  ldo3v3: { vset: 3.3, dropout: 0.3, rOut: 0.05, iq: 50e-6 } as RegulatorModel,
  opampGeneric: { gain: 1e5, rOut: 20, rIn: 1e9, headroom: 0.2 } as OpampModel
};

export function defaultPad(socPad: string, vdd = 3.3): PadElectrical {
  return {
    socPad, vdd, rOn: 40, rPull: 40e3, cIn: 5e-12,
    vRise: vdd * 1.775 / 3.3, vFall: vdd * 1.525 / 3.3,
    drive: 'z', pull: 'none'
  };
}

function terminals(d: DeviceSpec): string[] {
  switch (d.kind) {
  case 'ground': return [d.node];
  case 'resistor': case 'capacitor': case 'inductor': case 'switch':
    return [d.a, d.b];
  case 'vsource': case 'isource': return [d.p, d.n];
  case 'diode': case 'led': return [d.a, d.k];
  case 'npn': case 'pnp': return [d.c, d.b, d.e];
  case 'nmos': case 'pmos': return [d.d, d.g, d.s];
  case 'opamp': return [d.plus, d.minus, d.out, d.vcc, d.vee];
  case 'regulator': return [d.in, d.out, d.gnd];
  case 'pad': return [d.node];
  }
}

export function deviceTerminals(d: DeviceSpec): string[] {
  return terminals(d);
}

function positive(value: unknown): boolean {
  return typeof value === 'number' && Number.isFinite(value) && value > 0;
}

function finite(value: unknown): boolean {
  return typeof value === 'number' && Number.isFinite(value);
}

/** Returns a list of human-readable problems; empty means valid. */
export function validateNetlist(netlist: unknown): string[] {
  const errors: string[] = [];
  const n = netlist as Netlist;
  if (!n || typeof n !== 'object' || n.schemaVersion !== 1 || !Array.isArray(n.devices)) {
    return ['Netlist must be { schemaVersion: 1, devices: [...] }'];
  }
  if (n.devices.length > MAX_DEVICES) errors.push(`Netlist exceeds ${MAX_DEVICES} devices`);
  const ids = new Set<string>();
  const pads = new Set<string>();
  for (const d of n.devices) {
    if (!d || typeof d !== 'object' || typeof d.id !== 'string' || d.id === '') {
      errors.push('Every device needs a non-empty string id');
      continue;
    }
    if (ids.has(d.id)) errors.push(`Duplicate device id ${d.id}`);
    ids.add(d.id);
    const known = ['ground', 'resistor', 'capacitor', 'inductor', 'vsource', 'isource',
      'diode', 'led', 'switch', 'npn', 'pnp', 'nmos', 'pmos', 'opamp', 'regulator', 'pad'];
    if (!known.includes(d.kind)) {
      errors.push(`${d.id}: unsupported device kind ${String((d as { kind: unknown }).kind)}`);
      continue;
    }
    for (const t of terminals(d)) {
      if (typeof t !== 'string' || t === '') errors.push(`${d.id}: missing terminal net`);
    }
    switch (d.kind) {
    case 'resistor': if (!positive(d.ohms)) errors.push(`${d.id}: ohms must be > 0`); break;
    case 'capacitor': if (!positive(d.farads)) errors.push(`${d.id}: farads must be > 0`); break;
    case 'inductor': if (!positive(d.henries)) errors.push(`${d.id}: henries must be > 0`); break;
    case 'vsource': if (!finite(d.volts)) errors.push(`${d.id}: volts must be finite`); break;
    case 'isource': if (!finite(d.amps)) errors.push(`${d.id}: amps must be finite`); break;
    case 'diode': case 'led':
      if (!d.model || !positive(d.model.is) || !positive(d.model.n) ||
          (d.model.bv !== undefined && !positive(d.model.bv))) {
        errors.push(`${d.id}: diode model needs is > 0, n > 0, optional bv > 0`);
      }
      if (d.kind === 'led' && !positive(d.ratedAmps)) errors.push(`${d.id}: ratedAmps must be > 0`);
      break;
    case 'switch':
      if (typeof d.closed !== 'boolean') errors.push(`${d.id}: closed must be boolean`);
      if (d.ron !== undefined && !positive(d.ron)) errors.push(`${d.id}: ron must be > 0`);
      if (d.roff !== undefined && !positive(d.roff)) errors.push(`${d.id}: roff must be > 0`);
      break;
    case 'npn': case 'pnp':
      if (!d.model || !positive(d.model.is) || !positive(d.model.bf) || !positive(d.model.br)) {
        errors.push(`${d.id}: BJT model needs is, bf, br > 0`);
      }
      break;
    case 'nmos': case 'pmos':
      if (!d.model || !finite(d.model.vto) || !positive(d.model.kp) ||
          !(finite(d.model.lambda) && d.model.lambda >= 0)) {
        errors.push(`${d.id}: MOS model needs finite vto, kp > 0, lambda >= 0`);
      }
      break;
    case 'opamp':
      if (!d.model || !positive(d.model.gain) || !positive(d.model.rOut) ||
          !positive(d.model.rIn) || !(finite(d.model.headroom) && d.model.headroom >= 0)) {
        errors.push(`${d.id}: op-amp model needs gain, rOut, rIn > 0 and headroom >= 0`);
      }
      if (d.vcc === d.vee) errors.push(`${d.id}: supply rails must differ`);
      break;
    case 'regulator':
      if (!d.model || !positive(d.model.vset) || !(finite(d.model.dropout) && d.model.dropout >= 0) ||
          !positive(d.model.rOut) || !(finite(d.model.iq) && d.model.iq >= 0)) {
        errors.push(`${d.id}: regulator model needs vset, rOut > 0 and dropout, iq >= 0`);
      }
      break;
    case 'pad': {
      const p = d.pad;
      if (!p || typeof p.socPad !== 'string' || !/^P[A-Z][0-9]{1,2}$/.test(p.socPad)) {
        errors.push(`${d.id}: pad needs a socPad like PA5`);
        break;
      }
      if (pads.has(p.socPad)) errors.push(`${d.id}: pad ${p.socPad} appears twice`);
      pads.add(p.socPad);
      if (!positive(p.vdd) || !positive(p.rOn) || !positive(p.rPull) ||
          !(finite(p.cIn) && p.cIn >= 0) || !finite(p.vRise) || !finite(p.vFall) ||
          !(p.vFall < p.vRise)) {
        errors.push(`${d.id}: pad electrical parameters invalid`);
      }
      if (!['high', 'low', 'z'].includes(p.drive) || !['up', 'down', 'none'].includes(p.pull)) {
        errors.push(`${d.id}: pad drive/pull invalid`);
      }
      break;
    }
    default: break;
    }
  }
  return errors;
}
