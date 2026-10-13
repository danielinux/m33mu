# Getting Started

## Build Dependencies

`m33mu` has a small core build plus several optional feature-dependent libraries.
The CMake configuration summary reports what was detected and which features will be enabled.

Core build tooling:

- C compiler
- CMake
- `make` or another CMake-supported build tool

Optional libraries and what they enable:

- `ncurses`: interactive `--tui` mode
- `libcapstone`: decode/execute cross-checking and related diagnostics
- `libtpms`: TPM 2.0 emulation
- `wolfSSL`: TA-100 and related cryptographic integration
- `vde-2` / `libvdeplug` plus `vde_switch`: VDE Ethernet backend
- `libelf`: ELF loading and ELF-aware symbol/debug support
- `libdw` / `libdwfl`: richer symbolization and call-trace naming

Optional build outputs:

- tests: enabled by default with `M33MU_BUILD_TESTS=ON`

Example configuration summary:

```text
-- m33mu configuration summary:
--   capstone: detected
--   TPM/libtpms: detected
--   wolfSSL: detected
--   vde-2: detected (vde_switch)
--   ncurses: detected
--   libelf: detected
--   libdw/libdwfl: detected
--   tests enabled: ON
--   --------------------------------------------
--   features: capstone=TRUE tpm=TRUE wolfssl=TRUE vde=TRUE tui=TRUE libelf=TRUE libdw=TRUE
```

## Build

Configure and build:

```sh
cmake -S . -B build
cmake --build build
```

Run the test suite:

```sh
ctest --test-dir build
```

## Build Firmware Fixtures

Some in-repo firmware fixtures require an ARM embedded toolchain:

```sh
make -C tests/firmware/test-stm32h563 app.bin
make -C tests/firmware/test-rtos-exceptions app.bin
make -C tests/firmware/test-systick-wfi app.bin
make -C tests/firmware/test-tz-bxns-cmse-sau-mpu clean all
```

You can also trigger the bundled build/test targets from CMake:

```sh
cmake --build build --target firmware-build
cmake --build build --target test-firmware
cmake --build build --target test-stm32h5
cmake --build build --target test-stm32u5
cmake --build build --target test-stm32l5
cmake --build build --target test-mcxw
cmake --build build --target test-mcxn947
cmake --build build --target test-imxrt700
```

## First Runs

Run a simple firmware image:

```sh
build/m33mu tests/firmware/test-stm32h563/app.bin
```

Run in interactive TUI mode:

```sh
build/m33mu --tui tests/firmware/test-stm32h563/app.bin
```

Start the built-in GDB remote server:

```sh
build/m33mu --gdb tests/firmware/test-stm32h563/app.bin
```

Run an RTOS-oriented firmware sample:

```sh
build/m33mu tests/firmware/test-rtos-exceptions/app.bin
```

Run a SysTick + WFI sample:

```sh
M33MU_SYSTICK_TRACE=1 build/m33mu tests/firmware/test-systick-wfi/app.bin
```

## Notes

- `m33mu` can be used interactively for exploration or non-interactively in scripts.
- For detailed image-loading behavior, see [loading-images.md](loading-images.md).
- For the full CLI surface, see [cli-usage.md](cli-usage.md).
