/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "m33mu/execution_state.h"

#include <string.h>

void mm_execution_state_init(struct mm_execution_state *state)
{
    if (state == 0) {
        return;
    }
    memset(state, 0, sizeof(*state));
    mm_execution_clock_reset(&state->clock);
}

void mm_execution_state_end_run(struct mm_execution_state *state)
{
    if (state == 0) {
        return;
    }
    mm_code_cache_release(&state->code_cache);
}

void mm_execution_state_destroy(struct mm_execution_state *state)
{
    if (state == 0) {
        return;
    }
    mm_execution_state_end_run(state);
    mm_target_storage_destroy(&state->storage);
    memset(state, 0, sizeof(*state));
}

void mm_execution_state_reset_cores(struct mm_execution_state *state,
                                    mm_u32 core_count,
                                    mm_u32 vtor_s,
                                    mm_u32 vtor_ns)
{
    if (state == 0) {
        return;
    }
    mm_cpu_reset(&state->cores[0].cpu, vtor_s, vtor_ns, MM_FALSE, MM_FALSE);
    state->cores[0].it_pattern = 0u;
    state->cores[0].it_remaining = 0u;
    state->cores[0].it_cond = 0u;
    state->active_core = 0u;
    if (core_count > 1u) {
        mm_cpu_reset(&state->cores[1].cpu, vtor_s, vtor_ns, MM_TRUE, MM_TRUE);
        state->cores[1].it_pattern = 0u;
        state->cores[1].it_remaining = 0u;
        state->cores[1].it_cond = 0u;
    }
}
