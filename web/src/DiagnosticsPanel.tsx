import { formatSi } from './project/catalog.ts';
import type { Diagnostic as CircuitDiagnostic } from './project/netgraph.ts';
import type { DiagnosticEntry } from './diagnostics.ts';

type Props = {
  circuit: CircuitDiagnostic[];
  runtime: DiagnosticEntry[];
  metrics: { rejectedSteps: number; rollbacks: number; sampleOverflow: number } | null;
  onClear(): void;
  onExport(): void;
};

export function DiagnosticsPanel({ circuit, runtime, metrics, onClear, onExport }: Props) {
  const errors = circuit.filter((entry) => entry.severity === 'error').length +
    runtime.filter((entry) => entry.severity === 'error').length;
  return (
    <section className="diagnostics" aria-label="Diagnostics" data-testid="diagnostics">
      <div className="diagnostics-heading">
        <h3>Diagnostics</h3>
        <span>{errors} errors · {circuit.length + runtime.length - errors} warnings</span>
      </div>
      {metrics && <p className="diagnostics-metrics" data-testid="diagnostic-metrics">
        Solver: {metrics.rejectedSteps} rejected steps · {metrics.rollbacks} rollbacks · {metrics.sampleOverflow} dropped samples
      </p>}
      <div className="diagnostics-actions">
        <button type="button" onClick={onExport}>Export report</button>
        <button type="button" onClick={onClear} disabled={runtime.length === 0}>Clear history</button>
      </div>
      {circuit.length > 0 && <>
        <h4>Current circuit</h4>
        <ul>{circuit.slice(0, 50).map((entry, i) =>
          <li key={`${entry.message}-${i}`} className={entry.severity}>
            <strong>{entry.severity}</strong> {entry.message}
            {entry.refs?.length ? <small>{entry.refs.join(', ')}</small> : null}
          </li>)}</ul>
        {circuit.length > 50 && <p>{circuit.length - 50} more circuit issues</p>}
      </>}
      <h4>Runtime history</h4>
      {runtime.length === 0 ? <p className="muted">No runtime diagnostics.</p> :
        <ol>{[...runtime].reverse().map((entry, i) =>
          <li key={`${entry.source}-${entry.code}-${entry.message}-${i}`} className={entry.severity}>
            <strong>{entry.code}</strong> <small>{entry.source}{entry.lastTimeNs === null ? '' :
              ` · ${formatSi(entry.lastTimeNs * 1e-9, 's')}`}{entry.count > 1 ? ` · ×${entry.count}` : ''}</small>
            <span>{entry.message}</span>
          </li>)}</ol>}
    </section>
  );
}
