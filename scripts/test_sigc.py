# m33mu -- an ARMv8-M Emulator
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Unit tests for sigc.py and sigc_convert.py: python3 -m unittest discover -s scripts"""

import contextlib
import io
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sigc  # noqa: E402
import sigc_convert  # noqa: E402


def uniform_container(**trace_overrides):
    tb = sigc.Timebase.uniform(1, 2, 1000)
    fields = dict(id=1, name="vin", timebase_id=1, full_scale_mv=3300, samples_mv=[0, 3300])
    fields.update(trace_overrides)
    return [tb], [sigc.Trace(**fields)]


class SigcTest(unittest.TestCase):
    def test_round_trip_uniform_and_explicit(self):
        tbs = [sigc.Timebase.uniform(1, 3, 250), sigc.Timebase.explicit(2, [0, 10, 10, 400])]
        trs = [sigc.Trace(7, "a", 1, 3300, [-5, 0, 32767]),
               sigc.Trace(8, "b", 2, 1800, [1, 2, 3, -32768])]
        self.assertEqual(sigc.decode(sigc.encode(tbs, trs)), (tbs, trs))

    def test_layout_matches_loader(self):
        data = sigc.encode(*uniform_container())
        self.assertEqual(data[:16], struct.pack("<4sIII", b"SIGC", 1, 1, 1))
        self.assertEqual(data[16:28], struct.pack("<III", 1, 2, 1000))
        self.assertEqual(data[28:72], struct.pack("<I32sIi", 1, b"vin".ljust(32, b"\0"), 1, 3300))
        self.assertEqual(data[72:], struct.pack("<2h", 0, 3300))

    def test_rejects_invalid_containers(self):
        bad = [
            uniform_container(name="x" * 32),
            uniform_container(name=""),
            uniform_container(name="vïn"),
            uniform_container(samples_mv=[0, 32768]),
            uniform_container(samples_mv=[0]),
            uniform_container(timebase_id=2),
            ([sigc.Timebase.explicit(1, [0, 5, 4])], [sigc.Trace(1, "v", 1, 0, [0, 0, 0])]),
            ([sigc.Timebase.uniform(1, 1, 1), sigc.Timebase.uniform(1, 1, 1)], []),
            ([sigc.Timebase.uniform(1, 1, 1)],
             [sigc.Trace(1, "v", 1, 0, [0]), sigc.Trace(1, "w", 1, 0, [0])]),
            ([sigc.Timebase.uniform(1, 1, 1)],
             [sigc.Trace(1, "v", 1, 0, [0]), sigc.Trace(2, "v", 1, 0, [0])]),
            ([sigc.Timebase.uniform(i, 1, 1) for i in range(17)], []),
        ]
        for tbs, trs in bad:
            with self.assertRaises(sigc.SigcError):
                sigc.encode(tbs, trs)

    def test_decode_rejects_damaged_data(self):
        data = sigc.encode(*uniform_container())
        for damaged in (data[:-1], data + b"\0", b"XXXX" + data[4:]):
            with self.assertRaises(sigc.SigcError):
                sigc.decode(damaged)
        unterminated = bytearray(data)
        unterminated[32:64] = b"y" * 32
        with self.assertRaises(sigc.SigcError):
            sigc.decode(bytes(unterminated))


class ConvertTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.out = os.path.join(self.dir.name, "out.sigc")

    def tearDown(self):
        self.dir.cleanup()

    def run_convert(self, text, *args):
        src = os.path.join(self.dir.name, "in.csv")
        with open(src, "w", newline="") as f:
            f.write(text)
        stderr = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
            rc = sigc_convert.main([src, "-o", self.out, *args])
        return rc, stderr.getvalue()

    def converted(self, text, *args):
        rc, err = self.run_convert(text, *args)
        self.assertEqual(rc, 0, err)
        return sigc.read(self.out)

    def test_uniform_scope_capture(self):
        csv_text = "meta\nTime (s),CH1,CH2\n-1.0e-6,0.0,3.3\n-9.0e-7,1.65,0\n-8.0e-7,3.3,-0.0004\n"
        (tb,), trs = self.converted(csv_text, "--skip-rows", "1", "--rename", "CH1=vin")
        self.assertEqual((tb.period_ns, tb.sample_count), (100, 3))
        self.assertEqual([t.name for t in trs], ["vin", "CH2"])
        self.assertEqual(trs[0].samples_mv, [0, 1650, 3300])
        self.assertEqual(trs[1].samples_mv, [3300, 0, 0])

    def test_float_noise_stays_uniform(self):
        csv_text = "t,v\n0,0\n9.99999999e-08,1\n2.00000001e-07,0\n3e-07,1\n"
        (tb,), _ = self.converted(csv_text)
        self.assertEqual(tb.period_ns, 100)

    def test_irregular_times_are_explicit(self):
        csv_text = "Time [s],Channel 0\n0.5,0\n0.5000015,1\n0.500004,0\n"
        (tb,), (tr,) = self.converted(csv_text, "--digital", "3300", "--rename", "Channel 0=btn")
        self.assertEqual(tb.period_ns, 0)
        self.assertEqual(tb.timestamps_ns, [0, 1500, 4000])
        self.assertEqual((tr.name, tr.samples_mv), ("btn", [0, 3300, 0]))

    def test_fractional_ns_period_is_explicit(self):
        rows = "".join(f"{k * 2.5},{k % 2}\n" for k in range(40))
        (tb,), _ = self.converted("t,v\n" + rows, "--time-unit", "ns", "--value-unit", "mV")
        self.assertEqual(tb.period_ns, 0)
        self.assertEqual(tb.timestamps_ns[:5], [0, 2, 5, 8, 10])

    def test_crop_before_and_options(self):
        csv_text = "v;t\n1;-2\n2;-1\n3;0\n4;1\n"
        (tb,), (tr,) = self.converted(csv_text, "--time-col", "t", "--time-unit", "us",
                                      "--value-unit", "mV", "--delimiter", ";",
                                      "--crop-before", "0", "--full-scale-mv", "1800")
        self.assertEqual((tb.period_ns, tb.sample_count), (1000, 2))
        self.assertEqual((tr.samples_mv, tr.full_scale_mv), ([3, 4], 1800))

    def test_errors(self):
        cases = [
            ("t,v\n0,0\n0,1\n", [], "does not increase"),
            ("t,v\n0,0\n1e-10,1\n", [], "does not increase"),
            ("t,v\n0,0\n1,\n", [], "empty cell"),
            ("t,v\n0,0\n1,abc\n", [], "not a number"),
            ("t,v\n0,0\n1,nan\n", [], "not a number"),
            ("t,v\n0,40\n", [], "does not fit in i16"),
            ("t,v\n0,0.5\n", ["--digital", "3300"], "not 0 or 1"),
            ("t,v\n0,0\n", ["--channels", "w"], "no channel column"),
            ("t,v\n0,0\n", ["--rename", "w=x"], "--rename"),
            ("t," + "x" * 32 + "\n0,0\n", [], "must be 1 to 31 bytes"),
            ("t,v\n-1,0\n", ["--crop-before", "0"], "no samples left"),
            ("t,v,v\n0,0,0\n", [], "repeated column"),
        ]
        for text, args, message in cases:
            rc, err = self.run_convert(text, *args)
            self.assertEqual(rc, 1, (text, args))
            self.assertIn(message, err, (text, args))

    def test_sample_capture_fixture(self):
        here = os.path.dirname(os.path.abspath(__file__))
        src = os.path.join(here, "..", "tests", "firmware", "_signal_common", "sample_capture.csv")
        with contextlib.redirect_stdout(io.StringIO()):
            rc = sigc_convert.main([src, "-o", self.out, "--skip-rows", "2",
                                    "--crop-before", "0", "--rename", "CH1 (V)=vin"])
        self.assertEqual(rc, 0)
        (tb,), (tr,) = sigc.read(self.out)
        self.assertEqual((tb.period_ns, tb.sample_count, tr.name), (100, 51, "vin"))
        self.assertEqual(tr.samples_mv[0], 0)
        self.assertEqual(tr.samples_mv[-1], 3300)


if __name__ == "__main__":
    unittest.main()
