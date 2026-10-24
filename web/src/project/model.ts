// Project model, versioned serialization and migrations.

import { CATALOG, type PartKind, type Rotation } from './catalog.ts';

export type TerminalRef =
  | { kind: 'part'; part: string; terminal: string }
  | { kind: 'board'; board: string; contact: string };

export type Part = {
  id: string;
  kind: PartKind;
  x: number;
  y: number;
  rotation: Rotation;
  params: Record<string, number | string>;
};

export type Wire = { id: string; from: TerminalRef; to: TerminalRef; color: string };

export type BoardInstance = {
  id: string;
  cpu: 'stm32h563';
  layoutRevision: string;
  x: number;
  y: number;
};

export type ProbeTarget =
  | { kind: 'voltage'; at: TerminalRef }
  | { kind: 'current'; part: string };

export type ScopeTrigger = { channel: 0 | 1; edge: 'rising' | 'falling'; level: number };
export type ScopeAcquisition = 'auto' | 'normal' | 'single';

export type Project = {
  schemaVersion: 2;
  projectId: string;
  name: string;
  simulator: {
    cpu: 'stm32h563';
    firmwareFormat: 'elf';
    firmwareSha256: string | null;
    firmwareName: string;
    firmwareBase64?: string;
  };
  boards: BoardInstance[];
  parts: Part[];
  wires: Wire[];
  instruments: {
    scopes: ScopeConfig[];
    /** UART labels that have a terminal tab, e.g. "USART3". */
    terminals?: string[];
  };
  solver: { lookaheadNs: number; sampleNs: number };
  ui: { zoom: number; panX: number; panY: number };
};

export type ScopeConfig = {
  id: string;
  name: string;
  channels: (ProbeTarget | null)[];
  windowNs: number;
  trigger?: ScopeTrigger | null;
  acquisition?: ScopeAcquisition;
};

export const MAX_SCOPES = 8;

export function newScope(project: Project | null): ScopeConfig {
  const used = new Set(project?.instruments.scopes.map((s) => s.id) ?? []);
  let n = 1;
  while (used.has(`scope${n}`)) n++;
  return { id: `scope${n}`, name: `Scope ${n}`, channels: [null, null], windowNs: 200_000,
    trigger: null, acquisition: 'auto' };
}

export const H563_LAYOUT_REVISION = 'MB1404 H563ZI-C01';
export const MAX_PROJECT_BYTES = 64 * 1024 * 1024;
export const MAX_FIRMWARE_BYTES = 32 * 1024 * 1024;
const MAX_ITEMS = 2000;

export function emptyProject(): Project {
  return {
    schemaVersion: 2,
    projectId: newId('project'),
    name: 'Untitled bench',
    simulator: { cpu: 'stm32h563', firmwareFormat: 'elf', firmwareSha256: null, firmwareName: '' },
    boards: [{ id: 'board1', cpu: 'stm32h563', layoutRevision: H563_LAYOUT_REVISION, x: 0, y: 0 }],
    parts: [],
    wires: [],
    instruments: { scopes: [newScope(null)], terminals: ['USART3'] },
    solver: { lookaheadNs: 50_000, sampleNs: 1000 },
    ui: { zoom: 0.5, panX: 40, panY: 20 }
  };
}

let counter = 0;
export function newId(prefix: string): string {
  counter = (counter + 1) % 1e6;
  return `${prefix}-${Date.now().toString(36)}-${counter.toString(36)}`;
}

/** Next free id like r1, r2 for a part kind. */
export function nextPartId(project: Project, kind: PartKind): string {
  const prefix = { breadboard: 'bb', resistor: 'r', capacitor: 'c', led: 'led', diode: 'd', switch: 'sw',
    npn: 'q', nmos: 'm', opamp: 'u', regulator: 'ldo', 'i2c-eeprom': 'eeprom', 'i2c-temp': 'temp', 'spi-flash': 'flash',
    'usb-uart': 'uart', sniffer: 'sniff', supply: 'v', ground: 'gnd' }[kind];
  const used = new Set(project.parts.map((p) => p.id));
  for (let i = 1; ; i++) if (!used.has(`${prefix}${i}`)) return `${prefix}${i}`;
}

export class ProjectError extends Error {}

function fail(message: string): never {
  throw new ProjectError(message);
}

function isObject(v: unknown): v is Record<string, unknown> {
  return !!v && typeof v === 'object' && !Array.isArray(v);
}

function finite(v: unknown): v is number {
  return typeof v === 'number' && Number.isFinite(v);
}

function checkRef(ref: unknown, where: string): TerminalRef {
  if (!isObject(ref)) fail(`${where}: terminal reference missing`);
  if (ref.kind === 'part' && typeof ref.part === 'string' && typeof ref.terminal === 'string') {
    return { kind: 'part', part: ref.part, terminal: ref.terminal };
  }
  if (ref.kind === 'board' && typeof ref.board === 'string' && typeof ref.contact === 'string') {
    return { kind: 'board', board: ref.board, contact: ref.contact };
  }
  return fail(`${where}: terminal reference must be a part terminal or board contact`);
}

/** Validate a parsed v2 project; returns it normalized or throws ProjectError. */
export function validateProject(value: unknown): Project {
  if (!isObject(value)) fail('Project file is not a JSON object');
  if (value.schemaVersion !== 2) fail(`Unsupported project schemaVersion ${String(value.schemaVersion)}`);
  const p = value as unknown as Project;
  if (typeof p.projectId !== 'string' || typeof p.name !== 'string') fail('Project needs projectId and name');
  const sim = p.simulator;
  if (!isObject(sim) || sim.cpu !== 'stm32h563' || sim.firmwareFormat !== 'elf') {
    fail('Only stm32h563 ELF projects are supported');
  }
  if (sim.firmwareSha256 !== null && !/^[0-9a-f]{64}$/.test(String(sim.firmwareSha256))) {
    fail('firmwareSha256 must be null or 64 lowercase hex digits');
  }
  if (sim.firmwareBase64 !== undefined &&
      (typeof sim.firmwareBase64 !== 'string' || sim.firmwareBase64.length > MAX_FIRMWARE_BYTES * 4 / 3 + 4)) {
    fail('Embedded firmware is not base64 or exceeds 32 MiB');
  }
  for (const key of ['boards', 'parts', 'wires'] as const) {
    if (!Array.isArray(p[key]) || p[key].length > MAX_ITEMS) fail(`${key} must be an array of at most ${MAX_ITEMS}`);
  }
  if (p.boards.length !== 1) fail('Exactly one board is supported in this version');
  const ids = new Set<string>();
  const unique = (id: unknown, what: string) => {
    if (typeof id !== 'string' || !id) fail(`${what} needs a string id`);
    if (ids.has(id)) fail(`Duplicate id ${id}`);
    ids.add(id);
  };
  for (const b of p.boards) {
    unique(b.id, 'Board');
    if (b.cpu !== 'stm32h563') fail(`Board ${b.id}: unsupported CPU ${String(b.cpu)}`);
    if (b.layoutRevision !== H563_LAYOUT_REVISION) {
      fail(`Board ${b.id}: layout revision "${String(b.layoutRevision)}" is not ${H563_LAYOUT_REVISION}`);
    }
    if (!finite(b.x) || !finite(b.y)) fail(`Board ${b.id}: invalid position`);
  }
  for (const part of p.parts) {
    unique(part.id, 'Part');
    const def = CATALOG[part.kind as PartKind];
    if (!def) fail(`Part ${part.id}: unsupported component "${String(part.kind)}"`);
    if (!finite(part.x) || !finite(part.y) || ![0, 90, 180, 270].includes(part.rotation)) {
      fail(`Part ${part.id}: invalid position or rotation`);
    }
    if (!isObject(part.params)) fail(`Part ${part.id}: params must be an object`);
    for (const spec of def.params) {
      const v = part.params[spec.key];
      if (spec.choices) {
        if (!spec.choices.some((c) => c.value === String(v))) fail(`Part ${part.id}: invalid ${spec.label}`);
      } else if (!finite(v) || v < spec.min || v > spec.max) {
        fail(`Part ${part.id}: ${spec.label} must be between ${spec.min} and ${spec.max} ${spec.unit}`);
      }
    }
    for (const key of Object.keys(part.params)) {
      if (!def.params.some((s) => s.key === key)) fail(`Part ${part.id}: unknown parameter ${key}`);
    }
  }
  const partIds = new Set(p.parts.map((x) => x.id));
  const boardIds = new Set(p.boards.map((x) => x.id));
  for (const w of p.wires) {
    unique(w.id, 'Wire');
    for (const end of [checkRef(w.from, `Wire ${w.id}`), checkRef(w.to, `Wire ${w.id}`)]) {
      if (end.kind === 'part' && !partIds.has(end.part)) fail(`Wire ${w.id}: unknown part ${end.part}`);
      if (end.kind === 'board' && !boardIds.has(end.board)) fail(`Wire ${w.id}: unknown board ${end.board}`);
    }
    if (typeof w.color !== 'string') fail(`Wire ${w.id}: color must be a string`);
  }
  const inst = p.instruments as unknown as Record<string, unknown>;
  if (!isObject(inst)) fail('instruments must be an object');
  // Projects saved before multi-scope support had a single `scope`.
  if (inst.scopes === undefined && isObject(inst.scope)) {
    inst.scopes = [{ id: 'scope1', name: 'Scope 1', ...(inst.scope as object) }];
  }
  delete inst.scope;
  if (!Array.isArray(inst.scopes) || inst.scopes.length > MAX_SCOPES) {
    fail(`instruments.scopes must be an array of at most ${MAX_SCOPES}`);
  }
  for (const scope of p.instruments.scopes) {
    unique(scope?.id, 'Scope');
    if (!isObject(scope) || typeof scope.name !== 'string' || !Array.isArray(scope.channels) ||
        scope.channels.length !== 2 || !finite(scope.windowNs) || scope.windowNs <= 0) {
      fail('Each scope needs a name, two channels and a positive windowNs');
    }
    for (const ch of scope.channels) {
      if (ch === null) continue;
      if (ch.kind === 'voltage') checkRef(ch.at, 'Scope channel');
      else if (ch.kind !== 'current' || typeof ch.part !== 'string') fail('Scope channel target invalid');
    }
    if (scope.trigger !== undefined && scope.trigger !== null &&
        (!isObject(scope.trigger) || (scope.trigger.channel !== 0 && scope.trigger.channel !== 1) ||
         !['rising', 'falling'].includes(String(scope.trigger.edge)) || !finite(scope.trigger.level))) {
      fail('Scope trigger needs CH1 or CH2, a rising/falling edge and a finite level');
    }
    if (scope.acquisition !== undefined && !['auto', 'normal', 'single'].includes(String(scope.acquisition))) {
      fail('Scope acquisition must be auto, normal or single');
    }
    if (scope.acquisition !== undefined && scope.acquisition !== 'auto' && !scope.trigger) {
      fail('Normal and single scope acquisition need a trigger');
    }
  }
  if (inst.terminals !== undefined && (!Array.isArray(inst.terminals) || inst.terminals.length > 32 ||
      !inst.terminals.every((t) => typeof t === 'string' && /^[A-Z0-9_]{1,15}$/.test(t)))) {
    fail('instruments.terminals must list UART labels');
  }
  if (!isObject(p.solver) || !finite(p.solver.lookaheadNs) || p.solver.lookaheadNs <= 0 ||
      !finite(p.solver.sampleNs) || p.solver.sampleNs <= 0) {
    fail('solver needs positive lookaheadNs and sampleNs');
  }
  if (!isObject(p.ui) || !finite(p.ui.zoom) || p.ui.zoom <= 0 || !finite(p.ui.panX) || !finite(p.ui.panY)) {
    fail('ui needs zoom, panX and panY');
  }
  return p;
}

/** v1 projects (first web shell): one visual H563 board and an empty circuit. */
function migrateV1(v: Record<string, unknown>): Project {
  for (const key of ['contacts', 'nets', 'components', 'instruments']) {
    const list = v[key];
    if (list !== undefined && (!Array.isArray(list) || list.length > 0)) {
      fail(`Version 1 project has ${key} content that cannot be migrated; this version only migrated empty circuits`);
    }
  }
  const sim = isObject(v.simulator) ? v.simulator : {};
  const out = emptyProject();
  out.projectId = typeof v.projectId === 'string' ? v.projectId : out.projectId;
  out.name = typeof v.name === 'string' ? v.name : out.name;
  out.simulator = {
    cpu: 'stm32h563', firmwareFormat: 'elf',
    firmwareSha256: typeof sim.firmwareSha256 === 'string' ? sim.firmwareSha256 : null,
    firmwareName: typeof sim.firmwareName === 'string' ? sim.firmwareName : '',
    ...(typeof sim.firmwareBase64 === 'string' ? { firmwareBase64: sim.firmwareBase64 } : {})
  };
  const ui = isObject(v.ui) ? v.ui : {};
  if (finite(ui.zoom) && ui.zoom > 0) out.ui.zoom = ui.zoom;
  return out;
}

export function parseProject(text: string): Project {
  if (text.length > MAX_PROJECT_BYTES) fail('Project file exceeds 64 MiB');
  let raw: unknown;
  try {
    raw = JSON.parse(text);
  } catch (e) {
    fail(`Project file is not valid JSON (${(e as Error).message})`);
  }
  if (!isObject(raw)) fail('Project file is not a JSON object');
  if (raw.schemaVersion === 1) return validateProject(migrateV1(raw));
  return validateProject(raw);
}

/** Stable serialization: keys in a fixed order, two-space indent. */
export function serializeProject(project: Project): string {
  return JSON.stringify(validateProject(project), null, 2) + '\n';
}

export function refKey(ref: TerminalRef): string {
  return ref.kind === 'part' ? `part:${ref.part}:${ref.terminal}` : `board:${ref.board}:${ref.contact}`;
}

export function sameRef(a: TerminalRef, b: TerminalRef): boolean {
  return refKey(a) === refKey(b);
}
