// Selection inspector: parameters, solved voltages/currents, pad state, probes.

import { useEffect, useState } from 'react';
import { CATALOG, formatSi, parseSi } from './project/catalog.ts';
import { type Project, type ProbeTarget, type TerminalRef, refKey } from './project/model.ts';
import { type CompiledCircuit, type PlacedTerminal, partTerminals } from './project/netgraph.ts';
import type { Selection } from './canvas/BenchCanvas.tsx';

export type Observation = {
  timeNs: number;
  nets: Record<string, number>;
  currents: Record<string, number>;
  pads: Record<string, { volts: number; digital: 0 | 1; drive: string; pull: string }>;
};

export type McuPadInfo = { level: number; mode: number; pull: number; timeNs: number };

const MODES = ['input', 'output', 'alternate function', 'analog'];
const PULLS = ['none', 'pull-up', 'pull-down'];

type Props = {
  project: Project;
  selection: Selection;
  compiled: CompiledCircuit;
  terminals: Map<string, PlacedTerminal>;
  observation: Observation | null;
  mcuPads: Record<string, McuPadInfo>;
  benchActive: boolean;
  onParam(partId: string, key: string, value: number | string): void;
  onRotate(): void;
  onDelete(): void;
  onProbe(channel: 0 | 1, target: ProbeTarget | null): void;
};

function ParamInput({ value, unit, onCommit }: { value: number; unit: string; onCommit(v: number): void }) {
  const [text, setText] = useState(formatSi(value, unit));
  const [bad, setBad] = useState(false);
  useEffect(() => { setText(formatSi(value, unit)); setBad(false); }, [value, unit]);
  return (
    <input value={text} aria-invalid={bad} className={bad ? 'invalid' : ''}
      onChange={(e) => setText(e.target.value)}
      onBlur={() => { const v = parseSi(text); if (v === null) setBad(true); else onCommit(v); }}
      onKeyDown={(e) => { if (e.key === 'Enter') (e.target as HTMLInputElement).blur(); }} />
  );
}

export function Inspector(props: Props) {
  const { project, selection, compiled, observation } = props;
  const voltageOf = (ref: TerminalRef) => {
    const net = compiled.terminalNet.get(refKey(ref));
    if (!net || !observation) return null;
    return observation.nets[net] ?? 0;
  };
  const probeButtons = (target: ProbeTarget) => (
    <div className="probe-buttons">
      {[0, 1].map((c) => (
        <button key={c} type="button" onClick={() => props.onProbe(c as 0 | 1, target)}>Probe on CH{c + 1}</button>
      ))}
    </div>
  );

  if (!selection) {
    return (
      <div className="inspector empty">
        <p>Select a part, wire or board contact. Drag the background to pan, scroll to zoom.</p>
        <p><kbd>V</kbd> select · <kbd>W</kbd> wire · <kbd>R</kbd> rotate · <kbd>Del</kbd> delete · <kbd>Ctrl+Z</kbd>/<kbd>Ctrl+Y</kbd> undo/redo</p>
      </div>
    );
  }
  if (selection.kind === 'contact') {
    const t = props.terminals.get(selection.key);
    if (!t || t.ref.kind !== 'board') return null;
    const soc = t.socPad;
    const pad = soc ? observation?.pads[soc] : undefined;
    const mcu = soc ? props.mcuPads[soc] : undefined;
    return (
      <div className="inspector">
        <h3>Board contact {t.ref.contact}</h3>
        {t.enabled ? (
          <dl>
            <dt>SoC pad</dt><dd>{soc}</dd>
            <dt>Firmware mode</dt><dd>{mcu ? `${MODES[mcu.mode]}, ${PULLS[mcu.pull] ?? 'none'}` : 'not reported yet'}</dd>
            <dt>Driver</dt><dd>{pad ? `${pad.drive}, pull ${pad.pull}` : '—'}</dd>
            <dt>Voltage</dt><dd>{pad ? formatSi(pad.volts, 'V') : 'not wired / bench idle'}</dd>
            <dt>Input level</dt><dd>{pad ? (pad.digital ? 'high' : 'low') : '—'}</dd>
            <dt>Current out of pad</dt><dd>{observation ? formatSi(observation.currents[`pad-${soc}`] ?? 0, 'A') : '—'}</dd>
          </dl>
        ) : (
          <p className="warning">Visual locator only. Its electrical mapping has not been verified for this board revision, so it cannot be wired.</p>
        )}
        {t.enabled && probeButtons({ kind: 'voltage', at: t.ref })}
      </div>
    );
  }
  if (selection.kind === 'wire') {
    const w = project.wires.find((x) => x.id === selection.id);
    if (!w) return null;
    const v = voltageOf(w.from);
    return (
      <div className="inspector">
        <h3>Wire</h3>
        <dl>
          <dt>Net</dt><dd>{compiled.terminalNet.get(refKey(w.from)) ?? '—'}</dd>
          <dt>Voltage</dt><dd>{v === null ? 'bench idle' : formatSi(v, 'V')}</dd>
        </dl>
        {probeButtons({ kind: 'voltage', at: w.from })}
        <button type="button" onClick={props.onDelete}>Delete wire</button>
      </div>
    );
  }
  const part = project.parts.find((p) => p.id === selection.id);
  if (!part) return null;
  const def = CATALOG[part.kind];
  const current = observation?.currents[part.id];
  const terms = partTerminals(project, part.id);
  return (
    <div className="inspector">
      <h3>{def.title} <span className="muted">{part.id}</span></h3>
      {def.params.map((spec) => (
        <label key={spec.key} className="param">{spec.label}
          {spec.choices ? (
            <select value={String(part.params[spec.key])} onChange={(e) => props.onParam(part.id, spec.key, e.target.value)}>
              {spec.choices.map((c) => <option key={c.value} value={c.value}>{c.label}</option>)}
            </select>
          ) : (
            <ParamInput value={Number(part.params[spec.key])} unit={spec.unit}
              onCommit={(v) => props.onParam(part.id, spec.key, Math.min(spec.max, Math.max(spec.min, v)))} />
          )}
        </label>
      ))}
      {part.kind !== 'breadboard' && (
        <dl>
          {terms.map((t) => {
            const v = t.ref.kind === 'part' ? voltageOf(t.ref) : null;
            return [<dt key={`${t.key}-l`}>{t.label}</dt>,
              <dd key={`${t.key}-v`}>{v === null ? '—' : formatSi(v, 'V')}</dd>];
          })}
          {current !== undefined && part.kind !== 'ground' && (<><dt>Current</dt><dd>{formatSi(current, 'A')}</dd></>)}
          {part.kind === 'led' && current !== undefined && (
            <><dt>Brightness</dt><dd>{Math.round(Math.max(0, Math.min(1, current / 0.02)) * 100)}% of rated 20 mA</dd></>
          )}
        </dl>
      )}
      {part.kind !== 'breadboard' && part.kind !== 'ground' && probeButtons({ kind: 'current', part: part.id })}
      {part.kind !== 'breadboard' && terms[0] && probeButtons({ kind: 'voltage', at: terms[0].ref })}
      <div className="row">
        <button type="button" onClick={props.onRotate}>Rotate</button>
        <button type="button" onClick={props.onDelete}>Delete</button>
      </div>
      {props.benchActive && <p className="muted">Parameter edits pause the bench until you apply them.</p>}
    </div>
  );
}
