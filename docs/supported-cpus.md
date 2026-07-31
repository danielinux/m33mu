# Supported CPUs

These values are accepted by `--cpu`:

- `stm32h533`
- `stm32h563` (default)
- `stm32h5f4`
- `stm32u585`
- `stm32l552`
- `lpc55s69`
- `mcxw71c`
- `mcxn947`
- `rw612` — NXP RW612 (Cortex-M33 + ELS / PKA crypto offload)
- `nrf5340`
- `nrf54lm20`
- `rp2350`
- `pic32ck`

You can also print the list directly from the executable:

```sh
build/m33mu --cpu list
```

or:

```sh
build/m33mu --cpu ?
```

Use the CPU profile that matches the target firmware's memory map and peripheral set. If you are bringing up a new firmware image and are unsure which target model to use, start from the exact MCU family the firmware was built for and then confirm flash/RAM layout and required peripherals.
