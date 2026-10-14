#!/usr/bin/env python3
# m33mu -- an ARMv8-M Emulator
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Check a --signal-record CSV against expected levels and toggle timing."""

import argparse
import csv
import sys


def load(path):
    with open(path, newline="") as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if header != ["t_ns", "pin", "level"]:
            sys.exit(f"{path}: bad header {header}")
        rows = [(int(t), pin, level) for t, pin, level in reader]
    times = [r[0] for r in rows]
    if times != sorted(times):
        sys.exit(f"{path}: rows are not in time order")
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("csv")
    ap.add_argument("--levels", action="append", default=[], metavar="PIN=L,L,...",
                    help="exact level sequence of a pin over the whole run")
    ap.add_argument("--period", action="append", default=[], metavar="PIN:COUNT:PERIOD_NS:TOL_NS",
                    help="COUNT changes at t >= 0, PERIOD_NS apart within TOL_NS, "
                         "the first one PERIOD_NS after the trigger within TOL_NS")
    args = ap.parse_args()

    rows = load(args.csv)
    failed = False

    for spec in args.levels:
        pin, seq = spec.split("=", 1)
        want = seq.split(",")
        got = [level for _, p, level in rows if p == pin]
        if got != want:
            print(f"{pin}: levels {got}, expected {want}")
            failed = True

    for spec in args.period:
        pin, count, period, tol = spec.split(":")
        count, period, tol = int(count), int(period), int(tol)
        times = [t for t, p, _ in rows if p == pin and t >= 0]
        if len(times) != count:
            print(f"{pin}: {len(times)} changes after the trigger, expected {count}")
            failed = True
            continue
        prev = 0
        for i, t in enumerate(times):
            if abs((t - prev) - period) > tol:
                print(f"{pin}: change {i} at {t} ns is {t - prev} ns after the previous one, "
                      f"expected {period} +/- {tol}")
                failed = True
            prev = t

    if failed:
        sys.exit(1)
    print(f"{args.csv}: OK ({len(rows)} rows)")


if __name__ == "__main__":
    main()
