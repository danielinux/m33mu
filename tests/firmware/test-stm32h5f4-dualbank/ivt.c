/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2025  Daniele Lacamera <root@danielinux.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 */

#include <stdint.h>

extern void Reset_Handler(void);
extern unsigned long _estack;

/* Weak default handlers: spin. */
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
void TIM2_IRQHandler(void)        __attribute__((weak, alias("default_handler")));
void TIM3_IRQHandler(void)        __attribute__((weak, alias("default_handler")));
void TIM4_IRQHandler(void)        __attribute__((weak, alias("default_handler")));
void TIM5_IRQHandler(void)        __attribute__((weak, alias("default_handler")));

__attribute__((section(".isr_vector")))
const uint32_t vector_table[16 + 49] = {
    [0] = (uint32_t)&_estack,              /* 0: Initial SP */
    [1] = (uint32_t)&Reset_Handler,        /* 1: Reset */
    [2] = (uint32_t)&NMI_Handler,          /* 2: NMI */
    [3] = (uint32_t)&HardFault_Handler,    /* 3: HardFault */
    [4] = (uint32_t)&MemManage_Handler,    /* 4: MemManage */
    [5] = (uint32_t)&BusFault_Handler,     /* 5: BusFault */
    [6] = (uint32_t)&UsageFault_Handler,   /* 6: UsageFault */
    [7] = 0, [8] = 0, [9] = 0, [10] = 0,   /* 7-10: Reserved */
    [11] = (uint32_t)&SVC_Handler,         /* 11: SVCall */
    [12] = (uint32_t)&DebugMon_Handler,    /* 12: DebugMon */
    [13] = 0,                              /* 13: Reserved */
    [14] = (uint32_t)&PendSV_Handler,      /* 14: PendSV */
    [15] = (uint32_t)&SysTick_Handler,     /* 15: SysTick */
    [16 ... 64] = (uint32_t)&default_handler,
    [16 + 45] = (uint32_t)&TIM2_IRQHandler,
    [16 + 46] = (uint32_t)&TIM3_IRQHandler,
    [16 + 47] = (uint32_t)&TIM4_IRQHandler,
    [16 + 48] = (uint32_t)&TIM5_IRQHandler
};
