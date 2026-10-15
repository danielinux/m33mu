# GPIO pull resistors on STM32

An input pin that nothing drives reads the level of its internal pull
resistor, as on silicon. Firmware with an active-low button and the internal
pull-up therefore sees the button released from boot. This applies to every
STM32 target built on `cpu/stm32_gpio.c` (H5, U5, L5), with or without signal
injection.

## Behaviour

For a pin in input mode, `IDR` reads:

| Driven by injection | PUPDR | IDR |
|---|---|---|
| yes | any | the injected level (an external driver beats the ~40 kΩ pull) |
| no | `01` pull-up | 1 |
| no | `10` pull-down | 0 |
| no | `00` none, or `11` reserved | 0 (floating; the model picks 0) |

Other pins ignore `PUPDR`:

- Analog mode reads 0.
- Push-pull outputs read `ODR`, and open-drain outputs with `ODR` = 0 read 0.
- Released open-drain outputs (`ODR` = 1) read the injected level, or 1 when
  nothing is injected.
- AF pins read the injected level, or `ODR` when nothing is injected. The
  peripheral's own output is not modelled at the pad.

`IDR` is recomputed from these registers and the injected level on every
`MODER`, `PUPDR`, `OTYPER`, `ODR`, `BSRR` and `BRR` write, and EXTI is
notified of each change, so enabling a pull-up on an EXTI input produces the
edge it produces on silicon.

## Reset values

From RM0481 Rev 5 §13.4.1 and §13.4.4; the STM32U585 and STM32L552 SVD files
give the same values:

| Port | MODER | PUPDR |
|---|---|---|
| A | `0xABFF_FFFF` (PA13..15 AF0, JTAG/SWD) | `0x6400_0000` (PA13 and PA15 pull-up, PA14 pull-down) |
| B | `0xFFFF_FEBF` (PB3, PB4 AF0) | `0x0000_0100` (PB4 pull-up) |
| others | `0xFFFF_FFFF` | 0 |

The debug pins reset in AF mode, so their pulls only show in `IDR` once
firmware switches them to input without writing `PUPDR`. Not modelled: the
STM32H523/33 port B value (`0xFF3F_FEBF`) and the H5 port I value
(`0x00FF_FFFF`), which only differ on pins those devices lack.

## Tests

`tests/stm32_gpio_signal_test.c`: pull-up reads 1 and pull-down 0 through
both the Secure and Non-secure alias, injection beats the pull, a `PUPDR`
write raises an EXTI edge, pulls have no effect in push-pull output, AF and
analog mode, a released open-drain output follows the injected level and
otherwise reads 1 whatever its pull, reset values.

## Decisions

1. The reset values follow the reference manuals, including GPIOB `MODER`.
   The vendor STM32H563 SVD gives every port GPIOA's values, which RM0481
   contradicts.
2. Released open-drain outputs read the injected level, else 1, as upstream
   m33mu did. Board pull-ups are not modelled, and bit-banged I2C or 1-Wire
   code that relies on one polls for a released line to read high.
