// Interactive bench canvas: board artwork, parts, wires, pan/zoom, wiring.

import { useMemo, useRef, useState, type PointerEvent as ReactPointerEvent, type WheelEvent } from 'react';
import { CATALOG, PITCH, type PartKind } from '../project/catalog.ts';
import { type Project, type TerminalRef, newId, nextPartId, refKey } from '../project/model.ts';
import { type BoardLayout, type PlacedTerminal, hitTerminal } from '../project/netgraph.ts';
import { PartGlyph } from './parts.tsx';

export type Selection =
  | { kind: 'part'; id: string }
  | { kind: 'wire'; id: string }
  | { kind: 'contact'; key: string }
  | null;

export type Tool = { kind: 'select' } | { kind: 'wire' } | { kind: 'place'; part: PartKind };

type Props = {
  project: Project;
  layout: BoardLayout | null;
  artworkUrl: string;
  terminals: PlacedTerminal[];
  selection: Selection;
  tool: Tool;
  running: boolean;
  brightness: Record<string, number>;
  pressed: Set<string>;
  errorKeys: Set<string>;
  onSelect(selection: Selection): void;
  onCommit(next: Project, label: string): void;
  onView(next: Project): void;
  onPress(switchId: string, pressed: boolean): void;
  onHover(text: string): void;
};

const WIRE_COLORS = ['#d7263d', '#1b998b', '#2e86de', '#f49d37', '#6c4ab6', '#333333'];

function snap(v: number): number {
  return Math.round(v / PITCH) * PITCH;
}

export function BenchCanvas(props: Props) {
  const { project, layout, terminals, selection, tool } = props;
  const svgRef = useRef<SVGSVGElement>(null);
  const { zoom, panX, panY } = project.ui;
  const [drag, setDrag] = useState<null | { kind: 'pan'; sx: number; sy: number; px: number; py: number }
    | { kind: 'part'; id: string; ox: number; oy: number; dx: number; dy: number; moved: boolean }>(null);
  const [pending, setPending] = useState<null | { from: PlacedTerminal; x: number; y: number }>(null);
  const [hover, setHover] = useState<PlacedTerminal | null>(null);
  const byKey = useMemo(() => new Map(terminals.map((t) => [t.key, t])), [terminals]);

  const world = (e: { clientX: number; clientY: number }) => {
    const r = svgRef.current!.getBoundingClientRect();
    return { x: (e.clientX - r.left - panX) / zoom, y: (e.clientY - r.top - panY) / zoom };
  };
  const hitRadius = Math.min(8 / zoom, 0.45 * PITCH);
  const wireable = (t: PlacedTerminal) => t.ref.kind === 'part' || t.enabled;
  const hit = (x: number, y: number, onlyWireable: boolean) =>
    hitTerminal(onlyWireable ? terminals.filter(wireable) : terminals, x, y, hitRadius);

  function onWheel(e: WheelEvent) {
    const r = svgRef.current!.getBoundingClientRect();
    const sx = e.clientX - r.left;
    const sy = e.clientY - r.top;
    const factor = Math.exp(-e.deltaY * 0.0015);
    const z = Math.min(8, Math.max(0.1, zoom * factor));
    const wx = (sx - panX) / zoom;
    const wy = (sy - panY) / zoom;
    props.onView({ ...project, ui: { zoom: z, panX: sx - wx * z, panY: sy - wy * z } });
  }

  function onPointerDown(e: ReactPointerEvent<SVGSVGElement>) {
    if (e.button !== 0) return;
    const p = world(e);
    if (tool.kind === 'wire') {
      const t = hit(p.x, p.y, true);
      if (t) {
        setPending({ from: t, x: p.x, y: p.y });
        svgRef.current!.setPointerCapture(e.pointerId);
        return;
      }
    } else if (tool.kind === 'place') {
      const id = nextPartId(project, tool.part);
      const part = { id, kind: tool.part, x: snap(p.x), y: snap(p.y), rotation: 0 as const,
        params: { ...CATALOG[tool.part].defaults } };
      props.onCommit({ ...project, parts: [...project.parts, part] }, `Place ${CATALOG[tool.part].title}`);
      props.onSelect({ kind: 'part', id });
      return;
    } else {
      const t = hit(p.x, p.y, false);
      if (t && t.ref.kind === 'board') {
        props.onSelect({ kind: 'contact', key: t.key });
        return;
      }
    }
    props.onSelect(null);
    setDrag({ kind: 'pan', sx: e.clientX, sy: e.clientY, px: panX, py: panY });
    svgRef.current!.setPointerCapture(e.pointerId);
  }

  function onPointerMove(e: ReactPointerEvent<SVGSVGElement>) {
    const p = world(e);
    if (pending) setPending({ ...pending, x: p.x, y: p.y });
    if (drag?.kind === 'pan') {
      props.onView({ ...project, ui: { zoom, panX: drag.px + e.clientX - drag.sx, panY: drag.py + e.clientY - drag.sy } });
      return;
    }
    if (drag?.kind === 'part') {
      const part = project.parts.find((x) => x.id === drag.id)!;
      const nx = snap(p.x - drag.ox);
      const ny = snap(p.y - drag.oy);
      setDrag({ ...drag, dx: nx - part.x, dy: ny - part.y, moved: drag.moved || nx !== part.x || ny !== part.y });
      return;
    }
    const t = hit(p.x, p.y, tool.kind === 'wire');
    if (t !== hover) {
      setHover(t);
      props.onHover(t ? describe(t) : '');
    }
  }

  function onPointerUp(e: ReactPointerEvent<SVGSVGElement>) {
    if (pending) {
      const p = world(e);
      const t = hit(p.x, p.y, true);
      if (t && t.key !== pending.from.key) {
        const exists = project.wires.some((w) =>
          (refKey(w.from) === pending.from.key && refKey(w.to) === t.key) ||
          (refKey(w.to) === pending.from.key && refKey(w.from) === t.key));
        if (!exists) {
          const wire = { id: newId('w'), from: pending.from.ref, to: t.ref,
            color: WIRE_COLORS[project.wires.length % WIRE_COLORS.length] };
          props.onCommit({ ...project, wires: [...project.wires, wire] }, 'Add wire');
          props.onSelect({ kind: 'wire', id: wire.id });
        }
      }
      setPending(null);
    }
    if (drag?.kind === 'part' && drag.moved) {
      props.onCommit({ ...project, parts: project.parts.map((x) => x.id === drag.id ?
        { ...x, x: x.x + drag.dx, y: x.y + drag.dy } : x) }, 'Move part');
    }
    setDrag(null);
  }

  function partDown(e: ReactPointerEvent, id: string) {
    if (tool.kind !== 'select' || e.button !== 0) return;
    e.stopPropagation();
    const part = project.parts.find((x) => x.id === id)!;
    if (props.running && part.kind === 'switch') {
      props.onPress(id, true);
      const up = () => { props.onPress(id, false); window.removeEventListener('pointerup', up); };
      window.addEventListener('pointerup', up);
      props.onSelect({ kind: 'part', id });
      return;
    }
    const p = world(e);
    props.onSelect({ kind: 'part', id });
    setDrag({ kind: 'part', id, ox: p.x - part.x, oy: p.y - part.y, dx: 0, dy: 0, moved: false });
    svgRef.current!.setPointerCapture(e.pointerId);
  }

  function wireDown(e: ReactPointerEvent, id: string) {
    if (tool.kind !== 'select' || e.button !== 0) return;
    e.stopPropagation();
    props.onSelect({ kind: 'wire', id });
  }

  const pos = (key: string) => {
    const t = byKey.get(key);
    if (!t) return null;
    if (drag?.kind === 'part' && t.ref.kind === 'part' && t.ref.part === drag.id) {
      return { x: t.x + drag.dx, y: t.y + drag.dy };
    }
    return { x: t.x, y: t.y };
  };

  const board = project.boards[0];
  const vb = layout?.pixelViewbox;
  const boardContacts = terminals.filter((t) => t.ref.kind === 'board');
  // Parts render below the breadboard-mounted parts: breadboards first.
  const ordered = [...project.parts].sort((a, b) => Number(b.kind === 'breadboard') - Number(a.kind === 'breadboard'));
  const cursor = tool.kind === 'wire' ? 'crosshair' : tool.kind === 'place' ? 'copy' : drag?.kind === 'pan' ? 'grabbing' : 'default';

  return (
    <svg ref={svgRef} className="bench-canvas" data-testid="bench-canvas" style={{ cursor }}
      onWheel={onWheel} onPointerDown={onPointerDown} onPointerMove={onPointerMove} onPointerUp={onPointerUp}
      role="application" aria-label="Bench canvas">
      <defs>
        <pattern id="grid" width={PITCH} height={PITCH} patternUnits="userSpaceOnUse">
          <circle cx={0} cy={0} r={1.2} className="grid-dot" />
        </pattern>
      </defs>
      <g transform={`translate(${panX} ${panY}) scale(${zoom})`}>
        <rect x={-5000} y={-5000} width={12000} height={12000} fill="url(#grid)" />
        {board && vb && (
          <g transform={`translate(${board.x} ${board.y})`}>
            <image href={props.artworkUrl} x={vb[0]} y={vb[1]} width={vb[2]} height={vb[3]} />
          </g>
        )}
        {boardContacts.map((t) => (
          <circle key={t.key} cx={t.x} cy={t.y} r={t.enabled ? 9 : 6} data-contact={t.ref.kind === 'board' ? t.ref.contact : ''}
            className={[
              'contact', t.enabled ? 'enabled' : 'visual-only',
              selection?.kind === 'contact' && selection.key === t.key ? 'selected' : '',
              props.errorKeys.has(t.key) ? 'error' : ''
            ].join(' ')} />
        ))}
        {ordered.map((part) => {
          const dx = drag?.kind === 'part' && drag.id === part.id ? drag.dx : 0;
          const dy = drag?.kind === 'part' && drag.id === part.id ? drag.dy : 0;
          const sel = selection?.kind === 'part' && selection.id === part.id;
          return (
            <g key={part.id} data-part={part.id}
              transform={`translate(${part.x + dx} ${part.y + dy}) rotate(${part.rotation})`}
              className={sel ? 'part selected' : 'part'} onPointerDown={(e) => partDown(e, part.id)}>
              <PartGlyph part={part} brightness={props.brightness[part.id]} pressed={props.pressed.has(part.id)} />
            </g>
          );
        })}
        {(() => {
          // Wires between the same two owners (module, part, or board
          // connector) travel together as a ribbon held by a zip tie.
          const owner = (r: TerminalRef) => r.kind === 'part' ? `p:${r.part}` : `b:${r.board}:${r.contact.split('-')[0]}`;
          const groups = new Map<string, string[]>();
          for (const w of project.wires) {
            const k = [owner(w.from), owner(w.to)].sort().join('|');
            groups.set(k, [...(groups.get(k) ?? []), w.id]);
          }
          const ties: { x: number; y: number; angle: number; key: string }[] = [];
          const drawn = project.wires.map((w) => {
            const a = pos(refKey(w.from));
            const b = pos(refKey(w.to));
            if (!a || !b) return null;
            const sel = selection?.kind === 'wire' && selection.id === w.id;
            const k = [owner(w.from), owner(w.to)].sort().join('|');
            const group = groups.get(k)!;
            // A jumper sags under its own weight: control points below the ends.
            const len = Math.hypot(b.x - a.x, b.y - a.y);
            const sag = Math.min(120, 20 + len * 0.25);
            const d = `M ${a.x} ${a.y} C ${a.x} ${a.y + sag} ${b.x} ${b.y + sag} ${b.x} ${b.y}`;
            if (group.length > 1 && group[0] === w.id) {
              const mx = 0.125 * a.x + 0.375 * a.x + 0.375 * b.x + 0.125 * b.x;
              const my = 0.125 * a.y + 0.375 * (a.y + sag) + 0.375 * (b.y + sag) + 0.125 * b.y;
              ties.push({ x: mx, y: my, angle: Math.atan2(b.y - a.y, b.x - a.x) * 180 / Math.PI, key: k });
            }
            return (
              <g key={w.id} data-wire={w.id} onPointerDown={(e) => wireDown(e, w.id)}>
                <path d={d} className="wire-hit" />
                <path d={d} className="wire-shadow" />
                <path d={d} className={sel ? 'wire selected' : 'wire'} stroke={w.color} />
                <path d={d} className="wire-shine" />
                <rect x={a.x - 6} y={a.y - 2} width={12} height={22} rx={2} className="dupont" />
                <rect x={b.x - 6} y={b.y - 2} width={12} height={22} rx={2} className="dupont" />
              </g>
            );
          });
          return (
            <>
              {drawn}
              {ties.map((t) => (
                <g key={`tie-${t.key}`} transform={`translate(${t.x} ${t.y}) rotate(${t.angle})`} className="zip-tie">
                  <rect x={-4} y={-30} width={8} height={60} rx={3} />
                </g>
              ))}
            </>
          );
        })()}
        {tool.kind === 'wire' && terminals.filter((t) => t.ref.kind === 'part').map((t) => (
          <circle key={`lead-${t.key}`} cx={t.x} cy={t.y} r={5} className="lead-target" />
        ))}
        {hover && <circle cx={hover.x} cy={hover.y} r={hitRadius} className="hover-ring" />}
        {pending && (
          <path d={`M ${pending.from.x} ${pending.from.y} L ${pending.x} ${pending.from.y} L ${pending.x} ${pending.y}`}
            className="wire pending" />
        )}
      </g>
    </svg>
  );
}

export function describe(t: PlacedTerminal): string {
  if (t.ref.kind === 'board') {
    return t.enabled ? `${t.label} — electrical contact` : `${t.ref.contact} — visual only (no verified mapping)`;
  }
  return t.label;
}

export type { TerminalRef };
