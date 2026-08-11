# m33mu — External Signal Injection Design: ADC (Phase 2)

Status: **design in progress — needs more work before implementation
starts.** Not agreed to the same level as the GPIO phase.
Depends on: `m33mu-signal-injection-gpio.md` (Phase 1) — this phase builds on
top of the shared `signal.c` core, container format, and master-trigger
mechanism established there, and doesn't change their shape. Read that doc
first; this one only covers what's ADC-specific or still unresolved.
Target: STM32H5F4 profile (`cpu/stm32h5f4/`), same as Phase 1.
Scope: model a voltage-vs-time trace consumable by an ADC channel
(`stm32_adc.c`, new module), sharing the same firmware-defined time origin
and container format as GPIO bindings, so a single trace can drive an ADC
and a GPIO pin simultaneously from one capture.

---

## 1. Decisions made so far (ADC-specific)

| # | Question | Decision | Rationale |
|---|---|---|---|
| 12 | ADC conversion trigger model | Start with **software single-conversion** (`CR.ADSTART` → delay → `ISR.EOC` → `DR`), then **continuous mode** (`CFGR.CONT`) as a trivial extension, then **timer-triggered** (`EXTSEL`/`EXTEN`) later, reusing the same tick hook | Covers the majority of firmware ADC usage first; timer-trigger needs no new time source, just a new caller of the same tick |
| 18 | ADC reference voltage | **Separate global CLI flag (`--adc-vref_mv`)**, defaults to `--vdd_mv` if omitted, but independently overridable — never assumed equal to `VDD` | On STM32H5, ADC full-scale is set by `VREF+`, which sits on a distinct supply chain (`VDD` → possibly-independent `VDDA` → possibly-independent `VREF+`) — `VREF+` can be external, sourced from the internal `VREFBUF` (configurable ~1.8V/~2.048V/~2.5V), or bonded straight to `VDDA` depending on package. Defaulting to `VDD` matches the common case (smaller packages, `VREF+` tied to `VDDA` tied to `VDD`) without hardcoding it |

Note on `full_scale_mv` vs. `--adc-vref_mv`: the container's per-trace
`full_scale_mv` field (see GPIO doc §3.1) describes the *trace's own recorded
voltage range* (how the sample data was captured/scaled on disk), which is
conceptually independent from `adc-vref_mv` (the *emulated ADC's*
quantization reference). The intent is `stm32_adc.c` uses `adc-vref_mv`, not
a trace's `full_scale_mv`, when converting a sampled millivolt value into a
digital code — but see open item below, since nothing currently consumes
`full_scale_mv` at all.

## 2. Open items / concerns raised in review — need resolution before build

These came out of design review and aren't yet decided. Listed roughly in
order of how much they'd block a first working `stm32_adc.c`.

### 2.1 `ADRDY`/calibration are missing from the register model — likely blocking

The current sketch of the register model (§4 below) only covers `CR`
(`ADEN`, `ADSTART`), `CFGR` (`CONT`), `ISR`/`IER` (`EOC`), `DR`, `SQR1`.
There's no `ADRDY` and no `ADCAL`.

On real H5 silicon — and inside `HAL_ADC_Start()` — the sequence is:
calibrate (`ADCAL`) → set `ADEN` → **poll `ISR.ADRDY`** → then `ADSTART`. If
`ADRDY` is never modeled/set, any HAL-based test firmware will spin forever
before ever reaching the signal-injection path, and the ADC phase's tests
(§5 below) simply won't run against realistic firmware.

Needs a decision — even a deliberately simple one, e.g. "`ADCAL` is a no-op
that completes instantly, `ADRDY` sets itself the instruction after `ADEN`
is written" — but it needs to be an explicit line in this doc, not an
implicit gap.

### 2.2 No clamping/saturation rule for the ADC quantizer

"Quantize sampled mV against `adc-vref_mv`" (decision #12, and the
implementation trace below) needs an explicit formula, and, more
importantly, explicit behavior for out-of-range input — a trace sample above
`adc-vref_mv` or below 0 mV. Real ADCs saturate at `0x000`/`0xFFF` (for
12-bit); they don't wrap or produce undefined codes. This should be a stated
rule (`code = clamp(round(v_mv / vref_mv * 4095), 0, 4095)`, or similar) so
the implementer isn't guessing.

### 2.3 `full_scale_mv`'s actual role is unclear

As noted above, the container format defines `full_scale_mv` per trace, and
the design narrative distinguishes it from `adc-vref_mv`, but no code shown
anywhere — sample lookup, GPIO threshold logic, or the sketch of the ADC
quantization path — actually reads it. Before implementing `stm32_adc.c`'s
quantization step, decide: is this field pure provenance metadata from the
HDF5 conversion (and therefore fine to leave unused by the emulator), or is
it supposed to affect scaling in some way that hasn't been written down yet?
If it's metadata-only, say so explicitly in the format doc so it's not
mistaken for a wiring bug later.

### 2.4 Trigger-symbol fragility against inlining/LTO

Shared concern with the master-trigger mechanism from the GPIO phase, but
worth re-flagging here because ADC test firmware is more likely to anchor
the trigger on a HAL entry point (`HAL_ADC_Start`) than GPIO firmware is. If
that symbol gets inlined into its caller under a release/LTO build, `nm`
either won't find it or will resolve the wrong address, and the trigger
silently never fires — meaning ADC bindings sit at `elapsed_ns` uninitialized
and the test reads whatever the pre-trigger state happens to be, which could
pass or fail for the wrong reason. Recommend test firmware use a dedicated
`__attribute__((noinline))` marker function reached right before/after the
real HAL call, and add a diagnostic for "master trigger configured but never
fired" so this doesn't fail silently.

### 2.5 H5F4 as the first ADC target — worth a sentence of justification

STM32H5E4/H5F4 (covered by RM0517, confirmed distinct from RM0481's
H523/533/H562/563/573 family) is one of the newer, more feature-dense H5
variants — graphics/crypto-oriented, with Chrom-ART, JPEG, LTDC, and the PLAY
peripheral alongside the ADC. That's fine if it's simply what the existing
fleet/CI already targets, but if the near-term goal is specifically to shake
out a correct ADC register model, a simpler H5 variant already covered by
`cpu/stm32_*` (e.g. one under RM0481) might have been lower-friction to
prototype against first, with H5F4-specific wiring (real base addresses,
`ADC1_2_IRQn`) layered on after the model itself is validated. Worth a line
on why H5F4 specifically is the first ADC target, if it's not just "that's
the existing repo's primary profile."

## 3. Runtime architecture (ADC leg)

Extends the GPIO phase's diagram (see that doc §4) with one more consumer of
`signal.c`'s `mm_signal_sample(binding_id)`:

```
┌────────────────────────────────────────────────────────────────────┐
│ src/signal.c  (built in Phase 1, unchanged here)                     │
└───────────┬───────────────────────────────────────────────────────┘
            │ mm_signal_sample(binding_id)
            ▼
┌────────────────────────┐
│ cpu/stm32_adc.c         │
│ on ADSTART: quantize    │
│ sampled mV into DR,     │
│ set EOC, optional IRQ   │
└────────────────────────┘
```

`include/m33mu/signal.h`'s existing surface (`mm_signal_bind`,
`mm_signal_sample`, `mm_signal_group_trigger`) is reused as-is; the only
addition needed there is the ADC reference-voltage setter:

```c
void mm_signal_set_adc_vref_mv(mm_i32 vref_mv);     /* defaults to vdd_mv if never called */
```

## 4. Implementation trace (suggested order, ADC phase)

Picks up after the GPIO phase's item 5 (first end-to-end GPIO test passing).

6. **`cpu/stm32_adc.h` / `cpu/stm32_adc.c`** (new shared module, mirrors
   `cpu/stm32_timers.h`/`.c`)
   - Register model: `CR` (`ADEN`, `ADSTART`), `CFGR` (`CONT`), `ISR`/`IER`
     (`EOC`), `DR`, `SQR1` — **plus `ADRDY`/`ADCAL` per open item 2.1 above,
     to be resolved before writing this**.
   - On `ADSTART`: `mm_signal_sample(binding)` → quantize against
     `adc-vref_mv` (not a trace's own `full_scale_mv` — see open item 2.3)
     → clamp per open item 2.2 → `DR`, set `EOC`, optional NVIC IRQ.
   - `stm32_adc_soc` struct for per-chip RCC/IRQ config (same shape as
     `stm32_timers_soc`).

7. **`cpu/stm32h5f4/stm32h5f4_adc.c/.h`** (new, mirrors
   `stm32h5f4_timers.c`) — per-chip glue: real ADC1/ADC2 base addresses
   (confirm from RM0517 memory map — STM32H5E4/H5F4/H5E5/H5F5 reference
   manual, not RM0481 which covers the H523/533/543/553/562/563/573 family)
   and RCC/NVIC wiring.

8. **`cpu/stm32h5f4/stm32h5f4_mmio.c`** — register the ADC1/ADC2 MMIO
   region (currently entirely unmapped, unlike LPC55S69's stubbed `ADC0`).

9. **`include/m33mu/target.h`** — add `adc_init`/`adc_reset`/`adc_tick`
   function pointers to `mm_target_cfg`, same shape as the existing
   `timer_*` triple; wire STM32H5F4's `mm_target_cfg` instance to the new
   `stm32h5f4_adc.c` functions.

10. **Tests** — `tests/stm32_adc_test.c` (register-level conversion state
    machine, following the `stm32_timers_test.c` harness pattern), and the
    firmware integration tests in §5 below.

## 5. Testing (ADC phase — deferred until the register model lands)

- `gpio-adc-shared-trace` — one trace bound to both an ADC channel and a
  GPIO pin in the same group; firmware reads `ADC1->DR` and the GPIO `IDR`
  back-to-back and both should reflect the same instant of the same
  physical signal. Validates decision #6 from the GPIO doc (shared timebase
  across peripheral types) end-to-end, but depends on the ADC register
  model above being implemented first — not buildable until then.
- Additional ADC-only test firmware (single-conversion value accuracy,
  continuous-mode re-triggering, EOC/IRQ timing) still needs to be scoped
  once the register model (§4 items 6-9) and the open items in §2 are
  resolved — not detailed yet, unlike the GPIO phase's test matrix.

## 6. Other deferred items (not blocking, not yet scheduled)

- **Timer-triggered ADC conversion** (`EXTSEL`/`EXTEN`) — deferred until
  software-single-conversion and continuous mode are working (see decision
  #12).
- **Confirm real STM32H5F4 ADC1/ADC2 base addresses and `ADC1_2_IRQn`
  number** from **RM0517** (STM32H5E4/H5F4 and H5E5/H5F5 reference manual —
  m33mu's own `cpu/stm32h5_mmio.c` already cites RM0517 for H5F4's AHB2ENR
  bit layout, so this is the correct manual, not RM0481) before writing
  `stm32h5f4_adc.h`.
- **GPDMA `TRIGSEL`/EXTI trigger gating** — real STM32H5 silicon lets an
  EXTI-sourced GPIO edge gate a GPDMA memory-to-memory transfer;
  `cpu/stm32_gpdma.c` currently starts a channel immediately on `CxCR.EN`
  and doesn't model trigger sources at all. Independent, non-trivial work on
  the GPDMA model itself — only worth building if test firmware specifically
  exercises triggered-GPDMA+EXTI+GPIO capture. (Also noted in the GPIO doc,
  since the trigger source is a GPIO/EXTI event even though the gated
  peripheral is DMA, not ADC — listed here too since it's squarely
  "needs-more-work" territory.)
