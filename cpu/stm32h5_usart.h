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

#ifndef M33MU_STM32H5_USART_H
#define M33MU_STM32H5_USART_H

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "m33mu/types.h"

/* USART/UART/LPUART block shared by the STM32H5 parts. The peripheral
 * itself is the generic stm32_usart.c model; what differs between parts
 * is only which instances exist and, per instance, which RCC bit gates
 * its clock and which GTZC TZSC bit filters it. That is expressed as a
 * plain table so a part is described by data alone. */

/* rcc_off value meaning "this instance has no modelled clock gate". */
#define STM32H5_USART_CLOCK_ALWAYS 0xFFFFFFFFu

/* sec_reg selects which TZSC secure-configuration register holds this
 * instance's bit, as an offset in words from the TZSC base. */
#define STM32H5_USART_SECCFGR1 1u
#define STM32H5_USART_SECCFGR2 2u

struct stm32h5_usart_desc {
    mm_u32 base;
    int irq;
    const char *label;
    mm_u32 rcc_off;     /* RCC enable register byte offset, or _CLOCK_ALWAYS */
    mm_u32 rcc_bit;
    mm_u8 sec_reg;      /* STM32H5_USART_SECCFGR1 or _SECCFGR2 */
    mm_u32 sec_bitmask; /* 0 when the instance is not TZ-filtered */
};

struct stm32h5_usart_variant {
    const struct stm32h5_usart_desc *desc;
    size_t count;
};

/* rcc_regs_s may be null on parts that do not expose a secure RCC alias;
 * when both banks are supplied a clock counts as enabled if either view
 * has the bit set, the secure and non-secure aliases being the same
 * hardware clock. */
void stm32h5_usart_init(const struct stm32h5_usart_variant *variant,
                        struct mmio_bus *bus,
                        struct mm_nvic *nvic,
                        mm_u32 *rcc_regs,
                        mm_u32 *rcc_regs_s,
                        mm_u32 *tzsc_regs);
void stm32h5_usart_reset(void);
void stm32h5_usart_poll(void);

#endif /* M33MU_STM32H5_USART_H */
