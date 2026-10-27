# Signal Injection: ADC

m33mu can convert the voltage of an injected pin into ADC codes, on the same
time axis as GPIO injection, so one capture can drive digital inputs and analog
channels together. This document covers the ADC path only; the `.sigc`
container, `--signal-bind`, `--signal-sync` and the replay clock are described
in [signal-injection-gpio.md](signal-injection-gpio.md). Unmodified
STM32CubeH5 HAL code for the regular group (init, calibration, polled and
interrupt-driven conversions, stop) runs unchanged.

Supported targets: `stm32h5f4` (ADC1/2/3), `stm32h563` and `stm32h533`
(ADC1/2), through the shared model `cpu/stm32_adc.c` and the H5 glue
`cpu/stm32h5_adc.c`. Sources: RM0517 Rev 1 (H5F4), RM0481 Rev 5 (H563/H533),
DS14972 Rev 1, DS14258 Rev 6 and the STM32H533 datasheet.

## Usage

```sh
build/m33mu --cpu stm32h5f4 \
    --signal-file=stimulus.sigc \
    --signal-bind:trace=ain0:target=PA0 \
    --signal-sync=signal_trigger_marker \
    --adc-vref-mv=3300 --adc-noise-lsb=1 --adc-seed=0x1234 \
    firmware.elf
```

- A bound pin in analog mode feeds every ADC channel routed to it.
- `--adc-vref-mv=<mV>`: full scale, decimal 1..5500 (default `--vdd-mv`).
- `--adc-noise-lsb=<n>`: uniform ±n LSB before the clamp, decimal 0..4095 (default 0).
- `--adc-seed=<s>`: noise PRNG seed, decimal or `0x` hex (default 0).
- Any `--adc-*` flag on a CPU without the ADC hooks exits with 1.

## Design decisions

| Decision | Reason |
|---|---|
| ADC1 and ADC2 share one block at 0x4202 8000 (ADC2 at +0x100, common registers at +0x300); ADC3 (H5F4 only, RM0481 Table 282 note 1) is a single-instance block at 0x4202 D800 with its own common registers; secure aliases at +0x1000 0000; IRQs 37, 69 and 149 | HAL reads the other instance's `CR` before it writes `CCR`, so both must be mapped |
| Software start, polled or with `EOSMP`/`EOC`/`EOS`/`OVR` interrupts, regular scan of `SQR1.L` + 1 channels, continuous mode | What the HAL regular-group API needs |
| `ADCAL` clears 1335 ADC clocks after it is set (DS14972 Table 97); `ADRDY` sets 15 ADC clocks after `ADEN` | HAL polls both flags with timeouts |
| `CKMODE` 01/10/11 runs at HCLK/1, /2, /4; `CKMODE` 00 at HCLK/`PRESC` while `RCC_CCIPR5.ADCDACSEL` = 000 (rcc_hclk, its reset value) | Both give an exact ratio to CPU cycles; other kernel clocks would need the PLL2/HSE/CSI models |
| Other `ADCDACSEL` values and reserved `PRESC` codes warn once and run at HCLK/1; f_ADC outside 1.5–75 MHz warns once | The firmware keeps running, and the log says why timing differs |
| The pin is sampled when `EOSMP` sets, at the end of the sampling time; `EOC` follows after the resolution time | Matches silicon |
| `code = clamp(round(mV · (2^res − 1) / vref), 0, 2^res − 1)`, halves up, then offset and alignment | Ideal and saturating, so tests assert exact codes |
| Noise is a seeded PRNG, re-seeded at each reset; ADC3 uses the seed XOR 0xAD3 | A reboot replays the same codes |
| The settings live in `src/signal.c`, and every ADC model reads them at init | No per-SoC code for the CLI |
| The channel table maps (instance, channel) to a pin | ADC1 and ADC2 route some channels to different pins |
| Only analog-mode pins (`MODER` = 11) see the trace; an unbound pin, a channel without a pin and a non-analog pin convert 0 mV | Catches firmware that forgets analog mode |
| `RCC_AHB2ENR.ADCEN` (bit 10) and `ADC3EN` (bit 24) gate the blocks, from either RCC bank; with `GTZC1_TZSC_SECCFGR3.ADC12SEC` (bit 14) or `ADC3SEC` (bit 6) set, Non-secure accesses read 0 and writes are ignored | Same rules as `stm32_gpio.c` and the timers |
| A debugger `peek` of `DR` does not pop the FIFO; `save`/`load` hooks for `mmio_bus_save` (regions `ADC12` and `ADC3`); no undo hooks | Debugger reads stay side-effect free; `--record` does not rewind ADC state |
| Configurations the model ignores warn once: `EXTEN`, `DMAEN`, `JADSTART`, `AUTDLY`, `DISCEN`, `ROVSE`, `DIFSEL`, internal channels | Firmware that depends on them would otherwise fail silently |

## Register model

Every register HAL writes reads back; reset values follow RM0517 Table 303
(`CR` = 0x2000 0000, `CFGR` = 0x8000 0000). `CSR` mirrors the `ISR` of the
block's instances.

- **`CR`**, written by LL with `MODIFY_REG(CR, ADC_CR_BITS_PROPERTY_RS, bit)`:
  - `ADEN`, `ADCAL`, `ADSTART`, `ADSTP`, `ADDIS` are set-only, accepted in the
    states RM0517 §30.4 allows, and never restarted by rewriting a bit that is
    already set. `ADEN` is refused for 4 ADC clocks after `ADCAL` clears.
  - `ADSTP` aborts at once and keeps the FIFO; `ADDIS` empties it.
  - `ADVREGEN`, `DEEPPWD`, `ADCALDIF` are plain bits (`DEEPPWD = 1` clears
    `ADVREGEN`); `JADSTART` and `JADSTP` read 0.
- **`ISR`/`IER`/IRQ**: all flags are write-1-to-clear. The line is level-like:
  after any `ISR` update and any `IER` write, it pends while `ISR & IER` is
  non-zero. While the handler is active the request is held, not pended: if
  the flags are still set when it returns, the line pends at the next tick, so
  a `DR` read mid-handler does not tail-chain into an empty entry. The model
  never clears a pend.
- **Sequence and FIFO**: `SMPR1/2` and `SQR1..4` set times and order. With
  `OVRMOD = 0`, results queue in a three-stage FIFO, `EOC` stays set while it
  is not empty, and a result that finds it full sets `OVR` and is lost. With
  `OVRMOD = 1`, each result overwrites `DR` and `OVR` sets if `EOC` was still
  set. Writing 1 to `EOC` releases the queue; `DR` keeps its value.
- **Offset and alignment**: `OFR1..4` with the lowest matching index, `OFFSET`
  on the 12-bit scale (RM0517 Table 294), `SATEN` and `OFFSETPOS`; alignment
  and sign extension follow RM0517 Figures 270–273.
- `CKMODE` and `PRESC` are locked while any instance of the block is enabled.
  `RES`, `ALIGN` and the offset are latched at each slot start, `CONT` at `ADSTART`.

## Timing

- Delays count in half ADC clocks: `cycles = ceil(half_clocks · div / 2)`, with
  `div` from `CKMODE`/`PRESC`, latched at `ADCAL` and `ADEN`.
- Sampling time `SMPx` = 2.5 … 640.5 ADC clocks (`SMPPLUS` turns 2.5 into
  3.5); resolution time 12.5 / 10.5 / 8.5 / 6.5 clocks (RM0517 Table 293).
  Continuous mode accumulates slot times from the first start, without drift.
- The ADC advances at the end of each executed batch, so a start written in a
  batch takes effect at its end; flags are never early.

## Signal API

```c
mm_bool mm_signal_sample_mv(int bank, int pin, mm_u64 cycles_ahead, mm_i32 *mv_out);
```

The zero-order-hold value of the trace bound to the pin, `cycles_ahead` cycles
after the replay clock (`mm_signal_ns_ahead()`). The ADC tick runs before
`mm_signal_advance()` for the same batch, so the clock still stands at the
batch start. Before the trigger it returns sample 0; after the end it holds the
last sample; an unbound pin returns `MM_FALSE` and 0 mV.

An embedder that solves pin voltages itself installs a source with
`mm_signal_set_analog_source()`; it is asked first, and pins it does not drive
fall back to the bound traces. The web bridge uses it for
`mm_web_set_analog_input()` (WASM `mm_web_abi_analog_input`, ABI 4): a DC
level per pad that the host updates between run slices.

## Integration

- `mm_target_cfg` has four optional hooks: `adc_init`, `adc_reset`, `adc_tick`
  and `adc_cycles_until_event`. The H5 parts fill them through
  `<SOC>_ADC_INIT/_RESET/_TICK/_CYCLES_UNTIL_EVENT` in `cpu/<soc>/cpu_config.h`;
  every other `cpu_db.c` entry leaves them 0.
- `src/timer.c` calls them after the timer hooks: `mm_timer_reset()`,
  `mm_timer_init()` and `mm_timer_tick()`, so every time advance that reaches
  `mm_timer_tick()` also ticks the ADC. `mm_timer_adc_cycles_until_event()` bounds
  the idle fast-forward in `mm_execution_state_cycles_until_wake()`, so an
  `EOC` interrupt wakes WFI on time.
- `cpu/stm32h5_adc.c` has two init entry points, `stm32h5_adc_init` (ADC1/2)
  and `stm32h5_adc_init_with_adc3` (H5F4). Before the first init, reset and
  tick do nothing; each block has its own one-time warnings. An idle tick
  checks the held IRQ flags and the cached deadline.

## H5 channel table

Positive inputs from the datasheets and STM32_open_pin_data; `ADC12 INn` is
channel n on both ADC1 and ADC2. ADC1/2 are the same on all three parts; ADC3
is H5F4 only.

| Pin | Channel | Pin | Channel | Pin | Channel |
|---|---|---|---|---|---|
| PA0 | ADC12 IN0 | PB0 | ADC12 IN9 | PE7 | ADC3 IN1 |
| PA1 | ADC12 IN1 | PB1 | ADC12 IN5 | PE8 | ADC3 IN2 |
| PA2 | ADC12 IN14 | PC0 | ADC12 IN10 | PE9 | ADC3 IN3 |
| PA3 | ADC12 IN15 | PC1 | ADC12 IN11 | PE10 | ADC3 IN4 |
| PA4 | ADC12 IN18 | PC2 | ADC12 IN12 | PE11 | ADC3 IN5 |
| PA5 | ADC12 IN19 | PC3 | ADC12 IN13 | PE12 | ADC3 IN6 |
| PA6 | ADC12 IN3 | PC4 | ADC12 IN4 | PE13 | ADC3 IN7 |
| PA7 | ADC12 IN7 | PC5 | ADC12 IN8 | PE14 | ADC3 IN8 |
| PF11 | ADC1 IN2 | PF13 | ADC2 IN2 | PE15 | ADC3 IN9 |
| PF12 | ADC1 IN6 | PF14 | ADC2 IN6 | PB2 | ADC3 IN0 |

Internal channels (ADC1/2 IN16/IN17, RM0517 Table 290 and RM0481 Table 282;
ADC3 IN10 = OPAMP1 and IN14–IN19) convert 0 mV with a one-time warning. ADC3
IN11–IN13 have no pin. Profiles use the largest package; smaller H563/H533
packages lack some of these pins (PF11–PF14, PC0–PC5).

## Not implemented

- Hardware triggers (`EXTEN`/`EXTSEL`): conversions start on `ADSTART`.
- DMA requests, the injected group, discontinuous mode, `AUTDLY`, oversampling,
  analog watchdogs, dual mode (`CCR.DUAL` is stored only).
- Differential inputs (a `DIFSEL` channel converts single-ended), internal
  channels and their calibration values, OPAMP and DAC outputs.
- Kernel clocks other than HCLK, `ADC_OR.OP0`, `ADCRST`, the TZSC privilege
  bits, and undo hooks for `--record`.

## Tests

- `tests/stm32_adc_test.c`: the `CR` state machine with HAL write patterns,
  timing per `CKMODE`/`PRESC`, `SMPx` and resolution, quantizer, noise, scan,
  FIFO and overrun, offsets and alignment, IRQ hold in the handler, the one-time
  warnings and a snapshot round trip.
- `tests/stm32h5f4_adc_test.c` and `tests/stm32h5_adc_test.c`: the H5 glue
  through `mm_timer_*` and the `cpu_db.c` hooks: RCC gating, aliases, TZSC,
  channel tables, IRQ lines, `ADCDACSEL`, the idle bound, a reboot into
  another part, and snapshots of both blocks.
- `tests/signal_test.c`, `tests/signal_adc_settings_test.c`: `mm_signal_sample_mv`
  and the settings.
- `tests/firmware/test-stm32*-signal-adc*` (command lines in
  `.github/workflows/test-firmware.yml`): polled, scan with IRQ, WFI,
  continuous, stop/disable, vref, mode guard and ADC3 on H5F4; polled and scan
  on H563 and H533. They assert exact codes on flat trace segments.
