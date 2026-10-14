# m33mu -- an ARMv8-M Emulator
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Reader and writer for the .sigc signal container (docs/signal-injection-gpio.md)."""

import struct
from dataclasses import dataclass
from typing import List, Optional

MAGIC = b"SIGC"
VERSION = 1
NAME_LEN = 32
MAX_TIMEBASES = 16
MAX_TRACES = 32
I16_MIN = -32768
I16_MAX = 32767
U32_MAX = 0xFFFFFFFF
U64_MAX = 0xFFFFFFFFFFFFFFFF


class SigcError(ValueError):
    pass


@dataclass
class Timebase:
    id: int
    sample_count: int
    period_ns: int = 0
    timestamps_ns: Optional[List[int]] = None

    @classmethod
    def uniform(cls, tid, sample_count, period_ns):
        return cls(tid, sample_count, period_ns, None)

    @classmethod
    def explicit(cls, tid, timestamps_ns):
        return cls(tid, len(timestamps_ns), 0, list(timestamps_ns))


@dataclass
class Trace:
    id: int
    name: str
    timebase_id: int
    full_scale_mv: int
    samples_mv: List[int]


def _check_u32(value, what):
    if not 0 <= value <= U32_MAX:
        raise SigcError(f"{what} {value} does not fit in u32")


def validate(timebases, traces):
    if len(timebases) > MAX_TIMEBASES or len(traces) > MAX_TRACES:
        raise SigcError(f"at most {MAX_TIMEBASES} timebases and {MAX_TRACES} traces")
    counts = {}
    for tb in timebases:
        _check_u32(tb.id, "timebase id")
        _check_u32(tb.sample_count, f"timebase {tb.id} sample count")
        _check_u32(tb.period_ns, f"timebase {tb.id} period")
        if tb.id in counts:
            raise SigcError(f"duplicate timebase id {tb.id}")
        if tb.period_ns == 0:
            ts = tb.timestamps_ns
            if not ts or len(ts) != tb.sample_count:
                raise SigcError(f"timebase {tb.id} needs {tb.sample_count} timestamps")
            for i, t in enumerate(ts):
                if not 0 <= t <= U64_MAX:
                    raise SigcError(f"timebase {tb.id} timestamp {i} does not fit in u64")
                if i > 0 and t < ts[i - 1]:
                    raise SigcError(f"timebase {tb.id} timestamps go backwards at sample {i}")
        elif tb.timestamps_ns is not None:
            raise SigcError(f"timebase {tb.id} is uniform but has timestamps")
        counts[tb.id] = tb.sample_count
    ids = set()
    names = set()
    for tr in traces:
        _check_u32(tr.id, "trace id")
        if tr.id in ids:
            raise SigcError(f"duplicate trace id {tr.id}")
        ids.add(tr.id)
        try:
            name_b = tr.name.encode("ascii")
        except UnicodeEncodeError:
            raise SigcError(f"trace name {tr.name!r} is not ASCII") from None
        if not name_b or len(name_b) >= NAME_LEN or b"\0" in name_b:
            raise SigcError(f"trace name {tr.name!r} must be 1 to {NAME_LEN - 1} bytes")
        if tr.name in names:
            raise SigcError(f"duplicate trace name {tr.name!r}")
        names.add(tr.name)
        if tr.timebase_id not in counts:
            raise SigcError(f"trace {tr.name!r} references unknown timebase {tr.timebase_id}")
        if len(tr.samples_mv) != counts[tr.timebase_id]:
            raise SigcError(f"trace {tr.name!r} has {len(tr.samples_mv)} samples, "
                            f"its timebase has {counts[tr.timebase_id]}")
        if not -0x80000000 <= tr.full_scale_mv <= 0x7FFFFFFF:
            raise SigcError(f"trace {tr.name!r} full_scale_mv does not fit in i32")
        for i, s in enumerate(tr.samples_mv):
            if not I16_MIN <= s <= I16_MAX:
                raise SigcError(f"trace {tr.name!r} sample {i} ({s} mV) does not fit in i16")


def encode(timebases, traces):
    validate(timebases, traces)
    out = bytearray(struct.pack("<4sIII", MAGIC, VERSION, len(timebases), len(traces)))
    for tb in timebases:
        out += struct.pack("<III", tb.id, tb.sample_count, tb.period_ns)
        if tb.period_ns == 0:
            out += struct.pack(f"<{tb.sample_count}Q", *tb.timestamps_ns)
    for tr in traces:
        name = tr.name.encode("ascii").ljust(NAME_LEN, b"\0")
        out += struct.pack("<I32sIi", tr.id, name, tr.timebase_id, tr.full_scale_mv)
        out += struct.pack(f"<{len(tr.samples_mv)}h", *tr.samples_mv)
    return bytes(out)


def write(path, timebases, traces):
    data = encode(timebases, traces)
    with open(path, "wb") as f:
        f.write(data)


def decode(data):
    def take(fmt):
        nonlocal pos
        size = struct.calcsize(fmt)
        if pos + size > len(data):
            raise SigcError("truncated container")
        vals = struct.unpack_from(fmt, data, pos)
        pos += size
        return vals

    pos = 0
    magic, version, n_tb, n_tr = take("<4sIII")
    if magic != MAGIC:
        raise SigcError("bad magic")
    if version != VERSION:
        raise SigcError(f"unsupported version {version}")
    timebases = []
    for _ in range(n_tb):
        tid, count, period = take("<III")
        ts = list(take(f"<{count}Q")) if period == 0 else None
        timebases.append(Timebase(tid, count, period, ts))
    counts = {tb.id: tb.sample_count for tb in timebases}
    traces = []
    for _ in range(n_tr):
        trid, name_b, tbid, fs = take("<I32sIi")
        if b"\0" not in name_b:
            raise SigcError(f"trace {trid} name is not NUL-terminated")
        if tbid not in counts:
            raise SigcError(f"trace {trid} references unknown timebase {tbid}")
        samples = list(take(f"<{counts[tbid]}h"))
        traces.append(Trace(trid, name_b.split(b"\0", 1)[0].decode("ascii"), tbid, fs, samples))
    if pos != len(data):
        raise SigcError("trailing data after the last trace")
    validate(timebases, traces)
    return timebases, traces


def read(path):
    with open(path, "rb") as f:
        return decode(f.read())
