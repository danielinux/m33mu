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
#include "stm32_usart.h"

static struct stm32_usart_state g_usart;

static mm_bool rcc_bit_on(struct stm32_usart_inst *u, mm_u32 word_off, mm_u32 bit)
{
    /* The RCC clock-enable bits are shared hardware state: the secure and
     * non-secure register views both act on the same clock, so the enable
     * written through either alias must be honored whatever the security
     * state of the peripheral access. */
    if (u->rcc_regs == 0 && u->rcc_regs_s == 0) {
        return MM_TRUE;
    }
    if (u->rcc_regs != 0 && ((u->rcc_regs[word_off / 4u] >> bit) & 1u) != 0u) {
        return MM_TRUE;
    }
    if (u->rcc_regs_s != 0 && ((u->rcc_regs_s[word_off / 4u] >> bit) & 1u) != 0u) {
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool clock_apb2_usart1(struct stm32_usart_inst *u)
{
    return rcc_bit_on(u, 0xa4u, 14u);
}

static mm_bool clock_apb1lenr_generic(struct stm32_usart_inst *u)
{
    mm_u32 bit = 0;
    switch (u->index) {
    case 1: bit = 17; break;
    case 2: bit = 18; break;
    case 3: bit = 19; break;
    case 4: bit = 20; break;
    case 5: bit = 25; break;
    case 6: bit = 30; break;
    case 7: bit = 31; break;
    default: return MM_TRUE;
    }
    return rcc_bit_on(u, 0x9cu, bit);
}

static mm_bool clock_apb1henr_generic(struct stm32_usart_inst *u)
{
    mm_u32 bit = 0;
    switch (u->index) {
    case 8: bit = 0; break;
    case 11: bit = 1; break;
    default: return MM_TRUE;
    }
    return rcc_bit_on(u, 0xa0u, bit);
}

static mm_bool clock_apb3_lpuart1(struct stm32_usart_inst *u)
{
    return rcc_bit_on(u, 0xa8u, 6u);
}

void mm_stm32h563_usart_poll(void)
{
    stm32_usart_poll(&g_usart);
}

void mm_stm32h563_usart_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    static const mm_u32 bases[] = {
        0x40013800u, 0x40004400u, 0x40004800u, 0x40004C00u,
        0x40005000u, 0x40006400u, 0x40007800u, 0x40007C00u,
        0x40008000u, 0x40006800u, 0x40006C00u, 0x40008400u,
        0x44002400u
    };
    static const int irq_map[] = {
        58, 59, 60, 61, 62, 85, 98, 99, 100, 86, 87, 101, 63
    };
    static const char *labels[] = {
        "USART1", "USART2", "USART3", "UART4", "UART5", "USART6",
        "UART7", "UART8", "UART9", "USART10", "USART11", "UART12",
        "LPUART1"
    };
    mm_u32 *tz = mm_stm32h563_tzsc_regs();
    mm_u32 *tz2 = tz != 0 ? tz + (0x14u / 4u) : 0;
    mm_u32 *tz1 = tz != 0 ? tz + (0x10u / 4u) : 0;
    size_t i;
    stm32_usart_state_init(&g_usart, sizeof(bases) / sizeof(bases[0]), nvic);
    mm_stm32h563_rng_set_nvic(nvic);
    mm_stm32h563_usb_set_nvic(nvic);
    for (i = 0; i < g_usart.usart_count; ++i) {
        struct stm32_usart_inst *u;
        stm32_usart_register_instance(&g_usart, bus, i, bases[i], irq_map[i], labels[i],
                                      MM_TRUE, MM_TRUE, stm32_usart_uart_rx_trace_enabled());
        u = &g_usart.usarts[i];
        u->rcc_regs = mm_stm32h563_rcc_regs();
        u->rcc_regs_s = mm_stm32h563_rcc_secure_regs();
        u->watch_macro = (i == 2u) ? MM_TRUE : MM_FALSE;
        u->sec_reg = tz1;
        if (i == 0) {
            u->clock_on = clock_apb2_usart1;
            u->sec_bitmask = (1u << 11);
            u->sec_reg = tz2;
        } else if (i >= 1 && i <= 7) {
            u->clock_on = clock_apb1lenr_generic;
            if (i == 1) u->sec_bitmask = (1u << 13);
            else if (i == 2) u->sec_bitmask = (1u << 14);
            else if (i == 5) u->sec_bitmask = (1u << 21);
            else if (i == 9) u->sec_bitmask = (1u << 22);
            else if (i == 10) u->sec_bitmask = (1u << 23);
        } else if (i == 12u) {
            u->clock_on = clock_apb3_lpuart1;
            u->sec_reg = tz2;
            u->sec_bitmask = (1u << 25);
        } else {
            u->clock_on = clock_apb1henr_generic;
        }
    }
}

void mm_stm32h563_usart_reset(void)
{
    stm32_usart_reset(&g_usart);
}
