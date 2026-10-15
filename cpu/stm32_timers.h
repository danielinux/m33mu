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

#ifndef M33MU_STM32_TIMERS_H
#define M33MU_STM32_TIMERS_H

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "m33mu/types.h"

struct stm32_timers_state;

struct stm32_timer_soc {
    mm_u32 apb1enr_offset;
    mm_u32 *(*rcc_regs)(void);
    mm_u32 *(*tzsc_regs)(void);
    void (*exti_set_nvic)(struct mm_nvic *nvic);
    void (*watchdog_tick)(mm_u64 cycles);
};

struct stm32_timer_inst {
    struct stm32_timers_state *owner;
    mm_u32 index;
    mm_u32 base;
    mm_u32 cr1;
    mm_u32 dier;
    mm_u32 sr;
    mm_u32 cnt;
    mm_u32 psc;
    mm_u32 arr;
    mm_u32 arr_mask;
    mm_u32 smcr;
    mm_u32 ccmr[2];
    mm_u32 ccer;
    mm_u32 ccr[4];
    mm_u8 icpsc_count[4];
    mm_u64 psc_accum;
    int irq;
    mm_u32 *rcc_regs;
    mm_u32 *sec_reg;
    mm_u32 sec_bitmask;
    mm_bool secure_only;
    enum mm_sec_state current_sec;
};

struct stm32_timers_state {
    struct stm32_timer_inst timers[4];
    struct mm_nvic *nvic;
    const struct stm32_timer_soc *soc;
};

void stm32_timers_init(struct stm32_timers_state *state,
                       const struct stm32_timer_soc *soc,
                       struct mmio_bus *bus,
                       struct mm_nvic *nvic);
void stm32_timers_reset(struct stm32_timers_state *state);
void stm32_timers_tick(struct stm32_timers_state *state, mm_u64 cycles);

/* An edge on input TIti (1..4) of timer tim (0 = TIM2), late_cycles ago.
 * Feeds the capture channels and the slave reset mode. */
void stm32_timers_input_edge(struct stm32_timers_state *state, mm_u32 tim, int ti,
                             mm_bool rising, mm_u64 late_cycles);

#endif /* M33MU_STM32_TIMERS_H */
