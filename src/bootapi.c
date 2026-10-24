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


#include "m33mu/bootapi.h"
#include "mcxn947/mcxn947_romapi.h"
#include "lpc55s69/lpc55s69_romapi.h"
#include "rw612/rw612_romapi.h"
#include "imxrt700/imxrt700_romapi.h"
#include "rp2350/rp2350_bootrom.h"
#include "rp2350/rp2350_mmio.h"

mm_bool mm_target_bootapi_handle(struct mm_cpu *cpu, struct mm_memmap *map)
{
    if (mm_rp2350_bootrom_handle(cpu, map)) {
        return MM_TRUE;
    }
    if (mm_mcxn947_romapi_handle(cpu, map)) {
        return MM_TRUE;
    }
    if (mm_lpc55s69_romapi_handle(cpu, map)) {
        return MM_TRUE;
    }
    if (mm_rw612_romapi_handle(cpu, map)) {
        return MM_TRUE;
    }
    if (mm_imxrt700_romapi_handle(cpu, map)) {
        return MM_TRUE;
    }
    return MM_FALSE;
}

void mm_target_deactivate_boot_state(void)
{
    mm_rp2350_deactivate();
    mm_mcxn947_romapi_deactivate();
    mm_lpc55s69_romapi_deactivate();
    mm_rw612_romapi_deactivate();
    mm_imxrt700_romapi_deactivate();
}
