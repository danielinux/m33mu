import { ChangeEvent, useEffect, useRef, useState } from 'react';

type Contact = { number: number; x: number; y: number; signal: string | null; mapping_verified: boolean };
type Connector = { id: string; kind: string; rows: number; columns: number; contacts: Contact[] };
type BoardLayout = {
  cpu: string; boardName: string; manufacturerBoardVerified: boolean;
  artwork: string; pixelViewbox: [number, number, number, number];
  geometryStatus: string; electricalMapStatus: string; connectors: Connector[];
  warning?: string;
};
type Machine = {
  registers: number[]; xpsr: number; securityState: number; mode: number;
  sleeping: boolean; cpuHz: number; cycleTotal: string; virtualTimeNs: string;
  stopped?: boolean; paused?: boolean;
};
type WorkerMessage = { kind: 'response' | 'event'; requestId?: number; ok?: boolean;
  result?: any; error?: { code: string; message: string }; event?: string; payload?: any };
type WorkbenchProject = {
  schemaVersion: 1; projectId: string; name: string;
  simulator: { cpu: 'stm32h563'; firmwareFormat: 'elf'; firmwareSha256: string | null;
    firmwareName: string; cpuHz: number; firmwareBase64?: string };
  boards: { id: string; cpu: string; layoutVersion: number; x: number; y: number;
    rotation: 0; electricalEnabled: false }[];
  contacts: unknown[]; nets: unknown[]; components: unknown[]; instruments: unknown[];
  ui?: { zoom?: number };
};

const hex = (value: number) => `0x${(value >>> 0).toString(16).padStart(8, '0')}`;

function encodeBase64(bytes: Uint8Array) {
  let binary = '';
  const chunk = 0x8000;
  for (let i = 0; i < bytes.length; i += chunk) {
    binary += String.fromCharCode(...bytes.subarray(i, i + chunk));
  }
  return btoa(binary);
}

export function Workbench() {
  const cpu = 'stm32h563';
  const [layout, setLayout] = useState<BoardLayout | null>(null);
  const [selected, setSelected] = useState<Contact | null>(null);
  const [machine, setMachine] = useState<Machine | null>(null);
  const [firmwareName, setFirmwareName] = useState('No firmware loaded');
  const [firmwareHash, setFirmwareHash] = useState('');
  const [firmwareBytes, setFirmwareBytes] = useState<Uint8Array | null>(null);
  const [runState, setRunState] = useState<'idle' | 'running' | 'paused' | 'stopped'>('idle');
  const [fault, setFault] = useState('');
  const [message, setMessage] = useState('Select an ELF firmware image to start the H563 session.');
  const [zoom, setZoom] = useState(1);
  const worker = useRef<Worker | null>(null);
  const pending = useRef(new Map<number, { resolve: (value: any) => void; reject: (error: Error) => void }>());
  const nextId = useRef(1);
  useEffect(() => {
    let active = true;
    fetch(`/boards/layout/${cpu}.json`).then((r) => {
      if (!r.ok) throw new Error(`Board layout unavailable (${r.status})`);
      return r.json() as Promise<BoardLayout>;
    }).then((data) => { if (active) { setLayout(data); setSelected(null); } })
      .catch((error) => { if (active) setFault(String(error)); });
    return () => { active = false; };
  }, [cpu]);

  useEffect(() => () => {
    worker.current?.terminate();
    pending.current.forEach(({ reject }) => reject(new Error('Workbench closed')));
    pending.current.clear();
  }, []);

  function ensureWorker() {
    if (worker.current) return worker.current;
    const instance = new Worker('/worker/worker.mjs', { type: 'module' });
    instance.onmessage = ({ data }: MessageEvent<WorkerMessage>) => {
      if (data.kind === 'event') {
        if (data.event === 'state') {
          setMachine(data.payload);
          if (data.payload.stopped) setRunState('stopped');
          else if (data.payload.paused) setRunState('paused');
        } else if (data.event === 'fault') {
          setFault(`${data.payload.code}: ${data.payload.message}`);
          setRunState('paused');
        } else if (data.event === 'gpio') {
          setMessage(`GPIO ${data.payload.socPad} → ${['low', 'high', 'high-Z'][data.payload.level] ?? 'unknown'} at ${data.payload.eventTimeNs} ns`);
        }
        return;
      }
      const pendingRequest = pending.current.get(data.requestId ?? 0);
      if (!pendingRequest) return;
      pending.current.delete(data.requestId!);
      if (data.ok) pendingRequest.resolve(data.result);
      else pendingRequest.reject(new Error(`${data.error?.code ?? 'ERROR'}: ${data.error?.message ?? 'Request failed'}`));
    };
    instance.onerror = (event) => setFault(event.message || 'Worker failed');
    worker.current = instance;
    return instance;
  }

  function request(operation: string, payload: Record<string, unknown> = {}) {
    const instance = ensureWorker();
    const requestId = nextId.current++;
    const response = new Promise<any>((resolve, reject) => pending.current.set(requestId, { resolve, reject }));
    instance.postMessage({ schemaVersion: 1, kind: 'request', requestId, operation, payload });
    return response;
  }

  async function loadFirmware(event: ChangeEvent<HTMLInputElement>) {
    const file = event.target.files?.[0];
    if (!file) return;
    setFault(''); setMessage(`Loading ${file.name}…`); setRunState('idle');
    try {
      if (!file.name.toLowerCase().endsWith('.elf')) throw new Error('The browser worker currently accepts ELF32/ARM files only.');
      const bytes = new Uint8Array(await file.arrayBuffer());
      const digest = await crypto.subtle.digest('SHA-256', bytes);
      const hash = [...new Uint8Array(digest)].map((v) => v.toString(16).padStart(2, '0')).join('');
      await request('loadFirmware', { bytes },);
      setFirmwareName(file.name); setFirmwareHash(hash); setFirmwareBytes(bytes); setMessage('Firmware loaded into the H563 worker session.');
      setRunState('paused');
    } catch (error) { setFault(error instanceof Error ? error.message : String(error)); }
    finally { event.target.value = ''; }
  }

  async function run() {
    setFault('');
    try { await request('run'); setRunState('running'); setMessage('Firmware running in bounded worker slices.'); }
    catch (error) { setFault(error instanceof Error ? error.message : String(error)); }
  }
  async function pause() {
    try { await request('pause'); setRunState('paused'); }
    catch (error) { setFault(error instanceof Error ? error.message : String(error)); }
  }
  async function step() {
    setFault('');
    try { const state = await request('step', { instructionBudget: 1 }); setMachine(state); setRunState(state.stopped ? 'stopped' : 'paused'); }
    catch (error) { setFault(error instanceof Error ? error.message : String(error)); }
  }
  async function reset() {
    setFault('');
    try { await request('reset'); setRunState('paused'); setMessage('Firmware reset to its vector-table entry.'); }
    catch (error) { setFault(error instanceof Error ? error.message : String(error)); }
  }

  function saveProject() {
    const project: WorkbenchProject = {
      schemaVersion: 1, projectId: crypto.randomUUID(), name: firmwareName.replace(/\.elf$/i, '') || 'H563 project',
      simulator: { cpu: 'stm32h563', firmwareFormat: 'elf', firmwareSha256: firmwareHash || null,
        firmwareName: firmwareBytes ? firmwareName : '', cpuHz: machine?.cpuHz ?? 64_000_000,
        ...(firmwareBytes ? { firmwareBase64: encodeBase64(firmwareBytes) } : {}) },
      boards: [{ id: 'board-1', cpu: 'stm32h563', layoutVersion: 1, x: 0, y: 0, rotation: 0, electricalEnabled: false }],
      contacts: [], nets: [], components: [], instruments: [], ui: { zoom },
    };
    const blob = new Blob([JSON.stringify(project, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement('a');
    anchor.href = url; anchor.download = `${project.name.replace(/[^a-zA-Z0-9_-]+/g, '-') || 'm33mu-project'}.m33mu.json`;
    anchor.click(); URL.revokeObjectURL(url);
    setMessage('Project saved as a v1 JSON document, including the loaded ELF when present.');
  }

  async function openProject(event: ChangeEvent<HTMLInputElement>) {
    const file = event.target.files?.[0];
    if (!file) return;
    setFault(''); setMessage(`Opening ${file.name}…`);
    try {
      if (file.size > 48 * 1024 * 1024) throw new Error('Project JSON exceeds the 48 MiB import limit.');
      const project = JSON.parse(await file.text()) as WorkbenchProject;
      if (project.schemaVersion !== 1 || project.simulator?.cpu !== cpu || !Array.isArray(project.boards) ||
          project.simulator.firmwareFormat !== 'elf' || project.boards.length !== 1 ||
          project.boards[0].cpu !== cpu || project.boards[0].electricalEnabled !== false ||
          !Array.isArray(project.contacts) || project.contacts.length !== 0 ||
          !Array.isArray(project.nets) || project.nets.length !== 0 ||
          !Array.isArray(project.components) || project.components.length !== 0 ||
          !Array.isArray(project.instruments) || project.instruments.length !== 0) {
        throw new Error('This project is not a supported v1 STM32H563 workbench project.');
      }
      setZoom(Math.min(1.5, Math.max(.55, Number(project.ui?.zoom) || 1)));
      if (project.simulator.firmwareBase64) {
        if (project.simulator.firmwareBase64.length > 44_739_244) throw new Error('Embedded ELF exceeds the 32 MiB worker limit.');
        const binary = atob(project.simulator.firmwareBase64);
        const bytes = Uint8Array.from(binary, (character) => character.charCodeAt(0));
        const digest = await crypto.subtle.digest('SHA-256', bytes);
        const hash = [...new Uint8Array(digest)].map((value) => value.toString(16).padStart(2, '0')).join('');
        if (hash !== project.simulator.firmwareSha256) throw new Error('Embedded ELF SHA-256 does not match the project manifest.');
        await request('loadFirmware', { bytes });
        setFirmwareBytes(bytes); setFirmwareHash(hash); setFirmwareName(project.simulator.firmwareName || file.name);
        setRunState('paused'); setMessage(`Opened ${project.name || file.name}; ELF restored and verified.`);
      } else {
        worker.current?.terminate(); worker.current = null;
        pending.current.forEach(({ reject }) => reject(new Error('Worker session replaced by project')));
        pending.current.clear();
        setFirmwareBytes(null); setFirmwareHash(''); setFirmwareName('No firmware loaded');
        setMachine(null); setRunState('idle');
        setMessage(`Opened ${project.name || file.name}. Load an ELF to start the H563 session.`);
      }
    } catch (error) { setFault(error instanceof Error ? error.message : String(error)); }
    finally { event.target.value = ''; }
  }

  const view = layout?.pixelViewbox ?? [0, 0, 690, 950];
  const canRun = !!firmwareHash && runState !== 'running' && runState !== 'stopped';
  return <main className="app-shell">
    <header className="topbar">
      <div className="brand"><span className="brand-mark">m</span><div><strong>m33mu</strong><small>WEB WORKBENCH</small></div></div>
      <div className="target-select"><span>CPU PROFILE</span><select value={cpu} disabled aria-label="Supported CPU profile">
        <option value="stm32h563">STM32H563</option>
      </select></div>
      <div className="firmware-name" title={firmwareHash || undefined}><span className={`status-dot ${runState}`} />{firmwareName}</div>
      <div className="transport">
        <label className="button project-button">Open project<input type="file" accept=".json,.m33mu.json,application/json" onChange={openProject} /></label>
        <label className="button primary load-button">Load ELF<input type="file" accept=".elf,application/octet-stream" onChange={loadFirmware} /></label>
        {runState === 'running' ? <button className="button" onClick={pause}>Pause</button> : <button className="button" onClick={run} disabled={!canRun}>Run</button>}
        <button className="button" onClick={step} disabled={!firmwareHash || runState === 'running'}>Step</button>
        <button className="button icon-button" onClick={reset} disabled={!firmwareHash} title="Reset">↺</button>
      </div>
    </header>
    <div className="workspace">
      <aside className="left-panel">
        <div className="panel-heading"><span>BENCH</span><button className="quiet-button" title="Add board" onClick={() => setMessage('Additional board placement is part of the next canvas interaction pass.')}>＋</button></div>
        <div className="side-group"><span className="eyebrow">BOARDS</span><button className="board-row selected"><span className="board-icon">▰</span><span><strong>{layout?.boardName ?? cpu}</strong><small>{layout?.manufacturerBoardVerified ? 'Manufacturer board · visual only' : 'Nucleo-style proxy · visual only'}</small></span><i /></button></div>
        <div className="side-group parts-group"><span className="eyebrow">COMPONENTS</span>
          {['Resistor', 'Capacitor', 'LED', 'Voltage source', 'Ground'].map((item) => <button className="part-row" key={item} onClick={() => setMessage(`${item} analog behavior is not implemented yet.`)}><span className="part-glyph">＋</span>{item}<span className="locked">VIEW</span></button>)}
          <div className="locked-note">Analog components are catalog previews. Electrical solving is not connected yet.</div>
        </div>
        <div className="left-footer"><span className="offline-dot" /> LOCAL SESSION <span>·</span> NO SERVER</div>
      </aside>

      <section className="bench-area" aria-label="Workbench canvas">
        <div className="canvas-tools"><div className="tool-tabs"><button className="tool active">Select <kbd>V</kbd></button><button className="tool" onClick={() => setMessage('Wiring is disabled until board contact geometry and electrical maps are verified.')}>Wire <kbd>W</kbd></button><button className="tool" onClick={() => setMessage('Voltage probes require an analog solver, which is not yet connected.')}>Probe <kbd>P</kbd></button></div><div className="zoom-tools"><button onClick={() => setZoom(Math.max(.55, zoom - .1))}>−</button><span>{Math.round(zoom * 100)}%</span><button onClick={() => setZoom(Math.min(1.5, zoom + .1))}>＋</button><button onClick={() => setZoom(1)} title="Fit board">⌂</button></div></div>
        <div className="bench-scroll"><div className="bench-grid" />
          <div className="board-stage" style={{ transform: `scale(${zoom})` }}>
            {layout && <img className="board-art" src={`/boards/${layout.artwork.replace('../', '')}`} alt={`${layout.boardName} provisional board illustration`} draggable={false} />}
            {layout?.connectors.flatMap((connector) => connector.contacts.map((contact) => <button key={`${connector.id}-${contact.number}`} className={`contact ${selected === contact ? 'contact-selected' : ''}`} style={{ left: `${(contact.x / view[2]) * 100}%`, top: `${(contact.y / view[3]) * 100}%` }} title={`${connector.id} contact ${contact.number} · mapping unverified`} aria-label={`${connector.id} contact ${contact.number}, visual only`} onClick={() => setSelected(contact)} />))}
          </div>
        </div>
          <div className="canvas-status"><span className="visual-badge">VISUAL ONLY</span><span>{layout?.geometryStatus.replaceAll('_', ' ') ?? 'Loading board geometry'}</span><span className="status-spacer" /><button onClick={saveProject}>Save project</button></div>
      </section>

      <aside className="right-panel">
        <div className="panel-heading">INSPECTOR</div>
        <div className="inspector-block"><span className="eyebrow">FIRMWARE</span><h2>{firmwareName}</h2><p>{firmwareHash ? `SHA-256 ${firmwareHash.slice(0, 12)}…` : 'Load an ELF32/ARM firmware image to inspect and run the emulator.'}</p></div>
        <div className="machine-state"><div className="eyebrow">CPU STATE <span>{machine ? `${machine.cpuHz.toLocaleString()} Hz` : 'WAITING'}</span></div>
          <div className="register-grid">{['r0', 'r1', 'r2', 'r3', 'r12', 'lr', 'sp', 'pc'].map((name, i) => <div className="register" key={name}><span>{name}</span><b>{machine ? hex(name === 'sp' ? machine.registers[13] : name === 'lr' ? machine.registers[14] : machine.registers[[0, 1, 2, 3, 12, 14, 13, 15][i]]) : '—'}</b></div>)}</div>
          <div className="machine-meta"><div><span>VIRTUAL TIME</span><b>{machine ? `${(Number(machine.virtualTimeNs) / 1e6).toFixed(3)} ms` : '—'}</b></div><div><span>CYCLES</span><b>{machine ? BigInt(machine.cycleTotal).toLocaleString() : '—'}</b></div><div><span>SECURITY</span><b>{machine ? (machine.securityState ? 'NON-SECURE' : 'SECURE') : '—'}</b></div><div><span>MODE</span><b>{machine ? (machine.mode ? 'THREAD' : 'HANDLER') : '—'}</b></div></div>
        </div>
        <div className="inspector-block contact-inspector"><span className="eyebrow">BOARD CONTACT</span>{selected ? <><h2>{selected.number}</h2><p>Physical position only. The provisional artwork contact is not mapped to a verified connector pad.</p></> : <p>Select a board contact to inspect its visual locator.</p>}<div className="mapping-warning">⚑ &nbsp;Electrical mapping disabled</div></div>
        <div className="capability-card"><span className="eyebrow">CAPABILITIES</span><div><i className="on" /> Firmware execution</div><div><i /> Analog solver</div><div><i /> Verified H563 contacts</div><div><i /> ADC / UART</div></div>
      </aside>
    </div>
    <footer className="bottom-dock"><div className="dock-tabs"><button className="active">Firmware</button><button onClick={() => setMessage('The analog scope is unavailable until solver integration.')}>Scope <span className="soon">NOT CONNECTED</span></button><button onClick={() => setMessage('UART is not currently implemented in the browser worker.')}>Serial <span className="soon">NOT CONNECTED</span></button><button onClick={() => setMessage('Faults and worker messages appear here.')}>Problems</button></div><div className="dock-message">{fault ? <span className="fault-text">{fault}</span> : message}</div><div className="virtual-clock">{machine ? `${(Number(machine.virtualTimeNs) / 1e6).toFixed(3)} ms` : '0.000 ms'} <span>SIM TIME</span></div></footer>
  </main>;
}
