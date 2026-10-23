# MCU pad electrical model

The workbench treats every net as an analog node. An MCU pad that is wired into a circuit becomes a `pad` device (`web/sim/netlist.ts`) whose drive follows the firmware's GPIO registers, as reported by the emulator's pad listeners (`mm_gpio_pad_listener`, `mm_gpio_pad_config_listener`) through `mm_web_run_until`.

| Firmware state | Electrical drive |
|---|---|
| Output, push-pull, ODR=1 | VDD through `rOn` |
| Output, push-pull, ODR=0 | 0 V through `rOn` |
| Output, open-drain, ODR=0 | 0 V through `rOn` |
| Output, open-drain, ODR=1 | high impedance |
| Input | high impedance |
| Alternate function | high impedance, plus an `unsupported` bench event: peripheral outputs are not modelled electrically |
| Analog | high impedance; pulls are disconnected (STM32 RM) |
| PUPDR pull-up / pull-down | `rPull` to VDD / 0 V, in parallel with any drive |

Every pad also has `cIn` to ground. The MCU input level is derived from the solved pad voltage with Schmitt thresholds `vRise`/`vFall`; a crossing is delivered to the emulator at the first instruction boundary at or after the crossing time rounded up to the next nanosecond.

Default values (`defaultPad()`), all **documented approximations**, not datasheet-exact behaviour:

| Parameter | Default | Basis |
|---|---|---|
| `vdd` | 3.3 V | NUCLEO-H563ZI VDD |
| `rOn` | 40 Ohm | STM32H5 VOL/VOH guarantee of 0.4 V at 8 mA implies <= 50 Ohm; linear driver, no current limit |
| `rPull` | 40 kOhm | STM32H5 typical RPU/RPD |
| `cIn` | 5 pF | STM32H5 typical CIO |
| `vRise` / `vFall` | 1.775 V / 1.525 V | inside the guaranteed VIL (0.3 VDD) / VIH (0.7 VDD) window; same values as the historical Perun coupling probe |

Not modelled: output slew rate and speed settings (OSPEEDR), driver current limits, injection current, protection diodes, analog-switch resistance, ADC sampling load, and every peripheral alternate function. The bench reports these as unsupported rather than approximating them.
