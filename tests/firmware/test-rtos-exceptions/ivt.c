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
extern void HardFault_Handler(void);
extern void UsageFault_Handler(void);
extern void SVC_Handler(void);
extern void PendSV_Handler(void);
extern void SysTick_Handler(void);

__attribute__((section(".isr_vector")))
const uint32_t vector_table[] = {
    (uint32_t)&_estack, /* initial SP */
    (uint32_t)&Reset_Handler, /* Reset */
    0, /* NMI */
    (uint32_t)&HardFault_Handler, /* HardFault */
    0, /* MemManage */
    0, /* BusFault */
    (uint32_t)&UsageFault_Handler, /* UsageFault */
    0, 0, 0, 0, /* Reserved */
    (uint32_t)&SVC_Handler, /* SVC */
    0, /* DebugMon */
    0, /* Reserved */
    (uint32_t)&PendSV_Handler, /* PendSV */
    (uint32_t)&SysTick_Handler, /* SysTick */
};
