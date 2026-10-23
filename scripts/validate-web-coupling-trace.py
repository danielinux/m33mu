#!/usr/bin/env python3
# m33mu -- an ARMv8-M Emulator
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Recompute the analytic timing comparison for the archived Perun coupling trace."""

import json
import math
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "web/fixtures/solver/perun-firmware-coupling.json"


def interpolate(samples, time_ns):
    for left, right in zip(samples, samples[1:]):
        if left["t"] <= time_ns <= right["t"]:
            span = right["t"] - left["t"]
            if span == 0:
                return float(right["mv"])
            ratio = (time_ns - left["t"]) / span
            return left["mv"] + ratio * (right["mv"] - left["mv"])
    raise ValueError(f"source event at {time_ns} ns is outside the sample trace")


def main():
    data = json.loads(FIXTURE.read_text())
    circuit = data["circuit"]
    samples = data["samples"]
    events = [event for event in data["sourceEvents"] if event["t"] >= 0]
    crossings = data["crossings"]
    if len(events) != 6 or len(crossings) != len(events):
        raise SystemExit("expected six nonnegative source events and six paired crossings")

    tau_ns = circuit["resistanceOhms"] * circuit["capacitanceFarads"] * 1e9
    errors = []
    for event, crossing in zip(events, crossings):
        initial_mv = interpolate(samples, event["t"])
        if event["level"]:
            threshold = circuit["vihMv"]
            target_mv = circuit["sourceHighMv"]
            delay_ns = tau_ns * math.log((target_mv - initial_mv) / (target_mv - threshold))
        else:
            threshold = circuit["vilMv"]
            delay_ns = tau_ns * math.log(initial_mv / threshold)
        expected_ns = event["t"] + delay_ns
        error_ns = crossing["t"] - expected_ns
        errors.append(abs(error_ns))
        print(
            f"{'rise' if event['level'] else 'fall'}: "
            f"edge={event['t']} ns crossing={crossing['t']} ns "
            f"analytic={expected_ns:.0f} ns error={error_ns / 1000:.3f} us"
        )

    maximum_us = max(errors) / 1000
    print(f"max absolute timing error: {maximum_us:.3f} us")
    if maximum_us > 6611:
        raise SystemExit("archived timing error exceeds the recorded 6.611 us bound")


if __name__ == "__main__":
    main()
