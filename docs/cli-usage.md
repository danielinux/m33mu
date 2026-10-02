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
- `--tui`: start the interactive ncurses UI. Press F9 to switch between the emulator and built-in debugger views.
- `--gdb`: expose a GDB remote server on port `1234`
- `--port <n>`: override the GDB port
- `--gdb-symbols <elf>`: load debug symbols from one or more ELFs

- `--uart-stdout`: send UART output to stdout instead of PTYs. Guest bytes are never dropped: a host that drains stdout slowly stalls the emulator instead of losing output, and what the guest sees at the UART registers does not depend on the host side. The emulator's own messages keep their place relative to guest bytes on a blocking stdout, which is what a shell pipe, file or terminal provides. A PTY, by contrast, is best effort: with no reader or a slow one the oldest queued bytes are dropped, so use `--uart-stdout` when a complete transcript matters.
- `--dump`: print instruction/decode tracing
- `--record`: keep an in-memory execution trace for reverse/debug workflows
- `--call-trace`: log calls, returns, interrupts, and TrustZone SG transitions
- `--quit-on-faults`: terminate when the first fault is raised
- `--timeout <seconds>`: force a host-side timeout. On expiry the CPU state (PC, LR, SP, xPSR, mode, secure state and r0-r12) is written to stderr before exit, so a hang in an unattended run says where it hung.
- `--rng-seed <n>`: fix the stream every modeled RNG peripheral draws from, making a run reproducible. Without it a seed is drawn from the host and announced on stderr, so any run can be replayed.
- `--expect-bkpt <imm>`: turn a firmware BKPT into a pass/fail test signal
- `--capstone`: cross-check decode/execute behavior against Capstone
- `--fault-clock <NNN>`: skip the instruction fetched at virtual cycle `NNN`, for fault-injection testing. May be repeated up to 16 times; values must not be contiguous.

In the TUI debugger, Tab cycles through the command, code, text, and data panes; the selected pane title is black on white. Up/Down scroll the focused pane by one row; Left/Right and Page Up/Page Down scroll by eight rows. Typing in the text/data panes edits their start address fields; Enter pins the address. Text follows PC and data centers on SP by default; `text auto` and `data auto` restore following. Continue (`c` or F2) clears manual scrolling and makes all three panes follow PC and SP again. An empty Return in the command pane repeats its last command. The command line accepts `help`, `c`, `s`, `si` (step one instruction), `n`/`next` (step over a call), `b symbol_name`, `b *0xADDRESS`, `watch ADDRESS [1|2|4]`, `delete SLOT`, `info registers`, `x ADDRESS`, `disassemble ADDRESS`, `bt`, `up`, `down`, and `mon`. `bt` shows the current frame, the LR return address, and candidate return addresses found on the stack; stack candidates are approximate. `up` and `down` select a frame for the code pane. `mon` supports `info`, `reset`, `quit`, `capstone on/off`, and `fault-clock [N|clear]`. ELF symbols are loaded automatically from ELF images and used for breakpoints, backtraces, and text disassembly labels; `--gdb-symbols` also supplies symbols. The text pane shows assembly mnemonics with symbol annotations even when Capstone cross-checking is off. While stopped, its PC instruction row is black on white. The data pane shows hexadecimal bytes and aligned ASCII characters in separate colors; the four bytes starting at SP are black on white while stopped. The left pane shows syntax-colored C source when DWARF points to a readable source file, with the current line in black on white. Breakpoints and write watchpoints share six slots. CLI `--gdb` continues to serve external GDB clients.

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

## Coverage Options

For firmware built with `clang -fprofile-instr-generate -fcoverage-mapping`
(and `-fcoverage-mcdc` for MC/DC bitmaps):

- `--covdump <prefix>`: on exit, write the LLVM instrumentation regions read
  from the emulated memory map to `<prefix>.cnts.bin` and `<prefix>.bits.bin`
  (the verbatim `__llvm_prf_cnts`/`__llvm_prf_bits` contents), plus
  `<prefix>.json`, a manifest with the ELF used, each region's address and
  size, the security state read, the exit reason, and the cycle count.
  No `.profraw` is produced here — that versioned LLVM container is meant to
  be assembled host-side, next to the `llvm-profdata` that consumes it.
  Region bounds come from the firmware's `__start___llvm_prf_*` /
  `__stop___llvm_prf_*` linker symbols; a missing instrumentation symbol or
  an unmapped region is a hard error rather than a silently empty dump.
- `--covdump-elf <file>`: read the coverage symbols from `<file>` instead of
  the loaded image. Required when more than one ELF image is loaded.
- `--covdump-when success|always`: dump only on runs that satisfy
  `--expect-bkpt` (`success`), or on every exit including faults and
  timeouts (`always`, the default) — a run that ends badly still yields the
  counters it accumulated, and the manifest's `exit_reason` records which
  case applied.

## Debug Cross-Check Options

- `--capstone`: enable Capstone-based cross-check logging for decode/execute behavior
- `--capstone-verbose`: same as `--capstone`, with more operand-level detail in the logs

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
- `M33MU_STM32H5_IDCODE=<hex|decimal>`: replace the STM32H5 DBGMCU_IDCODE value (e.g. `0x10016484`)

See [m33mu.1](/home/dan/src/m33mu/m33mu.1) for full descriptions and examples.
