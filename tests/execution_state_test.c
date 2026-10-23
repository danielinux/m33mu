#include <stdio.h>
#include <string.h>

#include "m33mu/execution_state.h"

static int test_dual_core_reset_keeps_staged_peripheral_state(void)
{
    struct mm_execution_state state;
    memset(&state, 0xA5, sizeof(state));
    state.cores[0].scs.cpuid = 0x410fc241u;
    state.cores[1].scs.cpuid = 0x410fc241u;
    state.cores[0].nvic.enable_mask[0] = 0x12345678u;
    state.cores[1].nvic.enable_mask[0] = 0x87654321u;

    mm_execution_state_reset_cores(&state, 2u, 0x1000u, 0x2000u);
    if (state.cores[0].cpu.vtor_s != 0x1000u ||
        state.cores[0].cpu.vtor_ns != 0x2000u ||
        state.cores[0].cpu.sleeping || state.cores[0].cpu.sleep_wfe ||
        state.cores[1].cpu.vtor_s != 0x1000u ||
        state.cores[1].cpu.vtor_ns != 0x2000u ||
        !state.cores[1].cpu.sleeping || !state.cores[1].cpu.sleep_wfe ||
        state.active_core != 0u ||
        state.cores[0].it_pattern != 0u || state.cores[0].it_remaining != 0u ||
        state.cores[0].it_cond != 0u || state.cores[1].it_pattern != 0u ||
        state.cores[1].it_remaining != 0u || state.cores[1].it_cond != 0u) {
        return 1;
    }
    if (state.cores[0].scs.cpuid != 0x410fc241u ||
        state.cores[1].scs.cpuid != 0x410fc241u ||
        state.cores[0].nvic.enable_mask[0] != 0x12345678u ||
        state.cores[1].nvic.enable_mask[0] != 0x87654321u) {
        return 1;
    }
    return 0;
}

int main(void)
{
    struct mm_execution_state state;
    if (test_dual_core_reset_keeps_staged_peripheral_state() != 0) {
        fprintf(stderr, "execution_state_test: dual-core reset failed\n");
        return 1;
    }
    mm_execution_state_reset_cores(0, 1u, 0u, 0u);
    memset(&state, 0, sizeof(state));
    mm_execution_state_reset_cores(&state, 1u, 0x3000u, 0x4000u);
    if (state.cores[0].cpu.vtor_s != 0x3000u ||
        state.cores[0].cpu.vtor_ns != 0x4000u ||
        state.cores[1].cpu.sleeping || state.active_core != 0u) {
        fprintf(stderr, "execution_state_test: single-core reset failed\n");
        return 1;
    }
    puts("execution_state_test: ok");
    return 0;
}
