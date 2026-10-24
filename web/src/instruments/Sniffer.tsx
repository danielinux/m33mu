// Protocol sniffer: decoded bus transactions on the nets its clips touch.

import { useEffect, useRef } from 'react';
import { formatSi } from '../project/catalog.ts';
import type { BusRecord } from '../../sim/buses.ts';

type Props = {
  id: string;
  /** Clip name -> net it is attached to. */
  clips: Record<string, string>;
  records: BusRecord[];
  onClear(): void;
};

export function sniffed(records: BusRecord[], clips: Record<string, string>): BusRecord[] {
  const nets = new Set(Object.entries(clips).filter(([k]) => k !== 'GND').map(([, n]) => n));
  return records.filter((r) => r.nets.some((n) => nets.has(n)));
}

export function Sniffer({ id, clips, records, onClear }: Props) {
  const list = useRef<HTMLDivElement>(null);
  const shown = sniffed(records, clips);
  useEffect(() => {
    const el = list.current;
    if (el) el.scrollTop = el.scrollHeight;
  }, [shown.length]);
  const attached = Object.entries(clips).filter(([k]) => k !== 'GND');
  const csv = () => {
    const rows = ['time_ns,bus,ok,summary', ...shown.map((r) =>
      `${r.timeNs},${r.bus},${r.ok},"${r.summary.replaceAll('"', '""')}"`)];
    const url = URL.createObjectURL(new Blob([rows.join('\n') + '\n'], { type: 'text/csv' }));
    const a = document.createElement('a');
    a.href = url;
    a.download = `m33mu-${id}.csv`;
    a.click();
    URL.revokeObjectURL(url);
  };
  return (
    <section className="sniffer instrument" aria-label={`${id} sniffer`} data-testid={`sniffer-${id}`}>
      <header>
        <span className="brand-plate">m33·SNIFF</span>
        <strong>{id}</strong>
        <span className="muted">{attached.length ? attached.map(([k, n]) => `${k}→${n}`).join('  ') : 'clip CH1–CH4 onto bus wires'}</span>
        <span className="spacer" />
        <button type="button" onClick={csv} disabled={shown.length === 0}>Export CSV</button>
        <button type="button" onClick={onClear}>Clear</button>
      </header>
      <div className="sniff-list" ref={list}>
        {shown.length === 0 ? <p className="muted">No traffic on the clipped lines yet.</p> : shown.map((r, i) => (
          <div key={i} className={r.ok ? 'sniff-row' : 'sniff-row bad'}>
            <span className="t">{formatSi(r.timeNs * 1e-9, 's')}</span>
            <span className="bus">{r.bus}</span>
            <span className="msg">{r.summary}</span>
          </div>
        ))}
      </div>
    </section>
  );
}
