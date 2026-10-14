# Signal Injection: GPIO

m33mu can replay a recorded voltage-vs-time trace onto an emulated pin, so
firmware that reads GPIO inputs (polled or through EXTI) can be tested in CI
against real-world stimuli. This document covers the GPIO input path and the
shared signal core it is built on. The ADC path is a later phase, described
in [signal-injection-adc.md](signal-injection-adc.md).

Supported target: STM32H5 (`stm32h563`, `stm32h533`, `stm32h5f4`), through the
shared `cpu/stm32_gpio.c` model.

## Usage

```sh
build/m33mu --cpu stm32h5f4 \
    --vdd_mv=3300 \
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
- `--vdd_mv=<mV>`: supply voltage used to derive the input thresholds
  (default 3300).

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

`tests/firmware/_signal_common/gen_fixtures.py` writes this format and can
serve as a reference encoder. There is no HDF5 converter yet.

## Design decisions

| Decision | Reason |
|---|---|
| Traces are converted offline into a flat binary container, not read from HDF5 at run time | Keeps the emulator free of heavy format dependencies; replay only needs `(t, v)` pairs |
| Zero-order hold between samples | A sharp edge in the source stays sharp; matches a signal generator connected to a pin |
| Timestamps in nanoseconds | Same unit as the emulator's own pacing; independent of the core clock |
| One master trigger starts every bound trace at `t = 0` | Timing does not depend on boot-code length, and traces bound to different peripherals stay aligned, like a scope's external trigger |
| The trigger is a PC watch, not a breakpoint | No memory patching, no halt, no firmware change: one compare per dispatch |
| The trigger is one-shot per boot | One run is one deterministic scenario. A system reset re-arms it |
| GPIO crossings are precomputed at bind time | No per-cycle voltage comparison; replay only checks the next scheduled crossing |
| Schmitt thresholds at `VDD/2 ± 125 mV` | STM32H5 datasheet typical hysteresis is 250 mV. Voltages inside the band do not change the input state |
| Both edge directions are always forwarded | Edge selection belongs to the firmware (`EXTI_RTSR1`/`EXTI_FTSR1`) and can change at run time; the existing EXTI model gates on it and on `EXTI_IMR1` |
| Injection writes `IDR` and calls the bank's EXTI hook | Polled reads and interrupts see the same transition through the existing paths |
| Only pins in Input mode accept an injected level | Output and AF pins are driven by the MCU. Analog pins have the Schmitt trigger disabled, so `IDR` reads 0 for them |

## Timing model

Replay time is derived from the emulator's virtual cycle count (one
instruction is one cycle) and the target's current core clock. It is not
derived from the host clock. `mm_signal_check_trigger()` runs before each
dispatch with the PC about to execute. `mm_signal_advance()` runs after
execution with the number of cycles executed. Both must be called from every
execution path in `src/main.c` that advances `mm_timer_tick()`.

## Constraints

- **The trigger must be reached.** Translation blocks end before the trigger
  PC, so any instruction address works. A dedicated
  `__attribute__((noinline, used))` marker function is the most robust
  choice, because a HAL function can be inlined away.
- **Dispatch granularity.** On the fast path, crossings are applied after each
  translation block, and pending interrupts are taken when a block chain
  breaks (at most 512 blocks). Tests should wait for an interrupt with a
  generous budget, not assert exact interrupt latency.
- **Edges coalesce.** EXTI pending and NVIC pending are single bits, so
  crossings that arrive faster than the ISR can service them merge into one
  interrupt, as on hardware. Edge-counting fixtures space their crossings
  about 20 µs apart.
- **Pending bits on a masked line are not modelled precisely.** Tests assert
  only that a masked line's ISR never runs.
- **Limits:** 16 timebases, 32 traces, 32 bindings, and 4096 crossings per
  GPIO binding.

## Not implemented

- HDF5 to `.sigc` converter.
- A trigger based on a register write, for firmware without symbols.
- GPDMA transfers triggered by EXTI.
- Recording output pins. Firmware can read `ODR` itself.

## Tests

- `tests/signal_test.c`: loader, binding, trigger, hysteresis and re-arm,
  against a fake GPIO writer.
- `tests/stm32_gpio_signal_test.c`: the input-mode guard and Analog-mode
  `IDR` masking.
- `tests/firmware/test-stm32h5f4-signal-*`: end-to-end firmware. Each test
  ends with `bkpt #0x7f` on pass and `bkpt #0x7e` on fail. They share their
  startup code, linker script, fixtures and `signal_test.h` from
  `tests/firmware/_signal_common/`. The command lines are in
  `.github/workflows/test-firmware.yml`.

| Test | Checks |
|---|---|
| `gpio-basic` | Polled `IDR` follows a rising crossing |
| `input-exti-rising` / `-falling` / `-both` | EXTI edge selection: 2, 2 and 4 interrupts from 4 crossings |
| `input-exti-masked` | A masked line never runs its ISR |
| `input-hysteresis` | Noise inside the band raises no interrupt |
| `input-sharp-edge` | A crossing 16 ns after the trigger is visible within a few polls |
| `input-nonuniform` | Explicit timestamps |
| `input-mode-guard` | An output pin ignores the trace, then follows it again once back in input mode |
| `trigger-oneshot` | A second pass through the trigger PC does not restart replay |
| `trigger-midblock` | A trigger on a fall-through instruction, not a branch target, starts replay |
| `group-sync` | Two bindings in one group change state together |
| `output-loopback` / `output-timing` | `ODR`→`IDR` mirroring and cycle accounting are unaffected by the signal hooks |
