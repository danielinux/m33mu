# Scope CSV and SIGC captures

The scope exports its current sample buffer as CSV (`time_ns,ch1,ch2`) or as a SIGC v1 voltage capture. SIGC uses the same little-endian container as `scripts/sigc.py`. The browser reader accepts one timebase, one or two traces, at most 400,000 samples and an 8 MiB file. Uniform and explicit timebases are supported.

SIGC samples are signed 16-bit millivolts. Export rounds voltage samples to the nearest millivolt, names traces `ch1` and `ch2`, records a 3300 mV full-scale metadata value, and makes the first sample time zero. It refuses current channels, missing samples and voltages outside the representable range. Use CSV when those data or absolute timestamps matter.

Import displays a scope preview while the bench is paused. It does not inject voltages into MCU pins or create circuit sources. Running or resetting clears the preview. Project JSON does not store imported samples, so keep the `.sigc` file separately. CLI signal replay remains available through `--signal-file`; see `docs/signal-sigc-convert.md` at the repository root.
