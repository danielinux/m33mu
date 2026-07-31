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

#include "stm32h533/stm32h533_usart.h"
#include "stm32h533/stm32h533_mmio.h"
#include "stm32h533/stm32h533_usb.h"
#include "stm32h5_usart.h"

/* RCC offsets: APB1LENR 0x9c, APB2ENR 0xa4, APB3ENR 0xa8. The H533 carries
 * the smaller USART complement: no UART7..12, so no APB1HENR instance. */
static const struct stm32h5_usart_desc g_desc[] = {
    { 0x40013800u, 58, "USART1",  0xa4u, 14u, STM32H5_USART_SECCFGR2, 1u << 11 },
    { 0x40004400u, 59, "USART2",  0x9cu, 17u, STM32H5_USART_SECCFGR1, 1u << 13 },
    { 0x40004800u, 60, "USART3",  0x9cu, 18u, STM32H5_USART_SECCFGR1, 1u << 14 },
    { 0x40004C00u, 61, "UART4",   0x9cu, 19u, STM32H5_USART_SECCFGR1, 0u },
    { 0x40005000u, 62, "UART5",   0x9cu, 20u, STM32H5_USART_SECCFGR1, 0u },
    { 0x40006400u, 85, "USART6",  0x9cu, 25u, STM32H5_USART_SECCFGR1, 1u << 21 },
    { 0x44002400u, 63, "LPUART1", 0xa8u,  6u, STM32H5_USART_SECCFGR2, 1u << 25 }
};

static const struct stm32h5_usart_variant g_variant = {
    g_desc, sizeof(g_desc) / sizeof(g_desc[0])
};

void mm_stm32h533_usart_poll(void)
{
    stm32h5_usart_poll();
}

void mm_stm32h533_usart_reset(void)
{
    stm32h5_usart_reset();
}

void mm_stm32h533_usart_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    mm_stm32h533_rng_set_nvic(nvic);
    mm_stm32h533_usb_set_nvic(nvic);
    /* The H533 model keeps a single RCC bank shared by both aliases, so
     * there is no separate secure view to consult. */
    stm32h5_usart_init(&g_variant, bus, nvic,
                       mm_stm32h533_rcc_regs(), 0,
                       mm_stm32h533_tzsc_regs());
}
