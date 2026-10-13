/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdint.h>

extern void Reset_Handler(void);
extern unsigned long _estack;

static void default_handler(void) { while (1) { } }

void NMI_Handler(void)        __attribute__((weak, alias("default_handler")));
void HardFault_Handler(void)  __attribute__((weak, alias("default_handler")));
void SVC_Handler(void)        __attribute__((weak, alias("default_handler")));
void PendSV_Handler(void)     __attribute__((weak, alias("default_handler")));
void SysTick_Handler(void)    __attribute__((weak, alias("default_handler")));

/*
 * M2354 implements 132 exceptions: 16 system + 116 external.  ARMv8-M Baseline
 * has no MemManage/BusFault/UsageFault/DebugMonitor -- those escalate to
 * HardFault -- so their slots stay reserved.
 */
__attribute__((section(".isr_vector")))
const uint32_t vector_table[16 + 116] = {
    [0]  = (uint32_t)&_estack,
    [1]  = (uint32_t)&Reset_Handler,
    [2]  = (uint32_t)&NMI_Handler,
    [3]  = (uint32_t)&HardFault_Handler,
    [11] = (uint32_t)&SVC_Handler,
    [14] = (uint32_t)&PendSV_Handler,
    [15] = (uint32_t)&SysTick_Handler,
};
