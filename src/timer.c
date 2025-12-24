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

#include "m33mu/timer.h"

void mm_timer_init(const struct mm_target_cfg *cfg, struct mmio_bus *bus, struct mm_nvic *nvic)
{
    if (cfg == 0 || cfg->timer_init == 0) {
        return;
    }
    cfg->timer_init(bus, nvic);
}

void mm_timer_reset(const struct mm_target_cfg *cfg)
{
    if (cfg == 0 || cfg->timer_reset == 0) {
        return;
    }
    cfg->timer_reset();
}

void mm_timer_tick(const struct mm_target_cfg *cfg, mm_u64 cycles)
{
    if (cfg == 0 || cfg->timer_tick == 0) {
        return;
    }
    cfg->timer_tick(cycles);
}
