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

#ifndef M33MU_CPU_H
#define M33MU_CPU_H

#include "m33mu/types.h"

enum mm_sec_state {
    /* Keep numeric values aligned with common debug expectations:
     *  - 0 => Non-secure
     *  - 1 => Secure
     */
    MM_NONSECURE = 0,
    MM_SECURE = 1
};

enum mm_mode {
    MM_THREAD = 0,
    MM_HANDLER = 1
};

/*
 * CPU state with banked special registers and stack pointers for Secure/Non-secure.
 * PC is r[15] and should keep bit0 set for Thumb state.
 */
struct mm_cpu {
    mm_u32 r[16];  /* r0-r15 */
    mm_u32 xpsr;

    enum mm_sec_state sec_state;
    enum mm_mode mode;
    mm_bool priv_s;   /* 0 = privileged, 1 = unprivileged (CONTROL.nPRIV) */
    mm_bool priv_ns;

    /* Banked stack pointers */
    mm_u32 msp_s;
    mm_u32 psp_s;
    mm_u32 msp_ns;
    mm_u32 psp_ns;

    /* Banked stack limits */
    mm_u32 msplim_s;
    mm_u32 psplim_s;
    mm_u32 msplim_ns;
    mm_u32 psplim_ns;

    /* Banked special registers */
    mm_u32 control_s;
    mm_u32 control_ns;
    mm_u32 primask_s;
    mm_u32 primask_ns;
    mm_u32 basepri_s;
    mm_u32 basepri_ns;
    mm_u32 faultmask_s;
    mm_u32 faultmask_ns;

    /* Banked vector table bases */
    mm_u32 vtor_s;
    mm_u32 vtor_ns;

    /* Exception nesting stack: record which stack pointer was used on entry
     * and its value after hardware stacking so EXC_RETURN can unstack from
     * the exact frame without guessing, even if PSP is later adjusted.
     */
#define MM_EXC_STACK_MAX 64
    mm_u32 exc_sp[MM_EXC_STACK_MAX];
    mm_bool exc_use_psp[MM_EXC_STACK_MAX];
    enum mm_sec_state exc_sec[MM_EXC_STACK_MAX];
    int exc_depth;

    /* TrustZone call stack for Secure->Non-secure BLXNS callbacks.
     * This tracks the return target security/mode and address so BX LR
     * in Non-secure state can return back to Secure deterministically.
     */
#define MM_TZ_STACK_MAX 32
    mm_u32 tz_ret_pc[MM_TZ_STACK_MAX];
    enum mm_sec_state tz_ret_sec[MM_TZ_STACK_MAX];
    enum mm_mode tz_ret_mode[MM_TZ_STACK_MAX];
    int tz_depth;

    /* Low-power hint state */
    mm_bool sleeping;   /* WFI/WFE entered */
    mm_bool event_reg;  /* Event register for WFE/SEV */

    /* Local exclusive monitor (LDREX/STREX pairing).
     * Minimal model: remembers the last exclusive address/size/security and
     * validates the next STREX against it. CLREX clears this state.
     */
    mm_bool excl_valid;
    enum mm_sec_state excl_sec;
    mm_u32 excl_addr;
    mm_u32 excl_size;
};

/* Accessors for banked SP and SPLIM based on mode and security. */
mm_u32 mm_cpu_get_active_sp(const struct mm_cpu *cpu);
void mm_cpu_set_active_sp(struct mm_cpu *cpu, mm_u32 value);
mm_u32 mm_cpu_get_active_splim(const struct mm_cpu *cpu);

mm_u32 mm_cpu_get_msp(const struct mm_cpu *cpu, enum mm_sec_state sec);
void mm_cpu_set_msp(struct mm_cpu *cpu, enum mm_sec_state sec, mm_u32 value);
mm_u32 mm_cpu_get_psp(const struct mm_cpu *cpu, enum mm_sec_state sec);
void mm_cpu_set_psp(struct mm_cpu *cpu, enum mm_sec_state sec, mm_u32 value);

mm_u32 mm_cpu_get_control(const struct mm_cpu *cpu, enum mm_sec_state sec);
void mm_cpu_set_control(struct mm_cpu *cpu, enum mm_sec_state sec, mm_u32 value);

mm_u32 mm_cpu_get_vtor(const struct mm_cpu *cpu, enum mm_sec_state sec);
void mm_cpu_set_vtor(struct mm_cpu *cpu, enum mm_sec_state sec, mm_u32 value);

void mm_cpu_set_mode(struct mm_cpu *cpu, enum mm_mode mode);
enum mm_mode mm_cpu_get_mode(const struct mm_cpu *cpu);
void mm_cpu_set_security(struct mm_cpu *cpu, enum mm_sec_state sec);
enum mm_sec_state mm_cpu_get_security(const struct mm_cpu *cpu);

mm_bool mm_cpu_get_privileged(const struct mm_cpu *cpu);
void mm_cpu_set_privileged(struct mm_cpu *cpu, mm_bool unprivileged);

/* Exclusive monitor helpers. */
void mm_cpu_excl_set(struct mm_cpu *cpu, enum mm_sec_state sec, mm_u32 addr, mm_u32 size);
void mm_cpu_excl_clear(struct mm_cpu *cpu);
mm_bool mm_cpu_excl_check_and_clear(struct mm_cpu *cpu, enum mm_sec_state sec, mm_u32 addr, mm_u32 size);

#endif /* M33MU_CPU_H */
