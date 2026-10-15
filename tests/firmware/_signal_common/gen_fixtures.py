#!/usr/bin/env python3
"""Generates the .sigc fixtures used by tests/firmware/test-stm32h5f4-signal-*.
See docs/signal-injection-gpio.md for the container format.
Usage: gen_fixtures.py [OUT_DIR]   (default: this directory)"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "scripts"))
import sigc  # noqa: E402

OUT_DIR = sys.argv[1] if len(sys.argv) > 1 else HERE


def write_container(path, timebases, traces):
    sigc.write(path, timebases, traces)
    print(f"wrote {path}")


def uniform(tid, samples_mv, period_ns):
    return sigc.Timebase.uniform(tid, len(samples_mv), period_ns)


def nonuniform(tid, samples_mv, timestamps_ns):
    assert len(samples_mv) == len(timestamps_ns)
    return sigc.Timebase.explicit(tid, timestamps_ns)


# exti-both/-rising/-falling: 4 crossings. Edges closer than the ISR
# latency coalesce into one pending IRQ, so they are 20us apart.
tb = uniform(1, [0, 3300, 0, 3300, 0], 20000)
tr = sigc.Trace(1, "vin", 1, 3300, [0, 3300, 0, 3300, 0])
write_container(os.path.join(OUT_DIR, "fixture_4cross.sigc"), [tb], [tr])

# exti-masked: single rising crossing
tb = uniform(1, [0, 3300], 1000)
tr = sigc.Trace(1, "vin", 1, 3300, [0, 3300])
write_container(os.path.join(OUT_DIR, "fixture_1cross.sigc"), [tb], [tr])

# hysteresis: in-band noise, zero real crossings at vdd=3300mV
samples = [1600, 1700, 1600, 1700, 1600, 1700]
tb = uniform(1, samples, 1000)
tr = sigc.Trace(1, "vin", 1, 3300, samples)
write_container(os.path.join(OUT_DIR, "fixture_hysteresis.sigc"), [tb], [tr])

# sharp-edge: 2 samples, ~1 cycle apart at 64MHz (16ns)
tb = uniform(1, [0, 3300], 16)
tr = sigc.Trace(1, "vin", 1, 3300, [0, 3300])
write_container(os.path.join(OUT_DIR, "fixture_sharp_edge.sigc"), [tb], [tr])

# nonuniform: irregular timestamps, 3 crossings
samples = [0, 3300, 0, 3300]
timestamps = [0, 15000, 45000, 70000]
tb = nonuniform(1, samples, timestamps)
tr = sigc.Trace(1, "vin", 1, 3300, samples)
write_container(os.path.join(OUT_DIR, "fixture_nonuniform.sigc"), [tb], [tr])

# trigger-oneshot: single crossing
tb = uniform(1, [0, 3300], 1000)
tr = sigc.Trace(1, "vin", 1, 3300, [0, 3300])
write_container(os.path.join(OUT_DIR, "fixture_oneshot.sigc"), [tb], [tr])

# multi-binding: two identically-scheduled traces, one timebase
tb = uniform(1, [0, 3300], 1000)
tr_a = sigc.Trace(1, "trace_a", 1, 3300, [0, 3300])
tr_b = sigc.Trace(2, "trace_b", 1, 3300, [0, 3300])
write_container(os.path.join(OUT_DIR, "fixture_multi_binding.sigc"), [tb], [tr_a, tr_b])

# input-idle-high: starts high, one falling crossing
tb = uniform(1, [3300, 3300, 0], 20000)
tr = sigc.Trace(1, "vin", 1, 3300, [3300, 3300, 0])
write_container(os.path.join(OUT_DIR, "fixture_idle_high.sigc"), [tb], [tr])

# mode-guard: 4 crossings, widely spaced (100us)
tb = uniform(1, [0, 3300, 0, 3300, 0], 100000)
tr = sigc.Trace(1, "vin", 1, 3300, [0, 3300, 0, 3300, 0])
write_container(os.path.join(OUT_DIR, "fixture_mode_guard.sigc"), [tb], [tr])

# timer-capture: 100us square wave, 20 periods; linear ramps keep the
# rising and falling crossings exactly 100us apart and 50us high.
samples = [0, 3300] * 20
tb = uniform(1, samples, 50000)
tr = sigc.Trace(1, "vin", 1, 3300, samples)
write_container(os.path.join(OUT_DIR, "fixture_square.sigc"), [tb], [tr])
