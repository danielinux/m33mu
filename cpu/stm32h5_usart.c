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

#include "stm32h5_usart.h"
#include "stm32_usart.h"

static struct stm32_usart_state g_usart;
static const struct stm32h5_usart_variant *g_variant;

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

static mm_bool clock_from_table(struct stm32_usart_inst *u)
{
    const struct stm32h5_usart_desc *d;
    if (g_variant == 0 || u->index >= g_variant->count) {
        return MM_TRUE;
    }
    d = &g_variant->desc[u->index];
    if (d->rcc_off == STM32H5_USART_CLOCK_ALWAYS) {
        return MM_TRUE;
    }
    return rcc_bit_on(u, d->rcc_off, d->rcc_bit);
}

void stm32h5_usart_poll(void)
{
    stm32_usart_poll(&g_usart);
}

void stm32h5_usart_reset(void)
{
    stm32_usart_reset(&g_usart);
}

void stm32h5_usart_init(const struct stm32h5_usart_variant *variant,
                        struct mmio_bus *bus,
                        struct mm_nvic *nvic,
                        mm_u32 *rcc_regs,
                        mm_u32 *rcc_regs_s,
                        mm_u32 *tzsc_regs)
{
    size_t i;

    if (variant == 0) {
        return;
    }
    g_variant = variant;
    stm32_usart_state_init(&g_usart, variant->count, nvic);
    for (i = 0; i < g_usart.usart_count; ++i) {
        const struct stm32h5_usart_desc *d = &variant->desc[i];
        struct stm32_usart_inst *u;
        mm_u32 sec_off = (d->sec_reg == STM32H5_USART_SECCFGR2) ? 0x14u : 0x10u;

        stm32_usart_register_instance(&g_usart, bus, i, d->base, d->irq, d->label,
                                      MM_TRUE, MM_TRUE, stm32_usart_uart_rx_trace_enabled());
        u = &g_usart.usarts[i];
        u->rcc_regs = rcc_regs;
        u->rcc_regs_s = rcc_regs_s;
        /* USART3 is the console the firmware tests drive; the shared model
         * watches it for the trace macro. */
        u->watch_macro = (i == 2u) ? MM_TRUE : MM_FALSE;
        u->sec_reg = (tzsc_regs != 0) ? (tzsc_regs + (sec_off / 4u)) : 0;
        u->sec_bitmask = d->sec_bitmask;
        u->clock_on = clock_from_table;
    }
}
