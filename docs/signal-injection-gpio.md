# Signal Injection: GPIO

m33mu can replay a recorded voltage-vs-time trace onto an emulated pin, so
firmware that reads GPIO inputs (polled or through EXTI) can be tested in CI
against real-world stimuli. This document covers the GPIO input path and the
shared signal core it is built on. The ADC path is a later phase, described
in [signal-injection-adc.md](signal-injection-adc.md).

Supported target: STM32H5 (`stm32h563`, `stm32h533`, `stm32h5f4`), through the
shared `cpu/stm32_gpio.c` model. On other targets, or for a pin the target
does not have, `--signal-bind` is an error.

## Usage

```sh
build/m33mu --cpu stm32h5f4 \
    --vdd-mv=3300 \
    --signal-file=stimulus.sigc \
    --signal-bind:trace=vin:target=PA5:role=gpio \
    --signal-sync=signal_trigger_marker \
    firmware.elf
```

- `--signal-file=<path>`: the `.sigc` container holding the traces.
- `--signal-bind:trace=<name>:target=<PXn>:role=gpio`: drive pin
  `PXn` from the named trace. Can be repeated, up to 32 bindings.
- `--signal-sync=<symbol|address>`: the PC that starts replay, required with
  `--signal-bind`. A symbol is looked up in the ELF image's symbol table,
  which requires the image to be an ELF and m33mu to be built with libelf. A
  numeric address works with any image format. The Thumb bit is ignored. If
  the PC never reaches it, m33mu prints an error and exits with 1 (only a
  warning under `--gdb` or `--tui`).
- `--vdd-mv=<mV>`: supply voltage used to derive the input thresholds
  (default 3300).
- `--gpio-vil-mv=<mV>`, `--gpio-vih-mv=<mV>`: input thresholds of every bound
  pin. A pin reads low once the trace falls to VIL or below, and high once it
  rises to VIH or above. Each flag overrides one side only: a side that is not
  given keeps its default of `VDD/2 - 125 mV` (VIL) or `VDD/2 + 125 mV` (VIH).
  The effective VIL must be below the effective VIH, otherwise m33mu exits
  with 1. Use them to test slow edges at the datasheet limits, for example
  `--gpio-vil-mv=990 --gpio-vih-mv=2310` at 3.3 V.

## Container format

A `.sigc` file holds several timebases and traces. All fields are
little-endian:

```
header    char magic[4] = "SIGC"; u32 version; u32 num_timebases; u32 num_traces
timebase  u32 id; u32 sample_count; u32 period_ns
          if period_ns == 0: u64 timestamps_ns[sample_count]
trace     u32 id; char name[32]; u32 timebase_id; i32 full_scale_mv
          i16 samples_mv[sample_count of its timebase]
```

All timebases come first, then all traces. Each trace references a timebase
by id. Traces that share a timebase are sample-aligned, as in a simultaneous
multi-channel capture. `period_ns != 0` is a uniform timebase, and
`period_ns == 0` means explicit timestamps. `full_scale_mv` is not used by
GPIO bindings.

The loader accepts only `version == 1`. It rejects duplicate timebase ids,
duplicate trace ids or names, explicit timestamps that go backwards, a file
shorter than its counts claim, and trailing bytes after the last trace.

`tests/firmware/_signal_common/gen_fixtures.py` writes this format and can
serve as a reference encoder. There is no HDF5 converter yet.

## Design decisions

| Decision | Reason |
|---|---|
| Traces are converted offline into a flat binary container, not read from HDF5 at run time | Keeps the emulator free of heavy format dependencies; replay only needs `(t, v)` pairs |
| Zero-order hold between samples | A sharp edge in the source stays sharp; matches a signal generator connected to a pin |
| Timestamps in nanoseconds | Same unit as the emulator's own pacing; independent of the core clock |
| Before the trigger, a bound pin holds the level of the trace's first sample | Like a signal generator that is already connected at power-on. An idle-high line reads high from reset, and its first falling crossing is a real edge |
| One master trigger starts every bound trace at `t = 0` | Timing does not depend on boot-code length, and traces bound to different peripherals stay aligned, like a scope's external trigger |
| The trigger is a PC watch, not a breakpoint | No memory patching, no halt, no firmware change: one compare per dispatch |
| The trigger is one-shot per emulator run, and a system reset does not rewind replay | The stimulus models the outside world, which a watchdog or software reset does not restart. Bound pins get their current level again after the reset |
| GPIO crossings are precomputed at bind time | No per-cycle voltage comparison; replay only checks the next scheduled crossing |
| Default Schmitt thresholds at `VDD/2 ± 125 mV`, overridable with `--gpio-vil-mv`/`--gpio-vih-mv` | A model choice inside the datasheet band. The STM32H5 datasheets (DS14258 Rev 6 Table 58, and the same rows in the H533, H523 and H5Fxxx datasheets) guarantee only VIL max = 0.3·VDDIO and VIH min = 0.7·VDDIO (CMOS levels, tested in production), with wider by-design limits of 0.4·VDDIO − 0.1 V and 0.52·VDDIO + 0.18 V. The 250 mV hysteresis is a typical value, not tested. The real switching point can lie anywhere in the band, which matters for slow edges. Voltages between VIL and VIH do not change the input state |
| Both edge directions are always forwarded | Edge selection belongs to the firmware (`EXTI_RTSR1`/`EXTI_FTSR1`) and can change at run time; the existing EXTI model gates on it and on `EXTI_IMR1` |
| Injection writes `IDR` and calls the bank's EXTI hook | Polled reads and interrupts see the same transition through the existing paths |
| The GPIO model keeps the pad level of every injected pin, and only Input pins show it in `IDR` | Output and AF pins are driven by the MCU, so their `IDR` bit reads back `ODR`. Analog pins have the Schmitt trigger disabled, so `IDR` reads 0 for them. A pin switched back to Input reads the pad at once, as on silicon |

## Timing model

Replay time is derived from the emulator's virtual cycle count (one
instruction is one cycle) and the target's core clock. It is not derived from
the host clock. A clock change only affects the cycles that follow it, so
time already elapsed is not rescaled when the firmware switches PLLs. `mm_signal_check_trigger()` runs before each
dispatch with the PC about to execute. `mm_signal_advance()` runs after
execution with the number of cycles executed. Both must be called from every
execution path in `src/main.c` that advances `mm_timer_tick()`, including the
WFI/WFE idle paths. Idle fast-forwarding stops at
`mm_signal_cycles_until_next_event()`, so an injected edge wakes a sleeping
core at its scheduled time, with or without SysTick. Under `--gdb` or
`--tui`, no cycles advance while the core is halted, so replay pauses with it
and resumes where it left off; this is intended and keeps runs deterministic.

## Constraints

- **The trigger must be reached.** Translation blocks end before the trigger
  PC, so any instruction address works. A dedicated
  `__attribute__((noinline, used))` marker function is the most robust
  choice, because a HAL function can be inlined away.
- **Dispatch granularity.** On the fast path, crossings are applied after each
  translation block, and the block chain is left as soon as an interrupt can
  be taken, so latency is at most one translation block. Tests should wait
  for an interrupt with a generous budget, not assert exact interrupt
  latency.
- **Timing fidelity.** One instruction is one cycle and flash has no wait
  states, so firmware runs faster than silicon relative to the trace. Timing
  derived from timers or SysTick stays consistent with the trace; busy-wait
  delays and software debounce counters do not. Keep assertion windows loose.
- **Edges coalesce.** EXTI pending and NVIC pending are single bits, so
  crossings that arrive faster than the ISR can service them merge into one
  interrupt, as on hardware. Edge-counting fixtures space their crossings
  about 20 µs apart.
- **Pending bits on a masked line are not modelled precisely.** Tests assert
  only that a masked line's ISR never runs.
- **Limits:** 16 timebases, 32 traces and 32 bindings. The number of
  crossings per binding is limited only by memory.

## Not implemented

- HDF5 to `.sigc` converter.
- A trigger based on a register write, for firmware without symbols.
- GPDMA transfers triggered by EXTI.
- Recording output pins. Firmware can read `ODR` itself.
- Snapshots and reverse stepping (`--record`). Neither the replay position nor
  the GPIO pad levels are saved, so stepping back across an injected edge does
  not undo it.

## Tests

- `tests/signal_test.c`: loader, binding, trigger, hysteresis, configurable
  thresholds and reset, against a fake GPIO writer.
- `tests/stm32_gpio_signal_test.c`: the input-mode guard, Analog-mode `IDR`
  masking, the pad level kept across mode changes and `ODR` writes, and
  `ODR` read-back once an injected pin is an output.
- `tests/firmware/test-stm32h5f4-signal-*`: end-to-end firmware. Each test
  ends with `bkpt #0x7f` on pass and `bkpt #0x7e` on fail. They share their
  startup code, linker script, fixtures and `signal_test.h` from
  `tests/firmware/_signal_common/`. The command lines are in
  `.github/workflows/test-firmware.yml`.

| Test | Checks |
|---|---|
| `gpio-basic` | Polled `IDR` follows a rising crossing |
| `input-exti-rising` / `-falling` / `-both` | EXTI edge selection: 2, 2 and 4 interrupts from 4 crossings |
| `input-exti-wfi` | With SysTick off, injected edges wake the core from WFI |
| `input-exti-masked` | A masked line never runs its ISR |
| `input-hysteresis` | Noise inside the band raises no interrupt |
| `input-sharp-edge` | A crossing 16 ns after the trigger is visible within a few polls |
| `input-nonuniform` | Explicit timestamps |
| `input-idle-high` | An idle-high trace reads high before the trigger and raises one falling-edge interrupt |
| `input-mode-guard` | An output pin ignores the trace, and reads the pad level as soon as it is back in input mode |
| `trigger-oneshot` | A second pass through the trigger PC does not restart replay |
| `reset-keeps-running` | After a system reset the pin reads the level the trace has reached |
| `trigger-midblock` | A trigger on a fall-through instruction, not a branch target, starts replay |
| `multi-binding` | Two bindings started by the one trigger change state together |
| `output-loopback` / `output-timing` | `ODR`→`IDR` mirroring and cycle accounting are unaffected by the signal hooks |
