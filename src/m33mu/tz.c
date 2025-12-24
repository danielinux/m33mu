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

#include "m33mu/tz.h"

/* LR value used for a Secure->Non-secure BLXNS call so that a subsequent
 * BX LR in Non-secure state returns to Secure via the emulator.
 * Must not look like EXC_RETURN (0xFFxxxxxx). */
#define MM_TZ_RET_LR_SENTINEL 0xDEAD0001u

static void tz_sync_r13_from_active_sp(struct mm_cpu *cpu)
{
    mm_u32 sp;
    if (cpu == 0) {
        return;
    }
    sp = mm_cpu_get_active_sp(cpu);
    mm_cpu_set_active_sp(cpu, sp);
}

void mm_tz_exec_sg(struct mm_cpu *cpu)
{
    if (cpu == 0) {
        return;
    }
    /* Minimal model: SG is used for NS->S transition via an NSC veneer. */
    if (cpu->sec_state == MM_NONSECURE) {
        cpu->sec_state = MM_SECURE;
        tz_sync_r13_from_active_sp(cpu);
    }
}

void mm_tz_exec_bxns(struct mm_cpu *cpu, mm_u32 target)
{
    if (cpu == 0) {
        return;
    }
    /* BXNS is defined for Secure->Non-secure transition. */
    cpu->sec_state = MM_NONSECURE;
    tz_sync_r13_from_active_sp(cpu);
    cpu->r[15] = target | 1u;
}

void mm_tz_exec_blxns(struct mm_cpu *cpu, mm_u32 target, mm_u32 return_addr)
{
    mm_u32 ra;
    if (cpu == 0) {
        return;
    }
    /* BLXNS is defined for Secure->Non-secure call. */
    ra = return_addr | 1u;
    if (cpu->tz_depth < MM_TZ_STACK_MAX) {
        cpu->tz_ret_pc[cpu->tz_depth] = ra;
        cpu->tz_ret_sec[cpu->tz_depth] = cpu->sec_state;
        cpu->tz_ret_mode[cpu->tz_depth] = cpu->mode;
        cpu->tz_depth++;
    }
    cpu->r[14] = MM_TZ_RET_LR_SENTINEL;
    cpu->sec_state = MM_NONSECURE;
    tz_sync_r13_from_active_sp(cpu);
    cpu->r[15] = target | 1u;
}
