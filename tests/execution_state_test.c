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
    struct mmio_region external_regions[2];
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
    mm_memmap_init(&state.map, state.regions,
                   sizeof(state.regions) / sizeof(state.regions[0]));
    if (state.map.mmio.regions != state.regions ||
        state.map.mmio.region_capacity !=
            sizeof(state.regions) / sizeof(state.regions[0])) {
        fprintf(stderr, "execution_state_test: owned MMIO storage binding failed\n");
        return 1;
    }
    state.core_sys.dwt.vcycles = &state.clock.virtual_cycles;
    if (!mm_core_sys_register(&state.map.mmio, &state.core_sys) ||
        state.map.mmio.region_count == 0u ||
        state.core_sys.dwt.vcycles != &state.clock.virtual_cycles) {
        fprintf(stderr, "execution_state_test: owned core system registration failed\n");
        return 1;
    }
    mm_memmap_init(&state.map, external_regions,
                   sizeof(external_regions) / sizeof(external_regions[0]));
    if (state.map.mmio.regions != external_regions ||
        state.map.mmio.region_capacity != 2u) {
        fprintf(stderr, "execution_state_test: external MMIO storage binding failed\n");
        return 1;
    }
    puts("execution_state_test: ok");
    return 0;
}
