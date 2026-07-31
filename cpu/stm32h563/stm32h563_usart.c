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

#include "stm32h563/stm32h563_usart.h"
#include "stm32h563/stm32h563_mmio.h"
#include "stm32h563/stm32h563_usb.h"
#include "stm32h5_usart.h"

/* RCC offsets: APB1LENR 0x9c, APB1HENR 0xa0, APB2ENR 0xa4, APB3ENR 0xa8.
 * Clock and TZSC bit numbers are from RM0481 and the STM32H563 SVD. */
static const struct stm32h5_usart_desc g_desc[] = {
    { 0x40013800u,  58, "USART1",  0xa4u, 14u, STM32H5_USART_SECCFGR2, 1u << 11 },
    { 0x40004400u,  59, "USART2",  0x9cu, 17u, STM32H5_USART_SECCFGR1, 1u << 13 },
    { 0x40004800u,  60, "USART3",  0x9cu, 18u, STM32H5_USART_SECCFGR1, 1u << 14 },
    { 0x40004C00u,  61, "UART4",   0x9cu, 19u, STM32H5_USART_SECCFGR1, 1u << 15 },
    { 0x40005000u,  62, "UART5",   0x9cu, 20u, STM32H5_USART_SECCFGR1, 1u << 16 },
    { 0x40006400u,  85, "USART6",  0x9cu, 25u, STM32H5_USART_SECCFGR1, 1u << 21 },
    { 0x40007800u,  98, "UART7",   0x9cu, 30u, STM32H5_USART_SECCFGR1, 1u << 26 },
    { 0x40007C00u,  99, "UART8",   0x9cu, 31u, STM32H5_USART_SECCFGR1, 1u << 27 },
    { 0x40008000u, 100, "UART9",   0xa0u,  0u, STM32H5_USART_SECCFGR1, 1u << 28 },
    { 0x40006800u,  86, "USART10", 0x9cu, 26u, STM32H5_USART_SECCFGR1, 1u << 22 },
    { 0x40006C00u,  87, "USART11", 0x9cu, 27u, STM32H5_USART_SECCFGR1, 1u << 23 },
    { 0x40008400u, 101, "UART12",  0xa0u,  1u, STM32H5_USART_SECCFGR1, 1u << 29 },
    { 0x44002400u,  63, "LPUART1", 0xa8u,  6u, STM32H5_USART_SECCFGR2, 1u << 25 }
};

static const struct stm32h5_usart_variant g_variant = {
    g_desc, sizeof(g_desc) / sizeof(g_desc[0])
};

void mm_stm32h563_usart_poll(void)
{
    stm32h5_usart_poll();
}

void mm_stm32h563_usart_reset(void)
{
    stm32h5_usart_reset();
}

void mm_stm32h563_usart_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    mm_stm32h563_rng_set_nvic(nvic);
    mm_stm32h563_usb_set_nvic(nvic);
    stm32h5_usart_init(&g_variant, bus, nvic,
                       mm_stm32h563_rcc_regs(),
                       mm_stm32h563_rcc_secure_regs(),
                       mm_stm32h563_tzsc_regs());
}
