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

#ifndef M33MU_TABLE_BRANCH_H
#define M33MU_TABLE_BRANCH_H

#include "m33mu/types.h"
#include "m33mu/memmap.h"
#include "m33mu/cpu.h"

/* Compute target PC for Thumb-2 table branch instructions (TBB/TBH).
 * Returns MM_TRUE if the table entry read succeeds and outputs a Thumb PC
 * value (bit0 set). On failure, returns MM_FALSE and sets *fault_addr_out
 * to the address that faulted.
 *
 * For TBB: table index = Rm
 * For TBH: table index = Rm << 1
 * Target = (pc_fetch + 4) + (entry * 2)
 */
mm_bool mm_table_branch_target(const struct mm_memmap *map,
                               enum mm_sec_state sec,
                               mm_u32 pc_fetch,
                               mm_u32 rn_value,
                               mm_u32 rm_value,
                               mm_bool is_tbh,
                               mm_u32 *target_pc_out,
                               mm_u32 *fault_addr_out);

#endif /* M33MU_TABLE_BRANCH_H */

