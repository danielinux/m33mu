#include <stdio.h>
#include <string.h>

#include "m33mu/cpu.h"

static int test_primary_reset_clears_cpu_state(void)
{
    struct mm_cpu cpu;
    memset(&cpu, 0xA5, sizeof(cpu));
    mm_cpu_reset(&cpu, 0x1000u, 0x2000u, MM_FALSE, MM_FALSE);

    if (cpu.r[0] != 0u || cpu.r[13] != 0u || cpu.r[15] != 0u ||
        cpu.xpsr != 0u || cpu.ge_flags != 0u || cpu.q_flag != MM_FALSE ||
        cpu.s[31] != 0u || cpu.fpscr != 0u || cpu.fp_active != MM_FALSE ||
        cpu.sec_state != MM_SECURE || cpu.mode != MM_THREAD ||
        cpu.priv_s != MM_FALSE || cpu.priv_ns != MM_FALSE ||
        cpu.msp_s != 0u || cpu.psp_s != 0u || cpu.msplim_ns != 0u ||
        cpu.control_s != 0u || cpu.primask_ns != 0u || cpu.basepri_s != 0u ||
        cpu.faultmask_ns != 0u || cpu.vtor_s != 0x1000u ||
        cpu.vtor_ns != 0x2000u || cpu.exc_depth != 0 || cpu.tz_depth != 0 ||
        cpu.exc_sp[MM_EXC_STACK_MAX - 1] != 0u ||
        cpu.tz_ret_pc[MM_TZ_STACK_MAX - 1] != 0u ||
        cpu.sleeping != MM_FALSE || cpu.sleep_wfe != MM_FALSE ||
        cpu.event_reg != MM_FALSE || cpu.excl_valid != MM_FALSE ||
        cpu.excl_addr != 0u || cpu.vlstm_stack != 0u) {
        return 1;
    }
    return 0;
}

static int test_secondary_reset_starts_parked(void)
{
    struct mm_cpu cpu;
    memset(&cpu, 0xA5, sizeof(cpu));
    mm_cpu_reset(&cpu, 0x4000u, 0x5000u, MM_TRUE, MM_TRUE);
    if (!cpu.sleeping || !cpu.sleep_wfe || cpu.event_reg ||
        cpu.vtor_s != 0x4000u || cpu.vtor_ns != 0x5000u ||
        cpu.sec_state != MM_SECURE || cpu.mode != MM_THREAD) {
        return 1;
    }
    mm_cpu_reset(0, 0u, 0u, MM_FALSE, MM_FALSE);
    return 0;
}

int main(void)
{
    if (test_primary_reset_clears_cpu_state() != 0 ||
        test_secondary_reset_starts_parked() != 0) {
        fprintf(stderr, "cpu_reset_test: failure\n");
        return 1;
    }
    puts("cpu_reset_test: ok");
    return 0;
}
