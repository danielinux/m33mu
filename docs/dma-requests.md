# GPDMA peripheral requests: ADC to memory on STM32H5

Status: design, not implemented. Branch `gpdma_requests`, stacked on
`signal_injection_adc`.

Today `cpu/stm32_gpdma.c` copies a whole block as soon as a channel is
enabled, ignores `SWREQ`/`REQSEL`, and reloads a linked list from four fixed
words. The ADC model (`cpu/stm32_adc.c`) only warns once when `DMAEN` is set.
So firmware that calls `HAL_ADC_Start_DMA`, in particular continuous
conversions into a circular buffer, does not work. This change makes the
GPDMA serve hardware requests and makes the ADC raise them.

Sources: RM0517 Rev 1 (H5F4) §16 GPDMA and §30.4.26 (subsection "Managing
conversions using the DMA"); RM0481 Rev 5 (H563/H533), same sections;
STM32CubeH5 HAL `stm32h5xx_hal_adc.c` (`HAL_ADC_Start_DMA`). How HAL's DMA
driver programs a linked-list start is recalled, not read from source: the
tests below pin that sequence down.

## Terms

| Term | Meaning |
|---|---|
| Request | A peripheral asking the GPDMA to move one data item. Identified by its request number, the value firmware writes to `GPDMA_CxTR2.REQSEL`. ADC requests are levels: asserted while the ADC holds unread data with `DMAEN` = 1. |
| Request number | The `REQSEL` value of a request: `adc1_dma` = 0, `adc2_dma` = 1 (RM0517 and RM0481 Table "Programmed GPDMA1/2 request"), `adc3_dma` = 142 (RM0517; in RM0481 only on H543/H553). Both GPDMA1 and GPDMA2 accept every request. |
| Trigger | A `GPDMA_CxTR2.TRIGSEL` event that gates a channel. Unrelated to requests and not modelled. |
| Beat | One data item read from the source and written to the destination: `SDW` bytes read, `DDW` bytes written. One request moves one beat. |
| Block | `BNDT` source bytes, the unit that sets `HTF` (half) and `TCF` (complete) when `TCEM` = 00. |
| Linked-list item (LLI) | The register values a channel loads from memory at the end of a block: the words selected by the `GPDMA_CxLLR` update bits, at `CxLBAR` + `LA`. LLI0 is the register file firmware programs directly. |
| Null LLI0 | Enabling a channel with `BNDT` = 0 and `CxLLR.UB1` = 1: no data, the channel loads LLI1 first. HAL starts every linked-list queue this way. |
| Link transfer | Loading the next LLI. |
| Last beat | The beat after which the channel completes and clears `EN` (end of a block with `CxLLR` = 0). The ADC's one-shot mode stops on it. |

## Decisions

| Decision | Reason |
|---|---|
| Generic request lines: a source raises a request number, any enabled channel with `SWREQ` = 0 and that `REQSEL` serves one beat | USART/SPI DMA can reuse it later |
| `SWREQ` = 0: a channel waits for requests and moves one beat per request; `SWREQ` = 1 (memory to memory) keeps copying on enable | Matches RM; no test or fixture in the repo uses the GPDMA today |
| Requests are served synchronously and take no cycles | Fits 1 instruction = 1 cycle; an overrun then means no channel was ready, which is the firmware bug worth catching |
| Requests are levels: when a channel becomes ready (enable, end of a link transfer, resume from suspend), it asks the source of its `REQSEL` to re-raise | Firmware may enable the channel after the first conversion |
| `CxLLR` update bits are honoured (UT1, UT2, UB1, USA, UDA, ULL; UT3/UB2 on 2D channels), with `BNDT` restored when UB1 = 0 | HAL circular mode is a linked list that points back to itself |
| ADC `DMACFG` = 0 (one-shot): on the last beat the ADC stops requesting, freezes `DR`, aborts the ongoing conversion, resets the scan | RM §30.4.26; firmware that restarts after DMA complete sees real state |
| ADC `DMACFG` = 1 (circular): requests never stop on their own | RM §30.4.26 |
| After `OVR` the ADC raises no request until firmware clears `OVR` | RM §30.4.26 |
| Each channel accesses the bus with its own security: non-secure unless `SECCFGR.SECx` = 1, then `SSEC`/`DSEC` per side; secure channels fetch LLIs as secure | RM0517 §16.4.15 |
| DMA bus accesses bypass the CPU's SAU/MPU interceptor and never raise CPU faults; the target's MMIO handler sees the channel's security through `mmio_active_sec()` | A bus master is not checked against the CPU's MPU; a DMA access must not fault the CPU |
| `PAM` = 00 (right-align, zero-pad or truncate) when widths differ; other `PAM`, 2D addressing, `TRIGPOL` ≠ 00, burst length > 1, `PFREQ`, `BREQ`, dual-ADC `MDMA` warn once and behave as single beats | Covers HAL's ADC configurations |
| User setting errors that would otherwise never terminate or wrap are modelled as `USEF` (below) | Keeps every register write finite |
| Two enabled channels with the same `REQSEL` (forbidden by RM): the lowest GPDMA1 channel serves, warn once | Deterministic |
| A failed data access sets `DTEF`, a failed link transfer `ULEF`; both clear `EN` | RM |
| `SUSP` = 1: the channel serves nothing | RM |
| GPDMA gets `save`/`load` hooks (regions `GPDMA1`, `GPDMA2`) | Same rule as the ADC; `mmio_bus_save` covers it |
| Wired on `stm32h5f4`, `stm32h563`, `stm32h533`; `stm32u585` gets the GPDMA changes without request sources | Only the H5 parts have the ADC model |

## Interface

New `cpu/stm32_dma_req.h` (shared by the GPDMA and any request source):

```c
enum stm32_dma_req_result {
    STM32_DMA_REQ_NONE = 0,     /* no ready channel, or the beat failed */
    STM32_DMA_REQ_SERVED = 1,   /* one beat moved */
    STM32_DMA_REQ_LAST = 2      /* one beat moved and the channel completed */
};
```

In `cpu/stm32_gpdma.h`:

```c
#define STM32_DMA_REQ_LINES 256u

typedef void (*stm32_dma_ready_fn)(void *ctx);

struct stm32_dma_router {
    struct stm32_gpdma_state *dma[2];
    mm_u32 dma_count;
    mm_u32 depth;                        /* > 0 while serving a request */
    mm_u32 serving_line;                 /* line of the innermost request */
    stm32_dma_ready_fn ready[STM32_DMA_REQ_LINES];
    void *ready_ctx[STM32_DMA_REQ_LINES];
};

void stm32_dma_router_init(struct stm32_dma_router *r,
                           struct stm32_gpdma_state *dma0, struct stm32_gpdma_state *dma1);
void stm32_dma_router_bind(struct stm32_dma_router *r, mm_u32 line,
                           stm32_dma_ready_fn ready, void *ctx);
enum stm32_dma_req_result stm32_dma_request(struct stm32_dma_router *r, mm_u32 line);
```

- `stm32_dma_request()` looks for an enabled, non-suspended channel with
  `SWREQ` = 0 and that `REQSEL`, GPDMA1 first, lowest channel first, and runs
  one beat on it.
- Each `stm32_gpdma_state` gets a `router` pointer and `first_2d_channel`
  (H5F4 10, H563/H533 6, U585 12; RM0517/RM0481 `TCEM` description). When a
  channel with `SWREQ` = 0 becomes ready it calls `ready[REQSEL]`, if bound,
  unless `depth` > 0 and `REQSEL` is `serving_line`: inside a request that
  source is already looping. Any other line is notified, for example after a
  link transfer that changes `REQSEL`.
- `stm32_dma_router_init` clears the bindings and runs once, at MMIO
  registration; reset does not touch the router. Sources bind after it.

In `cpu/stm32_adc.h`, `struct stm32_adc_soc` gets:

```c
int dma_line[STM32_ADC_MAX_INST];                                  /* -1: no request */
enum stm32_dma_req_result (*dma_request)(void *ctx, mm_u32 line);  /* NULL: DMAEN warns */
```

and the model exports `void stm32_adc_dma_ready(struct stm32_adc_state *s, mm_u32 inst)`
for the glue to bind. `stm32_adc.c` includes only `stm32_dma_req.h`; the H5
glue connects the two models.

## DMA bus access

New in `src/memmap.c`: `mm_memmap_dma_read(map, sec, addr, size, &v)` and
`mm_memmap_dma_write(map, sec, addr, size, v)`. They do what
`mm_memmap_read/write` do without `MM_INTERCEPT_OK` (no SAU/MPU check, no
CPU fault recorded), and they restore `mmio_active_sec()` to its previous
value before returning, so a DMA access started from inside a CPU MMIO write
does not leave the CPU's security state changed. Every GPDMA data beat and
link transfer uses them.

## GPDMA channel behaviour

State added to `struct stm32_gpdma_channel`: `bndt_reload` (restored when
UB1 = 0; captured at each `CxBR1` write and each LLI load with UB1 = 1),
`block_bytes` (size of the current block, for `HTF`), `ht_done` (cleared at
enable and at each link transfer). `CxBR1.BNDT` reads the bytes still to
transfer.

**Enable** (`EN` 0 → 1):
1. Clear `IDLEF`, `ht_done`.
2. Null LLI0 (`BNDT` = 0, `CxLLR.UB1` = 1): run a link transfer before any data.
   No `HTF`. `TCEM` = 00/01/11: no `TCF` for LLI0. `TCEM` = 10: `TCF` once
   LLI1 is loaded (RM0517 44667-44687).
3. Setting checks (`USEF`, below).
4. `SWREQ` = 1: run beats and link transfers now, bounded at 1024 link
   transfers per enable; at the bound warn once and leave the channel
   enabled (a self-linked memory-to-memory list never completes on silicon
   either).
5. `SWREQ` = 0: call `ready[REQSEL]`.

**User setting errors** (`USEF`, IRQ if `USEIE`; clear `EN`, set `IDLEF`), checked
at enable and after each link transfer: `SDW` or `DDW` = 11; `SAR`/`DAR` not
aligned to its width; `BNDT` not a multiple of `SDW`; `BNDT` = 0 outside the
null-LLI0 case (RM0517 43726-43731, 43776-43777). `BNDT` = 0 with UB1 = 0 at
enable raises `USEF` without any link transfer.

**Register writes**: `CxLLR` writes are masked like an LLI load (bits 1:0 and
reserved bits read 0; UT3/UB2 only on 2D channels). While `EN` = 1 the
channel's `SECCFGR`/`PRIVCFGR` bits and `CxCR` bits 23:16 (`PRIO`, `LAP`,
`LSM`) are read-only (RM0517 43896, 43939, 44299-44322), as are the other
configuration registers. `SECx`/`PRIVx` of a channel locked in `RCFGLOCKR`
are read-only too; lock bits are set-only. Non-secure agents (by
`mmio_active_sec()`) cannot write `SECCFGR`, `RCFGLOCKR` or a secure channel's
`PRIVx`; a secure channel's registers read 0 and ignore their writes
(RM0517 43602-43606, 43893). Privilege is not checked.

**One beat** (per request, or per loop step with `SWREQ` = 1): read `SDW`
bytes at `SAR` with the source security, adapt the width (`PAM` = 00), write
`DDW` bytes at `DAR` with the destination security, advance `SAR`/`DAR` when
`SINC`/`DINC`, subtract `SDW` from `BNDT`. Half-transfer: when the bytes
transferred in this block reach ceil(`block_bytes` / 2) (RM0517 43866), set
`HTF` (IRQ if `HTIE`), once per block. Bus failure: `DTEF`, `IDLEF`, clear
`EN`, IRQ if `DTEIE`, result `NONE`.

**End of block** (`BNDT` = 0):
- `CxLLR` = 0: channel complete: clear `EN`, set `IDLEF` and `TCF`; result `LAST`.
- `CxLLR` ≠ 0: `TCEM` = 00/01 set `TCF`; if UB1 = 0 restore `BNDT`; run the
  link transfer; `TCEM` = 10 sets `TCF` after it; `TCEM` = 11 sets `TCF`/`HTF`
  only in the last LLI; then call `ready[REQSEL]`. A list whose last LLI
  points back to itself never completes.

**Link transfer**: read 32-bit words from `LBAR` + `LA`, in the order
`TR1`, `TR2`, `BR1`, `SAR`, `DAR`, (`TR3`, `BR2` on channels ≥
`first_2d_channel`), `LLR`, one word per set update bit; ULL = 0 keeps
`CxLLR`. Secure channels read as secure. A failed read sets `ULEF` and
`IDLEF`, clears `EN` (IRQ if `ULEIE`).

**Suspend, abort, disable**: writing `EN` = 0 is ignored (RM0517 44395).
`SUSP` = 1 on an enabled channel sets `SUSPF` and `IDLEF` at once, with `EN`
still 1 (RM0517 44380), IRQ if `SUSPIE`, and blocks requests. Clearing
`SUSP` clears `IDLEF` and calls `ready[REQSEL]`. `RESET` acts only on a
disabled or suspended channel (otherwise ignored): it clears `EN`, `SUSP` and
the internal block state, sets `IDLEF`, and keeps every other register and
the `CxSR` flags (RM0517 44371-44383). Firmware reprograms only `BR1`, `SAR`,
`DAR` before re-enabling (RM0517 41856-41861). This is the sequence
`HAL_DMA_Abort` and `HAL_DMA_Start_IT` use.

**Interrupts and `MISR`/`SMISR`**: channel x's IRQ is raised when a flag
sets whose enable bit is set, as today. `MISR` bit x (non-secure channels)
and `SMISR` bit x (`SECx` = 1) are recomputed from `CxSR` & the `CxCR`
enable bits on every flag change and every `CxFCR` write.

## ADC behaviour

State added to `struct stm32_adc_inst`: `dma_stopped` (one-shot completed)
and `dma_busy` (inside `adc_dma_service`), both in the ADC snapshot
(snapshot version bump).

- `adc_dma_service(s, in)`: returns at once if `dma_busy`; else sets it and,
  while `DMAEN` = 1, `OVR` = 0, `dma_stopped` = 0 and the FIFO is not empty,
  at most `STM32_ADC_FIFO_DEPTH` times, calls `dma_request(line)`:
  - `NONE`: stop; the data stays in the FIFO, and the normal FIFO/OVR rules apply.
  - `SERVED` or `LAST`: the GPDMA read `DR` through the bus, which pops the
    FIFO. If `fifo_count` did not decrease (the channel's `SAR` is not this
    `DR`, the TZSC gate denied a non-secure channel, the ADC clock is off),
    warn once and stop as for `NONE`.
  - `LAST` with `DMACFG` = 0: one-shot end (below); stop.
  - `LAST` with `DMACFG` = 1: loop; the next call returns `NONE`.
- Called after every `adc_push`, from `stm32_adc_dma_ready`, after a write
  that clears `OVR`, and on `DMAEN` 0 → 1.
- The FIFO pop on the DMA's `DR` read is the existing `adc_pop`; it never
  calls `adc_dma_service`.
- One-shot end: `dma_stopped` = 1; `DR` keeps the value just transferred;
  the rest of the FIFO is dropped and `EOC` cleared (same rule as an `ISR.EOC`
  write); the conversion in progress is discarded; the scan restarts at rank
  0. `ADSTART` stays set: RM0517 76918-76935 lists every hardware clear and
  this is not one. `dma_stopped` clears on `ADSTART` 0 → 1 (inside the start
  branch of `adc_cr_write`) and on `DMAEN` 0 → 1, which covers HAL's
  `Stop_DMA`/`Start_DMA` pair. Re-enabling the DMA channel does not re-arm it
  (RM0517 78318).
- `DMAEN` with `dma_request` = NULL keeps the one-time warning.
- The TZSC gate (`ADC12SEC`/`ADC3SEC`) reads `mmio_active_sec()`, which the
  DMA access sets from the channel's source security: a non-secure channel
  reading a secure ADC reads 0, as the model already does for CPU accesses
  (silicon raises an illegal-access event and a bus error instead).

## H5 glue

- `cpu/stm32h5_mmio.c` owns `gpdma1`/`gpdma2`; it adds a static
  `stm32_dma_router`, initialised once in the MMIO registration path (not in
  `gpdma_configure_and_reset`, which also runs at reset), plus
  `stm32h5_dma_request(line)` and `stm32h5_dma_bind(line, fn, ctx)`.
  `first_2d_channel` comes from the part's variant table.
- `cpu/stm32h5_adc.c` sets `dma_line` = {0, 1} for ADC1/2 and {142} for ADC3,
  sets `dma_request`, and binds `stm32_adc_dma_ready` per line. It also
  hands the NVIC to `gpdma1`/`gpdma2` (`stm32h5_gpdma_set_nvic`): no other H5
  hook did, so GPDMA channel IRQs never reached the NVIC before.
  `stm32h5_adc_init` must run after MMIO registration. It does: `main.c` and
  `web_bridge.c` both run `mm_execution_state_reset_soc` (SoC and ADC reset),
  then `mm_execution_state_register_target_mmio` (router init), then
  `mm_execution_state_init_target_devices`, whose `mm_timer_init` calls
  `adc_init` (bind). A system reset repeats the whole sequence; a SoC reset
  alone (`soc_reset`, `adc_reset`) touches neither the router nor the ADC's
  `soc`, so the bindings stay.
- `GPDMA1`/`GPDMA2` non-secure regions get `save`/`load`/`name`; the secure
  aliases register none, as `stm32_adc_init` does. Both parts register them
  through `stm32_gpdma_register(bus, d, base, name)` in `stm32_gpdma.c`.
  `stm32u585_mmio.c` does the same, with `first_2d_channel` = 12.

## Snapshots

The GPDMA snapshot holds the global registers, every channel register,
`remaining`, `bndt_reload`, `block_bytes`, `ht_done` and the warning mask;
not the router, `nvic` or `irq_map`.

`--record` undoes DMA transfers only partly. DMA RAM writes go through
`mm_memmap_write`, which logs them only inside an open trace step. With
`--record` the translation-block path is off (`mm_trace_enabled()` clears
`fast_mode` in `main.c`).
- Beats run by a time tick (an ADC conversion, the WFI fast-forward) are not
  logged: core 0's per-instruction path advances time before it opens the
  step, and the fast-forward runs outside any step. Only `step_core_simple`
  (core 1) ticks inside a step, and the H5 parts have one core. Reverse
  stepping leaves that data in RAM.
- Beats set off by a firmware store (enabling or resuming a GPDMA channel,
  setting `DMAEN`, clearing `OVR`, a `SWREQ` = 1 start) run inside the step,
  so their RAM writes are logged. Reverse stepping over the store restores
  RAM but not the popped ADC FIFO or the channel's `BNDT`/`SAR`/`DAR`/`CxSR`.

Neither model has `begin_step`/`end_step` hooks. A consistent undo needs
them in both models plus a main-loop reorder, which is out of scope here.

## Timing

Everything runs inside `stm32_adc_tick` at the conversion time, like the ADC
flags today: the DMA writes land in RAM at the end of the executed batch,
together with `EOC`. `stm32_gpdma` needs no tick and no next-event bound.

## Not implemented

Triggers (`TRIGSEL`/`TRIGM`), bursts > 1, `PFREQ`/`BREQ`/`DREQ`
peripheral-side semantics (requests always pace the source side), 2D
addressing (`TR3`/`BR2` load but are not applied), `PAM` ≠ 00, byte/halfword
exchange, ports and priorities, GTZC illegal-access events for DMA accesses,
dual-ADC DMA (`CCR.MDMA`), DMA requests from other peripherals.

## Tests

Host (`tests/`):
- `stm32_gpdma_test.c`: `SWREQ` = 1 block copy unchanged; self-linked
  `SWREQ` = 1 list stops at the bound; one beat per request; `NONE` when no
  channel matches, `SUSP`, or an enabled `SWREQ` = 1 channel with that
  `REQSEL`; duplicate `REQSEL`; `ready` called on enable/link/resume, not for
  the line being served, but for a line a link switches to; LLR update bits
  order and ULL = 0; `CxLLR` write mask; UB1 = 0 `BNDT` restore; HAL-shaped
  start (null LLI0, `CxLLR` with all update bits, node in RAM); self-linked
  circular list; `HTF`/`TCF` per `TCEM` (00, 01, 10, 11) including null LLI0;
  byte beats with an odd `BNDT` (rounded-up `HTF`); `LAST` on completion;
  `DTEF`/`ULEF`/`USEF` cases with `IDLEF`, including `BNDT` = 0 with UB1 = 0
  and an LLI loading `BNDT` = 0; `PAM` = 00 with halfword source/word
  destination and the reverse; secure/non-secure attributes and no CPU fault
  on a denied address; non-secure register access to a secure channel and to
  `SECCFGR`/`PRIVCFGR`/`RCFGLOCKR`; `SECCFGR`/`PRIVCFGR`/`CxCR` 23:16
  read-only while enabled, `SECx`/`PRIVx` read-only once locked;
  `mmio_active_sec()` unchanged after a beat; `SUSP` →
  `SUSPF`+`IDLEF` → `RESET` abort keeping the configuration, HAL restart, and
  `RESET` ignored on an active channel; `MISR`/`SMISR`; save/load round trip
  mid-block.
- `stm32_adc_test.c` additions with a fake `dma_request`: service loop,
  `NONE`, no-pop guard, one-shot end (`DR`, `EOC`, scan reset, `ADSTART`),
  re-arm on `ADSTART` 0 → 1 and `DMAEN` 0 → 1, `OVR` block and resume,
  snapshot with `dma_stopped`.
- `stm32h5_adc_dma_test.c`: ADC → router → GPDMA → RAM on the H5 memory map:
  3-channel scan into a buffer; continuous + circular with HT/TC and the
  channel IRQ pending in the NVIC (none with `HTIE`/`TCIE` off); one-shot
  end; `DMAEN` without a channel → `OVR`; late channel enable picks up the
  pending value; clearing `OVR` resumes; secure ADC read by a non-secure
  channel terminates (reads 0, warns, ADC overruns); a target reset clears the
  GPDMA but keeps the request bindings; snapshot with a non-empty FIFO and a
  half-done block.

Firmware (register-level, like the other `signal-adc-*` fixtures, BKPT 0x7f,
added to `firmware-build` and `.github/workflows/test-firmware.yml`):
- `test-stm32h5f4-signal-adc-dma-oneshot`: 4-channel scan, one-shot DMA,
  `TCF` interrupt, buffer matches the trace codes, `DR` frozen.
- `test-stm32h5f4-signal-adc-dma-circular`: continuous + self-linked LLI
  started HAL-style (null LLI0), `HTF`/`TCF` interrupts over two buffer laps;
  also run on `stm32h563` and `stm32h533`.
- `test-stm32h5f4-signal-adc-dma-ovr`: `DMAEN` with no channel enabled ends
  in `OVR`.

## Implementation steps

1. Bus access + GPDMA: `mm_memmap_dma_read/write`, `stm32_dma_req.h`,
   router, `SWREQ`/`REQSEL`, beats, end of block, LLR rules, `TCEM`, null
   LLI0, `USEF`, suspend/abort, `MISR`/`SMISR`, security, `PAM` = 00,
   warnings, save/load, `first_2d_channel`; `stm32_gpdma_test.c`. Existing
   H5/U585 callers keep building (router pointer NULL: no `ready` calls).
2. ADC: `DMAEN`/`DMACFG`, `adc_dma_service`, guards, `OVR` block, one-shot
   end, re-arm, snapshot fields; tests in `stm32_adc_test.c`.
3. H5 glue: router in `stm32h5_mmio.c`, request lines in `stm32h5_adc.c`,
   init order, GPDMA save/load registration on H5 and U585, `--record` TB
   check; `stm32h5_adc_dma_test.c`.
4. Firmware fixtures, CMake, workflow; update `docs/signal-injection-adc.md`
   (drop `DMAEN` from the warning list, link here).
