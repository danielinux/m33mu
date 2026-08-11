/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2025  Daniele Lacamera <root@danielinux.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 */

/*
 * External signal injection: peripheral-agnostic replay of a
 * voltage-vs-time trace, sourced from an offline-converted binary
 * container (see docs/... for the .sigc format), into an emulated pin.
 *
 * Phase 1 (this file) covers GPIO input bindings only: a trace's
 * millivolt values are used to precompute a Schmitt-trigger
 * threshold-crossing schedule at bind time, then dispatched at replay
 * time through mm_gpio_set_external_input() (m33mu/gpio.h), which in turn
 * reaches whichever target's GPIO/EXTI model is currently registered.
 *
 * See design docs (m33mu-signal-injection-gpio.md) for the full rationale
 * behind each decision referenced in comments below.
 */

#ifndef M33MU_SIGNAL_H
#define M33MU_SIGNAL_H

#include "m33mu/types.h"

#define MM_SIGNAL_MAX_TIMEBASES   16u
#define MM_SIGNAL_MAX_TRACES      32u
#define MM_SIGNAL_MAX_BINDINGS    32u
#define MM_SIGNAL_MAX_CROSSINGS   4096u  /* per GPIO binding */
#define MM_SIGNAL_TRACE_NAME_LEN  32u

#define MM_SIGNAL_MASTER_GROUP    0u

enum mm_signal_role {
    MM_SIGNAL_ROLE_GPIO = 0
    /* MM_SIGNAL_ROLE_ADC follows in the ADC phase (see
     * m33mu-signal-injection-adc.md); intentionally not added here. */
};

/* Reset all loaded state (timebases, traces, bindings). Safe to call
 * before mm_signal_load() even if nothing was ever loaded. */
void mm_signal_reset(void);

/* Lighter-weight than mm_signal_reset(): re-arms every configured master
 * trigger (fired -> not fired) and rewinds every binding's cursor to
 * un-triggered, without discarding the loaded container or binding table.
 * Call this once per emulator soft-reset (the "for(;;)" loop in main.c
 * that reinitialises the target on --record/watchdog/system reset), the
 * same way mm_timer_reset()/mm_iotsafe_uart_reset_all() etc. are called
 * per iteration -- CLI-configured bindings and the loaded trace file are
 * a one-time setup, but a fresh boot should see every trigger as
 * not-yet-fired again (decision #10: one fresh run = one deterministic
 * t=0). */
void mm_signal_prepare_for_run(void);

/* Loads a .sigc container (see file-format doc) from disk. Replaces any
 * previously loaded container. Returns MM_FALSE on I/O error, malformed
 * header, or a container exceeding the MM_SIGNAL_MAX_* limits above. */
mm_bool mm_signal_load(const char *path);

/* Sets the board electrical reference used to derive GPIO digital
 * thresholds (decision #17): rising = vdd_mv/2 + 125mV, falling =
 * vdd_mv/2 - 125mV, per the STM32H5F4 datasheet's typical 250mV Schmitt
 * hysteresis figure. Must be called before mm_signal_bind() for a GPIO
 * role, since the crossing schedule is precomputed at bind time using
 * the threshold in effect at that moment. */
void mm_signal_set_vdd_mv(mm_i32 vdd_mv);

/* Sets the cycle->ns conversion rate used for all replay timing (section 1
 * of the design doc: signal replay uses cycle-derived elapsed time, the
 * same way src/main.c's deadline_ns() does, not the host wall clock).
 * Call this with the same cpu_hz value used elsewhere in main.c for the
 * current target before the first mm_signal_advance() call; defaults to
 * 64MHz if never called. */
void mm_signal_set_cpu_hz(mm_u64 hz);

/* Checks the configured master trigger(s) against pc (Thumb bit already
 * masked off by the caller). Call this once per dispatch, with the PC
 * about to be executed, BEFORE that code actually runs -- for both the
 * scalar per-instruction interpreter path and the translation-block (tb)
 * fast path (src/main.c), using whichever PC that path is about to
 * dispatch (cpu.r[15] before mm_tb_run(), or before a single-instruction
 * step). Split from time advancement (mm_signal_advance() below)
 * specifically because the tb path only observes PC at block-dispatch
 * granularity, not per instruction -- see the design doc's note on this:
 * a trigger address must be a genuine branch target (a function entry
 * reached via BL/B, an interrupt vector, a loop head), which is exactly
 * what a translation block boundary is, so this still gives correct
 * one-shot semantics as long as the configured trigger_pc is such an
 * address (true for the recommended noinline marker-function pattern). A
 * trigger address that only occurs mid-block (never itself a branch
 * target) would never be observed and would silently never fire -- this
 * is a real constraint of the block-based fast path, not a bug to work
 * around here. */
void mm_signal_check_trigger(mm_u32 pc);

/* Advances every triggered binding's elapsed-time cursor by `cycles`
 * (already-executed instruction cycles -- may be a batch, e.g. an entire
 * translation block's ops_executed count, not necessarily 1) and
 * dispatches any GPIO threshold crossings whose scheduled time has been
 * reached. Call this once per dispatch, AFTER the corresponding code has
 * actually executed -- mirrors exactly how mm_timer_tick() is called
 * from both the scalar and tb-batched paths in src/main.c, so that
 * signal replay and the rest of the emulator's peripherals share the
 * identical cycle-accounting granularity. */
void mm_signal_advance(mm_u64 cycles);

/* Binds a named trace (as it appears in the container) to a peripheral
 * pin, as part of the given replay group. bank/pin follow the same
 * indexing as mm_gpio_bank_read() (bank 0=A, 1=B, ...; pin 0-15).
 * Returns a binding id (>=0) or -1 on failure (unknown trace name, no
 * timebase, binding table full, or -- for MM_SIGNAL_ROLE_GPIO -- a
 * crossing schedule that would exceed MM_SIGNAL_MAX_CROSSINGS). */
int mm_signal_bind(const char *trace_name, int bank, int pin,
                    enum mm_signal_role role, mm_u32 group_id);

/* Configures the one-shot master trigger (decisions #7-#10): the first
 * time the emulator's PC equals trigger_pc (Thumb bit already masked off
 * by the caller -- see main.c's PC-watch site), every binding in
 * group_id has its elapsed-time cursor reset to 0 and replay begins.
 * Only one trigger address is supported per group in this phase (matches
 * the CLI surface in the design doc); calling this again for the same
 * group replaces the previous configuration and re-arms it. */
void mm_signal_set_master_trigger(mm_u32 group_id, mm_u32 trigger_pc);

/* True once mm_signal_set_master_trigger()'s configured PC has been hit
 * at least once for that group. Intended for diagnostics: a test harness
 * or --signal-debug dump can use this to flag a trigger that never fired
 * (see the trigger-symbol-fragility open item in the design doc) rather
 * than silently reading a never-started trace. */
mm_bool mm_signal_group_triggered(mm_u32 group_id);

#endif /* M33MU_SIGNAL_H */
