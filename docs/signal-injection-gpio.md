# m33mu — External Signal Injection Design: GPIO (Phase 1)

Status: design agreed, **first increment implemented and verified** on the
`signal_injection_gpio` branch: core module (`signal.h`/`signal.c`), the
`mm_gpio_set_external_input()` registration path, CLI surface, and one
end-to-end test firmware all build cleanly and pass, alongside the full
existing regression suite (68/68). Not yet done: the fuller GPIO test
matrix in §7 (only the basic rising-crossing case has an end-to-end
firmware so far; the hysteresis, EXTI-mask, non-uniform-timebase, and
one-shot/group-sync cases described in §7.1 are still to be written), and
the offline HDF5→`.sigc` converter script (§5 item 2) hasn't been started
-- the current fixture used for testing was hand-built directly in the
on-disk format for validation purposes.
Target: STM32H5F4 profile (`cpu/stm32h5f4/`), built on the shared `cpu/stm32_*`
layer. m33mu's repo currently also carries `stm32h533` and `stm32h563` under
the same shared layer — this phase's GPIO work targets H5F4 specifically, but
the shared `signal.c` core (§4) is peripheral- and target-agnostic, so porting
to the sibling H5 variants later needs no re-derivation of the signal model.
Scope: model a voltage-vs-time trace applied to an MCU pin, consumable by a
GPIO input (both interrupt-driven and polled) and read back from a GPIO
output for verification, sharing a single, firmware-defined time origin, for
use in CI/CD regression testing. This phase also builds the shared signal
core (container format, timebase/trace model, master trigger) that the
follow-on ADC phase (see `m33mu-signal-injection-adc.md`) will depend on
without re-deriving.

---

## 1. Background: how m33mu tracks time (already in place, no changes needed)

- One virtual cycle = one executed instruction (`insn_cycles = 1`), accumulated
  into `vcycles` per instruction, in `src/main.c` (~line 3575-3586). This is
  also the fan-out point for `mm_timer_tick()` and `mm_scs_systick_advance()` —
  our new `mm_signal_tick()` and PC-watch hook go in the same place.
- Wall-clock pacing is separate: `deadline_ns()` converts `vcycles` to
  nanoseconds using a nominal core clock (`MM_CPU_HZ`), and
  `host_sync_if_needed()` sleeps against `CLOCK_MONOTONIC` to keep emulation
  paced to real time. This is orthogonal to signal replay — signal replay
  uses cycle-derived elapsed time, not the host wall clock.
- **STM32H5 runs its Cortex-M33 core up to 250 MHz** (not the generic 64 MHz
  default seen in `main.c`). At 250 MHz, 1 cycle ≈ 4 ns. The real per-target
  `cpu_hz` (wherever the STM32H5 profile sets it) must be used for any
  cycle→ns conversion touching signal replay, not the generic default.
- Note: "1 instruction = 1 cycle" is an existing emulator-wide simplification,
  not something this design changes. Timing tests here (e.g.
  `gpio-input-sharp-edge`) validate consistency with the *emulator's own*
  instruction-count model, not real Cortex-M33 sub-cycle/wait-state timing.

## 2. Decisions made in this design

### 2.1 Shared signal-core decisions (apply to GPIO now, ADC later)

| # | Question | Decision | Rationale |
|---|---|---|---|
| 1 | Target SoC | STM32H5 (`cpu/stm32h5f4/`) | Follows existing `cpu/stm32_*` shared-layer pattern (timers/usart/spi/gpio already split this way) |
| 2 | Read HDF5 at runtime? | **No.** Convert offline to a custom binary container. | No precedent for heavy format parsing in the C core; keeps every CI target's build minimal; HDF5's features (chunking/compression/metadata) buy nothing for a flat (t, v) replay |
| 3 | Interpolation between samples | **Zero-order hold** (most recent sample, no linear interpolation) | More physically honest for sharp edges; matches "connect a signal generator to a pin" semantics; cheaper (binary search only, no lerp) |
| 4 | Timestamp units | **Nanoseconds**, `mm_u64` | Matches existing `deadline_ns()`/`host_now_ns()` units; at 250 MHz (4 ns/cycle) this is close to the emulator's real time resolution, no need for cycle-unit timestamps |
| 5 | Non-uniform sampling | Supported: explicit `timestamps_ns[]` array per timebase, when `period_ns == 0` | Real captures/HDF5 sources are rarely uniformly sampled |
| 6 | Multiple traces, multiple pins | A single **container file** holds N timebases and M traces; each trace references one timebase; traces bind independently to peripherals (ADC channel, GPIO pin, etc.) | Same physical net can drive an ADC and a GPIO EXTI input simultaneously; traces sharing a timebase are guaranteed sample-aligned (real simultaneous capture) |
| 7 | Per-binding replay start ("t=0") | **Not** anchored to CPU boot, and **not** independently anchored per-peripheral (`ADEN`/EXTI-enable) either — see #8 | Boot-anchored timing is non-reproducible across unrelated code changes (different init path length shifts the sample offset); independent per-peripheral anchoring breaks cross-peripheral tests where relative timing between ADC and GPIO matters |
| 8 | Common time origin across all traces | **One external "master trigger"**, fired once, resets `elapsed_ns = 0` for every bound trace simultaneously (`mm_signal_group_trigger()`) | Matches a real bench setup (scope external trigger line qualifying all channels); decouples "when the simulated world starts" from firmware init ordering |
| 9 | How the master trigger fires, without modifying firmware under test | **PC watchpoint**: emulator compares `pc` against a configured address every instruction (same tick site as `vcycles`); on match, fires the trigger. Address is resolved from the **unmodified** firmware's own ELF/symbol table (e.g. `main`, a vendor HAL/CMSIS symbol already linked in, or a call's return address) — no source change, no relink, no injected instrumentation | Existing `src/gdbstub.c` breakpoints patch a `BKPT` opcode into memory (byte backup/restore) and halt the CPU — correct for interactive debugging, wrong here: we want a silent, non-halting, side-effect-free tap. A plain PC comparison costs one integer compare per instruction and touches no memory, working identically in flash/XIP/RAM |

**Implementation note (confirmed against the real codebase, not just assumed):** m33mu has two distinct execution paths that both need this check — a scalar per-instruction interpreter (used as a fallback and for a second CPU core on multi-core targets) and a translation-block (tb) fast path that executes a whole cached basic block per `mm_tb_run()` call, batching cycle/timer accounting (`mm_timer_tick(&cfg, ops_executed)`) once per block rather than per instruction. A plain "compare every instruction" implementation only works on the scalar path; on the tb path, PC is only observable at block-dispatch granularity. The trigger check must therefore run **before `mm_tb_run()`** using the same PC used for `mm_tb_lookup()`/`mm_tb_build()`, not inside a per-instruction tick. This is safe specifically because a translation block boundary is, by construction, a branch target (blocks end at branches and start at their destinations) — so a trigger address that is itself a function entry or other branch target (exactly what decision #9 already recommends) is always observed at a block boundary. A trigger address that only occurs *mid-block* would never fire under the tb path; this is a real constraint of the architecture, not a bug to work around. See the implementation's `mm_signal_check_trigger(pc)` (called once per dispatch, before execution) vs. `mm_signal_advance(cycles)` (called once per dispatch, after execution, with a possibly-batched cycle count) split in `signal.h`/`signal.c` for how this was actually implemented -- a single combined `mm_signal_tick(cycles, pc)` function, called after execution the same way `mm_timer_tick()` is, silently never observes any PC on the tb path (this exact bug was hit and fixed during implementation: the trigger simply never fired, with no error, until the check was moved before `mm_tb_run()`).
| 10 | Trigger re-arming | **One-shot.** Fires once per emulator run, then permanently disarmed. | One test case = one fresh emulator invocation = one deterministic t=0. Matches desired CI isolation (no cross-test state); repeated-scenario needs are explicitly out of scope for this mechanism |
| 11 | Trigger address resolution (v1 vs v2) | v1: resolve externally (`arm-none-eabi-nm firmware.elf \| grep <symbol>`), pass raw hex via CLI. v2 (future): m33mu's ELF loader parses `.symtab`/`.strtab` itself, accepts `--signal-master-trigger-symbol=<n>` directly | `load_elf_segments()` (`src/main.c` ~line 1720) currently only reads `PT_LOAD` segments — no symbol table parsing exists yet; v1 needs zero m33mu changes to start using this today |

**Resolved (verified empirically during implementation, not just discussed):**
`arm-none-eabi-nm` and `arm-none-eabi-readelf` disagree on this in practice —
tested against the real toolchain: `nm` reported a marker function's address
with bit 0 clear (`0x0c00013a`), `readelf -s` reported the same symbol with
bit 0 set (`0x0c00013b`), confirming the Thumb-bit ambiguity is real, not
hypothetical. The fix: `--signal-master-trigger-addr=` parsing masks bit 0
off exactly once, at CLI-parse time (`& ~1u`), matching the fact that
`cpu->r[15]`/`cpu.r[15]` — compared against everywhere else in the codebase
via the same `& ~1u` convention — never carries the Thumb bit when read.
Either address pasted from either tool now resolves to the same, correct
comparison value.

### 2.2 GPIO-specific decisions

| # | Question | Decision | Rationale |
|---|---|---|---|
| 13 | GPIO edge detection | Precompute threshold-crossing events from the loaded trace at bind time (not a per-cycle voltage recompute); `mm_signal_tick` checks against the next scheduled crossing per binding | Cheap; avoids re-evaluating a threshold comparison every instruction for every bound GPIO pin |
| 14 | GPIO input: polling vs. interrupt | **Single hook covers both.** A new `stm32_gpio_set_external_input()`, mirroring the existing `gpio_sync_odr()` shape, writes the `IDR` bit and calls `ctx->exti_update()` if the value changed. Polling firmware sees it because it's the same `IDR` a plain read already returns; interrupt-driven firmware sees it because `exti_update()` is the existing EXTI→NVIC path, unchanged | `stm32_gpio_read()` already serves `IDR` correctly on any read — the only gap is that `IDR` is currently *only* ever written as a mirror of `ODR` (in `gpio_sync_odr()`), with no path for an externally-driven input. One new setter closes both usage scenarios at once |
| 15 | GPIO output verification (firmware drives a pin) | Out of scope for signal *injection* — this is the mirror case: read `ODR` via the existing register-read path and compare against an expected trace in the test harness, no emulator changes needed | `ODR` is already a plain readable register; no new hook required, this is a test-harness-side comparison, not an m33mu feature |
| 17 | GPIO digital threshold value | **Fixed 250 mV hysteresis band, centered at `VDD/2`, derived only from `--vdd_mv`, no separate CLI parameter.** Rising threshold `VDD/2 + 125 mV`, falling threshold `VDD/2 − 125 mV`. `IDR` only flips 0→1 once the trace rises above the high threshold, and only flips 1→0 once it falls below the low threshold; between the two thresholds, the last state holds | STM32H5F4 datasheet electrical characteristics give a **typical Schmitt-trigger hysteresis of 250 mV** for standard I/O — a fixed voltage band is the physically correct model (Schmitt hysteresis is a fixed offset, not a fraction of VDD). This replaces an earlier flat-`VDD/2` (no hysteresis) approximation, which would have produced spurious multiple crossings on a noisy/slow trace. Note: centering the band at `VDD/2` still assumes a symmetric threshold around midpoint, which is a simplification of the real V_IL/V_IH characteristic — acceptable for this use case but worth re-checking against the datasheet table if a specific I/O type (FT/FTf/TT) matters for a given test |
| 19 | GPIO edge-type binding parameter (`edge=`) | **Removed from the CLI.** `signal.c` schedules and forwards *every* crossing (both directions) unconditionally via `stm32_gpio_set_external_input()`; the existing `exti_gpio_update()` already gates rising/falling delivery dynamically, based on firmware's own `EXTI_RTSR1`/`EXTI_FTSR1` | Edge selection is a firmware-owned runtime register, not a bind-time property, and can change mid-run. A bind-time `edge=rising` would silently drop falling-edge events if firmware later enables `FTSR1` too — a real correctness bug, not just a simplification |

**Open item on #19 — mask gating (`EXTI_IMR1`) is unverified, not just edge-type gating.**
The rationale above only covers *edge-direction* gating (`RTSR1`/`FTSR1`). It
implicitly assumes `exti_gpio_update()` also correctly gates on
`EXTI_IMR1` (interrupt mask/enable) before asserting the NVIC IRQ.

Two things, with different confidence levels:

- **Non-negotiable, well-established across all STM32 families:** the NVIC
  interrupt request is generated only from `IMR1 & PR1` (or H5's
  `IMR1 & (RPR1|FPR1)`). If firmware never sets the `IMR1` bit for that
  line, the ISR **must never run** in m33mu, regardless of how many
  qualifying edges cross the trace. This needs to be confirmed against
  the current `exti_gpio_update()` implementation and is the hard
  correctness bar for this decision.
- **Genuinely unverified, don't assume:** whether the pending bit itself
  (`PR1` on classic EXTI, `RPR1`/`FPR1` on H5) sets while the line is
  masked. Classic-EXTI block diagrams (F1/F4/L4) show the edge detector
  feeding the pending register independently of `IMR`, suggesting the bit
  sets regardless of mask — but real-world reports disagree even for those
  families, and H5's EXTI has a different internal structure (separate
  rising/falling pending registers, different async edge-detection front
  end) than the classic block diagram this intuition comes from. Don't
  carry the classic-EXTI assumption over to H5 — check RM0517's EXTI
  chapter and block diagram for H5F4 specifically (and ideally the current
  `exti_gpio_update()` code, since that's existing infra this design reuses
  unchanged) before deciding what `gpio-input-exti-masked` should actually
  assert about the pending register. If it stays ambiguous even after
  checking the RM, the safe default is to test only the hard requirement
  (no ISR call) and leave the pending-register question out of the
  assertion rather than encode an unverified guess into a regression test.

## 3. File format

This is the shared container format — also consumed unchanged by the ADC
phase (traces don't know or care what peripheral binds to them).

### 3.1 Container (multi-trace, multi-timebase)

```c
struct mm_signal_container_header {
    char   magic[4];        /* "SIGC" */
    mm_u32 version;
    mm_u32 num_timebases;
    mm_u32 num_traces;
};

struct mm_signal_timebase_desc {
    mm_u32 id;
    mm_u32 sample_count;
    mm_u32 period_ns;       /* 0 => non-uniform: mm_u64 timestamps_ns[sample_count] follows inline */
};

struct mm_signal_trace_desc {
    mm_u32 id;
    char   name[32];        /* referenced by CLI bindings, e.g. "vin_adc0" */
    mm_u32 timebase_id;     /* which timebase this trace's samples align to */
    mm_i32 full_scale_mv;
    /* mm_i16 values_mv[timebase.sample_count] follows inline, zero-order hold */
};
```

Layout on disk: header, then `num_timebases` × (`mm_signal_timebase_desc` +
inline timestamp array if non-uniform), then `num_traces` × (`mm_signal_trace_desc`
+ inline value array).

> Note: `full_scale_mv`'s actual consumer isn't nailed down yet — see the ADC
> doc's open items, since ADC quantization is the first place a "full scale"
> concept would matter. For GPIO (threshold comparison only, no quantization)
> this field is inert; don't block GPIO implementation on resolving it.

### 3.2 Sample lookup (zero-order hold)

```c
/* Returns the millivolt value in effect at elapsed_ns: the most recent
 * sample at or before elapsed_ns. No interpolation — a sharp edge in the
 * source data stays sharp. Before the first timestamp: holds the first
 * sample. Past the last timestamp: holds the final sample. */
static mm_i16 adc_signal_sample(const struct stm32_adc_channel *chan, mm_u64 elapsed_ns)
{
    mm_u32 lo = 0, hi = chan->sample_count;
    if (chan->sample_count == 0) return 0;
    if (elapsed_ns < chan->timestamps_ns[0]) return chan->samples_mv[0];
    while (lo + 1 < hi) {
        mm_u32 mid = lo + (hi - lo) / 2;
        if (chan->timestamps_ns[mid] <= elapsed_ns) lo = mid; else hi = mid;
    }
    return chan->samples_mv[lo];
}
```

Uniform-timebase traces (`period_ns != 0`) get an O(1) fast path:
`index = elapsed_ns / period_ns` instead of binary search.

Sharp-edge modeling: place two samples 1 ns apart (or at the same timestamp)
straddling the desired transition — the hold logic snaps to the new value
the instant `elapsed_ns` crosses that timestamp.

## 4. Runtime architecture

```
┌────────────────────┐   PC == trigger_addr (one-shot)   ┌──────────────────────┐
│ src/main.c tick site│ ─────────────────────────────────▶│ mm_signal_group_     │
│ (per-instruction,   │                                    │ trigger(group_id)    │
│  same site as       │                                    │  resets elapsed_ns=0 │
│  mm_timer_tick)      │                                    │  for every binding   │
└─────────┬───────────┘                                    │  in that group       │
          │  mm_signal_tick(cycles) every instruction       └──────────┬───────────┘
          ▼                                                            │
┌────────────────────────────────────────────────────────────────────▼───────────┐
│ src/signal.c  — per-binding elapsed_ns accumulation, zero-order-hold lookup,     │
│                 threshold-crossing schedule for GPIO bindings                    │
└───────────┬───────────────────────────────────────────────────────────────────┘
            │ crossing event
            ▼
┌──────────────────────────────┐
│ cpu/stm32_gpio.c (existing +  │
│ new external-input setter)    │
│ writes IDR bit; on change,    │
│ calls ctx->exti_update() —    │
│ same path serves both polling │
│ reads (IDR) and EXTI/NVIC IRQ │
└──────────────────────────────┘
```

(The ADC leg of this diagram — `mm_signal_sample(binding_id)` pulled by
`cpu/stm32_adc.c` on `ADSTART` — is documented in the ADC doc; `signal.c`
itself is built once here and doesn't change shape for that phase.)

### 4.1 New module: `include/m33mu/signal.h` + `src/signal.c`

Peripheral-agnostic, lives alongside `src/adc.c`/`src/timer.c` (not under any
`cpu/<target>/`), since any peripheral on any SoC profile can bind to it.

```c
struct mm_signal_binding {
    mm_u32 trace_id;
    mm_u32 group_id;
    mm_u64 elapsed_ns;     /* this group's cursor */
    mm_bool armed;
};

mm_bool  mm_signal_load(const char *path);
mm_u32   mm_signal_bind(const char *trace_name, mm_u32 group_id);
void     mm_signal_group_trigger(mm_u32 group_id);   /* one-shot, fired by PC-watch */
mm_i16   mm_signal_sample(mm_u32 binding_id);         /* zero-order-hold pull */
void     mm_signal_tick(mm_u64 cycles);               /* called from main.c, same site as mm_timer_tick */

/* Global board electrical config (§2.2, decision #17) — set once from
 * CLI at startup, not per-binding. */
void     mm_signal_set_vdd_mv(mm_i32 vdd_mv);           /* GPIO thresholds derived: mid=vdd/2, hi=mid+125mV, lo=mid-125mV */
```

GPIO bindings' crossing schedule (decision #13) is precomputed at bind time
using a **Schmitt-trigger state machine**, not a flat single-threshold
comparator: `vdd_mv/2 + 125` is the rising threshold, `vdd_mv/2 − 125` the
falling threshold (both derived automatically from `--vdd_mv`, no separate
CLI parameter, per the datasheet's typical 250 mV hysteresis figure). Walking
the trace once:

```c
/* IDR only flips on a genuine threshold crossing in the corresponding
 * direction; between the two thresholds, the previous state holds — this
 * is what suppresses spurious re-triggering on a noisy or slow trace,
 * matching real Schmitt-trigger hysteresis. */
mm_bool state = (chan->samples_mv[0] >= hi_threshold_mv);
for (i = 1; i < chan->sample_count; ++i) {
    mm_i16 v = chan->samples_mv[i];
    if (!state && v >= hi_threshold_mv) {
        state = MM_TRUE;   /* schedule a rising crossing at timestamps_ns[i] */
    } else if (state && v <= lo_threshold_mv) {
        state = MM_FALSE;  /* schedule a falling crossing at timestamps_ns[i] */
    }
    /* v between lo and hi: no state change, no crossing scheduled */
}
```

Both directions are always scheduled (see decision #19), regardless of what
`EXTI_RTSR1`/`EXTI_FTSR1` firmware has configured at that point; the actual
edge-type gating happens downstream in the existing `exti_gpio_update()`.

### 4.2 PC-watch master trigger (in `src/main.c`, same tick site)

```c
if (g_signal_master_trigger.armed && pc == g_signal_master_trigger.addr) {
    mm_signal_group_trigger(MM_SIGNAL_MASTER_GROUP);
    g_signal_master_trigger.armed = MM_FALSE;   /* one-shot, no re-arm */
}
```

(See §2.1's open item on Thumb-bit handling — resolve before finalizing this
comparison.)

### 4.3 GPIO external-input hook (in `cpu/stm32_gpio.c`, existing file)

**Implemented and verified** (`stm32_gpio_set_external_input()`, `cpu/stm32_gpio.c`).
Mirrors `gpio_sync_odr()`'s exact shape — same struct, same `exti_update`
call — but sourced from `signal.c`'s threshold-crossing schedule instead of
an `ODR` write. Guards on `stm32_gpio_get_pin_mode(g, pin) != 0u`, which
rejects Output (`01`), AF (`10`), *and* Analog (`11`) together — any mode
other than Input (`00`) — since real hardware only accepts an external
drive on a pin actually configured as a digital input.

**Second, related fix in `stm32_gpio_read()` (not `stm32_gpio_set_external_input()`):**
on real STM32 silicon, a pin configured `MODER=0b11` (Analog) has its
digital input buffer (Schmitt trigger) powered down to avoid parasitic
leakage current — `IDR` reads `0` for that pin regardless of the actual
analog voltage, and EXTI cannot fire from it. The write-side guard above
prevents `stm32_gpio_set_external_input()` from ever *setting* an `IDR` bit
for an Analog-mode pin, but doesn't address a pin that was driven while
still in Input mode and is *later* reconfigured to Analog: neither that
function nor `gpio_sync_odr()` ever clears a stale bit on a mode change
away from Input, so without a corresponding read-side fix the stale `1`
would remain visible in `IDR` forever, which doesn't match real silicon.
Fixed in `stm32_gpio_read()`: any `IDR` bit whose corresponding `MODER`
field currently reads Analog is masked to `0` on every read, evaluated
live against current `MODER` state (not latched at the mode-change
instant, so switching back to Input immediately un-masks it again).
Covered by `tests/stm32_gpio_signal_test.c` (`analog_mode_forces_idr_zero`,
which explicitly checks the underlying register word still holds the stale
bit — proving the fix is in the read path, not a side effect of the mode
change clearing storage — and `analog_mask_is_per_pin`, which confirms the
mask doesn't disturb other pins' bits).

Note: this only applies to pins in input mode (`MODER` not set to
output/AF/analog) — for a pin in output mode, firmware owns `ODR`→`IDR` via
the existing `gpio_sync_odr()` path, and an external-input write would
conflict; the setter no-ops if the pin's current mode isn't input.

### 4.4 CLI surface (GPIO phase)

```
--vdd_mv=3300                                         # GPIO hysteresis thresholds: mid=vdd/2, ±125mV (250mV typ. hysteresis)
--signal-file=board_stimulus.sigc
--signal-master-trigger-addr=0x08002104               # v1: resolved externally via nm
--signal-master-trigger-symbol=HAL_ADC_Start           # v2: once m33mu parses .symtab
--signal-bind:trace=vin_adc0:target=PA5:role=gpio:group=g0
```

(`--adc-vref_mv` and `--signal-bind:...:target=ADC1:...` are ADC-phase CLI
additions — see the ADC doc.)

## 5. Implementation trace (suggested order, GPIO phase)

GPIO-input is scoped first: it reuses almost entirely existing, working
infrastructure (`cpu/stm32_gpio.c`/`stm32h5_mmio.c` GPIO model, EXTI/NVIC
path), needs one new setter function, and gives an early end-to-end
validation of the `signal.c` core before the heavier ADC register model is
tackled in the follow-on phase.

1. **`include/m33mu/signal.h` + `src/signal.c`**
   Container loader (`mm_signal_load`), binding table, `mm_signal_tick`,
   `mm_signal_sample`, `mm_signal_group_trigger`, threshold-crossing schedule
   for GPIO bindings. No peripheral dependencies — testable standalone
   (mirrors `tests/stm32_timers_test.c` pattern with a new `tests/signal_test.c`).

2. **Offline HDF5 → `.sigc` converter script** (Python + h5py, outside the
   C build). Emits the container format from §3.1. Needed before any
   emulator-side testing can use real captured signals.

3. **`src/main.c`**
   - PC-watch struct + comparison at the existing per-instruction tick site
     (~line 3580-3586), alongside `mm_timer_tick`.
   - `mm_signal_tick(cfg, insn_cycles)` call at the same site.
   - CLI parsing for `--vdd_mv`, `--signal-file`,
     `--signal-master-trigger-addr`, `--signal-bind:...` (mirrors existing
     `--iotsafe-uart:addr:file=` parsing pattern already in the file).
   - `mm_signal_set_vdd_mv()` called before `mm_signal_load()`, so the GPIO
     crossing schedule is computed with the right threshold at load time;
     then binding setup at startup, alongside existing
     `usart_init`/`spi_init`/`timer_init` calls.

4. **`cpu/stm32_gpio.c`** (existing file) — add `stm32_gpio_set_external_input()`
   (§4.3), called from `signal.c`'s threshold-crossing schedule; writes `IDR`
   and calls the existing `ctx->exti_update()` on change. This alone gives
   both polling-read and interrupt-driven GPIO input consumers, with no
   changes needed to `stm32h5_mmio.c`'s existing EXTI/NVIC wiring.

5. **First end-to-end test** — `--signal-bind:trace=...:target=PA5:role=gpio`
   against a small firmware sample under `tests/firmware/`, validating the
   PC-watch trigger → `signal.c` → `stm32_gpio.c` chain.

## 6. Open items / deferred (GPIO phase)

- **v2**: native `.symtab`/`.strtab` parsing in `load_elf_segments()` so
  `--signal-master-trigger-symbol=<n>` works without an external `nm` step.
- **Fallback trigger source** (register-write watch instead of PC-watch) for
  binary-only firmware with no ELF symbols — noted as a fallback, not
  built unless a concrete need arises.
- **GPDMA `TRIGSEL`/EXTI trigger gating** — real STM32H5 silicon lets an
  EXTI-sourced GPIO edge gate a GPDMA memory-to-memory transfer;
  `cpu/stm32_gpdma.c` currently starts a channel immediately on `CxCR.EN`
  and doesn't model trigger sources at all. Separate, non-trivial feature on
  the GPDMA model itself, unrelated to signal injection proper — only build
  if test firmware actually exercises triggered-GPDMA+EXTI+GPIO capture.
- **GPIO output verification** — no emulator change needed; `ODR` is already
  plainly readable, this is a test-harness-side comparison against an
  expected trace, not an m33mu feature to build.
- **Trigger-symbol fragility against inlining/LTO** — a HAL/CMSIS symbol used
  as the trigger address can vanish or shift if inlined in a release build.
  Consider recommending test firmware use a dedicated
  `__attribute__((noinline))` marker function instead, and add a "master
  trigger never fired" diagnostic so a test that never actually triggers
  doesn't silently read all-zero/unbound traces and pass or fail for the
  wrong reason.

## 7. Testing

Follows the repo's existing CI convention (`docs/ci-testing.md`): firmware
signals results via `bkpt #<imm>` — a dedicated "pass" immediate reached
only if every check along the way succeeded, and a "fail" immediate (e.g.
`0x7e`, matching `test-stm32h563`'s existing pattern) hit as soon as any
assertion fails. CI then runs
`--uart-stdout --expect-bkpt 0x7f --timeout N` and asserts on the exit code
and captured UART log.

### 7.1 GPIO input — single-purpose firmware (`test-stm32h5f4-gpio-*`)

| Firmware dir | What it does | Validates |
|---|---|---|
| `gpio-input-poll` | Busy-loop reads `IDR` at known cycle counts, compares against expected zero-order-hold values from a known trace | Core `mm_signal_sample`/`stm32_gpio_set_external_input`, polling path |
| `gpio-input-exti-rising` | `RTSR1` only; ISR counts interrupts + logs timestamps over UART | `exti_gpio_update()` correctly gates on rising only |
| `gpio-input-exti-falling` | `FTSR1` only, same pattern | Falling-only gating |
| `gpio-input-exti-both` | Both `RTSR1`+`FTSR1` set; count should match every crossing in the trace | Decision #19 — confirms removing bind-time `edge=` and always forwarding both directions was correct |
| `gpio-input-exti-masked` | `RTSR1` set (edge armed) but `IMR1` bit cleared (line masked) for the duration of one or more crossings, then unmasked; firmware confirms the ISR was **not** invoked while masked (hard requirement — see decision #19 open item) | Confirms `IMR1`, not just edge-direction (`RTSR1`/`FTSR1`), gates NVIC delivery. Whether `PR1`/`RPR1` sets while masked is a separate, currently-unverified question (see open item) — don't assert on pending-register state here until that's resolved against RM0517/the existing `exti_gpio_update()` code |
| `gpio-input-hysteresis` | Trace with values deliberately wobbling inside the hysteresis band | Decision #17 — the Schmitt-trigger state machine must produce **zero** spurious interrupts for in-band noise; a naive flat-threshold implementation would fail this and over-count |
| `gpio-input-sharp-edge` | Two samples ~1 cycle apart bracketing a transition; firmware polls tightly and records which cycle first sees the new value | Validates the "instantaneous edge" modeling claim (§3.2), and the real ~4ns/cycle floor at 250MHz |
| `gpio-input-nonuniform` | Trace with explicit non-uniform `timestamps_ns[]`; firmware busy-waits (via SysTick) to specific target times and checks the value held | Binary-search zero-order-hold lookup path (§3.2) |
| `gpio-input-mode-guard` | Pin briefly reconfigured to output mid-test while a binding targets it, then back to input | §4.3's note that external-input writes must no-op when `MODER` isn't input — protects firmware's own `gpio_sync_odr()` behavior from corruption |
| `gpio-trigger-oneshot` | Firmware calls the trigger-address function twice (e.g. in a loop) | Decision #10 — only the *first* call should reset `elapsed_ns`; the second must be a no-op |
| `gpio-group-sync` | Two GPIO bindings in the same `group_id`; firmware timestamps its first read from each right after boot | Decision #8 — both cursors reset at exactly the same instant, order-independent of which pin firmware touches first |

### 7.2 GPIO output — single-purpose firmware

| Firmware dir | What it does | Validates |
|---|---|---|
| `gpio-output-loopback` | Firmware drives a known `ODR` sequence, reads back `IDR` after each write | Regression check that `gpio_sync_odr()`'s existing mirror behavior is untouched by the new external-input path — cheap but important, since both write to the same `IDR` |
| `gpio-output-timing` | Firmware toggles a pin at fixed instruction-count intervals, logs cycle-derived timestamps over UART | CI harness compares against expected real-time deltas at 250MHz — reuses GPIO as an observable channel to validate §1's cycle-accuracy model, no new emulator code needed |

### 7.3 Organizational choice: split vs. combined firmware

The repo supports both styles — many finely-split single-purpose dirs
(`test-stm32h563-uart-echo`) and one larger multi-assertion firmware with
several inline `bkpt #0x7e` checkpoints before a final `bkpt #0x7f`
(`test-stm32h563`). Recommendation: keep `gpio-input-hysteresis`,
`gpio-input-exti-both`, and `gpio-trigger-oneshot` as **separate** binaries
regardless of what's chosen for the rest — these three are the most likely
to catch a real regression in the trickiest parts of this design
(hysteresis math, RTSR/FTSR gating, one-shot semantics), and an unambiguous
per-test failure is worth the extra build for exactly these three. The more
mechanical checks (`gpio-input-poll`, `gpio-input-sharp-edge`,
`gpio-input-nonuniform`, `gpio-input-mode-guard`) are reasonable candidates
to merge into a single `test-stm32h5f4-gpio-signal-suite` firmware if
CI build/turnaround time matters more than isolated per-check diagnosis.
