# m33mu

m33mu (pron. mee-moo) is a Cortex-M33 emulator.
m33mu emulates ARMv8-M Cortex-M targets with TrustZone awareness.

Current status: Beta


## Copyright / License

(c) Daniele Lacamera 2025.

Released under AGPLv3. See `LICENSE` file.
For the TUI, m33mu uses 'termbox2' library (MIT License) - see `tui/termbox2.h`.


## Requirements
- Libcapstone (for optional self-debugging)

## Features
- ARMv8-M baseline (Cortex-M33) CPU core
- TrustZone security extensions
- GDB remote server for debugging
- MMIO bus with pluggable peripherals
- Interrupt handling (NVIC-like)
- Memory protection unit (MPU) and security attribution unit (SAU) support
- Secure/non-secure world switching
- Secure/non-secure peripheral mapping
- Thread/Handler mode switching
- Exception handling and nesting
- Memory-mapped I/O with configurable access permissions
- Integration with Capstone for correctness verification and self-tests

## Development notes
- Peripherals: expose each device as an MMIO object behind the main bus; keep side effects localized and deterministic.
- TrustZone: model security attribution in bus lookups and peripheral instances; ensure SAU and secure fault paths are explicit.
- Testing: add focused unit tests per module; include small integration traces for fetch/execute and MMIO edges when added.
- No IDAU: trustzone segments are SAU-only.

## Repository structure (planned)
- `src/`: core CPU, decoder, scheduler/interrupt logic, GDB server.
- `cpu/`: Target specific MMIO device implementations and registration helpers.
- `include/`: public headers for core/peripheral interfaces.
- `tests/`: unit and scenario tests; runners should keep stdout stable for comparisons.
- `tui/`  : TUI for running target interactively (see `--tui` command line option)


## Getting started
Build everything with warnings-as-errors:

```sh
make
```

Run the included sample firmwares:

- Cortex-M33 bring-up: `bin/m33mu test-cortex-m33/app.bin`
- RTOS exception exercise (SysTick/SVC/PendSV, NVIC priorities): `bin/m33mu test-rtos-exceptions/app.bin`
- SysTick + WFI blocking demo: `SYSTICK_TRACE=1 bin/m33mu test-systick-wfi/app.bin` (shows SysTick downcounter, COUNTFLAG, pended interrupt waking WFI).
- TrustZone + CMSE + SAU + MPU (Secure+Non-secure images):
  - Build: `make -C test-tz-bxns-cmse-sau-mpu clean all`
  - Run: `timeout 3s bin/m33mu test-tz-bxns-cmse-sau-mpu/build/secure.bin test-tz-bxns-cmse-sau-mpu/build/nonsecure.bin:0x2000`

Both tests execute entirely from the in-repo binaries; no downloads required. Use `--gdb` to start the integrated GDB RSP server on port 1234.

## Loading multiple images
`m33mu` can load more than one raw binary image into the emulated flash window. Each image argument can optionally include a `:offset` suffix, interpreted as a byte offset into flash (accepts `0x...`).

Example (Secure image at offset 0, Non-secure image at offset 0x2000):

```sh
bin/m33mu test-tz-bxns-cmse-sau-mpu/build/secure.bin test-tz-bxns-cmse-sau-mpu/build/nonsecure.bin:0x2000
```

## Command line usage

```
bin/m33mu [--cpu <cpu>] [--gdb] [--port <n>] [--gdb-symbols <elf>] [--dump] [--tui] [--persist] [--capstone] [--uart-stdout] [--quit-on-faults] [--meminfo] <image.bin[:offset]> [more images...]
```

Options:
- `--cpu <cpu>`: select the CPU profile (default: `stm32h563`).
- `--gdb`: start the GDB remote server (RSP) on port 1234 (override with `--port`).
- `--port <n>`: set the GDB server port (1-65535).
- `--gdb-symbols <elf>`: load symbols from the specified ELF (defaults to the first image).
- `--dump`: print per-instruction decode (`[DUMP] ...`) and selected TrustZone transitions.
- `--tui`: start the interactive terminal UI.
- `--persist`: write modified flash contents back to the input image files.
- `--capstone`: enable Capstone-based cross-check logging for decode/execute.
- `--uart-stdout`: route UART output to stdout instead of a PTY device.
- `--quit-on-faults`: stop execution after the first fault is raised.
- `--meminfo`: emit `[MEMINFO]` logs for SAU/MPU layout and register writes.

## Environment variables (optional)
- `CAPSTONE_PC=<hex>`: limit Capstone cross-check logging to a specific PC (hex).
- `M33MU_PC_TRACE=<start:end>`: trace instruction fetch/execute over a PC range (hex).
- `M33MU_PC_TRACE_MEM=<start:end>`: like `M33MU_PC_TRACE`, but includes memory addresses.
- `M33MU_STRCMP_TRACE=<start:end>`: trace strcmp-like loops; auto-seeds entry to range start.
- `M33MU_STRCMP_ENTRY=<hex>`: override the entry PC for strcmp tracing.
- `M33MU_MEMWATCH=<addr:size>`: watch a memory range (hex address + size).
- `M33MU_NVIC_TRACE=1`: trace NVIC state changes.
- `M33MU_FLASH_TRACE=1`: trace flash MMIO activity.
- `M33MU_PROT_TRACE=1..3`: print SAU/MPU attribution decisions; higher levels include region scans.
- `M33MU_DUMP_PSP_FRAME=1`: dump 8-word stacked frames during EXC_RETURN unstack.
- `M33MU_TZ_BOOT_TRACE=1`: trace security-state transitions during TZ boot/hand-off.
