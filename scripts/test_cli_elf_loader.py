#!/usr/bin/env python3
"""Exercise the CLI path that delegates ELF loading to the shared parser."""

import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def make_minimal_h563_elf(path):
    ident = b"\x7fELF" + bytes((1, 1, 1)) + bytes(9)
    header = struct.pack(
        "<16sHHIIIIIHHHHHH",
        ident, 2, 40, 1, 0x08000009, 52, 0, 0, 52, 32, 1, 0, 0, 0
    )
    program = struct.pack(
        "<IIIIIIII", 1, 84, 0x08000000, 0x08000000, 10, 10, 5, 4
    )
    vectors_and_code = struct.pack("<II", 0x30001000, 0x08000009) + bytes((0x7F, 0xBE))
    path.write_bytes(header + program + vectors_and_code)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_cli_elf_loader.py /path/to/m33mu")
    with tempfile.TemporaryDirectory(prefix="m33mu-elf-cli-") as temp:
        image = Path(temp) / "minimal-h563.elf"
        make_minimal_h563_elf(image)
        result = subprocess.run(
            [sys.argv[1], "--cpu", "stm32h563", "--rng-seed", "1",
             "--expect-bkpt", "0x7f", "--timeout", "3", str(image)],
            capture_output=True, text=True, check=False, timeout=10
        )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise SystemExit(f"CLI ELF smoke exited {result.returncode}:\n{output}")
    for expected in ("Loaded ELF image", "Initial SP=0x30001000", "[EXPECT BKPT] Success"):
        if expected not in output:
            raise SystemExit(f"CLI ELF smoke missing {expected!r}:\n{output}")
    print("cli_elf_loader_test: minimal H563 ELF loaded and executed")


if __name__ == "__main__":
    main()
