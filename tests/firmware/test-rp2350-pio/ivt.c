/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdint.h>

extern void Reset_Handler(void);
extern void PIO0_IRQ0_Handler(void);
extern unsigned long _estack;

static void default_handler(void)
{
    while (1) { }
}

void NMI_Handler(void)        __attribute__((weak, alias("default_handler")));
void HardFault_Handler(void)  __attribute__((weak, alias("default_handler")));

__attribute__((section(".isr_vector")))
const uint32_t vector_table[16 + 64] = {
    [0] = (uint32_t)&_estack,
    [1] = (uint32_t)&Reset_Handler,
    [2] = (uint32_t)&NMI_Handler,
    [3] = (uint32_t)&HardFault_Handler,
    /* PIO0_IRQ_0 is interrupt 15 */
    [16 + 15] = (uint32_t)&PIO0_IRQ0_Handler
};
