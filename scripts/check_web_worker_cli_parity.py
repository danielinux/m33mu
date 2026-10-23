#!/usr/bin/env python3
"""Run a shared generated ELF through CLI and save state for the worker smoke."""

import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def h563_elf():
    ident = b"\x7fELF" + bytes((1, 1, 1)) + bytes(9)
    header = struct.pack(
        "<16sHHIIIIIHHHHHH", ident, 0, 40, 0, 0x0C000000, 52, 0, 0,
        52, 32, 1, 0, 0, 0
    )
    program = struct.pack(
        "<IIIIIIII", 1, 84, 0x0C000000, 0x0C000000, 12, 12, 4, 0
    )
    code = struct.pack("<IIHH", 0x30080000, 0x0C000009, 0x2001, 0xBEAB)
    return header + program + code


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: check_web_worker_cli_parity.py /path/to/m33mu output.json")
    image = h563_elf()
    with tempfile.TemporaryDirectory(prefix="m33mu-worker-cli-") as directory:
        image_path = Path(directory) / "worker-parity.elf"
        image_path.write_bytes(image)
        result = subprocess.run(
            [sys.argv[1], "--cpu", "stm32h563", "--rng-seed", "1", "--dump",
             "--expect-bkpt", "0xab", "--timeout", "3", str(image_path)],
            capture_output=True, text=True, check=False, timeout=10,
            env={**os.environ, "M33MU_DISABLE_TB": "1"}
        )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise SystemExit(f"CLI parity run exited {result.returncode}:\n{output}")
    rows = re.findall(r"\[DUMP\] PC=0x([0-9a-f]+).*?r0=0x([0-9a-f]+)", result.stdout)
    if len(rows) < 2 or "Initial SP=0x30080000" not in output:
        raise SystemExit(f"CLI parity run did not report the expected state:\n{output}")
    state = {
        "sha256": hashlib.sha256(image).hexdigest(),
        "pc": int(rows[1][0], 16),
        "r0": int(rows[1][1], 16),
        "mspSecure": 0x30080000,
    }
    Path(sys.argv[2]).write_text(json.dumps(state), encoding="utf-8")


if __name__ == "__main__":
    main()
