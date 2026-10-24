// Logic-level buses between the MCU's peripherals and simulated devices.
//
// Transfers are byte/transaction level: the emulator's I2C/SPI models call
// into this layer synchronously and UART bytes are routed per worker slice.
// Wiring decides who is reached: a peripheral uses the header pins whose GPIO
// alternate function selects it (STM32H563 DS14258 Rev 6, Tables 9-10), and
// a device answers only with power, ground and every bus line it needs
// connected. Line levels come from the analog circuit, so a missing pull-up
// or a line held low shows up as the fault a real bus would give.

export type LogicDeviceKind = 'i2c-eeprom' | 'i2c-temp' | 'spi-flash' | 'usb-uart' | 'sniffer';

/** A device part as compiled from the project: terminal name -> net name. */
export type LogicDeviceSpec = {
  id: string;
  kind: LogicDeviceKind;
  nets: Record<string, string>;
  params: Record<string, number | string>;
};

export type LogicNetlist = {
  devices: LogicDeviceSpec[];
  /** Wired MCU pads: SoC pad -> net. */
  pins: Record<string, string>;
};

/** Read-only view of the solved circuit and the MCU pad configuration. */
export interface LineView {
  /** Digital level of a net as an input would see it (null: unknown net). */
  level(net: string): 0 | 1 | null;
  volts(net: string): number;
  /** Current GPIO mode/AF of a pad, or null if never reported. */
  padConfig(socPad: string): { mode: number; af: number } | null;
  nowNs(): number;
}

export type BusRecord = {
  timeNs: number;
  bus: string;
  /** Nets carrying the transfer, for sniffer filtering. */
  nets: string[];
  summary: string;
  ok: boolean;
};

// Alternate functions of the header pads (DS14258 Rev 6 AF0-AF15 tables).
export const H563_AF: Record<string, Record<number, string>> = {
  PA5: { 5: 'SPI1_SCK' },
  PA6: { 5: 'SPI1_MISO' },
  PB5: { 5: 'SPI1_MOSI' },
  PG9: { 5: 'SPI1_MISO' },
  PD7: { 5: 'SPI1_MOSI', 7: 'USART2_CK' },
  PB6: { 4: 'I2C1_SCL', 7: 'USART1_TX', 8: 'LPUART1_TX' },
  PB7: { 4: 'I2C1_SDA', 7: 'USART1_RX', 8: 'LPUART1_RX' },
  PB8: { 4: 'I2C1_SCL' },
  PB9: { 4: 'I2C1_SDA' },
  PD5: { 7: 'USART2_TX' },
  PD6: { 7: 'USART2_RX' },
  PF0: { 4: 'I2C2_SDA' },
  PF1: { 4: 'I2C2_SCL' }
};

const GPIO_MODE_AF = 2;
const POWER_MIN_VOLTS = 1.6;
const hex = (b: number) => b.toString(16).padStart(2, '0');
const bytes = (d: ArrayLike<number>) => Array.from(d, hex).join(' ');

/** Device behaviour, independent of wiring. */
interface I2cTarget {
  address: number;
  write(data: Uint8Array): void;
  read(len: number): Uint8Array;
}
interface SpiTarget {
  select(): void;
  exchange(out: number): number;
  deselect(): void;
}

/** 24C02-style 256-byte EEPROM: first written byte sets the word address;
 * writes wrap within 8-byte pages; reads auto-increment. */
export class Eeprom24c02 implements I2cTarget {
  address: number;
  mem = new Uint8Array(256).fill(0xff);
  private pointer = 0;
  constructor(address: number) { this.address = address; }
  write(data: Uint8Array): void {
    if (data.length === 0) return;
    this.pointer = data[0];
    for (let i = 1; i < data.length; i++) {
      this.mem[this.pointer] = data[i];
      this.pointer = (this.pointer & 0xf8) | ((this.pointer + 1) & 0x07);
    }
  }
  read(len: number): Uint8Array {
    const out = new Uint8Array(len);
    for (let i = 0; i < len; i++) {
      out[i] = this.mem[this.pointer];
      this.pointer = (this.pointer + 1) & 0xff;
    }
    return out;
  }
}

/** TMP102-style temperature sensor: register 0 holds a 12-bit reading in
 * 1/16 degC, left aligned; the pointer selects the register. */
export class Tmp102 implements I2cTarget {
  address: number;
  celsius: number;
  private pointer = 0;
  constructor(address: number, celsius: number) { this.address = address; this.celsius = celsius; }
  write(data: Uint8Array): void { if (data.length) this.pointer = data[0] & 3; }
  read(len: number): Uint8Array {
    const raw = this.pointer === 0 ? (Math.round(this.celsius * 16) & 0xfff) << 4 :
      this.pointer === 1 ? 0x60a0 : 0;
    const out = new Uint8Array(len);
    for (let i = 0; i < len; i++) out[i] = i % 2 === 0 ? raw >> 8 : raw & 0xff;
    return out;
  }
}

/** W25Q-style serial NOR flash (64 KiB array, JEDEC EF 40 18). */
export class SpiFlash implements SpiTarget {
  mem = new Uint8Array(64 * 1024).fill(0xff);
  private cmd = -1;
  private addr = 0;
  private count = 0;
  private wel = false;
  private reply: number[] = [];
  select(): void { this.cmd = -1; this.count = 0; this.addr = 0; this.reply = []; }
  deselect(): void {
    if (this.cmd === 0x02 || this.cmd === 0x20) this.wel = false;
    this.cmd = -1;
  }
  exchange(out: number): number {
    if (this.cmd < 0) {
      this.cmd = out;
      this.count = 0;
      if (out === 0x06) this.wel = true;
      if (out === 0x04) this.wel = false;
      if (out === 0x9f) this.reply = [0xef, 0x40, 0x18];
      return 0xff;
    }
    this.count++;
    const addrPhase = this.cmd === 0x03 || this.cmd === 0x02 || this.cmd === 0x20;
    if (addrPhase && this.count <= 3) {
      this.addr = ((this.addr << 8) | out) & 0xffffff;
      if (this.cmd === 0x20 && this.count === 3 && this.wel) {
        const base = this.addr & ~0xfff & (this.mem.length - 1);
        this.mem.fill(0xff, base, base + 0x1000);
      }
      return 0xff;
    }
    switch (this.cmd) {
    case 0x9f: return this.reply.shift() ?? 0xff;
    case 0x05: return this.wel ? 0x02 : 0x00;
    case 0x03: {
      const v = this.mem[this.addr & (this.mem.length - 1)];
      this.addr++;
      return v;
    }
    case 0x02:
      if (this.wel) {
        const a = this.addr & (this.mem.length - 1);
        this.mem[a] &= out;
        this.addr = (this.addr & ~0xff) | ((this.addr + 1) & 0xff);
      }
      return 0xff;
    default: return 0xff;
    }
  }
}

type Device = {
  spec: LogicDeviceSpec;
  i2c?: I2cTarget;
  spi?: SpiTarget;
  selected?: boolean;
};

export class BusLayer {
  readonly netlist: LogicNetlist;
  readonly lines: LineView;
  readonly devices: Device[];
  readonly log: BusRecord[] = [];
  readonly logLimit = 5000;
  /** Bytes received by each USB-UART adapter, drained by the worker. */
  readonly adapterRx = new Map<string, number[]>();
  private spiBytes = new Map<number, { out: number[]; in: number[]; selected: string[]; nets: string[] }>();
  private padNet = new Map<string, string>();
  private busyState = new Map<number, boolean>();

  constructor(netlist: LogicNetlist, lines: LineView) {
    this.netlist = netlist;
    this.lines = lines;
    for (const [pad, net] of Object.entries(netlist.pins)) this.padNet.set(pad, net);
    this.devices = netlist.devices.map((spec) => {
      const d: Device = { spec };
      const addrPins = Number(spec.params.addressPins ?? 0) & 7;
      if (spec.kind === 'i2c-eeprom') d.i2c = new Eeprom24c02(0x50 | addrPins);
      if (spec.kind === 'i2c-temp') d.i2c = new Tmp102(0x48 | (addrPins & 3), Number(spec.params.celsius ?? 25));
      if (spec.kind === 'spi-flash') d.spi = new SpiFlash();
      if (spec.kind === 'usb-uart') this.adapterRx.set(spec.id, []);
      return d;
    });
  }

  private record(rec: Omit<BusRecord, 'timeNs'>): void {
    this.log.push({ timeNs: this.lines.nowNs(), ...rec });
    if (this.log.length > this.logLimit) this.log.splice(0, this.log.length - this.logLimit);
  }

  takeLog(): BusRecord[] {
    return this.log.splice(0, this.log.length);
  }

  /** Header pad currently carrying a peripheral signal, and its net. */
  private signalNet(signal: string): { pad: string; net: string | null } | null {
    for (const [pad, afs] of Object.entries(H563_AF)) {
      const cfg = this.lines.padConfig(pad);
      if (!cfg || cfg.mode !== GPIO_MODE_AF || afs[cfg.af] !== signal) continue;
      return { pad, net: this.padNet.get(pad) ?? null };
    }
    return null;
  }

  private powered(d: Device): boolean {
    const vcc = d.spec.nets.VCC;
    const gnd = d.spec.nets.GND;
    if (!vcc || !gnd) return false;
    return this.lines.volts(vcc) - this.lines.volts(gnd) >= POWER_MIN_VOLTS;
  }

  // ------------------------------------------------------------------ I2C

  private i2cLines(bus: number) {
    return { scl: this.signalNet(`I2C${bus}_SCL`), sda: this.signalNet(`I2C${bus}_SDA`) };
  }

  /** SCL or SDA held low while idle: the controller sees a busy bus. */
  i2cBusy(bus: number): boolean {
    const { scl, sda } = this.i2cLines(bus);
    if (!scl?.net || !sda?.net) return false;
    const sclLow = this.lines.level(scl.net) === 0;
    const busy = sclLow || this.lines.level(sda.net) === 0;
    // Firmware polls BUSY; record only when the state changes.
    if (busy !== (this.busyState.get(bus) ?? false)) {
      this.busyState.set(bus, busy);
      const low = sclLow ? `SCL (${scl.pad})` : `SDA (${sda.pad})`;
      this.record({ bus: `I2C${bus}`, nets: [scl.net, sda.net], ok: !busy,
        summary: busy ? `I2C${bus}: BUSY, ${low} low — missing pull-up or held low` : `I2C${bus}: bus released` });
    }
    return busy;
  }

  /** Returns 0 OK, 1 NACK, 2 BUSY, 3 arbitration lost, -1 not handled. */
  i2c(bus: number, addr: number, read: boolean, data: Uint8Array): number {
    const name = `I2C${bus}`;
    const { scl, sda } = this.i2cLines(bus);
    const what = `${name} 0x${hex(addr)} ${read ? 'R' : 'W'}`;
    if (!scl || !sda) {
      // Not routed to a header pin: nothing on the board is listening.
      return -1;
    }
    const nets = [scl.net, sda.net].filter((n): n is string => !!n);
    if (!scl.net || !sda.net) {
      this.record({ bus: name, nets, ok: false, summary: `${what}: NACK (${!scl.net ? scl.pad : sda.pad} not wired)` });
      return 1;
    }
    if (this.lines.level(scl.net) === 0 || this.lines.level(sda.net) === 0) {
      const low = this.lines.level(scl.net) === 0 ? `SCL (${scl.pad})` : `SDA (${sda.pad})`;
      this.record({ bus: name, nets, ok: false, summary: `${what}: BUSY, ${low} low — missing pull-up or held low` });
      return 2;
    }
    const onBus = this.devices.filter((d) => d.i2c && (d.spec.nets.SDA === sda.net || d.spec.nets.SCL === scl.net));
    const responders: Device[] = [];
    const notes: string[] = [];
    for (const d of onBus) {
      if (d.i2c!.address !== addr) continue;
      if (d.spec.nets.SDA !== sda.net || d.spec.nets.SCL !== scl.net) {
        notes.push(`${d.spec.id}: ${d.spec.nets.SDA !== sda.net ? 'SDA' : 'SCL'} not connected`);
        continue;
      }
      if (!this.powered(d)) { notes.push(`${d.spec.id}: unpowered`); continue; }
      responders.push(d);
    }
    const note = notes.length ? ` (${notes.join('; ')})` : '';
    if (responders.length === 0) {
      this.record({ bus: name, nets, ok: false, summary: `${what}: NACK${note}` });
      return 1;
    }
    if (read) {
      // Open-drain: several targets reading back AND their bits together.
      data.fill(0xff);
      for (const d of responders) {
        const r = d.i2c!.read(data.length);
        for (let i = 0; i < data.length; i++) data[i] &= r[i];
      }
    } else {
      for (const d of responders) d.i2c!.write(data);
    }
    const clash = responders.length > 1 ? ` — ${responders.length} devices answer at 0x${hex(addr)} (address clash)` : '';
    this.record({ bus: name, nets, ok: true, summary: `${what} [${bytes(data)}]${clash}${note}` });
    return 0;
  }

  // ------------------------------------------------------------------ SPI

  private spiPins(bus: number) {
    return {
      sck: this.signalNet(`SPI${bus}_SCK`),
      mosi: this.signalNet(`SPI${bus}_MOSI`),
      miso: this.signalNet(`SPI${bus}_MISO`)
    };
  }

  /** CS is any net: a device is selected when its CS net reads low. */
  private spiSelected(sckNet: string): Device[] {
    return this.devices.filter((d) => d.spi && d.spec.nets.SCK === sckNet && d.spec.nets.CS &&
      this.lines.level(d.spec.nets.CS) === 0 && this.powered(d));
  }

  /** Clock one byte; returns the byte read, or -1 if not handled. */
  spi(bus: number, out: number): number {
    const { sck, mosi, miso } = this.spiPins(bus);
    if (!sck) return -1;
    let t = this.spiBytes.get(bus);
    const selected = sck.net ? this.spiSelected(sck.net) : [];
    // Devices whose CS went high since the last byte ended their transaction.
    for (const d of this.devices) {
      if (!d.spi) continue;
      const now = selected.includes(d);
      if (now && !d.selected) d.spi.select();
      if (!now && d.selected) d.spi.deselect();
      d.selected = now;
    }
    if (!t) {
      t = { out: [], in: [], selected: selected.map((d) => d.spec.id),
        nets: [sck.net, mosi?.net, miso?.net, ...selected.map((d) => d.spec.nets.CS)].filter((n): n is string => !!n) };
      this.spiBytes.set(bus, t);
    }
    let result = 0xff;
    const answers: number[] = [];
    for (const d of selected) {
      const heard = mosi?.net && d.spec.nets.MOSI === mosi.net ? out : 0xff;
      const reply = d.spi!.exchange(heard);
      if (miso?.net && d.spec.nets.MISO === miso.net) answers.push(reply);
    }
    if (answers.length > 0) {
      result = answers.reduce((a, b) => a & b, 0xff);
    } else if (miso?.net) {
      result = this.lines.level(miso.net) === 0 ? 0x00 : 0xff;
    }
    t.out.push(out);
    t.in.push(result);
    return result;
  }

  spiEnd(bus: number): void {
    const t = this.spiBytes.get(bus);
    this.spiBytes.delete(bus);
    if (!t) return;
    const who = t.selected.length === 0 ? 'no device selected' : t.selected.length > 1 ?
      `${t.selected.join(' + ')} selected together (bus contention)` : t.selected[0];
    this.record({ bus: `SPI${bus}`, nets: t.nets, ok: t.selected.length === 1,
      summary: `SPI${bus} → ${who}: MOSI [${bytes(t.out)}] MISO [${bytes(t.in)}]` });
  }

  /** A pad drive changed (for example a GPIO chip select): close transfers of
   * devices whose CS is no longer low. */
  onPadChange(): void {
    for (const [bus, t] of [...this.spiBytes]) {
      const { sck } = this.spiPins(bus);
      const still = sck?.net ? this.spiSelected(sck.net).map((d) => d.spec.id) : [];
      if (t.selected.some((id) => !still.includes(id))) this.spiEnd(bus);
    }
    for (const d of this.devices) {
      if (!d.spi || !d.selected) continue;
      const cs = d.spec.nets.CS;
      if (!cs || this.lines.level(cs) !== 0) { d.spi.deselect(); d.selected = false; }
    }
  }

  // ------------------------------------------------------------------ UART

  /** Route bytes a UART transmitted. Returns UART RX deliveries for the
   * emulator; adapter bytes collect in adapterRx. */
  uartTx(label: string, data: Uint8Array): { label: string; data: Uint8Array }[] {
    const tx = this.signalNet(`${label}_TX`);
    if (!tx?.net || data.length === 0) return [];
    const out: { label: string; data: Uint8Array }[] = [];
    for (const d of this.devices) {
      if (d.spec.kind === 'usb-uart' && d.spec.nets.RX === tx.net) {
        this.adapterRx.get(d.spec.id)!.push(...data);
      }
    }
    for (const [pad, afs] of Object.entries(H563_AF)) {
      const cfg = this.lines.padConfig(pad);
      if (!cfg || cfg.mode !== GPIO_MODE_AF || this.padNet.get(pad) !== tx.net) continue;
      const signal = afs[cfg.af];
      if (signal?.endsWith('_RX')) out.push({ label: signal.slice(0, -3), data });
    }
    this.record({ bus: label, nets: [tx.net], ok: true,
      summary: `${label} TX ${JSON.stringify(new TextDecoder().decode(data)).slice(0, 80)} [${data.length} B]` });
    return out;
  }

  /** Bytes typed into a USB-UART adapter: MCU UARTs whose RX is on the
   * adapter's TX net receive them. */
  adapterTx(id: string, data: Uint8Array): { label: string; data: Uint8Array }[] {
    const d = this.devices.find((x) => x.spec.id === id && x.spec.kind === 'usb-uart');
    const net = d?.spec.nets.TX;
    if (!net) return [];
    const out: { label: string; data: Uint8Array }[] = [];
    for (const [pad, afs] of Object.entries(H563_AF)) {
      const cfg = this.lines.padConfig(pad);
      if (!cfg || cfg.mode !== GPIO_MODE_AF || this.padNet.get(pad) !== net) continue;
      const signal = afs[cfg.af];
      if (signal?.endsWith('_RX')) out.push({ label: signal.slice(0, -3), data });
    }
    for (const other of this.devices) {
      if (other !== d && other.spec.kind === 'usb-uart' && other.spec.nets.RX === net) {
        this.adapterRx.get(other.spec.id)!.push(...data);
      }
    }
    this.record({ bus: id, nets: [net], ok: out.length > 0,
      summary: `${id} TX ${JSON.stringify(new TextDecoder().decode(data)).slice(0, 80)}${out.length ? '' : ' (no receiver wired)'}` });
    return out;
  }
}
