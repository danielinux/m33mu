# Command-Line Usage

## Basic Form

```sh
build/m33mu [options] <image.bin[:offset]|image.elf|image.hex|image.uf2> [more images...]
```

`m33mu` can run as a straightforward CLI process, as an interactive TUI with `--tui`, or as a debug target with `--gdb`.

## Core Modes

- Pure CLI:

```sh
build/m33mu tests/firmware/test-stm32h563/app.bin
```

- Interactive TUI:

```sh
build/m33mu --tui tests/firmware/test-stm32h563/app.bin
```

- GDB remote debugging:

```sh
build/m33mu --gdb --gdb-symbols firmware.elf firmware.bin
```

## Frequently Used Options

- `--cpu <cpu>`: select the MCU/SoC profile
- `--tui`: start the interactive ncurses UI
- `--gdb`: expose a GDB remote server on port `1234`
- `--port <n>`: override the GDB port
- `--gdb-symbols <elf>`: load debug symbols from one or more ELFs
- `--uart-stdout`: send UART output to stdout instead of PTYs
- `--dump`: print instruction/decode tracing
- `--record`: keep an in-memory execution trace for reverse/debug workflows
- `--call-trace`: log calls, returns, interrupts, and TrustZone SG transitions
- `--quit-on-faults`: terminate when the first fault is raised
- `--timeout <seconds>`: force a host-side timeout
- `--expect-bkpt <imm>`: turn a firmware BKPT into a pass/fail test signal

## Recording And Debug-Oriented Options

The trace/record options are useful when debugging difficult firmware behavior:

- `--record`
- `--record-start <pc>`
- `--record-start-dump`
- `--record-start-dump-ram`
- `--record-end-dump-ram`
- `--record-window <n>`
- `--record-dump <n>`
- `--record-trace <path>`
- `--record-quiet`

These let you capture a bounded execution window, dump machine state around a specific PC, and export traces for offline inspection.

## Peripheral / Backend Options

`m33mu` can attach optional emulated or host-backed peripherals:

- `--spiflash:SPIx:file=<path>:size=<n>[:mmap=0xaddr][:cs=GPIONAME]`
- `--usb`
- `--usb:udc=<name>`
- `--usb:path=/dev/gadget/<name>`
- `--tap[:name]`
- `--vde[:/path/to/vde.ctl]`
- `--tpm:SPIx:cs=GPIONAME[:file=<path>]`
- `--ta100:SPIx:cs=GPIONAME[:file=<path>][:profile=<name>][:serial=<hex>]`
- `--iotsafe-uart:<uart-base-hex>[:file=<path>]`

### Rust Plugin Secure Elements

The following devices are compiled in only when `cargo` is found at build time
(`M33MU_HAS_RUST_PLUGINS`). The simulator logic is provided by the
[wolfssl/simulators](https://github.com/wolfssl/simulators) Rust crates vendored
under `third_party/wolfssl-simulators/`.

- `--atecc608:SPIx:cs=GPIONAME[:file=<path>]`  
  Microchip ATECC608A secure element over SPI. Chip-select is a GPIO (e.g. `PA4`).
  Optional `file=` path enables NV persistence.

- `--se050:I2Cx[:addr=<hex>][:file=<path>]`  
  NXP SE050 secure element over I2C. Default address `0x48`.
  Optional `file=` path enables NV persistence.

- `--iotsafe-uart:<uart-base-hex>[:file=<path>]`  
  GSMA IoTSAFE modem+SIM simulator over an emulated UART. The modem currently
  implements an extensible AT dispatcher and `AT+CSIM` transport with IoTSAFE
  applet support for certificate files, RNG, ECC P-256 key generation, public
  key import/export, ECDSA sign/verify, ECDH, and HKDF extract. Slot and file
  identifiers are 16-bit so wolfSSL's `_ex` IoTSAFE APIs can be exercised.
  Optional `file=` path enables NV persistence for SIM files and key slots.

- `--stsafe:I2Cx[:addr=<hex>][:file=<path>]`  
  STMicro STSAFE-A120 secure element over I2C. Default address `0x20`.
  Optional `file=` path enables NV persistence.

- `--tropic01:SPIx:cs=GPIONAME[:file=<path>]`  
  Tropic Square TROPIC01 secure element over SPI (libtropic L1/L2/L3
  protocol: REQ_ID framing, X25519 handshake, AES-GCM encrypted channel).
  Chip-select is a GPIO (e.g. `PA4`). Optional `file=` path is a JSON
  NV file.

Only one Ethernet backend can be selected at a time.

## TrustZone / Memory / Flash Options

- `--no-tz`: run without TrustZone protections for the session
- `--secwm1=<strt>:<end>`, `--secwm2=<strt>:<end>`: provision a flash secure
  watermark on stm32h5 targets, as the option bytes of a real board carry it.
  Sectors are counted within the bank, and `strt > end` means the bank holds no
  secure sector. A bank left unnamed is secure in full.

  Passing either flag turns on the flash TrustZone filter, which reads back zero
  when the security attribute of an access does not match the attribution of the
  target sector, in both directions, exactly as silicon does. Passing neither
  leaves the filter inert and says so on stdout: which sectors are secure is a
  property of the board's option bytes rather than of the image, so there is
  nothing sound to infer from an image alone.

  wolfBoot's stm32h5 TrustZone layout, for example, is
  `--secwm1=0:47 --secwm2=0:127`: bootloader secure, boot partition non-secure so
  the application can run, bank 2 secure for the update partition at
  `0x0C100000`.
- `--dualbank`: enable STM32 dual-bank flash behavior
- `--persist`: write modified flash contents back to the original input BINs when supported
- `--puf-seed <value>`: fill initial RAM deterministically from a fixed PRNG seed
- `--puf-cold-boot <n>`: select the deterministic cold-boot index used for PUF noise derivation
- `--puf-noise <n>`: with `--puf-seed`, flip exactly `n` pseudo-random bits per 127-bit block
- `--meminfo`: print SAU/MPU layout and related logs
- `--boot flash|ram|spiflash`
- `--boot-offset=0x...`

## Environment Variables

- `CAPSTONE_PC=<hex>`
- `M33MU_MEMWATCH=<addr:size>`
- `M33MU_NVIC_TRACE=1`
- `M33MU_SYSTICK_TRACE=1`
- `M33MU_SLEEP_TRACE=1`
- `M33MU_PROT_TRACE=1..3`

See [m33mu.1](/home/dan/src/m33mu/m33mu.1) for full descriptions and examples.
