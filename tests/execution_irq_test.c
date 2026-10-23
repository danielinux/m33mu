/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "m33mu/execution_irq.h"
#include "m33mu/vector.h"

int main(void)
{
    struct mm_cpu cpu;
    struct mm_scs scs;
    struct mm_nvic nvic;
    enum mm_sec_state selected = MM_SECURE;
    mm_u8 preempt = 0u;
    mm_u8 raw = 0u;

    memset(&cpu, 0, sizeof(cpu));
    mm_scs_init(&scs, 0u);
    mm_nvic_init(&nvic);
    cpu.sec_state = MM_SECURE;

    cpu.primask_s = 1u;
    cpu.primask_ns = 0u;
    if (!mm_execution_irq_primask_blocks_current(&cpu) ||
        !mm_execution_irq_primask_blocks_for_sec(&cpu, MM_SECURE) ||
        mm_execution_irq_primask_blocks_for_sec(&cpu, MM_NONSECURE)) {
        printf("execution_irq_test: banked PRIMASK selection failed\n");
        return 1;
    }
    cpu.primask_s = 0u;
    cpu.primask_ns = 1u;
    cpu.faultmask_s = 1u;
    cpu.faultmask_ns = 0u;
    cpu.sec_state = MM_NONSECURE;
    if (!mm_execution_irq_primask_blocks_current(&cpu) ||
        mm_execution_irq_faultmask_blocks_current(&cpu) ||
        !mm_execution_irq_faultmask_blocks_for_sec(&cpu, MM_SECURE)) {
        printf("execution_irq_test: current security mask selection failed\n");
        return 1;
    }
    cpu.faultmask_ns = 1u;
    if (!mm_execution_irq_faultmask_blocks_current(&cpu)) {
        printf("execution_irq_test: current Non-secure FAULTMASK selection failed\n");
        return 1;
    }

    scs.pend_st_s = MM_TRUE;
    scs.pend_st_ns = MM_TRUE;
    scs.shpr3_s = 0x80000000u;
    scs.shpr3_ns = 0x20000000u;
    if (!mm_execution_irq_systick_pending_select(&cpu, &scs, &selected) ||
        selected != MM_NONSECURE) {
        printf("execution_irq_test: dual-bank SysTick priority selection failed\n");
        return 1;
    }
    scs.pend_st_s = MM_FALSE;
    if (!mm_execution_irq_systick_pending_select(&cpu, &scs, &selected) ||
        selected != MM_NONSECURE) {
        printf("execution_irq_test: single-bank SysTick selection failed\n");
        return 1;
    }

    cpu.basepri_s = 0x40u;
    scs.shpr3_s = 0x80000000u;
    if (!mm_execution_irq_basepri_blocks_system_exc_for_sec(
            &cpu, &scs, MM_SECURE, MM_VECT_SYSTICK)) {
        printf("execution_irq_test: BASEPRI did not block lower-priority SysTick\n");
        return 1;
    }
    scs.shpr3_s = 0x20000000u;
    if (mm_execution_irq_basepri_blocks_system_exc_for_sec(
            &cpu, &scs, MM_SECURE, MM_VECT_SYSTICK)) {
        printf("execution_irq_test: BASEPRI blocked higher-priority SysTick\n");
        return 1;
    }

    cpu.mode = MM_HANDLER;
    cpu.xpsr = 16u + 3u;
    cpu.sec_state = MM_SECURE;
    nvic.priority[3] = 0x60u;
    if (!mm_execution_irq_current_execution_priority(
            &cpu, &scs, &nvic, &preempt, &raw) || raw != 0x60u || preempt != 0x60u) {
        printf("execution_irq_test: active IRQ priority lookup failed\n");
        return 1;
    }

    printf("execution_irq_test: security banks and priority masks\n");
    return 0;
}
