# Signal Recording: GPIO outputs

Injection lets CI drive what a firmware image reads. Recording is the mirror
image: a timestamped log of the levels the firmware drives on selected output
pins. With both, a release image can be checked end to end: given this
stimulus, did it drive these outputs at these times?

Supported targets: the same as injection, see
[signal-injection-gpio.md](signal-injection-gpio.md).

## Usage

```sh
build/m33mu --cpu stm32h5f4 \
    --signal-file=stimulus.sigc \
    --signal-bind:trace=vin:target=PA5:role=gpio \
    --signal-sync=signal_trigger_marker \
    --signal-record=out.csv \
    --signal-watch:target=PB0 \
    --signal-watch:target=PB1 \
    firmware.elf
```

- `--signal-record=<path>`: write the recording to `path` as CSV. Requires at
  least one `--signal-watch`. If the file cannot be written, m33mu exits
  with 1.
- `--signal-watch:target=<PXn>`: record pin `PXn`. Can be repeated, up to 32
  pins, each once. Same pin syntax and target checks as `--signal-bind`. A pin
  may be both bound and watched.
- `--signal-sync` keeps its meaning and also works without `--signal-file`,
  so an output-only test can align time zero with a firmware marker. If the
  PC never reaches it, m33mu exits with 1, as for injection.

## Output format

```
t_ns,pin,level
-1250000,PB0,Z
-1250000,PB1,Z
-830000,PB0,0
0,PB1,1
4000,PB0,1
```

- One row per change of a watched pin, in time order. Rows with equal `t_ns`
  keep the order in which the writes happened.
- The file starts with one row per watched pin giving its reset level.
- `level` is `0`, `1` or `Z`:
  - Output mode, push-pull: the ODR bit.
  - Output mode, open-drain: `0` when ODR is 0, `Z` when ODR is 1.
  - Input, analog or AF mode: `Z`. The firmware is not driving the pad
    through ODR. AF outputs (timer PWM, USART TX) are not modelled at the pad.
- A pin that is injected and in input mode reports `Z`: the recording shows
  only what the firmware drives.
- After a SoC reset the pins return to analog mode, which shows up as `Z`
  rows.

## Time axis

`t_ns` comes from the replay clock in `src/signal.c`, the same one that
schedules injected crossings, so recorded and injected events share one axis.
The clock follows CPU clock changes.

- With `--signal-sync`: `t_ns` counts from the trigger. Changes before it
  have negative times; they are held in memory until the trigger fires. If it
  never fires, they are written at exit with times counted from the first
  instruction, and m33mu prints a warning.
- Without `--signal-sync`: `t_ns` counts from the first instruction.

**Granularity.** The replay clock advances after each translation block or
single step. A GPIO write in the middle of a block is therefore stamped with
the cycle count at the start of that block: it is early by at most one block.
This is the same granularity as injection and as `DWT_CYCCNT` reads. With
`M33MU_DISABLE_TB=1` every instruction is its own step and the stamps are
exact. Assertion windows must allow for one block plus the timing-fidelity
limit described in the injection doc.

**Wall-clock timeout.** `--timeout` ends the process from a signal handler
with exit code 127, so the CSV can be cut short. Every other exit path writes
it in full.

## Implementation

- `include/m33mu/gpio.h`: `mm_gpio_set_pad_listener` and
  `mm_gpio_notify_pad`, the mirror of `mm_gpio_set_external_input_writer`.
  Levels are 0, 1 or `MM_GPIO_LEVEL_Z`.
- `cpu/stm32_gpio.c`: keeps the last reported pad state per bank. After
  every register write it recomputes the driven pins and notifies each pin
  that changed; `stm32_gpio_reset` notifies every pin. No file I/O and no
  time. Nothing is computed while no listener is set.
- `src/signal.c`: `mm_signal_now_ns` and `mm_signal_trigger_ns` expose the
  replay clock.
- `src/signal_record.c`: owns the watch list and the CSV file, drops repeated
  levels, holds rows until the trigger fires, and writes the rest at exit.
- `src/main.c`: the two flags, the per-target pin checks, and closing the
  file at exit.

## Not covered

- AF and analog outputs (PWM duty, DAC). PWM could later be recorded from the
  timer model.
- `--record` and reverse stepping: the CSV is append-only. Stepping backwards
  under `--gdb` does not remove rows.
- Writing `.sigc` traces. CSV first; a `.sigc` writer can follow once B2 adds
  a shared encoder.

## Tests

- `tests/stm32_gpio_signal_test.c`: pad events for ODR, BSRR and BRR writes,
  no event when the level does not change, open-drain, mode changes, reset.
- `tests/signal_record_test.c`: CSV rows with and without a trigger, negative
  times before it, rows held until close, an unreached trigger, watch-list
  limits.
- `tests/firmware/test-stm32h5f4-signal-record`: toggles PB0 on each SysTick
  wrap after the marker and drives PB1 open-drain. CI records both, and
  `scripts/check_signal_record.py` checks the level sequences and that the
  PB0 changes are one SysTick period apart, within one microsecond.

## Decisions

1. The level column has `0`, `1` and `Z`, rather than skipping rows while a
   pin is not driven.
2. The file starts with one reset-level row per watched pin.
3. Block granularity is accepted for v1. Exact stamps would need the TB
   runner to publish the current op index, one store per op on the hot path.
4. The flags are `--signal-record` and `--signal-watch:target=`, separate
   from `--signal-bind`.
