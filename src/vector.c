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

#include "m33mu/vector.h"
#include <stdio.h>

mm_bool mm_vector_read(const struct mm_memmap *map, enum mm_sec_state sec, mm_u32 vtor, mm_u32 index, mm_u32 *value_out)
{
    mm_u32 addr;
    if (map == 0 || value_out == 0) {
        return MM_FALSE;
    }
    addr = vtor + (index * 4u);
    return mm_memmap_read(map, sec, addr, 4u, value_out);
}

mm_bool mm_vector_apply_reset(struct mm_cpu *cpu, const struct mm_memmap *map, enum mm_sec_state sec)
{
    mm_u32 initial_sp = 0;
    mm_u32 reset_pc = 0;

    if (cpu == 0) {
        return MM_FALSE;
    }

    if (!mm_vector_read(map, sec, (sec == MM_NONSECURE) ? cpu->vtor_ns : cpu->vtor_s, 0u, &initial_sp)) {
        fprintf(stderr, "vector: failed to read initial SP\n");
        return MM_FALSE;
    }
    if (!mm_vector_read(map, sec, (sec == MM_NONSECURE) ? cpu->vtor_ns : cpu->vtor_s, MM_VECT_RESET, &reset_pc)) {
        fprintf(stderr, "vector: failed to read reset PC\n");
        return MM_FALSE;
    }

    /* ARMv8‑M resets with T-bit set; keep other flags clear. */
    cpu->xpsr = 0x01000000u;
    mm_cpu_set_active_sp(cpu, initial_sp);
    cpu->r[15] = reset_pc | 1u;
    cpu->sec_state = sec;
    cpu->mode = MM_THREAD;
    mm_cpu_set_privileged(cpu, MM_FALSE); /* start privileged (nPRIV=0) */
    return MM_TRUE;
}
