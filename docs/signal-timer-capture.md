# Timer input capture from injected signals

Many applications read external signals through a timer's capture channel
rather than EXTI: frequency and period measurement, pulse width, PWM input.
On the STM32H5 targets, an injected edge on a pin whose alternate function is
a TIM2..TIM5 channel reaches that timer, and `cpu/stm32_timers.c` models the
capture channels and the slave reset mode that PWM input needs.

## What is modelled

- Timers: TIM2 to TIM5, the ones `stm32_timers.c` models, on the STM32H5
  family (the targets with signal injection).
- Channels 1 to 4 in input capture mode:
  - `CCMRx.CCxS`: `01` (TIx, direct) and `10` (the paired input, indirect:
    TI2 for channel 1, TI1 for channel 2, and so on). `11` (TRC) is not
    modelled.
  - `CCMRx.ICxPSC`: capture on every 1st, 2nd, 4th or 8th edge.
  - `CCER.CCxE`, and `CCxP`/`CCxNP` for rising, falling or both edges.
  - On a capture: `CCRx` latches the counter, `SR.CCxIF` is set, `SR.CCxOF`
    is set if `CCxIF` was still set, and `DIER.CCxIE` raises the timer IRQ.
    Reading `CCRx` clears `CCxIF`, as on silicon (not on a debugger peek).
    `CCRx` is read-only while its channel captures.
  - `EGR.CCxG` forces a capture from software, `EGR.TG` sets `TIF`.
- Slave mode controller, reset mode only (`SMCR.SMS = 100`, `TS = TI1FP1`
  or `TI2FP2`, edge chosen by `CC1P`/`CC2P`): the trigger edge restarts the
  counter and prescaler after the capture and sets `TIF` (`TIE` raises the
  IRQ) and, unless `URS` or `UDIS` is set, `UIF`. With channel 1 capturing
  TI1 rising and channel 2 TI1 falling, this is PWM input mode: `CCR1` is the
  period and `CCR2` the pulse width.
- Not modelled: input filter (`ICxF`, injected crossings are already
  hysteresis-clean), output compare and PWM, encoder mode, other slave modes,
  DMA requests, TIM1/TIM8/TIM15-17, U5 and L5.

## Routing an injected edge to a timer

The H5 injection writer in `cpu/stm32h5_mmio.c` compares the pad's previous
injected level with the new one. On a change, and unless the pin is analog,
it looks up the pin and its current AFR value in the variant's table and
forwards the edge to that timer's TIx input. Edges are pad transitions while
AFR selects the timer: selecting the AF while the pad is high is not an edge.

Each variant (STM32H563, H533, H5F4) has its own table, generated from the
datasheet's alternate function table: DS14258 Rev 6, DS14539 Rev 3 and
DS14972 Rev 1, Table 15 each (TIM2 is always AF1, TIM3 to TIM5 AF2). A pin
present on several parts has the same function on all of them. The DS14972
table covers the whole H5F family, so some of its PJ/PK pins are not bonded
out on H5F4 packages.

## Exact capture value

The run loop ticks the timers before it advances signal replay, so when an
edge arrives the counter already stands at the end of the block, and the
crossing happened some cycles before that. The signal core passes that delay
along:

```c
mm_bool mm_gpio_set_external_input(int bank, int pin, mm_bool level, mm_u64 late_cycles);
```

The timer counts the ticks its prescaler produced during `late_cycles` and
latches the counter value of that moment, wrapping through ARR in either
direction. A counter reset by the slave controller is back-dated the same
way, so the next period starts at the edge, not at the end of the block. The
captured value is exact to one timer tick.

## Clock

The timer counts CPU cycles divided by `PSC + 1`. On silicon the timer
kernel clock is PCLK1 (times 2 if the APB1 prescaler is not 1); the model
matches while firmware leaves the APB1 prescaler at 1.

## Tests

- `tests/stm32_timers_capture_test.c`: direct and indirect capture, each
  polarity, ICxPSC, CCxOF, the IRQ, EGR.CCxG, CCRx read-only in capture mode,
  peek reads, back-dating with a prescaler, across an ARR wrap and when
  counting down, PWM input, slave reset with URS.
- `tests/signal_test.c`: the delay reported with each injected edge.
- `tests/firmware/test-stm32h5f4-signal-timer-capture`: TIM2 channel 1 on PA5
  (AF1) measures an injected 100 us square wave (`fixture_square.sigc`),
  first capture to capture, then in PWM input mode, and passes when period
  and pulse width are within one tick of 6400 and 3200.

## Decisions

1. Slave reset mode is in this step, so PWM input mode works.
2. `mm_gpio_set_external_input` gains `late_cycles`, and captures and slave
   resets are back-dated to the crossing.
3. The pin table is complete for each H5 variant (H563, H533, H5F4).
