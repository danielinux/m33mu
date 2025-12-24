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

#include "m33mu/table_branch.h"

mm_bool mm_table_branch_target(const struct mm_memmap *map,
                               enum mm_sec_state sec,
                               mm_u32 pc_fetch,
                               mm_u32 rn_value,
                               mm_u32 rm_value,
                               mm_bool is_tbh,
                               mm_u32 *target_pc_out,
                               mm_u32 *fault_addr_out)
{
    mm_u32 addr;
    mm_u32 entry = 0;
    mm_u32 base_pc;
    mm_u32 offset;

    if (map == 0 || target_pc_out == 0) {
        return MM_FALSE;
    }
    if (fault_addr_out != 0) {
        *fault_addr_out = 0;
    }

    addr = rn_value + (is_tbh ? (rm_value << 1) : rm_value);

    if (is_tbh) {
        if (!mm_memmap_read(map, sec, addr, 2u, &entry)) {
            if (fault_addr_out != 0) {
                *fault_addr_out = addr;
            }
            return MM_FALSE;
        }
        entry &= 0xffffu;
    } else {
        mm_u8 b = 0;
        if (!mm_memmap_read8(map, sec, addr, &b)) {
            if (fault_addr_out != 0) {
                *fault_addr_out = addr;
            }
            return MM_FALSE;
        }
        entry = (mm_u32)b;
    }

    /* PC-relative base uses Thumb PC (address + 4, bit0 cleared). */
    base_pc = (pc_fetch + 4u) & ~1u;
    offset = entry * 2u;
    *target_pc_out = (base_pc + offset) | 1u;
    return MM_TRUE;
}
