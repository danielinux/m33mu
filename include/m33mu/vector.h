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

#ifndef M33MU_VECTOR_H
#define M33MU_VECTOR_H

#include "m33mu/types.h"
#include "m33mu/memmap.h"
#include "m33mu/cpu.h"

/* Vector indices for system exceptions. */
enum mm_vector_index {
    MM_VECT_RESET = 1,
    MM_VECT_NMI = 2,
    MM_VECT_HARDFAULT = 3,
    MM_VECT_MEMMANAGE = 4,
    MM_VECT_BUSFAULT = 5,
    MM_VECT_USAGEFAULT = 6,
    MM_VECT_SECUREFAULT = 7,
    MM_VECT_SVCALL = 11,
    MM_VECT_DEBUGMON = 12,
    MM_VECT_PENDSV = 14,
    MM_VECT_SYSTICK = 15
};

/* Initialize CPU state from vector table at VTOR for given security state. */
mm_bool mm_vector_apply_reset(struct mm_cpu *cpu, const struct mm_memmap *map, enum mm_sec_state sec);

/* Return vector table entry (word) for given index. */
mm_bool mm_vector_read(const struct mm_memmap *map, enum mm_sec_state sec, mm_u32 vtor, mm_u32 index, mm_u32 *value_out);

#endif /* M33MU_VECTOR_H */
