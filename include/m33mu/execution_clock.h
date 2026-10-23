/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_EXECUTION_CLOCK_H
#define M33MU_EXECUTION_CLOCK_H

#include "m33mu/scs.h"
#include "m33mu/target.h"

/* Advance the production virtual cycle clock and cycle-driven peripherals.
 * The counter pointers may be null for callers that do not track that view. */
void mm_execution_advance_cycles(struct mm_scs *scs,
                                 const struct mm_target_cfg *cfg,
                                 mm_u64 cycles,
                                 mm_bool check_signal_trigger,
                                 mm_u32 trigger_pc,
                                 mm_u64 *cycle_total,
                                 mm_u64 *virtual_cycles,
                                 mm_u64 *cycles_since_poll);

#endif
