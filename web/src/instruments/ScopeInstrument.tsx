// One oscilloscope: owns its sample ring, acquisition engine, hold and
// preview state. The app feeds it bench sample batches through `register`.

import { useEffect, useRef, useState } from 'react';
import type { ScopeAcquisition, ScopeConfig, ScopeTrigger } from '../project/model.ts';
import { Scope, ScopeAcquisitionEngine, ScopeBuffer } from './Scope.tsx';
import { decodeSigc, SIGC_MAX_BYTES, SigcError } from './sigc.ts';

/** Imperative hooks the app uses to stream data into a scope. */
export type ScopeSink = {
  append(flat: Float64Array, width: number, columns: (number | null)[]): void;
  /** Drop samples and rearm, e.g. on firmware load or bench start. */
  reset(): void;
};

type Held = { buffer: ScopeBuffer; labels: [string, string]; units: [string, string];
  kind: 'manual' | 'normal' | 'single'; triggerTime?: number };

type Props = {
  config: ScopeConfig;
  labels: [string, string];
  units: [string, string];
  running: boolean;
  register(id: string, sink: ScopeSink | null): void;
  onConfig(next: ScopeConfig): void;
  onMessage(text: string): void;
  onError(error: unknown): void;
};

function download(name: string, data: string | Uint8Array, type: string) {
  const url = URL.createObjectURL(new Blob([data as BlobPart], { type }));
  const a = document.createElement('a');
  a.href = url;
  a.download = name;
  a.click();
  URL.revokeObjectURL(url);
}

export function ScopeInstrument({ config, labels: liveLabels, units: liveUnits, running, register,
  onConfig, onMessage, onError }: Props) {
  const buffer = useRef(new ScopeBuffer());
  const engine = useRef(new ScopeAcquisitionEngine());
  const manualHold = useRef(false);
  const meta = useRef({ labels: liveLabels, units: liveUnits });
  const configRef = useRef(config);
  const [version, setVersion] = useState(0);
  const [held, setHeld] = useState<Held | null>(null);
  const [preview, setPreview] = useState<[string, string] | null>(null);
  const labels: [string, string] = preview ?? liveLabels;
  const units: [string, string] = preview ? ['V', 'V'] : liveUnits;
  meta.current = { labels, units };
  configRef.current = config;

  const arm = (c: ScopeConfig, afterNs: number) => {
    engine.current.arm(c.acquisition ?? 'auto', c.trigger ?? null, c.windowNs, afterNs);
    manualHold.current = false;
    setHeld(null);
  };

  useEffect(() => {
    const sink: ScopeSink = {
      append(flat, width, columns) {
        buffer.current.append(flat, width, columns);
        const captured = engine.current.observe(buffer.current);
        if (captured && !manualHold.current) {
          const m = meta.current;
          setHeld({ buffer: buffer.current.snapshotRange(captured.fromNs, captured.throughNs),
            labels: [...m.labels], units: [...m.units], kind: captured.mode, triggerTime: captured.timeNs });
        }
        setVersion(buffer.current.version);
      },
      reset() {
        buffer.current.clear();
        setPreview(null);
        arm(configRef.current, 0);
        setVersion(buffer.current.version);
      }
    };
    register(config.id, sink);
    return () => register(config.id, null);
  }, [config.id, register]);

  function update(next: ScopeConfig) {
    onConfig(next);
    arm(next, buffer.current.lastTime());
  }

  function toggleHold() {
    if (held?.kind === 'manual' || held?.kind === 'single') arm(config, buffer.current.lastTime());
    else if (held?.kind === 'normal') {
      manualHold.current = true;
      setHeld({ ...held, kind: 'manual' });
    } else {
      manualHold.current = true;
      setHeld({ buffer: buffer.current.snapshot(), labels, units, kind: 'manual' });
    }
  }

  async function importSigc(file: File) {
    try {
      if (running) throw new SigcError('Pause the bench before importing a SIGC preview');
      if (file.size > SIGC_MAX_BYTES) throw new SigcError('SIGC input exceeds 8 MiB');
      const capture = decodeSigc(new Uint8Array(await file.arrayBuffer()));
      buffer.current.loadSigc(capture);
      arm(config, buffer.current.lastTime());
      const names: [string, string] = [capture.traces[0].name, capture.traces[1]?.name ?? '—'];
      manualHold.current = true;
      setHeld({ buffer: buffer.current.snapshot(), labels: names, units: ['V', 'V'], kind: 'manual' });
      setPreview(names);
      setVersion(buffer.current.version);
      onMessage(`Imported ${capture.traces.length} voltage trace(s) into ${config.name}. Waveforms are not stored in the project; export them separately.`);
    } catch (err) { onError(err); }
  }

  const display = held?.buffer ?? buffer.current;
  const slug = config.name.replace(/[^a-zA-Z0-9_-]+/g, '-').toLowerCase();
  const acquisition: ScopeAcquisition = config.acquisition ?? 'auto';
  return (
    <Scope buffer={display} version={held ? display.version : version}
      labels={held?.labels ?? labels} units={held?.units ?? units} captureKind={held?.kind ?? null}
      onHold={toggleHold} windowNs={config.windowNs} trigger={config.trigger ?? null}
      acquisition={acquisition} fixedTriggerTime={held?.triggerTime}
      onAcquisition={(mode) => { if (mode === 'auto' || config.trigger) update({ ...config, acquisition: mode }); }}
      onWindow={(ns) => update({ ...config, windowNs: ns })}
      onTrigger={(trigger: ScopeTrigger | null) => update({ ...config, trigger, acquisition: trigger ? acquisition : 'auto' })}
      onExport={() => download(`m33mu-${slug}.csv`, display.toCsv(['ch1', 'ch2']), 'text/csv')}
      onExportSigc={() => {
        try {
          download(`m33mu-${slug}.sigc`, display.toSigc(held?.units ?? units), 'application/octet-stream');
          onMessage('Exported voltage traces as SIGC v1 (ch1/ch2, millivolts, time relative to first sample).');
        } catch (err) { onError(err); }
      }}
      onImportSigc={(file) => void importSigc(file)} />
  );
}
