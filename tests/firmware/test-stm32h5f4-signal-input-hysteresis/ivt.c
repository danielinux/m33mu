/* Shared vector table for the signal-injection GPIO test firmware family.
 * EXTI IRQn = 11 + line (see cpu/stm32h5_mmio.c's exti_raise_irq()), and
 * IRQn occupies vector_table[16 + IRQn], so EXTIn_Handler sits at
 * vector_table[27 + line]. Individual tests override the specific
 * EXTIn_Handler(s) they need via a strong definition in their own main.c;
 * everything else falls back to default_handler. */

#include <stdint.h>

extern void Reset_Handler(void);
extern unsigned long _estack;

static void default_handler(void)
{
    while (1) {
    }
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

void EXTI0_Handler(void)  __attribute__((weak, alias("default_handler")));
void EXTI1_Handler(void)  __attribute__((weak, alias("default_handler")));
void EXTI2_Handler(void)  __attribute__((weak, alias("default_handler")));
void EXTI3_Handler(void)  __attribute__((weak, alias("default_handler")));
void EXTI4_Handler(void)  __attribute__((weak, alias("default_handler")));
void EXTI5_Handler(void)  __attribute__((weak, alias("default_handler")));
void EXTI6_Handler(void)  __attribute__((weak, alias("default_handler")));
void EXTI7_Handler(void)  __attribute__((weak, alias("default_handler")));
void EXTI8_Handler(void)  __attribute__((weak, alias("default_handler")));
void EXTI9_Handler(void)  __attribute__((weak, alias("default_handler")));

__attribute__((section(".isr_vector")))
const uint32_t vector_table[16 + 61] = {
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
    [16 ... 76] = (uint32_t)&default_handler,
    /* IRQn = 11+line -> vector_table[16 + 11 + line] = vector_table[27+line] */
    [27] = (uint32_t)&EXTI0_Handler,
    [28] = (uint32_t)&EXTI1_Handler,
    [29] = (uint32_t)&EXTI2_Handler,
    [30] = (uint32_t)&EXTI3_Handler,
    [31] = (uint32_t)&EXTI4_Handler,
    [32] = (uint32_t)&EXTI5_Handler,
    [33] = (uint32_t)&EXTI6_Handler,
    [34] = (uint32_t)&EXTI7_Handler,
    [35] = (uint32_t)&EXTI8_Handler,
    [36] = (uint32_t)&EXTI9_Handler,
};
