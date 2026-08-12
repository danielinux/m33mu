#!/usr/bin/env python3
"""Generates .sigc fixtures for the signal-injection GPIO test firmware
suite (tests/firmware/test-stm32h5f4-signal-*). Mirrors the on-disk
container format defined in src/signal.c exactly:

  header:   magic[4]="SIGC", u32 version, u32 num_timebases, u32 num_traces
  timebase: u32 id, u32 sample_count, u32 period_ns
            (period_ns==0 => u64 timestamps_ns[sample_count] follows inline)
  trace:    u32 id, char name[32], u32 timebase_id, i32 full_scale_mv
            (i16 samples_mv[sample_count] follows inline)

This is NOT the offline HDF5->.sigc converter described in the design doc
(docs/signal-injection-gpio.md section 5 item 2) -- that's still an open
item. This is a narrower, hand-rolled generator producing exactly the
fixtures this test suite needs, for CI/local use.
"""
import struct
import os

OUT_DIR = os.path.join(os.path.dirname(__file__))


def write_container(path, timebases, traces):
    """timebases: list of (id, samples_or_None_if_uniform, period_ns,
                  explicit_timestamps_ns_or_None)
       Each timebase entry: (id, sample_count, period_ns, timestamps_ns_list_or_None)
       traces: list of (id, name, timebase_id, full_scale_mv, samples_mv_list)
    """
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


# --- exti-both / exti-rising / exti-falling: shared 4-crossing trace.
# Spaced 20us (1280 cycles at 64MHz) apart -- wide enough for the CPU to
# actually take and service each interrupt individually before the next
# crossing arrives. With crossings too close together (originally tried
# 1000ns / 64 cycles), all 4 edges dispatch before the CPU services the
# first one; since NVIC pending is a level bit (not a per-edge counter),
# rapid coalesced edges collapse into a single delivered interrupt --
# this is realistic hardware behavior (a real EXTI line has the same
# coalescing under events faster than interrupt latency), not a
# signal-injection bug, but it means the trace must give the CPU a
# realistic chance to keep up for an edge-counting test to be meaningful.
tb = uniform(1, [0, 3300, 0, 3300, 0], 20000)
tr = (1, "vin", 1, 3300, [0, 3300, 0, 3300, 0])
write_container(os.path.join(OUT_DIR, "fixture_4cross.sigc"), [tb], [tr])

# --- exti-masked: single rising crossing ---
tb = uniform(1, [0, 3300], 1000)
tr = (1, "vin", 1, 3300, [0, 3300])
write_container(os.path.join(OUT_DIR, "fixture_1cross.sigc"), [tb], [tr])

# --- hysteresis: in-band noise, zero real crossings at vdd=3300mV ---
samples = [1600, 1700, 1600, 1700, 1600, 1700]
tb = uniform(1, samples, 1000)
tr = (1, "vin", 1, 3300, samples)
write_container(os.path.join(OUT_DIR, "fixture_hysteresis.sigc"), [tb], [tr])

# --- sharp-edge: 2 samples, ~1 cycle apart at 64MHz (16ns) ---
tb = uniform(1, [0, 3300], 16)
tr = (1, "vin", 1, 3300, [0, 3300])
write_container(os.path.join(OUT_DIR, "fixture_sharp_edge.sigc"), [tb], [tr])

# --- nonuniform: irregular timestamps, 3 crossings, spaced widely for
# the same interrupt-servicing-latency reason as fixture_4cross above ---
samples = [0, 3300, 0, 3300]
timestamps = [0, 15000, 45000, 70000]
tb = nonuniform(1, samples, timestamps)
tr = (1, "vin", 1, 3300, samples)
write_container(os.path.join(OUT_DIR, "fixture_nonuniform.sigc"), [tb], [tr])

# --- trigger-oneshot: single crossing ---
tb = uniform(1, [0, 3300], 1000)
tr = (1, "vin", 1, 3300, [0, 3300])
write_container(os.path.join(OUT_DIR, "fixture_oneshot.sigc"), [tb], [tr])

# --- group-sync: two identically-scheduled traces, one timebase ---
tb = uniform(1, [0, 3300], 1000)
tr_a = (1, "trace_a", 1, 3300, [0, 3300])
tr_b = (2, "trace_b", 1, 3300, [0, 3300])
write_container(os.path.join(OUT_DIR, "fixture_group_sync.sigc"), [tb], [tr_a, tr_b])

# --- mode-guard: 4 crossings, widely spaced (100us) ---
tb = uniform(1, [0, 3300, 0, 3300, 0], 100000)
tr = (1, "vin", 1, 3300, [0, 3300, 0, 3300, 0])
write_container(os.path.join(OUT_DIR, "fixture_mode_guard.sigc"), [tb], [tr])
