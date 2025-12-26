/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdint.h>

extern void Reset_Handler(void);
extern unsigned long _estack;

static void default_handler(void)
{
    while (1) { }
}

void NMI_Handler(void)            __attribute__((weak, alias("default_handler")));
void HardFault_Handler(void)      __attribute__((weak, alias("default_handler")));
void MemManage_Handler(void)      __attribute__((weak, alias("default_handler")));
void BusFault_Handler(void)       __attribute__((weak, alias("default_handler")));
void UsageFault_Handler(void)     __attribute__((weak, alias("default_handler")));
void SVC_Handler(void)            __attribute__((weak, alias("default_handler")));
void DebugMon_Handler(void)       __attribute__((weak, alias("default_handler")));
void PendSV_Handler(void)         __attribute__((weak, alias("default_handler")));
void SysTick_Handler(void)        __attribute__((weak, alias("default_handler")));
void LPIT0_IRQHandler(void)       __attribute__((weak, alias("default_handler")));
void GPIOA_INT0_IRQHandler(void)  __attribute__((weak, alias("default_handler")));
void GPIOA_INT1_IRQHandler(void)  __attribute__((weak, alias("default_handler")));
void GPIOB_INT0_IRQHandler(void)  __attribute__((weak, alias("default_handler")));
void GPIOB_INT1_IRQHandler(void)  __attribute__((weak, alias("default_handler")));
void GPIOC_INT0_IRQHandler(void)  __attribute__((weak, alias("default_handler")));
void GPIOC_INT1_IRQHandler(void)  __attribute__((weak, alias("default_handler")));
void GPIOD_INT0_IRQHandler(void)  __attribute__((weak, alias("default_handler")));
void GPIOD_INT1_IRQHandler(void)  __attribute__((weak, alias("default_handler")));

__attribute__((section(".isr_vector")))
const uint32_t vector_table[16 + 67] = {
    [0] = (uint32_t)&_estack,
    [1] = (uint32_t)&Reset_Handler,
    [2] = (uint32_t)&NMI_Handler,
    [3] = (uint32_t)&HardFault_Handler,
    [4] = (uint32_t)&MemManage_Handler,
    [5] = (uint32_t)&BusFault_Handler,
    [6] = (uint32_t)&UsageFault_Handler,
    [7] = 0, [8] = 0, [9] = 0, [10] = 0,
    [11] = (uint32_t)&SVC_Handler,
    [12] = (uint32_t)&DebugMon_Handler,
    [13] = 0,
    [14] = (uint32_t)&PendSV_Handler,
    [15] = (uint32_t)&SysTick_Handler,
    [16 + 36] = (uint32_t)&LPIT0_IRQHandler,
    [16 + 59] = (uint32_t)&GPIOA_INT0_IRQHandler,
    [16 + 60] = (uint32_t)&GPIOA_INT1_IRQHandler,
    [16 + 61] = (uint32_t)&GPIOB_INT0_IRQHandler,
    [16 + 62] = (uint32_t)&GPIOB_INT1_IRQHandler,
    [16 + 63] = (uint32_t)&GPIOC_INT0_IRQHandler,
    [16 + 64] = (uint32_t)&GPIOC_INT1_IRQHandler,
    [16 + 65] = (uint32_t)&GPIOD_INT0_IRQHandler,
    [16 + 66] = (uint32_t)&GPIOD_INT1_IRQHandler
};
