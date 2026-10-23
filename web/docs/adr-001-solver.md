# ADR-001: primary circuit solver

**Status:** Open. No solver has passed the live firmware-coupling gate.  
**Date:** 2026-10-23

## Context

The workbench requires deterministic transient stepping with source changes during a run, state preservation, worker yielding, pause/resume, and event localization at MCU input thresholds. A completed batch `.tran` result is insufficient because firmware GPIO writes and circuit voltages must share virtual time.

## Evidence so far

- The public `eecircuit-engine` TypeScript API (`eelab-dev/EEcircuit-engine` at `75594eec516be3087e64eeb5347a958de70f410c`, MIT) exposes `setNetList()` and `runSim()`. Its implementation runs `source test.cir`, `destroy all`, `run`, and then reads `out.raw`. There is no public incremental step/source-update API. **Reject this wrapper as-is** for the live workbench; the underlying WASM core could be reconsidered only after an incremental control API and its license/model provenance are demonstrated.
- CircuitJS1 at `778723096a46f3c5ce4e11bd9edc24324754f02c` is GPL-2.0-or-later. `CircuitMatrix` is a plausible numerical component, but its `CirSim` controller imports menus, mouse handling, scopes, serializers, and element/model code. No isolated kernel or live-update test has been produced. Keep as the next extraction candidate, not a selected dependency.
- A local native ngspice Shared Library experiment did not produce a valid transient waveform. No ngspice executable or development headers are available, and an Emscripten build of ngspice has not been demonstrated. This is not evidence for or against the shared library's theoretical synchronization API.

## Decision

Defer selection. Do not wire firmware GPIO into an assumed ideal voltage source. Continue with a bounded CircuitJS kernel extraction spike and, separately, a reproducible ngspice Shared Library WASM build/control spike. Select one only after the same divider, RC, diode, BJT, and GPIO/RC fixtures pass documented numerical and causal-ordering checks.

## Required evidence to close

1. Mid-transient source changes without resetting unrelated reactive state.
2. A controllable simulation quantum, bounded worker execution, pause/resume, and deterministic replay.
3. Firmware output edge -> analog RC threshold crossing -> GPIO/EXTI event with ordering and timing tolerance measured against a reference fixture.
4. Numerical fixture comparisons, solver build ID, license notices and complete model/dependency provenance.
