/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_EXECUTION_STATE_H
#define M33MU_EXECUTION_STATE_H

#include "m33mu/cpu.h"
#include "m33mu/execution_clock.h"
#include "m33mu/nvic.h"
#include "m33mu/scs.h"
#include "m33mu/memmap.h"
#include "m33mu/code_cache.h"
#include "m33mu/mem_prot.h"
#include "m33mu/core_sys.h"
#include "m33mu/target_storage.h"

struct mm_execution_core_state {
    struct mm_cpu cpu;
    struct mm_scs scs;
    struct mm_nvic nvic;
    mm_u8 it_pattern;
    mm_u8 it_remaining;
    mm_u8 it_cond;
};

/* Owned architectural, memory and scheduler state for a single emulation
 * session. Target peripheral instances and signal state remain to be moved. */
struct mm_execution_state {
    struct mm_target_cfg target;
    struct mm_memmap map;
    struct mmio_region regions[256];
    struct mm_code_cache code_cache;
    struct mm_prot_ctx protection[2];
    struct mm_core_sys core_sys;
    struct mm_target_storage storage;
    struct mm_execution_core_state cores[2];
    struct mm_scs_mux scs_mux;
    struct mm_execution_clock clock;
    mm_u32 active_core;
};

void mm_execution_state_init(struct mm_execution_state *state);
void mm_execution_state_end_run(struct mm_execution_state *state);
void mm_execution_state_destroy(struct mm_execution_state *state);

/* Reset CPU/IT state while preserving the staged SCS and NVIC initialization
 * order used by target and protection setup. */
void mm_execution_state_reset_cores(struct mm_execution_state *state,
                                    mm_u32 core_count,
                                    mm_u32 vtor_s,
                                    mm_u32 vtor_ns);

#endif /* M33MU_EXECUTION_STATE_H */
