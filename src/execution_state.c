/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "m33mu/execution_state.h"
#include "m33mu/execution_irq.h"

#include <string.h>

#include "m33mu/cpu_db.h"
#include "m33mu/exec_helpers.h"
#include "m33mu/target_hal.h"
#include "m33mu/timer.h"

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

mm_bool mm_execution_state_fetch_decode(
    struct mm_execution_state *state,
    mm_execution_fpu_access_fn fpu_access_allowed,
    mm_execution_decode_fn decode,
    struct mm_fetch_result *fetch_out,
    struct mm_decoded *decoded_out,
    mm_bool *fpu_ok_out)
{
    struct mm_execution_core_state *core;
    mm_bool fpu_ok = MM_TRUE;

    if (fetch_out != 0) {
        memset(fetch_out, 0, sizeof(*fetch_out));
    }
    if (decoded_out != 0) {
        memset(decoded_out, 0, sizeof(*decoded_out));
    }
    if (fpu_ok_out != 0) {
        *fpu_ok_out = MM_TRUE;
    }
    if (state == 0 || fetch_out == 0 || decoded_out == 0 || fpu_ok_out == 0) {
        return MM_FALSE;
    }
    core = &state->cores[state->active_core < 2u ? state->active_core : 0u];
    *fetch_out = mm_fetch_t32_memmap(&core->cpu, &state->map,
                                     core->cpu.sec_state);
    if (fetch_out->fault) {
        return MM_FALSE;
    }
    if (fetch_out->len == 4u && fpu_access_allowed != 0 &&
        !fpu_access_allowed(&core->cpu, &core->scs) &&
        mm_is_vfp_insn_fast(fetch_out->insn)) {
        fpu_ok = MM_FALSE;
    }
    if (!mm_icache_lookup(&state->code_cache, fetch_out,
                          core->cpu.sec_state, fpu_ok, decoded_out)) {
        *decoded_out = (decode != 0) ? decode(fetch_out, &core->cpu, &core->scs)
                                     : mm_decode_t32(fetch_out);
        mm_icache_store(&state->code_cache, fetch_out,
                        core->cpu.sec_state, fpu_ok, decoded_out);
    }
    mm_memmap_set_last_pc(fetch_out->pc_fetch);
    *fpu_ok_out = fpu_ok;
    return MM_TRUE;
}

enum mm_execution_fetch_status mm_execution_state_fetch_decode_or_fault(
    struct mm_execution_state *state,
    mm_execution_fpu_access_fn fpu_access_allowed,
    mm_execution_decode_fn decode,
    mm_execution_mem_fault_fn raise_mem_fault,
    struct mm_fetch_result *fetch_out,
    struct mm_decoded *decoded_out,
    mm_bool *fpu_ok_out)
{
    struct mm_execution_core_state *core;
    if (state == 0 || state->active_core >= 2u || raise_mem_fault == 0 ||
        fetch_out == 0 || decoded_out == 0 || fpu_ok_out == 0) {
        return MM_EXECUTION_FETCH_INVALID;
    }
    core = &state->cores[state->active_core];
    if (mm_execution_state_fetch_decode(state, fpu_access_allowed, decode,
                                        fetch_out, decoded_out, fpu_ok_out)) {
        return MM_EXECUTION_FETCH_READY;
    }
    if (!fetch_out->fault) {
        return MM_EXECUTION_FETCH_INVALID;
    }
    if (raise_mem_fault(&core->cpu, &state->map, &core->scs,
                        core->cpu.r[15] & ~1u, core->cpu.xpsr,
                        fetch_out->fault_addr, MM_TRUE)) {
        return MM_EXECUTION_FETCH_FAULT_HANDLED;
    }
    return MM_EXECUTION_FETCH_FAULT_UNHANDLED;
}

mm_bool mm_execution_state_should_execute(struct mm_execution_state *state,
                                          mm_u32 core_index,
                                          const struct mm_decoded *decoded)
{
    struct mm_execution_core_state *core;
    if (state == 0 || core_index >= 2u || decoded == 0) {
        return MM_TRUE;
    }
    core = &state->cores[core_index];
    return mm_it_should_execute(&core->cpu, decoded, &core->it_pattern,
                                &core->it_remaining, &core->it_cond);
}

void mm_execution_state_advance_it(struct mm_execution_state *state,
                                   mm_u32 core_index,
                                   const struct mm_decoded *decoded)
{
    struct mm_execution_core_state *core;
    if (state == 0 || core_index >= 2u || decoded == 0) {
        return;
    }
    core = &state->cores[core_index];
    mm_it_advance(&core->cpu, decoded, &core->it_pattern,
                  &core->it_remaining, &core->it_cond);
}

mm_bool mm_execution_state_advance_it_if_sequential(
    struct mm_execution_state *state,
    mm_u32 core_index,
    const struct mm_fetch_result *fetch,
    const struct mm_decoded *decoded)
{
    struct mm_execution_core_state *core;
    mm_u32 expected_next;
    if (state == 0 || core_index >= 2u || fetch == 0 || decoded == 0) {
        return MM_FALSE;
    }
    core = &state->cores[core_index];
    expected_next = (fetch->pc_fetch + fetch->len) | 1u;
    if (core->cpu.r[15] != expected_next) {
        return MM_FALSE;
    }
    mm_execution_state_advance_it(state, core_index, decoded);
    return MM_TRUE;
}

enum mm_exec_status mm_execution_state_execute_decoded(
    struct mm_execution_state *state,
    mm_u32 core_index,
    struct mm_execute_ctx *context,
    const struct mm_fetch_result *fetch,
    const struct mm_decoded *decoded,
    mm_bool execute_it)
{
    struct mm_execution_core_state *core;
    enum mm_exec_status status;
    if (state == 0 || core_index >= 2u || context == 0 || fetch == 0 ||
        decoded == 0) {
        return MM_EXEC_CONTINUE;
    }
    core = &state->cores[core_index];
    context->cpu = &core->cpu;
    context->map = &state->map;
    context->scs = &core->scs;
    context->nvic = &core->nvic;
    context->fetch = fetch;
    context->dec = decoded;
    context->it_pattern = &core->it_pattern;
    context->it_remaining = &core->it_remaining;
    context->it_cond = &core->it_cond;
    status = mm_execute_decoded(context);
    if (status == MM_EXEC_OK || execute_it) {
        (void)mm_execution_state_advance_it_if_sequential(state, core_index,
                                                          fetch, decoded);
    }
    return status;
}

void mm_execution_state_init_memory_map(struct mm_execution_state *state)
{
    if (state == 0) {
        return;
    }
    mm_memmap_init(&state->map, state->regions,
                   sizeof(state->regions) / sizeof(state->regions[0]));
}

mm_bool mm_execution_state_configure_memory(struct mm_execution_state *state)
{
    mm_bool ok;
    if (state == 0 || state->storage.flash == 0 || state->storage.ram == 0) {
        return MM_FALSE;
    }
    ok = mm_memmap_configure_flash(&state->map, &state->target,
                                   state->storage.flash, MM_TRUE);
    ok = mm_memmap_configure_flash(&state->map, &state->target,
                                   state->storage.flash, MM_FALSE) && ok;
    ok = mm_memmap_configure_ram(&state->map, &state->target,
                                 state->storage.ram, MM_TRUE) && ok;
    ok = mm_memmap_configure_ram(&state->map, &state->target,
                                 state->storage.ram, MM_FALSE) && ok;
    return ok;
}

void mm_execution_state_init_code_cache(struct mm_execution_state *state)
{
    if (state == 0) {
        return;
    }
    mm_code_cache_init(&state->code_cache, &state->map);
    mm_memmap_set_code_cache(&state->map, &state->code_cache);
}

void mm_execution_state_reset_soc(struct mm_execution_state *state)
{
    if (state == 0) {
        return;
    }
    mm_target_soc_reset(&state->target);
    mm_timer_reset(&state->target);
}

void mm_execution_state_init_target_devices(struct mm_execution_state *state)
{
    if (state == 0) {
        return;
    }
    mm_target_usart_reset(&state->target);
    mm_target_usart_init(&state->target, &state->map.mmio,
                         &state->cores[0].nvic);
    mm_target_spi_reset(&state->target);
    mm_target_spi_init(&state->target, &state->map.mmio,
                       &state->cores[0].nvic);
    mm_target_eth_reset(&state->target);
    mm_target_eth_init(&state->target, &state->map.mmio,
                       &state->cores[0].nvic);
    mm_timer_init(&state->target, &state->map.mmio,
                  &state->cores[0].nvic);
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

mm_bool mm_execution_state_wake_sleeping_core(
    struct mm_execution_state *state,
    mm_u32 core_index)
{
    struct mm_execution_core_state *core;
    enum mm_sec_state systick_sec = MM_SECURE;
    mm_bool wake = MM_FALSE;

    if (state == 0 || core_index >= 2u) {
        return MM_FALSE;
    }
    core = &state->cores[core_index];
    if (!core->cpu.sleeping) {
        return MM_FALSE;
    }
    if (core->cpu.event_reg ||
        mm_execution_irq_systick_pending_select(&core->cpu, &core->scs,
                                                &systick_sec) ||
        core->scs.pend_sv ||
        mm_nvic_select_ex(&core->nvic, &core->cpu, &core->scs) >= 0) {
        wake = MM_TRUE;
    } else if (core->cpu.sleep_wfe) {
        wake = mm_nvic_any_pending(&core->nvic);
    } else {
        /* WFI wakes on enabled pending interrupts even when masked. */
        wake = mm_nvic_any_pending_enabled(&core->nvic);
    }
    if (wake) {
        core->cpu.sleeping = MM_FALSE;
        core->cpu.sleep_wfe = MM_FALSE;
        core->cpu.event_reg = MM_FALSE;
    }
    return wake;
}

enum mm_execution_irq_dispatch_result
mm_execution_state_dispatch_pending(
    struct mm_execution_state *state,
    mm_u32 core_index,
    enum mm_execution_irq_block_policy block_policy,
    mm_execution_irq_enter_fn enter_exception,
    void *enter_opaque,
    mm_bool *done)
{
    struct mm_execution_core_state *core;
    if (state == 0 || core_index >= 2u) {
        return MM_EXECUTION_IRQ_NONE;
    }
    core = &state->cores[core_index];
    return mm_execution_irq_dispatch_pending(
        &core->cpu, &state->map, &core->scs, &core->nvic, block_policy,
        enter_exception, enter_opaque, &core->it_pattern, &core->it_remaining,
        &core->it_cond, done);
}
