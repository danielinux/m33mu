// Part catalog. World units are 0.1 mm (the board artwork is drawn at
// 10 px/mm), so the 2.54 mm breadboard pitch is 25.4 units.

import type { DeviceSpec } from '../../sim/netlist.ts';
import { MODELS } from '../../sim/netlist.ts';

export const PITCH = 25.4;

export type PartKind =
  | 'breadboard' | 'resistor' | 'capacitor' | 'led' | 'diode' | 'switch'
  | 'npn' | 'nmos' | 'supply' | 'ground';

export type ParamSpec = {
  key: string;
  label: string;
  unit: string;
  min: number;
  max: number;
  choices?: { value: string; label: string }[];
};

export type Terminal = { id: string; x: number; y: number; label: string };

export type PartDef = {
  kind: PartKind;
  title: string;
  params: ParamSpec[];
  defaults: Record<string, number | string>;
  /** Terminal offsets for rotation 0. */
  terminals(params: Record<string, number | string>): Terminal[];
  /** Solver devices; `net(terminalId)` gives the net name of a terminal. */
  devices(id: string, params: Record<string, number | string>, net: (t: string) => string): DeviceSpec[];
};

// Breadboard: 30 columns, rows a-e and f-j, plus two rail pairs.
export const BB_COLUMNS = 30;
const BB_ROWS = ['a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j'];

function breadboardTerminals(): Terminal[] {
  const out: Terminal[] = [];
  for (let c = 0; c < BB_COLUMNS; c++) {
    BB_ROWS.forEach((r, i) => {
      const y = (i < 5 ? 3 + i : 5 + i) * PITCH;
      out.push({ id: `${r}${c + 1}`, x: (c + 1) * PITCH, y, label: `${r}${c + 1}` });
    });
    for (const [rail, y] of [['tp', 0], ['tn', 1], ['bp', 16], ['bn', 17]] as const) {
      if (c % 6 === 5) continue; // rail gaps every five holes
      out.push({ id: `${rail}${c + 1}`, x: (c + 1) * PITCH, y: y * PITCH, label: `${rail}${c + 1}` });
    }
  }
  return out;
}

/** Breadboard strip that a hole belongs to: holes in the same strip share a net. */
export function breadboardStrip(hole: string): string {
  const m = /^([a-j]|tp|tn|bp|bn)(\d+)$/.exec(hole);
  if (!m) return hole;
  const [, row, col] = m;
  if (row.length === 2) return `rail-${row}`;
  return `${'abcde'.includes(row) ? 'top' : 'bottom'}-${col}`;
}

const BB_TERMINALS = breadboardTerminals();

const twoLead = (span: number): Terminal[] => [
  { id: 'a', x: 0, y: 0, label: 'A' },
  { id: 'b', x: span * PITCH, y: 0, label: 'B' }
];

export const CATALOG: Record<PartKind, PartDef> = {
  breadboard: {
    kind: 'breadboard', title: 'Breadboard (30 columns)', params: [], defaults: {},
    terminals: () => BB_TERMINALS,
    devices: () => []
  },
  resistor: {
    kind: 'resistor', title: 'Resistor',
    params: [{ key: 'ohms', label: 'Resistance', unit: 'Ω', min: 0.001, max: 1e9 }],
    defaults: { ohms: 330 },
    terminals: () => twoLead(4),
    devices: (id, p, net) => [{ kind: 'resistor', id, a: net('a'), b: net('b'), ohms: Number(p.ohms) }]
  },
  capacitor: {
    kind: 'capacitor', title: 'Capacitor',
    params: [{ key: 'farads', label: 'Capacitance', unit: 'F', min: 1e-15, max: 1 }],
    defaults: { farads: 10e-9 },
    terminals: () => twoLead(2),
    devices: (id, p, net) => [{ kind: 'capacitor', id, a: net('a'), b: net('b'), farads: Number(p.farads) }]
  },
  led: {
    kind: 'led', title: 'LED',
    params: [{ key: 'color', label: 'Colour', unit: '', min: 0, max: 0,
      choices: [{ value: 'red', label: 'Red' }, { value: 'green', label: 'Green' }] }],
    defaults: { color: 'red' },
    terminals: () => [{ id: 'a', x: 0, y: 0, label: 'Anode' }, { id: 'k', x: PITCH, y: 0, label: 'Cathode' }],
    devices: (id, p, net) => [{ kind: 'led', id, a: net('a'), k: net('k'),
      model: p.color === 'green' ? MODELS.ledGreen : MODELS.ledRed, color: String(p.color), ratedAmps: 0.02 }]
  },
  diode: {
    kind: 'diode', title: 'Diode (1N4148-like)', params: [], defaults: {},
    terminals: () => [{ id: 'a', x: 0, y: 0, label: 'Anode' }, { id: 'k', x: 3 * PITCH, y: 0, label: 'Cathode' }],
    devices: (id, _p, net) => [{ kind: 'diode', id, a: net('a'), k: net('k'), model: MODELS.diode1n4148 }]
  },
  switch: {
    kind: 'switch', title: 'Push button (normally open)', params: [], defaults: {},
    terminals: () => twoLead(2),
    devices: (id, _p, net) => [{ kind: 'switch', id, a: net('a'), b: net('b'), closed: false, ron: 0.05, roff: 1e10 }]
  },
  npn: {
    kind: 'npn', title: 'NPN transistor (2N3904-like)', params: [], defaults: {},
    terminals: () => [{ id: 'e', x: 0, y: 0, label: 'E' }, { id: 'b', x: PITCH, y: 0, label: 'B' },
      { id: 'c', x: 2 * PITCH, y: 0, label: 'C' }],
    devices: (id, _p, net) => [{ kind: 'npn', id, c: net('c'), b: net('b'), e: net('e'), model: MODELS.npn2n3904 }]
  },
  nmos: {
    kind: 'nmos', title: 'N-MOSFET (generic, Vth 1.5 V)', params: [], defaults: {},
    terminals: () => [{ id: 'g', x: 0, y: 0, label: 'G' }, { id: 'd', x: PITCH, y: 0, label: 'D' },
      { id: 's', x: 2 * PITCH, y: 0, label: 'S' }],
    devices: (id, _p, net) => [{ kind: 'nmos', id, d: net('d'), g: net('g'), s: net('s'), model: MODELS.nmosGeneric }]
  },
  supply: {
    kind: 'supply', title: 'DC supply',
    params: [{ key: 'volts', label: 'Voltage', unit: 'V', min: -50, max: 50,
      choices: [{ value: '3.3', label: '3.3 V' }, { value: '5', label: '5 V' }] }],
    defaults: { volts: '3.3' },
    terminals: () => [{ id: 'p', x: 0, y: 0, label: '+' }, { id: 'n', x: PITCH, y: 0, label: '−' }],
    devices: (id, p, net) => [{ kind: 'vsource', id, p: net('p'), n: net('n'), volts: Number(p.volts) }]
  },
  ground: {
    kind: 'ground', title: 'Ground reference', params: [], defaults: {},
    terminals: () => [{ id: 'g', x: 0, y: 0, label: 'GND' }],
    devices: (id, _p, net) => [{ kind: 'ground', id, node: net('g') }]
  }
};

export type Rotation = 0 | 90 | 180 | 270;

export function rotate(x: number, y: number, r: Rotation): [number, number] {
  switch (r) {
  case 0: return [x, y];
  case 90: return [-y, x];
  case 180: return [-x, -y];
  case 270: return [y, -x];
  }
}

export function formatSi(value: number, unit: string): string {
  if (!Number.isFinite(value)) return `— ${unit}`;
  const abs = Math.abs(value);
  const prefixes: [number, string][] = [[1e9, 'G'], [1e6, 'M'], [1e3, 'k'], [1, ''], [1e-3, 'm'],
    [1e-6, 'µ'], [1e-9, 'n'], [1e-12, 'p'], [1e-15, 'f']];
  if (abs === 0) return `0 ${unit}`;
  for (const [scale, p] of prefixes) {
    if (abs >= scale * 0.9995) return `${(value / scale).toPrecision(4).replace(/\.?0+$/, '')} ${p}${unit}`;
  }
  return `${value.toExponential(3)} ${unit}`;
}

/** Parse "4.7k", "10n", "330", "1e-6" into a number. */
export function parseSi(text: string): number | null {
  const m = /^\s*([-+]?\d*\.?\d+(?:e[-+]?\d+)?)\s*([GMkmuµnpf]?)\s*[A-Za-zΩ]*\s*$/.exec(text);
  if (!m) return null;
  const scale: Record<string, number> = { G: 1e9, M: 1e6, k: 1e3, '': 1, m: 1e-3, u: 1e-6, 'µ': 1e-6,
    n: 1e-9, p: 1e-12, f: 1e-15 };
  const v = Number(m[1]) * scale[m[2]];
  return Number.isFinite(v) ? v : null;
}
