# Converting captures to `.sigc`

Real stimuli come from oscilloscopes and logic analysers, which export CSV.
`scripts/sigc_convert.py` turns such a CSV into a `.sigc` container for
`--signal-file`. It and the fixture generator share one encoder,
`scripts/sigc.py`.

## Converter: `scripts/sigc_convert.py`

```sh
scripts/sigc_convert.py capture.csv -o stimulus.sigc \
    --skip-rows 2 --crop-before 0 --rename "CH1 (V)=vin"
```

Input: a CSV with a header row, one time column, and one column per channel.

| Option | Meaning | Default |
|---|---|---|
| `-o PATH` | output container | required |
| `--time-col NAME` | name of the time column | first column |
| `--time-unit s\|ms\|us\|ns` | unit of the time column | `s` |
| `--value-unit V\|mV` | unit of the channel columns | `V` |
| `--channels A,B,...` | columns to convert | every column except time |
| `--rename COL=NAME` | trace name for a column, repeatable | the column header |
| `--digital MV` | columns hold logic 0/1; 1 becomes `MV` mV | off |
| `--skip-rows N` | metadata lines before the header | 0 |
| `--delimiter C` | field separator, `\t` for tab | `,` |
| `--crop-before T` | drop samples before time `T` (in `--time-unit`) | keep all |
| `--full-scale-mv MV` | value written in `full_scale_mv` | 3300 |

Rules:

- **Time origin.** The first kept sample becomes t = 0, and the converter
  prints the shift it applied. Scope exports often start at a negative time
  (pre-trigger); `--crop-before 0` starts the trace at the scope trigger.
  Replay starts at `--signal-sync`, so t = 0 is where the firmware marker
  sits.
- **Timebase.** All channels of one CSV share one timebase. Times are rounded
  to whole nanoseconds. If every sample lies within 1 ns of a whole-ns grid
  (`t0 + k * period`), the timebase is uniform; otherwise it keeps explicit
  timestamps, so a 2.5 ns step is stored exactly rather than drifting.
  A time that does not increase is an error, with the line number. The
  container counts whole nanoseconds, so a capture sampled faster than 1 GS/s
  is an error too: decimate it in the scope software first.
- **Values.** Converted to mV and rounded to the nearest integer. A value
  outside the `i16` range, an empty cell or a non-number is an error, with the
  line and column. The converter does not interpolate.
- **Names.** The column header or `--rename`; a name that is not ASCII, is 32
  bytes or longer, or repeats another is an error. Spaces are kept.
- **Logic analyser exports** (for example Saleae's `Time [s],Channel 0,...`,
  one row per change) fit as they are: rows at irregular times become an
  explicit timebase, and `--digital 3300` turns 0/1 into mV.

Out of scope: scope-specific binary formats, resampling or downsampling,
several CSVs into one container (run the converter once per capture).

## Shared module: `scripts/sigc.py`

Standard library only. `Timebase` and `Trace` describe a container;
`write`/`encode` produce it and `read`/`decode` parse it.

`encode` checks everything the loader checks, so a bad container fails in
Python with a clear message rather than in m33mu: names are ASCII, under 32
bytes and unique; ids are unique; every trace references an existing
timebase and has its sample count; samples fit in `i16`; explicit timestamps
never go backwards; at most 16 timebases and 32 traces.

`tests/firmware/_signal_common/gen_fixtures.py` keeps its fixture definitions
and imports the module. It takes an optional output directory, so CI can
regenerate the fixtures into a temporary directory and compare.

## Tests

- `scripts/test_sigc.py` (`python3 -m unittest discover -s scripts`, also
  registered in ctest as `sigc_python_test` when `python3` is found): round
  trip, byte layout, uniform versus explicit detection, every validation
  error, the converter options.
- `test-firmware.yml` regenerates the fixtures and checks they are
  byte-identical to the committed ones.
- `tests/firmware/_signal_common/sample_capture.csv` is a small scope-style
  capture (metadata lines, negative start time, volts). CI converts it and
  runs `test-stm32h5f4-signal-gpio-basic` with the result.

## Decisions

1. Both files live in `scripts/`.
2. The first kept sample becomes t = 0, and `--crop-before T` drops earlier
   samples first.
3. The Python tests run in ctest when `python3` is found, and in CI.
