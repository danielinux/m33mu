# GDB Monitor Commands For Test Harnesses

With `--gdb`, `m33mu` accepts `monitor` commands that let a program drive the
emulated device the way a power-loss or update test needs: run for a stretch of
virtual time, talk to the firmware over a UART, watch and cut flash operations,
save and restore the flash, and cycle the power. They work from `gdb` itself
(`monitor flash ops`) and from any client that speaks the GDB remote protocol
(`qRcmd`). `scripts/gdb-harness-test.py` is a complete client in about 100
lines of Python, and the test ctest runs.

Start the emulator with `--no-realtime` for this kind of use, so virtual time is
not paced against the wall clock:

```sh
build/m33mu --cpu stm32h563 --gdb --port 3333 --no-realtime firmware.bin
```

## Replies

Each command answers with zero or more console lines (`O` packets), then `OK`.
A command that fails prints a line starting with `error:` and answers `E01`.
`m33mu` sends one stop reply (`S05`) unasked when a client connects; a client
that does not start with `?` should skip it. A reset while the core runs a
`run-for` (the firmware resetting itself, a watchdog) does not stop it; other
resets of a running core stop it with `S05`, as when debugging.

Under a debugger the core runs on the fast translation-block path as long as no
breakpoint, single step, reverse execution or fault clock is pending, so a
harness that sets none of those runs at full speed.

## Time

| Command | Effect |
|---|---|
| `vtime` | `vtime <cycles> <hz>`: virtual cycles since start, across resets, and the current core clock |
| `run-for <us>` | the next `c` stops (`S05`) after `us` microseconds of virtual time, or earlier at a breakpoint or a power cut. A core asleep in WFI does not sleep past it |
| `run-for off` | drop the deadline |

## Power

| Command | Effect |
|---|---|
| `power-cycle` | off and on again: RAM is cleared, flash keeps what it holds, the core resets. The flash operation count starts again at 0. A cut that fired is spent; one still armed waits for this power-up, so arm a cut and then power-cycle into an update that starts at boot |

## Flash

Flash commands need flash boot. Operations are counted, traced and cut by the
layer described with `--flash-trace` in [CLI usage](cli-usage.md); these
commands control it at run time.

| Command | Effect |
|---|---|
| `flash info` | `flash <base> <size>` |
| `flash ops` | `flash ops <n>`: operations since the last power-up |
| `flash ops reset` | count from 0 again |
| `flash cut <n>` | cut the power just before operation `n` |
| `flash cut erase <addr>` | cut the power just before the first erase covering `addr` |
| `flash cut off` | disarm |
| `flash cut status` | `flash cut none`, or `flash cut <n> <program\|erase\|mass-erase\|bulk\|option> <addr> <len>` once a cut fired (`option` with address and length 0) |
| `flash trace <file>` / `flash trace off` | append one line per operation to `file` (format in [CLI usage](cli-usage.md)) |
| `flash timing none\|typical\|max` | as `--flash-timing` |
| `flash latches` | `flash latches refused <n> <first addr> protected <n> <first addr>`: programs the chip's rules refused, and programs or erases of write-protected flash, since the last power-up. Each is also an operation (traced `X` / `W`, counted, cuttable) that changed nothing |
| `flash latches clear` | zero them |
| `flash save <file>` | write the whole flash array to `file` |
| `flash load <file>` | replace the whole flash array with `file` (a shorter file leaves the rest erased); not an operation, nothing is counted or persisted |

A core that locks up (a HardFault it cannot take, an exception return it
cannot unstack) stays locked up, as on silicon: it runs nothing, time and the
peripherals go on (a running watchdog still resets the chip), and only a reset
or `power-cycle` ends it. A `run-for` still ends at its deadline; without one,
a debugger is told with `SIGSEGV`. Without `--gdb`, a lockup ends the run.

When a cut fires the core halts with `S05`. It stays halted, and every later
flash operation is undone, until `power-cycle`: a `c` before that stops again
at once.

## Option bytes

Option bytes live outside the flash array: `flash save` and `flash load` do
not include them. A harness that restores a device saves these alongside.

A change the firmware makes (OPTSTRT on the STM32H5) is a flash operation: it
is counted, a cut can fall just before it (the option bytes then stay as they
were), and the trace has one line for it with every option byte as it is
after the change, `O SWAP_BANK=0x00000000 WRPSG1=0xFFFFFFFE ...`, so a
harness can replay it. A change made with `optbytes set` is not an
operation.

| Command | Effect |
|---|---|
| `optbytes` | one `<NAME> <value>` line per option byte the target models |
| `optbytes get <NAME>` | `<NAME> <value>` |
| `optbytes set <NAME> <value>` | as a completed option-byte change on the chip (`--optbyte` does the same before boot) |

STM32H5 has `SWAP_BANK`, `WRPSG1`, `WRPSG2` (bit N cleared write-protects
sectors 4N..4N+3 of that bank) and `SECWM1`, `SECWM2`.

## UART

| Command | Effect |
|---|---|
| `uart attach <base>` | keep the UART at `<base>` inside the emulator. Attach before the firmware enables it |
| `uart read <base>` | `uart <pending> <hex>`: up to 256 bytes the firmware sent, `-` when none; ask again while `pending` is not 0 |
| `uart write <base> <hex>` | queue up to 128 bytes for the firmware to receive |

Every byte the firmware sends is kept until read, up to 1 MiB.

## A power-loss test

```text
monitor uart attach 0x40004800
monitor flash save original.bin
monitor flash trace golden.txt
monitor run-for 2000000            # an update, uninterrupted
continue
monitor flash trace off            # golden.txt: N operations
# for each n < N:
monitor flash load original.bin
monitor flash cut <n>
monitor power-cycle
monitor run-for 2000000
continue                           # stops at the cut
monitor flash cut status
monitor power-cycle                # power comes back
monitor run-for 2000000
continue
monitor uart read 0x40004800       # did the device recover?
```
