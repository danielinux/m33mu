import { type ChangeEvent, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { BenchCanvas, type Selection, type Tool } from './canvas/BenchCanvas.tsx';
import { Inspector, type McuPadInfo, type Observation } from './Inspector.tsx';
import { Scope, ScopeBuffer } from './instruments/Scope.tsx';
import { decodeSigc, SIGC_MAX_BYTES, SigcError } from './instruments/sigc.ts';
import { CATALOG, formatSi, type PartKind, type Rotation } from './project/catalog.ts';
import { commit, createHistory, redo, replace, undo } from './project/history.ts';
import {
  type Project, type ProbeTarget, MAX_FIRMWARE_BYTES, ProjectError, emptyProject, parseProject,
  refKey, serializeProject
} from './project/model.ts';
import { type BoardLayout, allTerminals, compileCircuit } from './project/netgraph.ts';
import { WorkerClient, type WorkerEvent } from './worker-client.ts';

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
const PALETTE: PartKind[] = ['breadboard', 'resistor', 'capacitor', 'led', 'diode', 'switch', 'npn', 'nmos', 'opamp', 'supply', 'ground'];

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
  const [notices, setNotices] = useState<string[]>([]);
  const [mcuPads, setMcuPads] = useState<Record<string, McuPadInfo>>({});
  const [pressed, setPressed] = useState<Set<string>>(new Set());
  const [message, setMessage] = useState('Place parts from the palette, wire them to CN7-10 (PA5) or CN9-1 (PA6), load an ELF and run.');
  const [fault, setFault] = useState('');
  const [scopeVersion, setScopeVersion] = useState(0);
  const [scopePreview, setScopePreview] = useState<[string, string] | null>(null);
  const [realTime, setRealTime] = useState(true);
  const [examples, setExamples] = useState<{ id: string; name: string; project: string; firmware: string | null; description: string }[]>([]);
  const scope = useRef(new ScopeBuffer());
  const scopeMap = useRef<(number | null)[]>([null, null]);
  const client = useRef<WorkerClient | null>(null);

  useEffect(() => {
    let live = true;
    fetch(assetUrl('boards/layout/stm32h563.json'))
      .then((r) => { if (!r.ok) throw new Error(`Board layout unavailable (${r.status})`); return r.json(); })
      .then((d: BoardLayout) => { if (live) setLayout(d); })
      .catch((e) => { if (live) setFault(String(e)); });
    return () => { live = false; };
  }, []);
  useEffect(() => () => client.current?.terminate(), []);
  useEffect(() => {
    fetch(assetUrl('examples/index.json')).then((r) => r.ok ? r.json() : { examples: [] })
      .then((d) => setExamples(d.examples ?? [])).catch(() => setExamples([]));
  }, []);

  const layouts = useMemo(() => new Map(layout ? [['stm32h563', layout]] : []), [layout]);
  const compiled = useMemo(() => compileCircuit(project, layouts), [project, layouts]);
  const terminals = useMemo(() => allTerminals(project, layouts), [project, layouts]);
  const terminalMap = useMemo(() => new Map(terminals.map((t) => [t.key, t])), [terminals]);
  const errors = compiled.diagnostics.filter((d) => d.severity === 'error');
  const errorKeys = useMemo(() => new Set(errors.flatMap((d) => d.refs ?? [])), [errors]);
  const hasCircuit = compiled.netlist.devices.length > 0;

  const onEvent = useCallback((e: WorkerEvent) => {
    const p = e.payload;
    if (e.event === 'state') {
      setMachine(p);
      if (p.stopped) setRunState('stopped');
      else if (p.paused) setRunState('paused');
    } else if (e.event === 'fault') {
      setFault(`${p.code}: ${p.message}`);
      setRunState('paused');
    } else if (e.event === 'gpio') {
      setMessage(`GPIO ${p.socPad} → ${['low', 'high', 'high-Z'][p.level] ?? '?'} at ${p.eventTimeNs} ns`);
    } else if (e.event === 'bench') {
      const width = p.probes.length + 1;
      if (p.samples?.length) {
        scope.current.append(p.samples as Float64Array, width, scopeMap.current);
        setScopeVersion(scope.current.version);
      }
      if (p.observation) setObservation(p.observation);
      setStats({ timeNs: Number(p.timeNs), simulatedPerWall: p.simulatedPerWall, solverSteps: p.solverSteps,
        rejectedSteps: p.rejectedSteps, rollbacks: p.rollbacks, sampleOverflow: p.sampleOverflow });
      const gpio: Record<string, McuPadInfo> = {};
      const fresh: string[] = [];
      for (const ev of p.events as any[]) {
        if (ev.kind === 'gpio') gpio[ev.socPad] = { level: ev.level, mode: ev.mode, pull: ev.pull, timeNs: ev.timeNs };
        if (ev.kind === 'unsupported') fresh.push(`${ev.socPad}: ${ev.message}`);
      }
      if (Object.keys(gpio).length) setMcuPads((old) => ({ ...old, ...gpio }));
      if (fresh.length) setNotices((old) => [...new Set([...old, ...fresh])]);
      if (p.mcuStopped) setRunState('stopped');
      else if (p.reachedTarget && p.timeNs !== '0' && p.requestId) setRunState('paused');
    }
  }, []);

  function worker(): WorkerClient {
    if (!client.current) {
      const c = new WorkerClient(new URL('worker/worker.mjs', document.baseURI));
      c.subscribe(onEvent);
      c.onFailure((m) => setFault(`Worker failed: ${m}`));
      client.current = c;
    }
    return client.current;
  }

  const fail = (e: unknown) => setFault(e instanceof Error ? e.message : String(e));

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
        instruments: { ...project.instruments, scope: { ...project.instruments.scope,
          channels: project.instruments.scope.channels.map((c) => (c?.kind === 'current' && c.part === id) ||
            (c?.kind === 'voltage' && c.at.kind === 'part' && c.at.part === id) ? null : c) } } }, `Deleted ${id}`);
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

  function setProbe(channel: 0 | 1, target: ProbeTarget | null) {
    const channels = [...project.instruments.scope.channels];
    channels[channel] = target;
    edit({ ...project, instruments: { ...project.instruments, scope: { ...project.instruments.scope, channels } } },
      `Scope CH${channel + 1} set`);
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

  async function loadElf(bytes: Uint8Array, name: string, hash: string) {
    await worker().request('loadFirmware', { bytes });
    setFirmware({ name, hash, bytes });
    setBenchActive(false); setBenchStale(false); setObservation(null); setStats(null);
    scope.current.clear(); setScopeVersion(scope.current.version);
    setScopePreview(null);
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
    const map: (number | null)[] = [null, null];
    p.instruments.scope.channels.forEach((ch, i) => {
      if (!ch) return;
      if (ch.kind === 'voltage') {
        const net = compiled.terminalNet.get(refKey(ch.at));
        if (!net) return;
        map[i] = probes.length;
        probes.push({ id: `ch${i + 1}`, kind: 'voltage', net });
      } else if (compiled.netlist.devices.some((d) => d.id === ch.part)) {
        map[i] = probes.length;
        probes.push({ id: `ch${i + 1}`, kind: 'current', device: ch.part });
      }
    });
    return { probes, map };
  }

  async function startBench(initial: 'zero' | 'dc' = 'zero') {
    if (errors.length) throw new Error(`Fix the circuit first: ${errors[0].message}`);
    if (compiled.pads.length && !firmware) throw new Error(`The circuit uses ${compiled.pads.join(', ')}: load firmware first`);
    const { probes, map } = probesFor(project);
    scopeMap.current = map;
    await worker().request('createBench', { netlist: compiled.netlist, probes,
      options: { lookaheadNs: project.solver.lookaheadNs, sampleNs: project.solver.sampleNs, initial } });
    setBenchActive(true);
    setBenchStale(false);
  }

  async function run() {
    setFault('');
    try {
      if (scopePreview) {
        scope.current.clear(); setScopeVersion(scope.current.version); setScopePreview(null);
      }
      if (hasCircuit) {
        if (!benchActive || benchStale) await startBench();
        await worker().request('runUntil', realTime ? { maxRatio: 1 } : {});
        setMessage(compiled.pads.length ? `Firmware and circuit running on one virtual clock (${compiled.pads.join(', ')} coupled).`
          : 'Analog-only circuit running.');
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
      setBenchActive(false); setBenchStale(false); setObservation(null); setStats(null); setMcuPads({});
      scope.current.clear(); setScopeVersion(scope.current.version);
      setScopePreview(null);
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
      setMessage(`Project saved (circuit, probes, solver settings, view and embedded ELF).${scopePreview ? ' Imported waveform preview is not included.' : ''}`);
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
        await loadElf(bytes, p.simulator.firmwareName || file.name, hash);
      } else {
        client.current?.terminate();
        client.current = null;
        setFirmware(null); setMachine(null); setRunState('idle'); setBenchActive(false);
        scope.current.clear(); setScopeVersion(scope.current.version); setScopePreview(null);
      }
      const { firmwareBase64: _drop, ...simulator } = p.simulator;
      void _drop;
      setHistory(createHistory({ ...p, simulator }));
      setSelection(null);
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
        await loadElf(bytes, ex.firmware, hash);
      } else {
        if (benchActive) await worker().request('destroyBench');
        setBenchActive(false); setBenchStale(false); setObservation(null); setStats(null);
        scope.current.clear(); setScopeVersion(scope.current.version);
        setScopePreview(null);
        setRunState(firmware ? 'paused' : 'idle');
      }
      setHistory(createHistory(p));
      setSelection(null);
      setMessage(`${ex.name}: ${ex.description}`);
    } catch (err) { fail(err); }
  }

  function exportCsv() {
    download('m33mu-scope.csv', scope.current.toCsv(['ch1', 'ch2']), 'text/csv');
  }

  function exportSigc() {
    try {
      download('m33mu-scope.sigc', scope.current.toSigc([u1, u2]), 'application/octet-stream');
      setMessage('Exported voltage traces as SIGC v1 (ch1/ch2, millivolts, time relative to first sample).');
    } catch (err) { fail(err); }
  }

  async function importSigc(file: File) {
    setFault('');
    try {
      if (runState === 'running') throw new SigcError('Pause the bench before importing a SIGC preview');
      if (file.size > SIGC_MAX_BYTES) throw new SigcError('SIGC input exceeds 8 MiB');
      const capture = decodeSigc(new Uint8Array(await file.arrayBuffer()));
      scope.current.loadSigc(capture);
      setScopeVersion(scope.current.version);
      setScopePreview([capture.traces[0].name, capture.traces[1]?.name ?? '—']);
      setMessage(`Imported ${capture.traces.length} voltage trace(s) for scope preview. Waveforms are not stored in the project; export them separately.`);
    } catch (err) { fail(err); }
  }

  // ------------------------------------------------------------ render

  const brightness: Record<string, number> = {};
  for (const p of project.parts) {
    if (p.kind === 'led' && observation) {
      const i = observation.currents[p.id] ?? 0;
      brightness[p.id] = Math.sqrt(Math.max(0, Math.min(1, i / 0.02)));
    }
  }
  const channelLabel = (ch: ProbeTarget | null): [string, string] => {
    if (!ch) return ['—', 'V'];
    if (ch.kind === 'current') return [`I(${ch.part})`, 'A'];
    const t = terminalMap.get(refKey(ch.at));
    return [`V(${t?.label ?? 'net'})`, 'V'];
  };
  const [l1, u1] = scopePreview ? [scopePreview[0], 'V'] : channelLabel(project.instruments.scope.channels[0]);
  const [l2, u2] = scopePreview ? [scopePreview[1], 'V'] : channelLabel(project.instruments.scope.channels[1]);
  const timeNs = stats?.timeNs ?? (machine ? Number(machine.virtualTimeNs) : 0);
  const canRun = runState !== 'running' && (hasCircuit ? errors.length === 0 : !!firmware && runState !== 'stopped');

  return (
    <main className="app">
      <header className="topbar">
        <div className="brand"><span className="mark">m</span><div><strong>m33mu</strong><small>WORKBENCH</small></div></div>
        <div className="board-name">{layout ? `${layout.boardName} · ${layout.boardRevision ?? ''}` : 'Loading board…'}</div>
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
            <li className="on">Firmware execution (STM32H563, ELF)</li>
            <li className="on">Analog transient solver (R, C, L, diode, LED, BJT, MOSFET, switch, supplies)</li>
            <li className="on">GPIO electrical coupling: CN7-10 (PA5), CN9-1 (PA6)</li>
            <li>Other board contacts: visual only</li>
            <li>ADC, PWM/timer outputs, UART, SPI, I²C: not modelled</li>
          </ul>
          {notices.length > 0 && <ul className="caps notices">{notices.map((n) => <li key={n}>{n}</li>)}</ul>}
        </aside>
        <section className="bench">
          {benchStale && (
            <div className="banner">Circuit edited while the bench was active.
              <button onClick={applyChanges} disabled={errors.length > 0}>Apply and run</button></div>
          )}
          <BenchCanvas project={project} layout={layout}
            artworkUrl={layout ? assetUrl(`boards/${layout.artwork.replace('../', '')}`) : ''}
            terminals={terminals} selection={selection} tool={tool} running={benchActive && runState === 'running'}
            brightness={brightness} pressed={pressed} errorKeys={errorKeys}
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
        </aside>
      </div>
      <Scope buffer={scope.current} version={scopeVersion} labels={[l1, l2]} units={[u1, u2]}
        windowNs={project.instruments.scope.windowNs}
        trigger={project.instruments.scope.trigger ?? null}
        onWindow={(ns) => view({ ...project, instruments: { ...project.instruments, scope: { ...project.instruments.scope, windowNs: ns } } })}
        onTrigger={(trigger) => view({ ...project, instruments: { ...project.instruments, scope: { ...project.instruments.scope, trigger } } })}
        onExport={exportCsv} onExportSigc={exportSigc} onImportSigc={(file) => void importSigc(file)} />
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
