#!/usr/bin/env python3
# m33mu -- an ARMv8-M Emulator
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Archive reproducible Perun output and build an H563 EXTI replay trace."""

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from sigc import Timebase, Trace, write  # noqa: E402

REVISION = "067d1c9c59e27f72f6498ec224f3735d0da6364a"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("probe_json", type=Path)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--sigc", type=Path, required=True)
    args = parser.parse_args()
    result = json.loads(args.probe_json.read_text())
    data = {
        "schemaVersion": 1,
        "provenance": {
            "solver": "Perun / CircuitJS1 TypeScript port",
            "solverRevision": REVISION,
            "probe": "reproducible staged m33mu output -> Perun RC -> m33mu EXTI; firmware and solver run in separate passes",
            "outputFixture": "test-stm32h5f4-signal-output-timing",
            "extiFixture": "test-stm32h563-signal-exti-both",
            "syncMarker": "signal_trigger_marker",
            "extiReplay": "expected BKPT 0x7f; replay is checked by run-web-coupling-probe.sh",
            "license": "GPL-2.0-or-later; evaluation only, no solver source imported",
        },
        "circuit": {
            "resistanceOhms": 1000,
            "capacitanceFarads": 1e-8,
            "sourceLowMv": 0,
            "sourceHighMv": 3300,
            "solverQuantumNs": result["quantumNs"],
            "vihMv": 1775,
            "vilMv": 1525,
        },
        "sourceEvents": result["sourceEvents"],
        "samples": result["samples"],
        "crossings": result["crossings"],
    }
    args.fixture.write_text(json.dumps(data, indent=2) + "\n")
    events = [crossing for crossing in result["crossings"] if crossing["t"] >= 0][:4]
    timestamps = [0] + [crossing["t"] for crossing in events]
    levels = [0] + [1775 if crossing["level"] else 1525 for crossing in events]
    write(args.sigc, [Timebase.explicit(0, timestamps)],
          [Trace(0, "vin", 0, 3300, levels)])
    print(f"archived {len(result['samples'])} samples, {len(result['crossings'])} crossings")


if __name__ == "__main__":
    main()
