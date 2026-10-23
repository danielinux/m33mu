// Two-channel scope over bench sample batches.

import { useEffect, useRef, useState } from 'react';
import { formatSi } from '../project/catalog.ts';

export const SCOPE_CAPACITY = 400_000;

/** Ring buffer of [time ns, ch1, ch2] rows. */
export class ScopeBuffer {
  readonly t = new Float64Array(SCOPE_CAPACITY);
  readonly v = [new Float64Array(SCOPE_CAPACITY), new Float64Array(SCOPE_CAPACITY)];
  start = 0;
  length = 0;
  dropped = 0;
  version = 0;

  clear(): void {
    this.start = 0; this.length = 0; this.dropped = 0; this.version++;
  }

  /** `flat` rows are [t, probe0, probe1, ...]; `map` gives the probe column per channel. */
  append(flat: Float64Array, width: number, map: (number | null)[]): void {
    for (let i = 0; i + width <= flat.length; i += width) {
      let idx: number;
      if (this.length < SCOPE_CAPACITY) {
        idx = (this.start + this.length) % SCOPE_CAPACITY;
        this.length++;
      } else {
        idx = this.start;
        this.start = (this.start + 1) % SCOPE_CAPACITY;
        this.dropped++;
      }
      this.t[idx] = flat[i];
      for (let c = 0; c < 2; c++) {
        const col = map[c];
        this.v[c][idx] = col === null ? NaN : flat[i + 1 + col];
      }
    }
    this.version++;
  }

  at(k: number): number { return (this.start + k) % SCOPE_CAPACITY; }

  lastTime(): number { return this.length ? this.t[this.at(this.length - 1)] : 0; }

  /** Index of the first row with time >= tNs. */
  lowerBound(tNs: number): number {
    let lo = 0;
    let hi = this.length;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (this.t[this.at(mid)] < tNs) lo = mid + 1; else hi = mid;
    }
    return lo;
  }

  valueAt(c: number, tNs: number): number {
    const k = this.lowerBound(tNs);
    if (k >= this.length) return NaN;
    return this.v[c][this.at(k)];
  }

  toCsv(labels: string[]): string {
    const rows = [`time_ns,${labels.join(',')}`];
    for (let k = 0; k < this.length; k++) {
      const i = this.at(k);
      rows.push(`${this.t[i]},${this.v[0][i]},${this.v[1][i]}`);
    }
    return rows.join('\n') + '\n';
  }
}

type Props = {
  buffer: ScopeBuffer;
  version: number;
  labels: [string, string];
  units: [string, string];
  windowNs: number;
  onWindow(ns: number): void;
  onExport(): void;
};

const COLORS = ['#e8a33d', '#3dc1e8'];
const WINDOWS = [10_000, 50_000, 200_000, 1_000_000, 5_000_000, 20_000_000, 50_000_000, 100_000_000, 1_000_000_000, 5_000_000_000];

export function Scope({ buffer, version, labels, units, windowNs, onWindow, onExport }: Props) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const [cursors, setCursors] = useState<[number | null, number | null]>([null, null]);
  const end = buffer.lastTime();
  const begin = Math.max(0, end - windowNs);

  useEffect(() => {
    const el = canvas.current;
    if (!el) return;
    const dpr = window.devicePixelRatio || 1;
    const w = el.clientWidth;
    const h = el.clientHeight;
    el.width = Math.round(w * dpr);
    el.height = Math.round(h * dpr);
    const g = el.getContext('2d')!;
    g.scale(dpr, dpr);
    g.fillStyle = getComputedStyle(el).getPropertyValue('--scope-bg') || '#101418';
    g.fillRect(0, 0, w, h);
    g.strokeStyle = 'rgba(255,255,255,0.08)';
    g.fillStyle = 'rgba(255,255,255,0.35)';
    g.font = '10px system-ui, sans-serif';
    g.fillText(`${formatSi(windowNs / 10 * 1e-9, 's')}/div`, w - 70, h - 6);
    for (let i = 1; i < 10; i++) {
      g.beginPath(); g.moveTo((w * i) / 10, 0); g.lineTo((w * i) / 10, h); g.stroke();
    }
    for (let i = 1; i < 8; i++) {
      g.beginPath(); g.moveTo(0, (h * i) / 8); g.lineTo(w, (h * i) / 8); g.stroke();
    }
    const first = buffer.lowerBound(begin);
    for (let c = 0; c < 2; c++) {
      let lo = Infinity;
      let hi = -Infinity;
      for (let k = first; k < buffer.length; k++) {
        const v = buffer.v[c][buffer.at(k)];
        if (Number.isFinite(v)) { lo = Math.min(lo, v); hi = Math.max(hi, v); }
      }
      if (!Number.isFinite(lo)) continue;
      if (hi - lo < 1e-9) { hi += 0.5; lo -= 0.5; }
      const pad = (hi - lo) * 0.08;
      lo -= pad; hi += pad;
      const yOf = (v: number) => h - ((v - lo) / (hi - lo)) * h;
      // Min/max per pixel column keeps narrow pulses visible.
      g.strokeStyle = COLORS[c];
      g.lineWidth = 1.5;
      g.beginPath();
      let col = -1;
      let cmin = 0;
      let cmax = 0;
      const flush = () => {
        if (col < 0) return;
        g.moveTo(col + 0.5, yOf(cmin));
        g.lineTo(col + 0.5, yOf(cmax) - 0.5);
      };
      let prev: [number, number] | null = null;
      for (let k = first; k < buffer.length; k++) {
        const i = buffer.at(k);
        const x = ((buffer.t[i] - begin) / windowNs) * w;
        const v = buffer.v[c][i];
        if (!Number.isFinite(v)) continue;
        const px = Math.floor(x);
        if (px !== col) {
          flush();
          if (prev) { g.moveTo(prev[0], yOf(prev[1])); g.lineTo(x, yOf(v)); }
          col = px; cmin = v; cmax = v;
        } else {
          cmin = Math.min(cmin, v); cmax = Math.max(cmax, v);
        }
        prev = [x, v];
      }
      flush();
      g.stroke();
      g.fillStyle = COLORS[c];
      g.font = '11px system-ui, sans-serif';
      const latest = buffer.length ? buffer.v[c][buffer.at(buffer.length - 1)] : NaN;
      g.fillText(`${labels[c]}  ${formatSi(latest, units[c])}   (scale ${formatSi((hi - lo) / 8, units[c])}/div)`, 6, 14 + c * 14);
    }
    cursors.forEach((t, i) => {
      if (t === null || t < begin || t > begin + windowNs) return;
      const x = ((t - begin) / windowNs) * w;
      g.strokeStyle = i === 0 ? '#ffffff' : '#ff6bd6';
      g.setLineDash([4, 3]);
      g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h); g.stroke();
      g.setLineDash([]);
    });
  }, [version, windowNs, cursors, labels, units, begin, buffer]);

  function click(e: React.MouseEvent<HTMLCanvasElement>) {
    const r = e.currentTarget.getBoundingClientRect();
    const t = begin + ((e.clientX - r.left) / r.width) * windowNs;
    setCursors(e.shiftKey ? [cursors[0], t] : [t, cursors[1]]);
  }

  const readout = cursors.map((t) => t === null ? null :
    [buffer.valueAt(0, t), buffer.valueAt(1, t)] as const);
  return (
    <section className="scope" aria-label="Oscilloscope">
      <header>
        <strong>Scope</strong>
        <label>Window <select value={windowNs} onChange={(e) => onWindow(Number(e.target.value))}>
          {[...new Set([...WINDOWS, windowNs])].sort((a, b) => a - b).map((w) =>
            <option key={w} value={w}>{formatSi(w * 1e-9, 's')}</option>)}
        </select></label>
        <button type="button" onClick={onExport} disabled={buffer.length === 0}>Export CSV</button>
        {buffer.dropped > 0 && <span className="warning">{buffer.dropped} oldest samples discarded</span>}
      </header>
      <canvas ref={canvas} onClick={click} data-testid="scope" title="Click: cursor A · Shift-click: cursor B" />
      <footer>
        {cursors.map((t, i) => t === null ? null : (
          <span key={i}>{i ? 'B' : 'A'} {formatSi(t * 1e-9, 's')}: {labels[0]} {formatSi(readout[i]![0], units[0])}, {labels[1]} {formatSi(readout[i]![1], units[1])}</span>
        ))}
        {cursors[0] !== null && cursors[1] !== null && <span>Δt {formatSi((cursors[1]! - cursors[0]!) * 1e-9, 's')}</span>}
      </footer>
    </section>
  );
}
