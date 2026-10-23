/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "m33mu/execution_state.h"

#include <string.h>

#include "m33mu/cpu_db.h"

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

mm_bool mm_execution_state_select_target(struct mm_execution_state *state,
                                         const char *cpu_name)
{
    if (state == 0 || cpu_name == 0 || state->storage.flash != 0 ||
        state->storage.ram != 0) {
        return MM_FALSE;
    }
    return mm_cpu_lookup(cpu_name, &state->target);
}

mm_bool mm_execution_state_allocate_storage(struct mm_execution_state *state)
{
    if (state == 0 || state->storage.flash != 0 || state->storage.ram != 0 ||
        !mm_target_storage_init(&state->storage, &state->target)) {
        return MM_FALSE;
    }
    state->load_targets.flash = state->storage.flash;
    state->load_targets.flash_size = state->storage.flash_size;
    state->load_targets.ram = state->storage.ram;
    state->load_targets.ram_size = state->storage.ram_size;
    memset(state->storage.flash, 0xff, state->storage.flash_size);
    memset(state->storage.ram, 0, state->storage.ram_size);
    return MM_TRUE;
}

int mm_execution_state_load_elf(struct mm_execution_state *state,
                                const mm_u8 *image,
                                size_t image_size,
                                size_t max_image_size,
                                mm_u32 flash_offset,
                                struct mm_image_load_result *result)
{
    if (state == 0) {
        if (result != 0) {
            memset(result, 0, sizeof(*result));
        }
        return -1;
    }
    return mm_elf_load_memory_bounded(image, image_size, max_image_size,
                                      &state->target, &state->load_targets,
                                      flash_offset, result);
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
