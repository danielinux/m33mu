import { type ChangeEvent, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { BenchCanvas, type Selection, type Tool } from './canvas/BenchCanvas.tsx';
import { Inspector, type McuPadInfo, type Observation } from './Inspector.tsx';
import { DiagnosticsPanel } from './DiagnosticsPanel.tsx';
import { appendDiagnostic, type DiagnosticEntry, type DiagnosticSeverity, type DiagnosticSource } from './diagnostics.ts';
import { ScopeInstrument, type ScopeSink } from './instruments/ScopeInstrument.tsx';
import { appendTerminal, Terminal, type UartInfo } from './instruments/Terminal.tsx';
import { Sniffer } from './instruments/Sniffer.tsx';
import type { BusRecord } from '../sim/buses.ts';
import { CATALOG, formatSi, type PartKind, type Rotation } from './project/catalog.ts';
import { commit, createHistory, redo, replace, undo } from './project/history.ts';
import {
  type CpuName, BOARDS, isCpu, type Project, type ProbeTarget, type ScopeConfig, MAX_FIRMWARE_BYTES, MAX_SCOPES, newScope, ProjectError, emptyProject, parseProject,
  refKey, serializeProject
} from './project/model.ts';
import { type BoardLayout, allTerminals, compileCircuit } from './project/netgraph.ts';
import { WorkerClient, WorkerRequestError, type WorkerEvent } from './worker-client.ts';

type Machine = {
  registers: number[]; xpsr: number; securityState: number; mode: number; sleeping: boolean;
  cpuHz: number; cycleTotal: string; virtualTimeNs: string; stopped?: boolean; paused?: boolean;
};

type RunState = 'idle' | 'running' | 'paused' | 'stopped';

type BenchStats = {
  timeNs: number; simulatedPerWall: number; solverSteps: number; rejectedSteps: number;
  rollbacks: number; sampleOverflow: number;
};

const hex = (v: number) => `0x${(v >>> 0).toString(16).padStart(8, '0')}`;
const PALETTE: PartKind[] = ['breadboard', 'resistor', 'capacitor', 'led', 'diode', 'switch', 'npn', 'nmos', 'opamp', 'regulator', 'supply', 'ground',
  'i2c-eeprom', 'i2c-temp', 'spi-flash', 'usb-uart', 'sniffer'];

async function sha256(bytes: Uint8Array): Promise<string> {
  const digest = await crypto.subtle.digest('SHA-256', bytes as BufferSource);
  return [...new Uint8Array(digest)].map((v) => v.toString(16).padStart(2, '0')).join('');
}

function toBase64(bytes: Uint8Array): string {
  let s = '';
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(s);
}

function download(name: string, data: string | Uint8Array, type: string) {
  const blobData = typeof data === 'string' ? data : data.slice().buffer;
  const url = URL.createObjectURL(new Blob([blobData], { type }));
  const a = document.createElement('a');
  a.href = url;
  a.download = name;
  a.click();
  URL.revokeObjectURL(url);
}

const assetUrl = (path: string) => new URL(path, document.baseURI).href;

export function App() {
  const [history, setHistory] = useState(() => createHistory<Project>(emptyProject()));
  const project = history.present;
  const [layout, setLayout] = useState<BoardLayout | null>(null);
  const [selection, setSelection] = useState<Selection>(null);
  const [tool, setTool] = useState<Tool>({ kind: 'select' });
  const [hoverText, setHoverText] = useState('');
  const [firmware, setFirmware] = useState<{ name: string; hash: string; bytes: Uint8Array } | null>(null);
  const [machine, setMachine] = useState<Machine | null>(null);
  const [runState, setRunState] = useState<RunState>('idle');
  const [benchActive, setBenchActive] = useState(false);
  const [benchStale, setBenchStale] = useState(false);
  const [observation, setObservation] = useState<Observation | null>(null);
  const [stats, setStats] = useState<BenchStats | null>(null);
  const [diagnosticEntries, setDiagnosticEntries] = useState<DiagnosticEntry[]>([]);
  const [mcuPads, setMcuPads] = useState<Record<string, McuPadInfo>>({});
  const [padDuty, setPadDuty] = useState<Record<string, number>>({});
  const [pressed, setPressed] = useState<Set<string>>(new Set());
  const [message, setMessage] = useState('Place parts from the palette, wire them to the highlighted header contacts (GPIO, I²C, SPI, UART, GND, 3V3, 5V), load an ELF and run.');
  const [fault, setFault] = useState('');
  const [realTime, setRealTime] = useState(true);
  const [examples, setExamples] = useState<{ id: string; name: string; project: string; firmware: string | null; description: string }[]>([]);
  const scopeSinks = useRef(new Map<string, ScopeSink>());
  const scopeColumns = useRef(new Map<string, (number | null)[]>());
  const registerScope = useCallback((id: string, sink: ScopeSink | null) => {
    if (sink) scopeSinks.current.set(id, sink); else scopeSinks.current.delete(id);
  }, []);
  const resetScopes = () => { for (const sink of scopeSinks.current.values()) sink.reset(); };
  const client = useRef<WorkerClient | null>(null);

  const cpu = project.simulator.cpu;
  useEffect(() => {
    let live = true;
    setLayout((old) => old?.cpu === cpu ? old : null);
    fetch(assetUrl(`boards/layout/${cpu}.json`))
      .then((r) => { if (!r.ok) throw new Error(`Board layout unavailable (${r.status})`); return r.json(); })
      .then((d: BoardLayout) => { if (live) setLayout(d); })
      .catch((e) => { if (live) setFault(String(e)); });
    return () => { live = false; };
  }, [cpu]);
  useEffect(() => () => client.current?.terminate(), []);
  useEffect(() => {
    fetch(assetUrl('examples/index.json')).then((r) => r.ok ? r.json() : { examples: [] })
      .then((d) => setExamples(d.examples ?? [])).catch(() => setExamples([]));
  }, []);

  const layouts = useMemo(() => new Map(layout ? [[layout.cpu, layout]] : []), [layout]);
  const compiled = useMemo(() => compileCircuit(project, layouts), [project, layouts]);
  const terminals = useMemo(() => allTerminals(project, layouts), [project, layouts]);
  const terminalMap = useMemo(() => new Map(terminals.map((t) => [t.key, t])), [terminals]);
  const errors = compiled.diagnostics.filter((d) => d.severity === 'error');
  const errorKeys = useMemo(() => new Set(errors.flatMap((d) => d.refs ?? [])), [errors]);
  const hasCircuit = compiled.netlist.devices.length > 0;

  function recordDiagnostic(source: DiagnosticSource, severity: DiagnosticSeverity, code: string,
    message: string, timeNs?: number | null) {
    setDiagnosticEntries((entries) => appendDiagnostic(entries, { source, severity, code, message, timeNs }));
  }

  const [uarts, setUarts] = useState<UartInfo[]>([]);
  // Default: the board's debugger virtual COM port (USART3 on the NUCLEO-H563ZI).
  const openUarts = project.instruments.terminals ?? (BOARDS[cpu].vcp ? [BOARDS[cpu].vcp] : []);
  const [uartText, setUartText] = useState<Record<number, string>>({});
  const [busLog, setBusLog] = useState<BusRecord[]>([]);
  const [adapterText, setAdapterText] = useState<Record<string, string>>({});
  const [uartLost, setUartLost] = useState<Set<number>>(new Set());
  const [dockTab, setDockTab] = useState<string>('scope1');
  const [dockOpen, setDockOpen] = useState<boolean>(() => {
    try { return localStorage.getItem('m33mu.dockOpen') !== '0'; } catch { return true; }
  });
  const [crt, setCrt] = useState<boolean>(() => {
    try { return localStorage.getItem('m33mu.crt') !== '0'; } catch { return true; }
  });
  function setCrtMode(on: boolean) {
    setCrt(on);
    try { localStorage.setItem('m33mu.crt', on ? '1' : '0'); } catch { /* per-viewer convenience only */ }
  }
  function showDock(open: boolean) {
    setDockOpen(open);
    try { localStorage.setItem('m33mu.dockOpen', open ? '1' : '0'); } catch { /* per-viewer convenience only */ }
  }
  function toggleUart(label: string) {
    const next = openUarts.includes(label) ? openUarts.filter((l) => l !== label) : [...openUarts, label];
    view({ ...project, instruments: { ...project.instruments, terminals: next } });
    if (next.includes(label)) setDockTab(label);
    else if (dockTab === label) setDockTab(project.instruments.scopes[0]?.id ?? '');
  }
  // Boards without a known VCP: open a terminal for the first UART that talks.
  const autoOpenTerminal = useRef<((label: string) => void) | null>(null);
  autoOpenTerminal.current = (label: string) => {
    if (openUarts.length === 0) toggleUart(label);
  };
  async function sendUart(index: number, text: string) {
    try {
      const r = await worker().request<{ accepted: number; dropped: number }>('uartWrite', { index, text });
      if (r.dropped) setMessage(`UART input queue full: ${r.dropped} bytes dropped`);
    } catch (err) { setFault(err instanceof Error ? err.message : String(err)); }
  }

  const onEvent = useCallback((e: WorkerEvent) => {
    const p = e.payload;
    if (e.event === 'busLog') {
      setBusLog((old) => {
        const next = [...old, ...(p.records as BusRecord[])];
        return next.length > 5000 ? next.slice(next.length - 5000) : next;
      });
      return;
    }
    if (e.event === 'adapter') {
      setAdapterText((old) => ({ ...old, [p.id]: appendTerminal(old[p.id] ?? '', p.bytes as Uint8Array) }));
      return;
    }
    if (e.event === 'uart') {
      const bytes = p.bytes as Uint8Array;
      // UARTs of some targets register when firmware first enables them.
      if (typeof p.label === 'string') {
        setUarts((old) => old.some((u) => u.index === p.index) ? old :
          [...old, { index: p.index, label: p.label, base: p.base ?? 0 }]);
        if (bytes.length) autoOpenTerminal.current?.(p.label);
      }
      setUartText((old) => ({ ...old, [p.index]: appendTerminal(old[p.index] ?? '', bytes) }));
      if (p.overflow) setUartLost((old) => new Set(old).add(p.index));
      return;
    }
    if (e.event === 'state') {
      setMachine(p);
      if (p.stopped) setRunState('stopped');
      else if (p.paused) setRunState('paused');
    } else if (e.event === 'fault') {
      setFault(`${p.code}: ${p.message}`);
      recordDiagnostic('worker', 'error', String(p.code ?? 'FAULT'), String(p.message ?? 'Worker fault'));
      setRunState('paused');
    } else if (e.event === 'gpio') {
      setMessage(`GPIO ${p.socPad} → ${['low', 'high', 'high-Z'][p.level] ?? '?'} at ${p.eventTimeNs} ns`);
    } else if (e.event === 'bench') {
      const width = p.probes.length + 1;
      if (p.samples?.length) {
        for (const [id, cols] of scopeColumns.current) {
          scopeSinks.current.get(id)?.append(p.samples as Float64Array, width, cols);
        }
      }
      if (p.observation) setObservation(p.observation);
      setStats({ timeNs: Number(p.timeNs), simulatedPerWall: p.simulatedPerWall, solverSteps: p.solverSteps,
        rejectedSteps: p.rejectedSteps, rollbacks: p.rollbacks, sampleOverflow: p.sampleOverflow });
      const gpio: Record<string, McuPadInfo> = {};
      for (const ev of p.events as any[]) {
        if (ev.kind === 'gpio') gpio[ev.socPad] = { level: ev.level, mode: ev.mode, pull: ev.pull, timeNs: ev.timeNs };
        if (ev.kind === 'unsupported') recordDiagnostic('capability', 'warning', 'UNSUPPORTED',
          `${ev.socPad}: ${ev.message}`, Number(ev.timeNs));
      }
      if (Object.keys(gpio).length) setMcuPads((old) => ({ ...old, ...gpio }));
      if (p.padDuty) setPadDuty((old) => ({ ...old, ...p.padDuty }));
      if (p.mcuStopped) setRunState('stopped');
      else if (p.reachedTarget && p.timeNs !== '0' && p.requestId) setRunState('paused');
    }
  }, []);

  function worker(): WorkerClient {
    if (!client.current) {
      const c = new WorkerClient(new URL('worker/worker.mjs', document.baseURI));
      c.subscribe(onEvent);
      c.onFailure((m) => { setFault(`Worker failed: ${m}`); recordDiagnostic('worker', 'error', 'WORKER_ERROR', m); });
      client.current = c;
    }
    return client.current;
  }

  const fail = (e: unknown) => {
    const message = e instanceof Error ? e.message : String(e);
    setFault(message);
    recordDiagnostic(e instanceof WorkerRequestError ? 'worker' : 'workbench', 'error',
      e instanceof WorkerRequestError ? e.code : 'ACTION_FAILED', message);
  };

  // ------------------------------------------------------------ editing

  function edit(next: Project, label: string) {
    setHistory((h) => commit(h, next));
    if (benchActive) {
      if (runState === 'running') void worker().request('pause').then(() => setRunState('paused'));
      setBenchStale(true);
      setMessage(`${label}. The bench is paused: apply the change to restart the circuit from its DC operating point at the current time; firmware state is kept.`);
    } else {
      setMessage(label);
    }
  }

  function view(next: Project) {
    setHistory((h) => replace(h, next));
  }

  function deleteSelection() {
    if (!selection) return;
    if (selection.kind === 'part') {
      const id = selection.id;
      edit({ ...project, parts: project.parts.filter((p) => p.id !== id),
        wires: project.wires.filter((w) => !(w.from.kind === 'part' && w.from.part === id) && !(w.to.kind === 'part' && w.to.part === id)),
        instruments: { ...project.instruments, scopes: project.instruments.scopes.map((sc) => ({ ...sc,
          channels: sc.channels.map((c) => (c?.kind === 'current' && c.part === id) ||
            (c?.kind === 'voltage' && c.at.kind === 'part' && c.at.part === id) ? null : c) })) } }, `Deleted ${id}`);
    } else if (selection.kind === 'wire') {
      edit({ ...project, wires: project.wires.filter((w) => w.id !== selection.id) }, 'Deleted wire');
    }
    setSelection(null);
  }

  function rotateSelection() {
    if (selection?.kind !== 'part') return;
    edit({ ...project, parts: project.parts.map((p) => p.id === selection.id ?
      { ...p, rotation: ((p.rotation + 90) % 360) as Rotation } : p) }, `Rotated ${selection.id}`);
  }

  function setParam(partId: string, key: string, value: number | string) {
    edit({ ...project, parts: project.parts.map((p) => p.id === partId ? { ...p, params: { ...p.params, [key]: value } } : p) },
      `Changed ${partId} ${key}`);
  }

  /** Probe into the scope shown in the dock, or the first scope (creating one if needed). */
  function setProbe(channel: 0 | 1, target: ProbeTarget | null) {
    let scopes = project.instruments.scopes;
    let scope = scopes.find((sc) => sc.id === dockTab) ?? scopes[0];
    if (!scope) {
      scope = newScope(project);
      scopes = [scope];
    }
    const channels = [...scope.channels];
    channels[channel] = target;
    const next = { ...scope, channels };
    edit({ ...project, instruments: { ...project.instruments,
      scopes: scopes.map((sc) => sc.id === next.id ? next : sc) } }, `${scope.name} CH${channel + 1} set`);
    setDockTab(next.id);
    showDock(true);
  }

  function updateScope(next: ScopeConfig) {
    view({ ...project, instruments: { ...project.instruments,
      scopes: project.instruments.scopes.map((sc) => sc.id === next.id ? next : sc) } });
  }

  function addScope() {
    if (project.instruments.scopes.length >= MAX_SCOPES) return;
    const scope = newScope(project);
    // An empty scope adds no probes, so the running bench is unaffected.
    setHistory((h) => commit(h, { ...project, instruments: { ...project.instruments,
      scopes: [...project.instruments.scopes, scope] } }));
    setDockTab(scope.id);
  }

  function removeScope(id: string) {
    const scopes = project.instruments.scopes.filter((sc) => sc.id !== id);
    setHistory((h) => commit(h, { ...project, instruments: { ...project.instruments, scopes } }));
    if (dockTab === id) setDockTab(scopes[0]?.id ?? openUarts[0] ?? '');
  }

  useEffect(() => {
    function onKey(e: KeyboardEvent) {
      const target = e.target as HTMLElement;
      if (target.tagName === 'INPUT' || target.tagName === 'SELECT' || target.tagName === 'TEXTAREA') return;
      const mod = e.ctrlKey || e.metaKey;
      if (mod && e.key.toLowerCase() === 'z' && !e.shiftKey) { e.preventDefault(); setHistory(undo); }
      else if (mod && (e.key.toLowerCase() === 'y' || (e.key.toLowerCase() === 'z' && e.shiftKey))) { e.preventDefault(); setHistory(redo); }
      else if (e.key === 'Delete' || e.key === 'Backspace') { e.preventDefault(); deleteSelection(); }
      else if (e.key === 'Escape') { setTool({ kind: 'select' }); setSelection(null); }
      else if (e.key.toLowerCase() === 'r' && !mod) rotateSelection();
      else if (e.key.toLowerCase() === 'w' && !mod) setTool({ kind: 'wire' });
      else if (e.key.toLowerCase() === 'v' && !mod) setTool({ kind: 'select' });
      else if (e.key.startsWith('Arrow') && selection?.kind === 'part') {
        e.preventDefault();
        const d = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] }[e.key] ?? [0, 0];
        edit({ ...project, parts: project.parts.map((p) => p.id === selection.id ?
          { ...p, x: p.x + d[0] * 25.4, y: p.y + d[1] * 25.4 } : p) }, `Moved ${selection.id}`);
      }
    }
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  });

  // ------------------------------------------------------------ firmware

  async function loadElf(bytes: Uint8Array, name: string, hash: string, target: CpuName = cpu) {
    await worker().request('loadFirmware', { bytes, cpu: target });
    setUartText({}); setUartLost(new Set());
    const list = await worker().request<{ uarts: UartInfo[] }>('uartList');
    setUarts(list.uarts);
    setFirmware({ name, hash, bytes });
    setBenchActive(false); setBenchStale(false); setObservation(null); setStats(null);
    resetScopes();
    setDiagnosticEntries([]);
    setRunState('paused');
  }

  async function onElf(e: ChangeEvent<HTMLInputElement>) {
    const file = e.target.files?.[0];
    e.target.value = '';
    if (!file) return;
    setFault('');
    try {
      if (file.size > MAX_FIRMWARE_BYTES) throw new Error('ELF exceeds the 32 MiB limit');
      const bytes = new Uint8Array(await file.arrayBuffer());
      const hash = await sha256(bytes);
      await loadElf(bytes, file.name, hash);
      setHistory((h) => commit(h, { ...h.present, simulator: { ...h.present.simulator, firmwareName: file.name, firmwareSha256: hash } }));
      setMessage(`Loaded ${file.name}.`);
    } catch (err) { fail(err); }
  }

  // ------------------------------------------------------------ bench

  function probesFor(p: Project) {
    const probes: { id: string; kind: 'voltage' | 'current'; net?: string; device?: string }[] = [];
    const columns = new Map<string, (number | null)[]>();
    for (const sc of p.instruments.scopes) {
      const map: (number | null)[] = [null, null];
      sc.channels.forEach((ch, i) => {
        if (!ch) return;
        const id = `${sc.id}.ch${i + 1}`;
        if (ch.kind === 'voltage') {
          const net = compiled.terminalNet.get(refKey(ch.at));
          if (!net) return;
          map[i] = probes.length;
          probes.push({ id, kind: 'voltage', net });
        } else if (compiled.netlist.devices.some((d) => d.id === ch.part)) {
          map[i] = probes.length;
          probes.push({ id, kind: 'current', device: ch.part });
        }
      });
      columns.set(sc.id, map);
    }
    return { probes, columns };
  }

  async function startBench(initial: 'zero' | 'dc' = 'zero') {
    if (errors.length) throw new Error(`Fix the circuit first: ${errors[0].message}`);
    if (compiled.pads.length && !firmware) throw new Error(`The circuit uses ${compiled.pads.join(', ')}: load firmware first`);
    const { probes, columns } = probesFor(project);
    scopeColumns.current = columns;
    setBusLog([]);
    await worker().request('createBench', { netlist: compiled.netlist, probes, logic: compiled.logic,
      options: { lookaheadNs: project.solver.lookaheadNs, sampleNs: project.solver.sampleNs, initial } });
    resetScopes();
    setBenchActive(true);
    setBenchStale(false);
  }

  async function run() {
    setFault('');
    try {
      if (hasCircuit) {
        if (!benchActive || benchStale) await startBench();
        await worker().request('runUntil', realTime ? { maxRatio: 1 } : {});
        setMessage(compiled.pads.length ? `Firmware and circuit running on one virtual clock (${compiled.pads.join(', ')} coupled).`
          : firmware ? 'Firmware and circuit running on one virtual clock (no MCU pad wired).' : 'Analog-only circuit running.');
      } else {
        await worker().request('run');
        setMessage('Firmware running in bounded worker slices (no circuit attached).');
      }
      setRunState('running');
    } catch (err) { fail(err); }
  }

  async function pause() {
    try { await worker().request('pause'); setRunState('paused'); } catch (err) { fail(err); }
  }

  async function step() {
    setFault('');
    try {
      const s = await worker().request('step', { instructionBudget: 1 });
      setMachine(s);
      setRunState(s.stopped ? 'stopped' : 'paused');
    } catch (err) { fail(err); }
  }

  async function reset() {
    setFault('');
    try {
      await worker().request('reset');
      setBenchActive(false); setBenchStale(false); setObservation(null); setStats(null); setMcuPads({}); setPadDuty({});
      resetScopes();
      setRunState('paused');
      setMessage('Firmware reset; the circuit restarts from its DC operating point on the next run.');
    } catch (err) { fail(err); }
  }

  async function applyChanges() {
    setFault('');
    try {
      await startBench('dc');
      await worker().request('runUntil', realTime ? { maxRatio: 1 } : {});
      setRunState('running');
      setMessage('Changes applied; circuit restarted from its operating point at the current virtual time.');
    } catch (err) { fail(err); }
  }

  function press(id: string, down: boolean) {
    setPressed((old) => { const n = new Set(old); if (down) n.add(id); else n.delete(id); return n; });
    if (benchActive && !benchStale) void worker().request('setSwitch', { device: id, closed: down }).catch(fail);
  }

  // ------------------------------------------------------------ project files

  function save() {
    try {
      const p: Project = { ...project, simulator: { ...project.simulator,
        firmwareName: firmware?.name ?? '', firmwareSha256: firmware?.hash ?? null,
        ...(firmware ? { firmwareBase64: toBase64(firmware.bytes) } : {}) } };
      if (!firmware) delete p.simulator.firmwareBase64;
      download(`${p.name.replace(/[^a-zA-Z0-9_-]+/g, '-') || 'bench'}.m33mu.json`, serializeProject(p), 'application/json');
      setMessage(`Project saved (circuit, probes, solver settings, view and embedded ELF).`);
    } catch (err) { fail(err); }
  }

  async function open(e: ChangeEvent<HTMLInputElement>) {
    const file = e.target.files?.[0];
    e.target.value = '';
    if (!file) return;
    setFault('');
    try {
      const p = parseProject(await file.text());
      if (p.simulator.firmwareBase64) {
        const bytes = Uint8Array.from(atob(p.simulator.firmwareBase64), (ch) => ch.charCodeAt(0));
        const hash = await sha256(bytes);
        if (hash !== p.simulator.firmwareSha256) throw new ProjectError('Embedded ELF SHA-256 does not match the project manifest');
        await loadElf(bytes, p.simulator.firmwareName || file.name, hash, p.simulator.cpu);
      } else {
        client.current?.terminate();
        client.current = null;
        setFirmware(null); setMachine(null); setRunState('idle'); setBenchActive(false);
        resetScopes();
      }
      const { firmwareBase64: _drop, ...simulator } = p.simulator;
      void _drop;
      setHistory(createHistory({ ...p, simulator }));
      setDiagnosticEntries([]);
      setSelection(null);
      setDockTab(p.instruments.scopes[0]?.id ?? '');
      setMessage(`Opened ${p.name}${p.simulator.firmwareBase64 ? ' with its verified ELF' : ''}.`);
    } catch (err) { fail(err); }
  }

  async function openExample(id: string) {
    const ex = examples.find((e) => e.id === id);
    if (!ex) return;
    setFault('');
    try {
      const p = parseProject(await (await fetch(assetUrl(`examples/${ex.project}`))).text());
      if (ex.firmware) {
        const bytes = new Uint8Array(await (await fetch(assetUrl(`examples/${ex.firmware}`))).arrayBuffer());
        const hash = await sha256(bytes);
        if (hash !== p.simulator.firmwareSha256) throw new ProjectError(`${ex.firmware} does not match the example's SHA-256`);
        await loadElf(bytes, ex.firmware, hash, p.simulator.cpu);
      } else {
        if (benchActive) await worker().request('destroyBench');
        setBenchActive(false); setBenchStale(false); setObservation(null); setStats(null);
        resetScopes();
        setRunState(firmware ? 'paused' : 'idle');
      }
      setHistory(createHistory(p));
      setDiagnosticEntries([]);
      setSelection(null);
      setDockTab(p.instruments.scopes[0]?.id ?? '');
      setMessage(`${ex.name}: ${ex.description}`);
    } catch (err) { fail(err); }
  }

  /** Switch to a fresh bench on another board; firmware is per CPU. */
  async function chooseBoard(next: CpuName) {
    if (next === cpu) return;
    if ((project.parts.length || project.wires.length) &&
        !window.confirm(`Start a new ${BOARDS[next].name} bench? The current circuit is not carried over.`)) return;
    setFault('');
    if (benchActive) await worker().request('destroyBench').catch(() => undefined);
    setBenchActive(false); setBenchStale(false); setObservation(null); setStats(null);
    setMcuPads({}); setPadDuty({}); setUarts([]); setUartText({}); setUartLost(new Set());
    setFirmware(null); setMachine(null); setRunState('idle');
    resetScopes();
    setHistory(createHistory(emptyProject(next)));
    setSelection(null);
    setMessage(`${BOARDS[next].name}: load an ELF built for ${next}.`);
  }

  function exportDiagnostics() {
    download('m33mu-diagnostics.json', JSON.stringify({ schemaVersion: 1, projectId: project.projectId,
      firmwareSha256: firmware?.hash ?? null, virtualTimeNs: timeNs, circuit: compiled.diagnostics,
      runtime: diagnosticEntries, metrics: stats }, null, 2) + '\n', 'application/json');
  }


  // ------------------------------------------------------------ render

  const brightness: Record<string, number> = {};
  for (const p of project.parts) {
    if (p.kind === 'led' && observation) {
      const i = observation.currents[p.id] ?? 0;
      brightness[p.id] = Math.sqrt(Math.max(0, Math.min(1, i / 0.02)));
    }
  }
  // On-board LEDs: share of time their pad drove the anode high.
  const boardLeds: Record<string, number> = {};
  for (const led of layout?.leds ?? []) {
    const lit = padDuty[led.pad] ?? (mcuPads[led.pad]?.level === 1 ? 1 : 0);
    boardLeds[led.id] = led.activeHigh ? lit : 1 - lit;
  }
  const channelLabel = (ch: ProbeTarget | null): [string, string] => {
    if (!ch) return ['—', 'V'];
    if (ch.kind === 'current') return [`I(${ch.part})`, 'A'];
    const t = terminalMap.get(refKey(ch.at));
    return [`V(${t?.label ?? 'net'})`, 'V'];
  };
  const timeNs = stats?.timeNs ?? (machine ? Number(machine.virtualTimeNs) : 0);
  const canRun = runState !== 'running' && (hasCircuit ? errors.length === 0 : !!firmware && runState !== 'stopped');

  return (
    <main className="app">
      <header className="topbar">
        <div className="brand"><span className="mark">m</span><div><strong>m33mu</strong><small>WORKBENCH</small></div></div>
        <select className="board-name" value={cpu} aria-label="Board" data-testid="board"
          title={layout ? `${layout.boardName}${layout.boardRevision ? ` · ${layout.boardRevision}` : ''}` : 'Loading board…'}
          onChange={(e) => { if (isCpu(e.target.value)) void chooseBoard(e.target.value); }}>
          {(Object.keys(BOARDS) as CpuName[]).map((c) => (
            <option key={c} value={c}>{BOARDS[c].name} · {c}{c === 'stm32h563' ? ` · ${BOARDS[c].layoutRevision}` : ''}</option>
          ))}
        </select>
        <div className="firmware" title={firmware?.hash}>
          <span className={`dot ${runState}`} />{firmware ? firmware.name : 'No firmware'}
        </div>
        <div className="actions">
          <select className="button" value="" onChange={(e) => void openExample(e.target.value)} aria-label="Examples" data-testid="examples">
            <option value="">Examples…</option>
            {examples.map((ex) => <option key={ex.id} value={ex.id}>{ex.name}</option>)}
          </select>
          <label className="button">Open<input type="file" accept=".json,application/json" onChange={open} data-testid="open-input" /></label>
          <button className="button" onClick={save}>Save</button>
          <label className="button primary">Load ELF<input type="file" accept=".elf,application/octet-stream" onChange={onElf} data-testid="elf-input" /></label>
          {runState === 'running'
            ? <button className="button" onClick={pause}>Pause</button>
            : <button className="button go" onClick={run} disabled={!canRun} data-testid="run">Run</button>}
          <label className="toggle" title="Cap the bench at real time; off runs as fast as possible">
            <input type="checkbox" checked={realTime} onChange={(e) => setRealTime(e.target.checked)} data-testid="real-time" /> 1× real time
          </label>
          <button className="button" onClick={step} disabled={!firmware || runState === 'running' || benchActive}>Step</button>
          <button className="button" onClick={reset} disabled={!firmware} title="Reset MCU">Reset</button>
        </div>
      </header>
      <div className="workspace">
        <aside className="palette" aria-label="Parts">
          <h2>Tools</h2>
          <div className="tools">
            <button className={tool.kind === 'select' ? 'active' : ''} onClick={() => setTool({ kind: 'select' })}>Select <kbd>V</kbd></button>
            <button className={tool.kind === 'wire' ? 'active' : ''} onClick={() => setTool({ kind: 'wire' })} data-testid="wire-tool">Wire <kbd>W</kbd></button>
          </div>
          <div className="row">
            <button onClick={() => setHistory(undo)} disabled={!history.past.length}>Undo</button>
            <button onClick={() => setHistory(redo)} disabled={!history.future.length}>Redo</button>
          </div>
          <h2>Parts</h2>
          {PALETTE.map((k) => (
            <button key={k} className={tool.kind === 'place' && tool.part === k ? 'part-button active' : 'part-button'}
              onClick={() => setTool({ kind: 'place', part: k })} data-testid={`place-${k}`}>{CATALOG[k].title}</button>
          ))}
          <h2>Capabilities</h2>
          <ul className="caps">
            <li className="on">Firmware execution ({cpu}, ELF){cpu === 'imxrt700' ? ' — boots from a BIN/FCB image in the CLI only' : ''}</li>
            <li className="on">Analog transient solver (R, C, L, diode, LED, BJT, MOSFET, switch, supplies)</li>
            {(() => {
              const all = layout?.connectors.flatMap((c) => c.contacts) ?? [];
              const wireable = all.filter((c) => c.electricalEnabled).length;
              return wireable > 0
                ? <li className="on">{wireable} of {all.length} header contacts wireable (reviewed pin map)</li>
                : <li>Board contacts: visual only until this board's pin map is reviewed</li>;
            })()}
            {(layout?.leds?.length ?? 0) > 0 && <li className="on">On-board LEDs lit from {layout!.leds!.map((l) => l.pad).join('/')}</li>}
            <li className="on">TIM2–TIM5 output compare and PWM on their AF pins</li>
            <li className="on">UART terminals; logic-level I²C/SPI/UART devices</li>
            <li>Solder-bridge, SWD, clock and unmodelled-net contacts: visual only</li>
            <li>ADC, DAC, TIM1/TIM8 and center-aligned PWM: not modelled</li>
          </ul>
        </aside>
        <section className="bench">
          {benchStale && (
            <div className="banner">Circuit edited while the bench was active.
              <button onClick={applyChanges} disabled={errors.length > 0}>Apply and run</button></div>
          )}
          <BenchCanvas project={project} layout={layout}
            artworkUrl={layout ? assetUrl(`boards/${layout.artwork.replace('../', '')}`) : ''}
            terminals={terminals} selection={selection} tool={tool} running={benchActive && runState === 'running'}
            brightness={brightness} boardLeds={boardLeds} pressed={pressed} errorKeys={errorKeys}
            onSelect={setSelection} onCommit={edit} onView={view} onPress={press} onHover={setHoverText} />
          <div className="canvas-status">
            <span>{hoverText || `${Math.round(project.ui.zoom * 100)}%`}</span>
            <span className="spacer" />
            {errors.length > 0 && <span className="error" data-testid="circuit-error">{errors[0].message}</span>}
            {errors.length === 0 && compiled.diagnostics.length > 0 && <span className="warning">{compiled.diagnostics[0].message}</span>}
          </div>
        </section>
        <aside className="side">
          <Inspector project={project} selection={selection} compiled={compiled} terminals={terminalMap}
            observation={observation} mcuPads={mcuPads} benchActive={benchActive}
            onParam={setParam} onRotate={rotateSelection} onDelete={deleteSelection} onProbe={setProbe} />
          <div className="cpu">
            <h3>CPU {machine && <span className="muted">{formatSi(machine.cpuHz, 'Hz')}</span>}</h3>
            <div className="regs">
              {['r0', 'r1', 'r2', 'r3', 'sp', 'lr', 'pc', 'xpsr'].map((n, i) => (
                <div key={n}><span>{n}</span><b>{machine ? hex(i < 4 ? machine.registers[i] : n === 'sp' ? machine.registers[13]
                  : n === 'lr' ? machine.registers[14] : n === 'pc' ? machine.registers[15] : machine.xpsr) : '—'}</b></div>
              ))}
            </div>
            <p className="muted">{machine ? `${machine.securityState ? 'Non-secure' : 'Secure'} ${machine.mode ? 'thread' : 'handler'}${machine.sleeping ? ', sleeping' : ''}` : 'Load an ELF to start the session.'}</p>
          </div>
          <DiagnosticsPanel circuit={compiled.diagnostics} runtime={diagnosticEntries} metrics={stats}
            onClear={() => setDiagnosticEntries([])} onExport={exportDiagnostics} />
        </aside>
      </div>
      <section className={dockOpen ? 'dock' : 'dock collapsed'}>
        <nav className="dock-tabs" role="tablist" aria-label="Instruments">
          {project.instruments.scopes.map((sc) => (
            <span key={sc.id} className={dockTab === sc.id ? 'tab active' : 'tab'}>
              <button role="tab" aria-selected={dockTab === sc.id} data-testid={`tab-${sc.id}`}
                onClick={() => { setDockTab(sc.id); showDock(true); }}>⏦ {sc.name}</button>
              <button className="tab-close" title={`Remove ${sc.name}`} aria-label={`Remove ${sc.name}`}
                onClick={() => removeScope(sc.id)}>×</button>
            </span>
          ))}
          {uarts.filter((u) => openUarts.includes(u.label)).map((u) => (
            <span key={u.label} className={dockTab === u.label ? 'tab active' : 'tab'}>
              <button role="tab" aria-selected={dockTab === u.label} data-testid={`tab-${u.label}`}
                onClick={() => { setDockTab(u.label); showDock(true); }}>
                ▤ {u.label}{(uartText[u.index] ?? '').length > 0 && dockTab !== u.label ? ' •' : ''}
              </button>
              <button className="tab-close" title={`Close ${u.label}`} aria-label={`Close ${u.label}`}
                onClick={() => toggleUart(u.label)}>×</button>
            </span>
          ))}
          {project.parts.filter((pt) => pt.kind === 'sniffer' || pt.kind === 'usb-uart').map((pt) => (
            <span key={pt.id} className={dockTab === pt.id ? 'tab active' : 'tab'}>
              <button role="tab" aria-selected={dockTab === pt.id} data-testid={`tab-${pt.id}`}
                onClick={() => { setDockTab(pt.id); showDock(true); }}>
                {pt.kind === 'sniffer' ? '⌁' : '⇄'} {pt.id}
              </button>
            </span>
          ))}
          <button className="dock-add" onClick={addScope} disabled={project.instruments.scopes.length >= MAX_SCOPES}
            data-testid="add-scope">+ Scope</button>
          <details className="uart-picker">
            <summary data-testid="uart-picker">+ UART terminal…</summary>
            <div className="uart-menu">{uarts.length === 0 ? <p className="muted">Load firmware to list the UARTs.</p> : uarts.map((u) => (
              <label key={u.label}><input type="checkbox" checked={openUarts.includes(u.label)}
                onChange={() => toggleUart(u.label)} data-testid={`uart-${u.label}`} /> {u.label}
                <span className="muted"> 0x{u.base.toString(16)}</span></label>
            ))}</div>
          </details>
          <span className="spacer" />
          <label className="toggle" title="Green-on-black CRT look for terminals">
            <input type="checkbox" checked={crt} onChange={(e) => setCrtMode(e.target.checked)} data-testid="crt" /> CRT
          </label>
          <button className="dock-toggle" onClick={() => showDock(!dockOpen)} data-testid="dock-toggle"
            aria-expanded={dockOpen}>{dockOpen ? '▾ Hide' : '▴ Show'}</button>
        </nav>
        {project.instruments.scopes.map((sc) => (
          <div className="dock-body" key={sc.id} hidden={!dockOpen || dockTab !== sc.id} data-testid={`scope-${sc.id}`}>
            <div className="instrument scope-case">
              <div className="faceplate">
                <span className="brand-plate">m33·SCOPE</span>
                <span className="model">{sc.name}</span>
                <span className="knob" /><span className="knob small" /><span className="knob small" />
                <span className="power-led" data-on={runState === 'running'} />
              </div>
              <div className="screen">
                <ScopeInstrument config={sc} labels={[channelLabel(sc.channels[0])[0], channelLabel(sc.channels[1])[0]]}
                  units={[channelLabel(sc.channels[0])[1], channelLabel(sc.channels[1])[1]]}
                  running={runState === 'running'} register={registerScope} onConfig={updateScope}
                  onMessage={setMessage} onError={fail} />
              </div>
            </div>
          </div>
        ))}
        {dockOpen && project.parts.filter((pt) => pt.id === dockTab && pt.kind === 'sniffer').map((pt) => (
          <div className="dock-body" key={pt.id}>
            <Sniffer id={pt.id} records={busLog} onClear={() => setBusLog([])}
              clips={compiled.logic.devices.find((d) => d.id === pt.id)?.nets ?? {}} />
          </div>
        ))}
        {dockOpen && project.parts.filter((pt) => pt.id === dockTab && pt.kind === 'usb-uart').map((pt) => (
          <div className="dock-body" key={pt.id}>
            <Terminal uart={{ index: -1, label: pt.id, base: 0 }} text={adapterText[pt.id] ?? ''} lost={false} crt={crt}
              onSend={(text) => void worker().request('adapterWrite', { id: pt.id, text }).catch(fail)}
              onClear={() => setAdapterText((old) => ({ ...old, [pt.id]: '' }))} />
          </div>
        ))}
        {dockOpen && uarts.filter((u) => openUarts.includes(u.label) && dockTab === u.label).map((u) => (
          <div className="dock-body" key={u.label}>
            <Terminal uart={u} text={uartText[u.index] ?? ''} lost={uartLost.has(u.index)} crt={crt}
              onSend={(text) => void sendUart(u.index, text)}
              onClear={() => setUartText((old) => ({ ...old, [u.index]: '' }))} />
          </div>
        ))}
        {dockOpen && project.instruments.scopes.length === 0 && openUarts.length === 0 && (
          <div className="dock-body empty"><p className="muted">Add a scope or a UART terminal.</p></div>
        )}
      </section>
      <footer className="status">
        <span className={fault ? 'fault' : ''} data-testid="status-message">{fault || message}</span>
        <span className="spacer" />
        {stats && <span title="Simulated seconds per wall-clock second">×{stats.simulatedPerWall.toPrecision(3)} real time</span>}
        {stats && <span>{stats.solverSteps} steps · {stats.rejectedSteps} rejected · {stats.rollbacks} rollbacks</span>}
        {stats && stats.sampleOverflow > 0 && <span className="warning">{stats.sampleOverflow} samples dropped</span>}
        <span className="clock" data-testid="virtual-time">{formatSi(timeNs * 1e-9, 's')}</span>
      </footer>
    </main>
  );
}
