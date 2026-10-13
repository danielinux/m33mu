/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * CPU0 vector table.  The RT700 boot ROM reads its image header from the
 * reserved vector slots: +0x20 image length, +0x24 image type, +0x34 image
 * execution address (0 = execute in place).
 */
#include <stdint.h>

extern void Reset_Handler(void);
extern void fault_handler(void);
extern void CTIMER0_IRQHandler(void);
extern void MU1_A_IRQHandler(void);
extern void OS_EVENT_IRQHandler(void);
extern uint32_t _estack;
extern uint32_t _image_size;
extern uint32_t _image_exec_addr;

static void default_handler(void)
{
    __asm volatile("bkpt #0x7e");
    for (;;) {
    }
}

#define IRQ_CTIMER0 3
#define IRQ_MU1_A 30
#define IRQ_OS_EVENT 34

__attribute__((section(".isr_vector")))
const uint32_t vector_table[16 + 64] = {
    [0] = (uint32_t)&_estack,
    [1] = (uint32_t)&Reset_Handler,
    [2] = (uint32_t)&default_handler,        /* NMI */
    [3] = (uint32_t)&fault_handler,          /* HardFault */
    [4] = (uint32_t)&fault_handler,          /* MemManage */
    [5] = (uint32_t)&fault_handler,          /* BusFault */
    [6] = (uint32_t)&fault_handler,          /* UsageFault */
    [7] = (uint32_t)&fault_handler,          /* SecureFault */
    [8] = (uint32_t)&_image_size,            /* imageLength */
    [9] = 0u,                                /* imageType: plain image */
    [13] = (uint32_t)&_image_exec_addr,      /* imageExecutionAddress */
    [11] = (uint32_t)&default_handler,       /* SVC */
    [14] = (uint32_t)&default_handler,       /* PendSV */
    [15] = (uint32_t)&default_handler,       /* SysTick */
    [16 + IRQ_CTIMER0] = (uint32_t)&CTIMER0_IRQHandler,
    [16 + IRQ_MU1_A] = (uint32_t)&MU1_A_IRQHandler,
    [16 + IRQ_OS_EVENT] = (uint32_t)&OS_EVENT_IRQHandler,
};
