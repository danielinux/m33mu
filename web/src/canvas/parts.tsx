// SVG drawings for catalog parts, in part-local coordinates (rotation 0).

import { PITCH, BB_COLUMNS, CATALOG, formatSi } from '../project/catalog.ts';
import type { Part } from '../project/model.ts';

const P = PITCH;

function Leads({ part }: { part: Part }) {
  return (
    <>
      {CATALOG[part.kind].terminals(part.params).map((t) => (
        <circle key={t.id} cx={t.x} cy={t.y} r={3} className="lead" />
      ))}
    </>
  );
}

export function PartGlyph({ part, brightness, pressed }: { part: Part; brightness?: number; pressed?: boolean }) {
  switch (part.kind) {
  case 'breadboard': {
    const w = (BB_COLUMNS + 1) * P;
    return (
      <g className="breadboard">
        <rect x={0} y={-P * 0.7} width={w} height={P * 19.4} rx={8} className="bb-body" />
        <line x1={P * 0.5} x2={w - P * 0.5} y1={-P * 0.5} y2={-P * 0.5} className="rail-plus" />
        <line x1={P * 0.5} x2={w - P * 0.5} y1={P * 1.5} y2={P * 1.5} className="rail-minus" />
        <line x1={P * 0.5} x2={w - P * 0.5} y1={P * 15.5} y2={P * 15.5} className="rail-plus" />
        <line x1={P * 0.5} x2={w - P * 0.5} y1={P * 17.5} y2={P * 17.5} className="rail-minus" />
        <rect x={P * 0.5} y={P * 7.75} width={w - P} height={P * 1.5} className="bb-gap" />
        {CATALOG.breadboard.terminals({}).map((t) => (
          <rect key={t.id} x={t.x - 4} y={t.y - 4} width={8} height={8} className="bb-hole" />
        ))}
      </g>
    );
  }
  case 'resistor':
    return (
      <g>
        <line x1={0} y1={0} x2={4 * P} y2={0} className="wire-lead" />
        <rect x={P * 1.1} y={-9} width={P * 1.8} height={18} rx={6} className="resistor-body" />
        <text x={2 * P} y={-14} className="part-label">{formatSi(Number(part.params.ohms), 'Ω')}</text>
        <Leads part={part} />
      </g>
    );
  case 'capacitor':
    return (
      <g>
        <line x1={0} y1={0} x2={2 * P} y2={0} className="wire-lead" />
        <rect x={P * 0.55} y={-16} width={P * 0.9} height={20} rx={8} className="cap-body" />
        <text x={P} y={-20} className="part-label">{formatSi(Number(part.params.farads), 'F')}</text>
        <Leads part={part} />
      </g>
    );
  case 'led': {
    const color = part.params.color === 'green' ? '#2fd35a' : '#ff3b30';
    const b = Math.max(0, Math.min(1, brightness ?? 0));
    return (
      <g>
        {b > 0.01 && <circle cx={P / 2} cy={-14} r={18 + 14 * b} fill={color} opacity={0.15 + 0.5 * b} className="glow" />}
        <line x1={0} y1={0} x2={P} y2={0} className="wire-lead" />
        <circle cx={P / 2} cy={-14} r={11} fill={color} opacity={0.35 + 0.65 * b} stroke="#333" strokeWidth={1.5} />
        <text x={P / 2} y={-30} className="part-label">{part.id}</text>
        <Leads part={part} />
      </g>
    );
  }
  case 'diode':
    return (
      <g>
        <line x1={0} y1={0} x2={3 * P} y2={0} className="wire-lead" />
        <rect x={P * 0.9} y={-7} width={P * 1.2} height={14} rx={3} className="diode-body" />
        <rect x={P * 1.85} y={-7} width={4} height={14} fill="#111" />
        <Leads part={part} />
      </g>
    );
  case 'switch':
    return (
      <g>
        <rect x={-6} y={-P * 0.9} width={2 * P + 12} height={P * 0.8} rx={4} className="switch-body" />
        <circle cx={P} cy={-P * 0.5} r={P * 0.32} className={pressed ? 'switch-cap pressed' : 'switch-cap'} />
        <Leads part={part} />
      </g>
    );
  case 'npn': case 'nmos':
    return (
      <g>
        <path d={`M ${-6} -8 L ${2 * P + 6} -8 L ${2 * P + 6} -26 A ${P + 6} ${P + 6} 0 0 0 ${-6} -26 Z`}
          className={part.kind === 'npn' ? 'to92' : 'to92 mos'} />
        <text x={P} y={-14} className="part-label light">{part.kind === 'npn' ? 'NPN' : 'NMOS'}</text>
        <Leads part={part} />
      </g>
    );
  case 'opamp':
    return (
      <g>
        <path d={`M ${P * 0.45} ${-P * 0.55} L ${P * 3.55} ${P} L ${P * 0.45} ${P * 2.55} Z`}
          className="supply-body" />
        <text x={P * 0.8} y={P * 0.15} className="part-label light">+</text>
        <text x={P * 0.8} y={P * 2.05} className="part-label light">−</text>
        <text x={P * 2} y={P * 1.05} className="part-label light">{part.id}</text>
        <Leads part={part} />
      </g>
    );
  case 'i2c-eeprom': case 'i2c-temp': case 'spi-flash': case 'usb-uart': case 'sniffer': {
    const terms = CATALOG[part.kind].terminals(part.params);
    const w = (terms.length - 1) * P + P;
    const style = { 'i2c-eeprom': ['#1c5d99', 'EEPROM 24C02'], 'i2c-temp': ['#2d7d46', 'TMP102'],
      'spi-flash': ['#6b3fa0', 'W25Q FLASH'], 'usb-uart': ['#b23a48', 'USB-UART'],
      sniffer: ['#30343b', 'BUS SNIFFER'] }[part.kind];
    return (
      <g>
        {part.kind === 'usb-uart' && <rect x={w / 2 - P * 0.9 - P / 2} y={-P * 3.3} width={P * 1.8} height={P * 0.9} rx={3} fill="#c9ced6" stroke="#666" />}
        {part.kind === 'sniffer' && terms.slice(0, 4).map((t, i) => (
          <path key={t.id} d={`M ${t.x} -8 C ${t.x} -${P * 0.9} ${t.x + 6} -${P * 0.9} ${t.x + 3} -${P * 1.4}`}
            stroke={['#f4d35e', '#ee964b', '#4ea8de', '#90be6d'][i]} strokeWidth={4} fill="none" />
        ))}
        <rect x={-P / 2} y={-P * 2.6} width={w} height={P * 2.2} rx={5} fill={style[0]} stroke="#111" strokeWidth={1.5} />
        <rect x={w / 2 - P * 0.8 - P / 2} y={-P * 2.1} width={P * 1.6} height={P * 0.9} rx={2} fill="#151515" />
        <text x={w / 2 - P / 2} y={-P * 0.75} className="part-label light">{style[1]}</text>
        {terms.map((t) => (
          <text key={`l-${t.id}`} x={t.x} y={P * 0.75} className="pin-label">{t.label}</text>
        ))}
        <Leads part={part} />
      </g>
    );
  }
  case 'regulator':
    return (
      <g>
        <rect x={-10} y={-40} width={2 * P + 20} height={32} rx={3} className="to92" />
        <text x={P} y={-19} className="part-label light">{part.params.vset} V LDO</text>
        <Leads part={part} />
      </g>
    );
  case 'supply':
    return (
      <g>
        <rect x={-10} y={-34} width={P + 20} height={28} rx={4} className="supply-body" />
        <text x={P / 2} y={-15} className="part-label light">{part.params.volts} V</text>
        <text x={0} y={16} className="part-label">+</text>
        <text x={P} y={16} className="part-label">−</text>
        <Leads part={part} />
      </g>
    );
  case 'ground':
    return (
      <g>
        <line x1={0} y1={0} x2={0} y2={12} className="wire-lead" />
        <line x1={-12} y1={12} x2={12} y2={12} className="ground-sym" />
        <line x1={-8} y1={17} x2={8} y2={17} className="ground-sym" />
        <line x1={-4} y1={22} x2={4} y2={22} className="ground-sym" />
        <Leads part={part} />
      </g>
    );
  }
}
