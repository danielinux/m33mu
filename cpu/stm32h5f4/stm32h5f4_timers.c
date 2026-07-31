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

#include "stm32h5f4/stm32h5f4_timers.h"
#include "stm32h5f4/stm32h5f4_mmio.h"
#include "stm32_timers.h"

static struct stm32_timers_state g_timers;

static const struct stm32_timer_soc g_soc = {
    0x9cu,
    mm_stm32h5f4_rcc_regs,
    mm_stm32h5f4_tzsc_regs,
    mm_stm32h5f4_exti_set_nvic,
    mm_stm32h5f4_watchdog_tick
};

void mm_stm32h5f4_timers_tick(mm_u64 cycles)
{
    stm32_timers_tick(&g_timers, cycles);
}

void mm_stm32h5f4_timers_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    stm32_timers_init(&g_timers, &g_soc, bus, nvic);
}

void mm_stm32h5f4_timers_reset(void)
{
    stm32_timers_reset(&g_timers);
}
