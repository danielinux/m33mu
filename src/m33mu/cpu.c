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

#include "m33mu/cpu.h"

#define CONTROL_SPSEL_MASK 0x2u

static mm_bool control_sp_sel(const struct mm_cpu *cpu)
{
    mm_u32 control = cpu->sec_state == MM_NONSECURE ? cpu->control_ns : cpu->control_s;
    return (control & CONTROL_SPSEL_MASK) != 0u;
}

static void cpu_init_msp_top(struct mm_cpu *cpu, enum mm_sec_state sec, mm_u32 value)
{
    if (sec == MM_NONSECURE) {
        if (!cpu->msp_top_ns_valid && value != 0u) {
            cpu->msp_top_ns = value;
            cpu->msp_min_ns = value;
            cpu->msp_top_ns_valid = MM_TRUE;
        }
        return;
    }
    if (!cpu->msp_top_s_valid && value != 0u) {
        cpu->msp_top_s = value;
        cpu->msp_min_s = value;
        cpu->msp_top_s_valid = MM_TRUE;
    }
}

static void cpu_update_msp_min(struct mm_cpu *cpu, enum mm_sec_state sec, mm_u32 value)
{
    if (sec == MM_NONSECURE) {
        if (cpu->msp_top_ns_valid && value < cpu->msp_min_ns) {
            cpu->msp_min_ns = value;
        }
        return;
    }
    if (cpu->msp_top_s_valid && value < cpu->msp_min_s) {
        cpu->msp_min_s = value;
    }
}

void mm_cpu_note_msp_top(struct mm_cpu *cpu, enum mm_sec_state sec)
{
    mm_u32 value;
    if (cpu == 0) {
        return;
    }
    value = (sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s;
    cpu_init_msp_top(cpu, sec, value);
}

mm_u32 mm_cpu_get_active_sp(const struct mm_cpu *cpu)
{
    mm_bool use_psp;
    if (cpu == 0) {
        return 0;
    }
    if (cpu->mode == MM_HANDLER) {
        use_psp = MM_FALSE;
    } else {
        use_psp = control_sp_sel(cpu);
    }
    if (use_psp) {
        return (cpu->sec_state == MM_NONSECURE) ? cpu->psp_ns : cpu->psp_s;
    }
    return (cpu->sec_state == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s;
}

void mm_cpu_set_active_sp(struct mm_cpu *cpu, mm_u32 value)
{
    mm_bool use_psp;
    if (cpu == 0) {
        return;
    }
    if (cpu->mode == MM_HANDLER) {
        use_psp = MM_FALSE;
    } else {
        use_psp = control_sp_sel(cpu);
    }
    if (use_psp) {
        if (cpu->sec_state == MM_NONSECURE) cpu->psp_ns = value;
        else cpu->psp_s = value;
    } else {
        if (cpu->sec_state == MM_NONSECURE) {
            cpu->msp_ns = value;
        } else {
            cpu->msp_s = value;
        }
        cpu_init_msp_top(cpu, cpu->sec_state, value);
        cpu_update_msp_min(cpu, cpu->sec_state, value);
    }
    /* Mirror active SP into general-purpose R13 so push/pop style instructions
     * operate on the currently selected stack pointer.
     */
    cpu->r[13] = value;
}

mm_u32 mm_cpu_get_active_splim(const struct mm_cpu *cpu)
{
    mm_bool use_psp;
    if (cpu == 0) {
        return 0;
    }
    if (cpu->mode == MM_HANDLER) {
        use_psp = MM_FALSE;
    } else {
        use_psp = control_sp_sel(cpu);
    }
    if (use_psp) {
        return (cpu->sec_state == MM_NONSECURE) ? cpu->psplim_ns : cpu->psplim_s;
    }
    return (cpu->sec_state == MM_NONSECURE) ? cpu->msplim_ns : cpu->msplim_s;
}

mm_u32 mm_cpu_get_msp(const struct mm_cpu *cpu, enum mm_sec_state sec)
{
    if (cpu == 0) {
        return 0;
    }
    return (sec == MM_NONSECURE) ? cpu->msp_ns : cpu->msp_s;
}

void mm_cpu_set_msp(struct mm_cpu *cpu, enum mm_sec_state sec, mm_u32 value)
{
    if (cpu == 0) {
        return;
    }
    if (sec == MM_NONSECURE) cpu->msp_ns = value;
    else cpu->msp_s = value;
    if (sec == MM_SECURE) {
        cpu_init_msp_top(cpu, sec, value);
    }
    cpu_update_msp_min(cpu, sec, value);
}

mm_u32 mm_cpu_get_psp(const struct mm_cpu *cpu, enum mm_sec_state sec)
{
    if (cpu == 0) {
        return 0;
    }
    return (sec == MM_NONSECURE) ? cpu->psp_ns : cpu->psp_s;
}

void mm_cpu_set_psp(struct mm_cpu *cpu, enum mm_sec_state sec, mm_u32 value)
{
    if (cpu == 0) {
        return;
    }
    if (sec == MM_NONSECURE) cpu->psp_ns = value;
    else cpu->psp_s = value;
}

mm_u32 mm_cpu_get_control(const struct mm_cpu *cpu, enum mm_sec_state sec)
{
    if (cpu == 0) {
        return 0;
    }
    return (sec == MM_NONSECURE) ? cpu->control_ns : cpu->control_s;
}

void mm_cpu_set_control(struct mm_cpu *cpu, enum mm_sec_state sec, mm_u32 value)
{
    if (cpu == 0) {
        return;
    }
    if (sec == MM_NONSECURE) cpu->control_ns = value;
    else cpu->control_s = value;
    if (sec == MM_NONSECURE) {
        cpu->priv_ns = (value & 0x1u) != 0u;
    } else {
        cpu->priv_s = (value & 0x1u) != 0u;
    }
}

mm_u32 mm_cpu_get_vtor(const struct mm_cpu *cpu, enum mm_sec_state sec)
{
    if (cpu == 0) {
        return 0;
    }
    return (sec == MM_NONSECURE) ? cpu->vtor_ns : cpu->vtor_s;
}

void mm_cpu_set_vtor(struct mm_cpu *cpu, enum mm_sec_state sec, mm_u32 value)
{
    if (cpu == 0) {
        return;
    }
    if (sec == MM_NONSECURE) cpu->vtor_ns = value;
    else cpu->vtor_s = value;
}

void mm_cpu_set_mode(struct mm_cpu *cpu, enum mm_mode mode)
{
    if (cpu != 0) {
        cpu->mode = mode;
    }
}

enum mm_mode mm_cpu_get_mode(const struct mm_cpu *cpu)
{
    if (cpu == 0) {
        return MM_THREAD;
    }
    return cpu->mode;
}

void mm_cpu_set_security(struct mm_cpu *cpu, enum mm_sec_state sec)
{
    if (cpu != 0) {
        cpu->sec_state = sec;
    }
}

enum mm_sec_state mm_cpu_get_security(const struct mm_cpu *cpu)
{
    if (cpu == 0) {
        return MM_SECURE;
    }
    return cpu->sec_state;
}

mm_bool mm_cpu_get_privileged(const struct mm_cpu *cpu)
{
    if (cpu == 0) {
        return MM_TRUE;
    }
    if (cpu->sec_state == MM_NONSECURE) {
        return cpu->priv_ns ? MM_FALSE : MM_TRUE;
    }
    return cpu->priv_s ? MM_FALSE : MM_TRUE;
}

void mm_cpu_set_privileged(struct mm_cpu *cpu, mm_bool unprivileged)
{
    if (cpu == 0) {
        return;
    }
    if (cpu->sec_state == MM_NONSECURE) {
        cpu->priv_ns = unprivileged;
        cpu->control_ns = (cpu->control_ns & ~1u) | (unprivileged ? 1u : 0u);
    } else {
        cpu->priv_s = unprivileged;
        cpu->control_s = (cpu->control_s & ~1u) | (unprivileged ? 1u : 0u);
    }
}

void mm_cpu_excl_set(struct mm_cpu *cpu, enum mm_sec_state sec, mm_u32 addr, mm_u32 size)
{
    if (cpu == 0) {
        return;
    }
    cpu->excl_valid = MM_TRUE;
    cpu->excl_sec = sec;
    cpu->excl_addr = addr;
    cpu->excl_size = size;
}

void mm_cpu_excl_clear(struct mm_cpu *cpu)
{
    if (cpu == 0) {
        return;
    }
    cpu->excl_valid = MM_FALSE;
    cpu->excl_addr = 0;
    cpu->excl_size = 0;
    cpu->excl_sec = MM_NONSECURE;
}

mm_bool mm_cpu_excl_check_and_clear(struct mm_cpu *cpu, enum mm_sec_state sec, mm_u32 addr, mm_u32 size)
{
    mm_bool ok;

    if (cpu == 0) {
        return MM_FALSE;
    }

    ok = cpu->excl_valid &&
         cpu->excl_sec == sec &&
         cpu->excl_addr == addr &&
         cpu->excl_size == size;

    /* Both success and failure clear the local exclusive monitor. */
    mm_cpu_excl_clear(cpu);
    return ok;
}
