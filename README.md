![m33mu logo](png/m33mu.png)

# m33mu

`m33mu` is an embedded microcontroller emulator for ARMv8-M Cortex-M33 targets.
It is used to test, debug, and demonstrate firmware without any physical hardware.

It can run as:

- an interactive terminal UI with `--tui`
- a pure command-line emulator
- a tool inside automated tests and CI jobs

## What It Does

- Emulates Cortex-M33 / ARMv8-M firmware with TrustZone awareness
- Runs firmware images directly from your host machine
- Supports debugging through a built-in GDB remote server
- Can load multiple images, including Secure and Non-secure firmware combinations
- Exposes UART, SPI flash, TPM, TA-100, ATECC608A, SE050, STSAFE-A120, and IoTSAFE modem+SIM secure-element paths, plus USB, Ethernet, and other target-specific peripherals
- Ships SoC profiles for STM32H5/U5/L5, NXP LPC55S69, MCXW71C, MCXN947, RW612 (with ELS/PKA crypto offload), Nordic nRF5340 / nRF54LM20, RP2350, Microchip PIC32CK, Nuvoton M2354, and NXP i.MX RT700 (dual core) — see [supported CPUs](docs/supported-cpus.md) for the full list

> **Note:** ATECC608A, SE050, and STSAFE-A120 simulation requires `cargo` (Rust toolchain) at build time.
> These features are automatically disabled if `cargo` is not found.

## Documentation Index

- [Getting started](docs/getting-started.md)
- [Supported CPUs](docs/supported-cpus.md)
- [Loading images](docs/loading-images.md)
- [Command-line usage](docs/cli-usage.md)
- [Supported HSE simulators](docs/supported-HSEs.md)
- [CI and automated testing](docs/ci-testing.md)

## Example

Run the STM32H563 wolfSSL IoTSAFE mutual-TLS demo against the built-in modem+SIM
emulator on `USART3`:

```sh
build/m33mu \
  --cpu stm32h563 \
  --uart-stdout \
  --expect-bkpt 0x42 \
  --timeout 20 \
  --iotsafe-uart:0x40004800:file=/tmp/m33mu-iotsafe-sim.bin \
  tests/firmware/test-stm32h563-wolfssl-iotsafe/app.bin
```

This exercises wolfSSL's 16-bit IoTSAFE `_ex` API path for SIM-backed
certificate reads, RNG, ECC sign/verify, ECDH, and an end-to-end mTLS
handshake.

## Screenshots for TUI mode

#### m33mu TUI, stopped, stepping with GDB:

![m33mu TUI, stopped, stepping with GDB](png/screen01.png)

#### m33mu TUI, running in secure domain:

![m33mu TUI, running in secure domain (green bar)](png/screen02.png)

#### m33mu TUI, running in non-secure domain:

![m33mu TUI, running in non-secure domain (blue bar)](png/screen03.png)

## Reporting Issues

Please report issues to the [GitHub issue tracker](https://github.com/danielinux/m33mu/issues).

Include, if possible, a full capture using `--capstone`, a reproducer firmware image, and a clear explanation of how to reproduce the problem.

## Copyright / License

Copyright (c) Daniele Lacamera 2025.

Released under AGPLv3. See [LICENSE](/home/dan/src/m33mu/LICENSE).
