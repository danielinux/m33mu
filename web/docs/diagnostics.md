# Workbench diagnostics

The Diagnostics panel shows current circuit validation issues separately from runtime history. Circuit issues update with project edits. Worker faults, rejected worker requests, unsupported pad operations and failed workbench actions enter a bounded history of 100 distinct messages; repeated messages show a count and their first and latest virtual times when supplied by the worker.

The panel also shows cumulative solver rejected-step and rollback counts, plus sample overflow. These are measurements, not necessarily failures. Clear history removes runtime messages only. Loading a different project, example or firmware starts a new history. Reset keeps the history so a fault can be inspected after recovery.

**Export report** downloads JSON with the current project ID, firmware SHA-256 if loaded, current virtual time, circuit issues, runtime history and latest solver metrics. The report contains diagnostic metadata, not the firmware or scope samples. Export the project and scope capture separately when a full reproduction is needed. Fault events without an exact virtual timestamp show no time instead of inventing one.
