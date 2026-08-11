#!/usr/bin/env python3
"""Generates the .sigc fixtures used by tests/firmware/test-stm32h5f4-signal-*.
See docs/signal-injection-gpio.md for the container format."""
import struct
import os

OUT_DIR = os.path.dirname(os.path.abspath(__file__))


def write_container(path, timebases, traces):
    """timebases: (id, sample_count, period_ns, timestamps_ns or None)
    traces: (id, name, timebase_id, full_scale_mv, samples_mv)"""
    with open(path, "wb") as f:
        f.write(struct.pack("<4sIII", b"SIGC", 1, len(timebases), len(traces)))
        for (tid, sample_count, period_ns, timestamps_ns) in timebases:
            f.write(struct.pack("<III", tid, sample_count, period_ns))
            if period_ns == 0:
                assert timestamps_ns is not None and len(timestamps_ns) == sample_count
                for ts in timestamps_ns:
                    f.write(struct.pack("<Q", ts))
        for (trid, name, tbid, full_scale_mv, samples_mv) in traces:
            name_b = name.encode("ascii")
            assert len(name_b) < 32
            name_padded = name_b + b"\x00" * (32 - len(name_b))
            f.write(struct.pack("<I32sIi", trid, name_padded, tbid, full_scale_mv))
            for s in samples_mv:
                f.write(struct.pack("<h", s))
    print(f"wrote {path}")


def uniform(tid, samples_mv, period_ns):
    return (tid, len(samples_mv), period_ns, None)


def nonuniform(tid, samples_mv, timestamps_ns):
    return (tid, len(samples_mv), 0, timestamps_ns)


# exti-both/-rising/-falling: 4 crossings. Edges closer than the ISR
# latency coalesce into one pending IRQ, so they are 20us apart.
tb = uniform(1, [0, 3300, 0, 3300, 0], 20000)
tr = (1, "vin", 1, 3300, [0, 3300, 0, 3300, 0])
write_container(os.path.join(OUT_DIR, "fixture_4cross.sigc"), [tb], [tr])

# exti-masked: single rising crossing
tb = uniform(1, [0, 3300], 1000)
tr = (1, "vin", 1, 3300, [0, 3300])
write_container(os.path.join(OUT_DIR, "fixture_1cross.sigc"), [tb], [tr])

# hysteresis: in-band noise, zero real crossings at vdd=3300mV
samples = [1600, 1700, 1600, 1700, 1600, 1700]
tb = uniform(1, samples, 1000)
tr = (1, "vin", 1, 3300, samples)
write_container(os.path.join(OUT_DIR, "fixture_hysteresis.sigc"), [tb], [tr])

# sharp-edge: 2 samples, ~1 cycle apart at 64MHz (16ns)
tb = uniform(1, [0, 3300], 16)
tr = (1, "vin", 1, 3300, [0, 3300])
write_container(os.path.join(OUT_DIR, "fixture_sharp_edge.sigc"), [tb], [tr])

# nonuniform: irregular timestamps, 3 crossings
samples = [0, 3300, 0, 3300]
timestamps = [0, 15000, 45000, 70000]
tb = nonuniform(1, samples, timestamps)
tr = (1, "vin", 1, 3300, samples)
write_container(os.path.join(OUT_DIR, "fixture_nonuniform.sigc"), [tb], [tr])

# trigger-oneshot: single crossing
tb = uniform(1, [0, 3300], 1000)
tr = (1, "vin", 1, 3300, [0, 3300])
write_container(os.path.join(OUT_DIR, "fixture_oneshot.sigc"), [tb], [tr])

# multi-binding: two identically-scheduled traces, one timebase
tb = uniform(1, [0, 3300], 1000)
tr_a = (1, "trace_a", 1, 3300, [0, 3300])
tr_b = (2, "trace_b", 1, 3300, [0, 3300])
write_container(os.path.join(OUT_DIR, "fixture_multi_binding.sigc"), [tb], [tr_a, tr_b])

# input-idle-high: starts high, one falling crossing
tb = uniform(1, [3300, 3300, 0], 20000)
tr = (1, "vin", 1, 3300, [3300, 3300, 0])
write_container(os.path.join(OUT_DIR, "fixture_idle_high.sigc"), [tb], [tr])

# mode-guard: 4 crossings, widely spaced (100us)
tb = uniform(1, [0, 3300, 0, 3300, 0], 100000)
tr = (1, "vin", 1, 3300, [0, 3300, 0, 3300, 0])
write_container(os.path.join(OUT_DIR, "fixture_mode_guard.sigc"), [tb], [tr])
