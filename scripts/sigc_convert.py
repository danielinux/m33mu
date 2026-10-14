#!/usr/bin/env python3
# m33mu -- an ARMv8-M Emulator
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Convert a CSV capture (time column plus one column per channel) to a .sigc
container. See docs/signal-sigc-convert.md."""

import argparse
import csv
import os
import sys
from decimal import Decimal, InvalidOperation, ROUND_HALF_EVEN

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sigc  # noqa: E402

TIME_SCALE_NS = {"s": Decimal(10) ** 9, "ms": Decimal(10) ** 6, "us": Decimal(10) ** 3, "ns": Decimal(1)}
VALUE_SCALE_MV = {"V": Decimal(1000), "mV": Decimal(1)}


class ConvertError(Exception):
    pass


def to_int(value):
    return int(value.to_integral_value(rounding=ROUND_HALF_EVEN))


def parse_number(text, line, column):
    text = text.strip()
    if text == "":
        raise ConvertError(f"line {line}: empty cell in column {column!r}")
    try:
        value = Decimal(text)
    except InvalidOperation:
        value = None
    if value is None or not value.is_finite():
        raise ConvertError(f"line {line}: {text!r} in column {column!r} is not a number")
    return value


def read_csv(path, skip_rows, delimiter):
    with open(path, newline="", encoding="utf-8-sig") as f:
        for _ in range(skip_rows):
            f.readline()
        reader = csv.reader(f, delimiter=delimiter)
        header = next(reader, None)
        if not header:
            raise ConvertError(f"{path}: no header row")
        header = [h.strip() for h in header]
        rows = []
        for i, row in enumerate(reader):
            line = skip_rows + 2 + i
            if not any(cell.strip() for cell in row):
                continue
            if len(row) < len(header):
                row = row + [""] * (len(header) - len(row))
            rows.append((line, row))
    if len(set(header)) != len(header):
        raise ConvertError(f"{path}: repeated column names in the header")
    return header, rows


def build_timebase(times_ns, rows):
    for k in range(1, len(times_ns)):
        if times_ns[k] <= times_ns[k - 1]:
            raise ConvertError(f"line {rows[k][0]}: time does not increase "
                               f"(steps under 1 ns cannot be stored; decimate the capture)")
    if len(times_ns) >= 2:
        period = round((times_ns[-1] - times_ns[0]) / (len(times_ns) - 1))
        if period >= 1 and all(abs(t - times_ns[0] - k * period) <= 1 for k, t in enumerate(times_ns)):
            return sigc.Timebase.uniform(1, len(times_ns), period)
    return sigc.Timebase.explicit(1, [t - times_ns[0] for t in times_ns])


def convert(args):
    header, rows = read_csv(args.csv, args.skip_rows, args.delimiter)
    time_col = args.time_col if args.time_col is not None else header[0]
    if time_col not in header:
        raise ConvertError(f"no time column {time_col!r} in {header}")
    channels = args.channels.split(",") if args.channels else [h for h in header if h != time_col]
    for ch in channels:
        if ch not in header or ch == time_col:
            raise ConvertError(f"no channel column {ch!r} in {header}")
    if not channels:
        raise ConvertError("no channel columns")
    renames = {}
    for spec in args.rename:
        col, sep, name = spec.partition("=")
        if not sep or col not in channels:
            raise ConvertError(f"--rename {spec!r}: expected COL=NAME with COL one of {channels}")
        renames[col] = name

    t_idx = header.index(time_col)
    t_scale = TIME_SCALE_NS[args.time_unit]
    crop = Decimal(args.crop_before) if args.crop_before is not None else None
    kept = []
    for line, row in rows:
        t = parse_number(row[t_idx], line, time_col)
        if crop is None or t >= crop:
            kept.append((line, row, t))
    if not kept:
        raise ConvertError("no samples left after --crop-before")

    times_ns = [to_int(t * t_scale) for _, _, t in kept]
    origin_ns = times_ns[0]
    times_ns = [t - origin_ns for t in times_ns]
    timebase = build_timebase(times_ns, kept)

    traces = []
    v_scale = VALUE_SCALE_MV[args.value_unit]
    for n, ch in enumerate(channels):
        c_idx = header.index(ch)
        samples = []
        for line, row, _ in kept:
            v = parse_number(row[c_idx], line, ch)
            if args.digital is not None:
                if v not in (0, 1):
                    raise ConvertError(f"line {line}: {row[c_idx].strip()!r} in column {ch!r} is not 0 or 1")
                mv = args.digital if v == 1 else 0
            else:
                mv = to_int(v * v_scale)
            if not sigc.I16_MIN <= mv <= sigc.I16_MAX:
                raise ConvertError(f"line {line}: {mv} mV in column {ch!r} does not fit in i16")
            samples.append(mv)
        traces.append(sigc.Trace(n + 1, renames.get(ch, ch), 1, args.full_scale_mv, samples))

    try:
        sigc.write(args.output, [timebase], traces)
    except sigc.SigcError as e:
        raise ConvertError(str(e)) from None

    kind = f"uniform, {timebase.period_ns} ns" if timebase.period_ns else "explicit timestamps"
    print(f"wrote {args.output}: {len(traces)} trace(s) x {timebase.sample_count} samples ({kind})")
    print(f"  traces: {', '.join(t.name for t in traces)}")
    if origin_ns != 0:
        print(f"  shifted times by {-origin_ns} ns so the first sample is at t = 0")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("csv")
    ap.add_argument("-o", "--output", required=True, help="output .sigc file")
    ap.add_argument("--time-col", help="name of the time column (default: the first column)")
    ap.add_argument("--time-unit", choices=sorted(TIME_SCALE_NS), default="s")
    ap.add_argument("--value-unit", choices=sorted(VALUE_SCALE_MV), default="V")
    ap.add_argument("--channels", help="comma-separated columns to convert (default: all but time)")
    ap.add_argument("--rename", action="append", default=[], metavar="COL=NAME",
                    help="trace name for a column, repeatable")
    ap.add_argument("--digital", type=int, metavar="MV",
                    help="columns hold logic 0/1; 1 becomes MV mV")
    ap.add_argument("--skip-rows", type=int, default=0, metavar="N",
                    help="metadata lines before the header")
    ap.add_argument("--delimiter", default=",", help="field separator, '\\t' for tab (default: ',')")
    ap.add_argument("--crop-before", metavar="T", help="drop samples before time T (in --time-unit)")
    ap.add_argument("--full-scale-mv", type=int, default=3300)
    args = ap.parse_args(argv)
    if args.delimiter == "\\t":
        args.delimiter = "\t"
    if len(args.delimiter) != 1:
        ap.error("--delimiter must be one character")
    if args.crop_before is not None:
        try:
            Decimal(args.crop_before)
        except InvalidOperation:
            ap.error(f"--crop-before {args.crop_before!r} is not a number")
    try:
        convert(args)
    except (ConvertError, OSError) as e:
        print(f"sigc_convert: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
