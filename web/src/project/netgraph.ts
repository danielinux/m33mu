// Semantic net graph: union of wires, breadboard strips and coincident leads,
// compiled to a solver netlist.

import type { DeviceSpec, Netlist } from '../../sim/netlist.ts';
import type { LogicNetlist } from '../../sim/buses.ts';
import { defaultPad } from '../../sim/netlist.ts';
import { breadboardStrip, CATALOG, rotate, type Terminal } from './catalog.ts';
import { type Project, type TerminalRef, refKey } from './model.ts';

export type LayoutContact = {
  number: number;
  contactId?: string;
  x: number;
  y: number;
  signal: string | null;
  mapping_verified: boolean;
  electricalEnabled?: boolean;
};

export type BoardLayout = {
  cpu: string;
  boardName: string;
  boardRevision?: string;
  artwork: string;
  pixelViewbox: [number, number, number, number];
  connectors: { id: string; kind: string; rows: number; columns: number; contacts: LayoutContact[] }[];
  warning?: string;
};

export type PlacedTerminal = {
  key: string;
  ref: TerminalRef;
  x: number;
  y: number;
  label: string;
  /** Board contacts only. */
  socPad?: string | null;
  enabled: boolean;
};

export type Diagnostic = { severity: 'error' | 'warning'; message: string; refs?: string[] };

export type CompiledCircuit = {
  netlist: Netlist;
  /** terminal key -> net name */
  terminalNet: Map<string, string>;
  /** net name -> terminal keys */
  nets: Map<string, string[]>;
  /** SoC pads that are wired into the circuit. */
  pads: string[];
  /** Logic-level bus devices and the wired MCU pads, for sim/buses.ts. */
  logic: LogicNetlist;
  diagnostics: Diagnostic[];
};

export function contactId(connector: string, c: LayoutContact): string {
  return c.contactId ?? `${connector}-${c.number}`;
}

export function partTerminals(project: Project, partId: string): PlacedTerminal[] {
  const part = project.parts.find((p) => p.id === partId);
  if (!part) return [];
  const def = CATALOG[part.kind];
  return def.terminals(part.params).map((t: Terminal) => {
    const [dx, dy] = rotate(t.x, t.y, part.rotation);
    const ref: TerminalRef = { kind: 'part', part: part.id, terminal: t.id };
    return { key: refKey(ref), ref, x: part.x + dx, y: part.y + dy, label: `${part.id}.${t.label}`, enabled: true };
  });
}

export function boardTerminals(project: Project, layouts: Map<string, BoardLayout>): PlacedTerminal[] {
  const out: PlacedTerminal[] = [];
  for (const b of project.boards) {
    const layout = layouts.get(b.cpu);
    if (!layout) continue;
    for (const conn of layout.connectors) {
      for (const c of conn.contacts) {
        const id = contactId(conn.id, c);
        const ref: TerminalRef = { kind: 'board', board: b.id, contact: id };
        out.push({ key: refKey(ref), ref, x: b.x + c.x, y: b.y + c.y, label: c.signal ? `${id} (${c.signal})` : id,
          socPad: c.electricalEnabled ? c.signal : null, enabled: c.electricalEnabled === true });
      }
    }
  }
  return out;
}

export function allTerminals(project: Project, layouts: Map<string, BoardLayout>): PlacedTerminal[] {
  return [...boardTerminals(project, layouts), ...project.parts.flatMap((p) => partTerminals(project, p.id))];
}

/** Nearest terminal within radius (world units), for hit testing. */
export function hitTerminal(terminals: PlacedTerminal[], x: number, y: number, radius: number): PlacedTerminal | null {
  let best: PlacedTerminal | null = null;
  let bestD = radius * radius;
  for (const t of terminals) {
    const d = (t.x - x) ** 2 + (t.y - y) ** 2;
    if (d <= bestD) { best = t; bestD = d; }
  }
  return best;
}

class UnionFind {
  parent = new Map<string, string>();
  find(a: string): string {
    let p = this.parent.get(a) ?? a;
    if (p !== a) { p = this.find(p); this.parent.set(a, p); }
    return p;
  }
  union(a: string, b: string): void {
    const ra = this.find(a);
    const rb = this.find(b);
    if (ra !== rb) {
      // Keep the lexicographically smaller root for stable naming.
      if (ra < rb) this.parent.set(rb, ra); else this.parent.set(ra, rb);
    }
  }
}

const COINCIDENT = 2; // world units (0.2 mm)

export function compileCircuit(project: Project, layouts: Map<string, BoardLayout>): CompiledCircuit {
  const diagnostics: Diagnostic[] = [];
  const terms = allTerminals(project, layouts);
  const byKey = new Map(terms.map((t) => [t.key, t]));
  const uf = new UnionFind();
  for (const t of terms) uf.find(t.key);
  const touched = new Set<string>();

  const usable = (t: PlacedTerminal | undefined, why: string): t is PlacedTerminal => {
    if (!t) {
      diagnostics.push({ severity: 'error', message: `${why}: terminal no longer exists` });
      return false;
    }
    if (t.ref.kind === 'board' && !t.enabled) {
      diagnostics.push({ severity: 'error', refs: [t.key],
        message: `${why}: board contact ${t.ref.contact} is visual-only (no verified electrical mapping)` });
      return false;
    }
    return true;
  };

  for (const w of project.wires) {
    const a = byKey.get(refKey(w.from));
    const b = byKey.get(refKey(w.to));
    if (usable(a, `Wire ${w.id}`) && usable(b, `Wire ${w.id}`)) {
      uf.union(a.key, b.key);
      touched.add(a.key); touched.add(b.key);
    }
  }
  // Breadboard strips.
  for (const p of project.parts) {
    if (p.kind !== 'breadboard') continue;
    const strips = new Map<string, string>();
    for (const t of partTerminals(project, p.id)) {
      if (t.ref.kind !== 'part') continue;
      const strip = breadboardStrip(t.ref.terminal);
      const first = strips.get(strip);
      if (first) uf.union(first, t.key); else strips.set(strip, t.key);
    }
  }
  // Coincident leads: a component lead in a breadboard hole or header socket.
  const sorted = [...terms].sort((a, b) => a.x - b.x || a.y - b.y);
  for (let i = 0; i < sorted.length; i++) {
    for (let j = i + 1; j < sorted.length && sorted[j].x - sorted[i].x <= COINCIDENT; j++) {
      const a = sorted[i];
      const b = sorted[j];
      if (Math.abs(a.y - b.y) > COINCIDENT) continue;
      if (a.ref.kind === 'part' && b.ref.kind === 'part' && a.ref.part === b.ref.part) continue;
      if (a.ref.kind === 'board' && b.ref.kind === 'board') continue;
      const board = a.ref.kind === 'board' ? a : b.ref.kind === 'board' ? b : null;
      if (board && !board.enabled) {
        diagnostics.push({ severity: 'error', refs: [board.key],
          message: `A lead sits on visual-only board contact ${board.ref.kind === 'board' ? board.ref.contact : ''}` });
        continue;
      }
      uf.union(a.key, b.key);
      touched.add(a.key); touched.add(b.key);
    }
  }

  // Net naming: ground nets are "gnd"; pad nets carry the pad name.
  const groups = new Map<string, string[]>();
  for (const t of terms) {
    const r = uf.find(t.key);
    const list = groups.get(r);
    if (list) list.push(t.key); else groups.set(r, [t.key]);
  }
  const terminalNet = new Map<string, string>();
  const nets = new Map<string, string[]>();
  let n = 0;
  const roots = [...groups.keys()].sort();
  for (const r of roots) {
    const keys = groups.get(r)!;
    if (keys.length === 1 && !touched.has(keys[0])) {
      // Lone terminal: still give it a private net so devices compile.
      const name = `n${++n}`;
      terminalNet.set(keys[0], name);
      nets.set(name, keys);
      continue;
    }
    const pad = keys.map((k) => byKey.get(k)!).find((t) => t.ref.kind === 'board' && t.socPad);
    const name = pad?.socPad ?? `n${++n}`;
    for (const k of keys) terminalNet.set(k, name);
    nets.set(name, keys);
  }

  const devices: DeviceSpec[] = [];
  let hasGround = false;
  let hasSource = false;
  for (const part of project.parts) {
    const def = CATALOG[part.kind];
    if (part.kind === 'breadboard') continue;
    const net = (tid: string) => terminalNet.get(refKey({ kind: 'part', part: part.id, terminal: tid })) ?? `${part.id}.${tid}`;
    devices.push(...def.devices(part.id, part.params, net));
    if (part.kind === 'ground') hasGround = true;
    if (part.kind === 'supply') hasSource = true;
    for (const t of partTerminals(project, part.id)) {
      // Spare sniffer clips are normal.
      if (!touched.has(t.key) && part.kind !== 'sniffer') {
        diagnostics.push({ severity: 'warning', refs: [t.key], message: `${t.label} is not connected` });
      }
    }
  }
  const pads: string[] = [];
  const RAILS: Record<string, number> = { '3V3': 3.3, '5V': 5 };
  // Every contact of one rail is the same board net; stamp each rail once.
  const railsDone = new Set<string>();
  for (const [name, keys] of nets) {
    const signals = [...new Set(keys.map((k) => byKey.get(k)!)
      .filter((t) => t.ref.kind === 'board' && t.socPad).map((t) => t.socPad!))];
    if (signals.length === 0 || keys.length < 2) continue;
    if (signals.length > 1) {
      diagnostics.push({ severity: 'error', refs: keys.filter((k) => byKey.get(k)!.ref.kind === 'board'),
        message: `Short circuit: board signals ${signals.join(', ')} are wired together` });
      continue;
    }
    const signal = signals[0];
    if (signal === 'GND' || signal in RAILS) {
      hasGround = true;
      if (!railsDone.has('GND')) {
        // The board ground is the MCU's reference: node 0.
        devices.push({ kind: 'ground', id: 'board-GND', node: 'GND' });
        railsDone.add('GND');
      }
      if (signal in RAILS && !railsDone.has(signal)) {
        // NUCLEO-H563ZI regulator/USB rail, approximated as an ideal source.
        devices.push({ kind: 'vsource', id: `board-${signal}`, p: name, n: 'GND', volts: RAILS[signal] });
        railsDone.add(signal);
        hasSource = true;
      }
    } else {
      pads.push(signal);
      devices.push({ kind: 'pad', id: `pad-${signal}`, node: name, pad: defaultPad(signal) });
      hasSource = true;
    }
  }
  if (hasSource && !hasGround) {
    diagnostics.push({ severity: 'error', message: 'Add a ground reference: supplies and MCU pads are referenced to ground' });
  }
  // Ground symbols are nets named after their terminal; unify the name.
  const logic: LogicNetlist = { devices: [], pins: {} };
  for (const pad of pads) logic.pins[pad] = pad;
  for (const part of project.parts) {
    const def = CATALOG[part.kind];
    if (!def.logic) continue;
    const nets: Record<string, string> = {};
    for (const t of def.terminals(part.params)) {
      const key = refKey({ kind: 'part', part: part.id, terminal: t.id });
      if (touched.has(key)) nets[t.id] = terminalNet.get(key)!;
    }
    logic.devices.push({ id: part.id, kind: def.logic, nets, params: part.params });
  }
  return { netlist: { schemaVersion: 1, devices }, terminalNet, nets, pads: pads.sort(), logic, diagnostics };
}
