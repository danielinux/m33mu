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

#ifndef M33MU_SIGNAL_H
#define M33MU_SIGNAL_H

#include "m33mu/types.h"

#define MM_SIGNAL_MAX_TIMEBASES   16u
#define MM_SIGNAL_MAX_TRACES      32u
#define MM_SIGNAL_MAX_BINDINGS    32u
#define MM_SIGNAL_TRACE_NAME_LEN  32u

enum mm_signal_role {
    MM_SIGNAL_ROLE_GPIO = 0
};

void mm_signal_reset(void);

/* Call after each SoC reset. Replay keeps running; bound pins get their
 * level (sample 0 before the trigger) written by the next mm_signal_advance(). */
void mm_signal_prepare_for_run(void);

mm_bool mm_signal_load(const char *path);

/* GPIO thresholds default to VDD/2 +/- 125mV; an explicit VIL or VIH overrides
 * its side only. Set all of them before mm_signal_bind(). */
void mm_signal_set_vdd_mv(mm_i32 vdd_mv);
void mm_signal_set_gpio_vil_mv(mm_i32 vil_mv);
void mm_signal_set_gpio_vih_mv(mm_i32 vih_mv);
mm_i32 mm_signal_gpio_vil_mv(void);
mm_i32 mm_signal_gpio_vih_mv(void);
void mm_signal_set_cpu_hz(mm_u64 hz);

/* Call with the PC about to execute, before mm_tb_run() or a single step.
 * pc has the Thumb bit cleared. */
void mm_signal_check_trigger(mm_u32 pc);

/* Translation blocks end before a trigger PC so that it is seen at entry. */
mm_bool mm_signal_is_trigger_pc(mm_u32 pc);

/* Call after execution with the cycles just executed. */
void mm_signal_advance(mm_u64 cycles);

/* Cycles until the next scheduled pin change, or (mm_u64)-1 if none.
 * Idle fast-forwarding must not skip past it. */
mm_u64 mm_signal_cycles_until_next_event(void);

/* bank: 0=A, 1=B, ...; returns the binding id or -1. */
int mm_signal_bind(const char *trace_name, int bank, int pin,
                    enum mm_signal_role role);

/* One-shot: the first time pc == trigger_pc, every binding starts at t=0. */
void mm_signal_set_trigger(mm_u32 trigger_pc);
mm_bool mm_signal_trigger_set(void);
mm_bool mm_signal_triggered(void);

/* Replay clock: ns since the first instruction, advanced by mm_signal_advance(). */
mm_u64 mm_signal_now_ns(void);
/* MM_FALSE until the trigger has fired. */
mm_bool mm_signal_trigger_ns(mm_u64 *ns_out);

#endif /* M33MU_SIGNAL_H */
