export type DiagnosticSeverity = 'error' | 'warning';
export type DiagnosticSource = 'worker' | 'solver' | 'capability' | 'workbench';

export type DiagnosticEntry = {
  source: DiagnosticSource;
  severity: DiagnosticSeverity;
  code: string;
  message: string;
  firstTimeNs: number | null;
  lastTimeNs: number | null;
  count: number;
};

export const DIAGNOSTIC_LIMIT = 100;

/** Keep distinct events in arrival order and fold repeats without unbounded growth. */
export function appendDiagnostic(entries: DiagnosticEntry[], event: Omit<DiagnosticEntry, 'count' | 'firstTimeNs' | 'lastTimeNs'> &
  { timeNs?: number | null }): DiagnosticEntry[] {
  const code = String(event.code).slice(0, 80);
  const message = String(event.message).slice(0, 1000);
  const timeNs = Number.isSafeInteger(event.timeNs) && Number(event.timeNs) >= 0 ? Number(event.timeNs) : null;
  const index = entries.findIndex((entry) => entry.source === event.source && entry.severity === event.severity &&
    entry.code === code && entry.message === message);
  const next = entries.filter((_, i) => i !== index);
  const previous = index < 0 ? null : entries[index];
  next.push({ source: event.source, severity: event.severity, code, message,
    firstTimeNs: previous?.firstTimeNs ?? timeNs, lastTimeNs: timeNs ?? previous?.lastTimeNs ?? null,
    count: (previous?.count ?? 0) + 1 });
  return next.slice(-DIAGNOSTIC_LIMIT);
}
