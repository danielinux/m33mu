# Timer outputs and PWM

The STM32 general-purpose timers (TIM2 to TIM5, `cpu/stm32_timers.c`) drive
their channel outputs onto GPIO pads. The STM32H563, H533 and H5F4 route them
through the same AF table the input-capture path uses (`g_tim_pins` in each
part's `*_mmio.c`, from the datasheet alternate-function tables).

## What is modelled

- Output compare modes: frozen, active/inactive on match, toggle, force
  inactive/active, PWM mode 1 and 2 (OCxM[3:0] = 0..7). PWM1 is active while
  CNT < CCRx counting up and CNT <= CCRx counting down (RM0481 OCxM).
- CCxE enables the output; CCxP inverts it. With CCxE clear the timer stops
  driving the pin and the pad floats (high impedance).
- CCxIF is set on every compare match of an output channel; CCxIE raises the
  timer interrupt.
- A pin carries the timer output only while it is in AF mode with the AF that
  the table assigns to that channel. Any other AF, or GPIO output mode, keeps
  its own behaviour.

## Timing

Timers advance in batches of CPU cycles. Each output edge carries the cycle
inside the batch where the counter hit the update or compare value, so the
pad change lands at the exact virtual time regardless of batch size
(`mm_gpio_notify_pad_ahead()`; `stm32_timers_pwm_test` checks single-cycle,
7-cycle, 64-cycle and one-shot batching for the same edge times). The web bench
and `--signal-record` both use this time.

## Not modelled

- Preload: CCRx and ARR writes take effect immediately (OCxPE and ARPE are
  stored but not applied at the update event).
- Center-aligned counting (CMS), one-pulse retrigger and combined/asymmetric
  modes (OCxM[3] = 1), dead time, complementary outputs, break inputs.
- The advanced timers TIM1/TIM8, TIM15-TIM17 and LPTIM.
- STM32U585 and STM32L552 timers count but do not yet drive pads.

## In the workbench

A timer output pad is a driven pad in the analog circuit (push-pull or
open-drain as GPIO configures it). The bench also reports, per pad, the share
of time it drove high, which lights the on-board LEDs (`leds` in the board
layout) with PWM dimming. The example "H563 timer PWM" breathes LD1 with
TIM3_CH3 on PB0 and feeds a 1 kHz, 25 % TIM4_CH4 output on PD15 (CN7-18, D9)
into an RC filter; `scripts/test-web-pwm.mjs` checks the 1 ms period, the
250 us high time, the 0.825 V filtered mean and LD1's ramping duty.
