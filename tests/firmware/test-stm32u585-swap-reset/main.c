/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2026  Daniele Lacamera <root@danielinux.net>
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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * SWAP_BANK is a non-volatile option bit on the STM32U5: it must survive
 * a system reset. First boot sets SWAP_BANK and requests a reset via
 * AIRCR.SYSRESETREQ; second boot checks that SWAP_BANK is still set.
 * Load this image into BOTH banks (offset 0 and 0x100000) with
 * --dualbank, so that execution continues after the mapping swap.
 */

#include <stdint.h>

#define FLASH_OPTR   (*(volatile uint32_t *)0x40022040u)
#define OPTR_SWAP    (1u << 20)
#define AIRCR        (*(volatile uint32_t *)0xE000ED0Cu)
#define AIRCR_RESET  0x05FA0004u

/* RAM survives the in-process reset; use it as a boot counter. */
#define MARKER       (*(volatile uint32_t *)0x20000000u)
#define MARKER_MAGIC 0xCAFEBABEu

extern uint32_t _estack;

static void bkpt_ok(void)
{
    __asm volatile("bkpt #0x7f");
    while (1) { }
}

static void bkpt_fail(void)
{
    __asm volatile("bkpt #0x7e");
    while (1) { }
}

void Reset_Handler(void)
{
    if (MARKER != MARKER_MAGIC) {
        MARKER = MARKER_MAGIC;
        FLASH_OPTR |= OPTR_SWAP;
        AIRCR = AIRCR_RESET;
        while (1) { }
    }
    if ((FLASH_OPTR & OPTR_SWAP) != 0u) {
        bkpt_ok();
    }
    bkpt_fail();
}

static void spin(void)
{
    while (1) { }
}

__attribute__((section(".isr_vector"), used))
const void * const vector_table[16] = {
    (void *)&_estack,
    (void *)Reset_Handler,
    (void *)spin, (void *)spin, (void *)spin, (void *)spin,
    (void *)spin, (void *)spin, (void *)spin, (void *)spin,
    (void *)spin, (void *)spin, (void *)spin, (void *)spin,
    (void *)spin, (void *)spin,
};
