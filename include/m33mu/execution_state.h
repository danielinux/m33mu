/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_EXECUTION_STATE_H
#define M33MU_EXECUTION_STATE_H

#include "m33mu/cpu.h"
#include "m33mu/execution_clock.h"
#include "m33mu/nvic.h"
#include "m33mu/scs.h"

struct mm_execution_core_state {
    struct mm_cpu cpu;
    struct mm_scs scs;
    struct mm_nvic nvic;
    mm_u8 it_pattern;
    mm_u8 it_remaining;
    mm_u8 it_cond;
};

/* Owned architectural and scheduler state for a single emulation session.
 * Memory maps, protection contexts, and peripheral instances are added as
 * their remaining process-global dependencies are extracted. */
struct mm_execution_state {
    struct mm_execution_core_state cores[2];
    struct mm_scs_mux scs_mux;
    struct mm_execution_clock clock;
    mm_u32 active_core;
};

/* Reset CPU/IT state while preserving the staged SCS and NVIC initialization
 * order used by target and protection setup. */
void mm_execution_state_reset_cores(struct mm_execution_state *state,
                                    mm_u32 core_count,
                                    mm_u32 vtor_s,
                                    mm_u32 vtor_ns);

#endif /* M33MU_EXECUTION_STATE_H */
