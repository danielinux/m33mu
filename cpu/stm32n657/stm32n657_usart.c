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

/* STM32N657: USART1 debug console (PE5 TX / PE6 RX, AF7, 115200 8N1).
 * The neural-bass firmware drives it in blocking mode; the shared STM32
 * USART model handles the transmit path to the host. */

#include "stm32n657/stm32n657_usart.h"
#include "stm32_usart.h"

static struct stm32_usart_state g_usart;

void mm_stm32n657_usart_poll(void)
{
    stm32_usart_poll(&g_usart);
}

void mm_stm32n657_usart_reset(void)
{
    stm32_usart_reset(&g_usart);
}

void mm_stm32n657_usart_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    stm32_usart_state_init(&g_usart, 1, nvic);
    /* USART1: APB2, base 0x42001000 (NS) / 0x52001000 (S), IRQ 159. The
     * console instance is index 0. */
    stm32_usart_register_instance(&g_usart, bus, 0, 0x42001000u, 159,
                                  "USART1", MM_TRUE, MM_TRUE,
                                  stm32_usart_uart_rx_trace_enabled());
}
