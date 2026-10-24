// Serial terminal for one emulated UART.

import { useEffect, useRef, useState } from 'react';

export type UartInfo = { index: number; label: string; base: number };

/** Terminal scrollback bound, in characters. */
export const TERMINAL_LIMIT = 200_000;

export function appendTerminal(old: string, bytes: Uint8Array): string {
  let text = old;
  for (const b of bytes) {
    if (b === 0x0d) continue; // CR: lines end on LF
    if (b === 0x08) { text = text.slice(0, -1); continue; }
    text += b === 0x0a || b === 0x09 || (b >= 0x20 && b < 0x7f) ? String.fromCharCode(b) : '·';
  }
  return text.length > TERMINAL_LIMIT ? text.slice(text.length - TERMINAL_LIMIT) : text;
}

type Props = {
  uart: UartInfo;
  text: string;
  lost: boolean;
  /** Green-on-black CRT styling. */
  crt: boolean;
  onSend(text: string): void;
  onClear(): void;
};

export function Terminal({ uart, text, lost, crt, onSend, onClear }: Props) {
  const [line, setLine] = useState('');
  const [ending, setEnding] = useState<'\r' | '\n' | '\r\n' | ''>('\r');
  const out = useRef<HTMLPreElement>(null);
  useEffect(() => {
    const el = out.current;
    if (el) el.scrollTop = el.scrollHeight;
  }, [text]);
  return (
    <section className={crt ? 'terminal crt' : 'terminal'} aria-label={`${uart.label} terminal`} data-testid={`terminal-${uart.label}`}>
      <header>
        <strong>{uart.label}</strong>
        <span className="muted">0x{uart.base.toString(16).padStart(8, '0')}</span>
        {lost && <span className="warning">output overflowed: some bytes were lost</span>}
        <span className="spacer" />
        <button type="button" onClick={onClear}>Clear</button>
      </header>
      <div className="tube"><pre ref={out}>{text}<span className="cursor">█</span></pre></div>
      <form onSubmit={(e) => { e.preventDefault(); onSend(line + ending); setLine(''); }}>
        <input value={line} onChange={(e) => setLine(e.target.value)} placeholder={`Send to ${uart.label}`}
          aria-label={`Send to ${uart.label}`} />
        <select value={ending} onChange={(e) => setEnding(e.target.value as typeof ending)} aria-label="Line ending">
          <option value={'\r'}>CR</option>
          <option value={'\n'}>LF</option>
          <option value={'\r\n'}>CR+LF</option>
          <option value="">none</option>
        </select>
        <button type="submit">Send</button>
      </form>
    </section>
  );
}
