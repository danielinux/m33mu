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

#ifndef M33MU_SCS_H
#define M33MU_SCS_H

#include "m33mu/types.h"
#include "m33mu/mmio.h"
#include "m33mu/cpu.h"
#include "m33mu/nvic.h"

struct mm_scs {
    mm_u32 cpuid;
    mm_u32 icsr_s;
    mm_u32 icsr_ns;
    mm_u32 vtor_s;
    mm_u32 vtor_ns;
    mm_u32 scr_s;
    mm_u32 scr_ns;
    mm_u32 ccr;
    mm_u32 aircr_s;
    mm_u32 aircr_ns;
    mm_u32 shpr1_s;
    mm_u32 shpr2_s;
    mm_u32 shpr3_s;
    mm_u32 shpr1_ns;
    mm_u32 shpr2_ns;
    mm_u32 shpr3_ns;
    mm_u32 shcsr_s;
    mm_u32 shcsr_ns;
    mm_u32 cfsr;
    mm_u32 hfsr;
    mm_u32 dfsr;
    mm_u32 mmfar;
    mm_u32 bfar;
    mm_u32 afsr;
    /* Minimal MPU model (ARMv8-M Mainline; banked secure/non-secure). */
    mm_u32 mpu_type;
    mm_u32 mpu_ctrl_s;
    mm_u32 mpu_ctrl_ns;
    mm_u32 mpu_rnr_s;
    mm_u32 mpu_rnr_ns;
    mm_u32 mpu_rbar_s[8];
    mm_u32 mpu_rbar_ns[8];
    mm_u32 mpu_rlar_s[8];
    mm_u32 mpu_rlar_ns[8];
    mm_u32 mpu_mair0_s;
    mm_u32 mpu_mair0_ns;
    mm_u32 mpu_mair1_s;
    mm_u32 mpu_mair1_ns;
    /* Minimal SAU model (secure-only) */
    mm_u32 sau_type;
    mm_u32 sau_ctrl;
    mm_u32 sau_rnr;
    mm_u32 sau_rbar[8];
    mm_u32 sau_rlar[8];
    mm_u32 sau_sfsr;
    mm_u32 sau_sfar;
    mm_bool securefault_pending;
    /* Last security state observed by the memory protection layer for an SCS access.
     * Used to bank PPB/SCS registers correctly when Non-secure code accesses the
     * 0xE000_E000 window (CMSIS also provides a 0xE002_E000 alias).
     */
    enum mm_sec_state last_access_sec;
    /* SysTick (shared) */
    mm_u32 systick_ctrl;
    mm_u32 systick_load;
    mm_u32 systick_val;
    mm_u32 systick_calib;
    mm_bool systick_countflag;
    mm_u64 systick_wraps;  /* cumulative wraps for debug/diagnostics */
    mm_bool pend_sv;
    mm_bool pend_st;
    mm_bool trace_enabled;
};

void mm_scs_init(struct mm_scs *scs, mm_u32 cpuid_const);

/* Registers MMIO regions for secure and non-secure SCS views. */
mm_bool mm_scs_register_regions(struct mm_scs *scs, struct mmio_bus *bus, mm_u32 base_secure, mm_u32 base_nonsecure, struct mm_nvic *nvic);

/* Enable/disable verbose MPU/SAU setup logging. */
void mm_scs_set_meminfo(mm_bool enabled);

/* Advance SysTick by one "tick" (instruction-step granularity). */
void mm_scs_systick_step(struct mm_scs *scs);

/* Advance SysTick by an arbitrary number of core cycles (virtual time). */
mm_u32 mm_scs_systick_advance(struct mm_scs *scs, mm_u64 cycles);

/* Return cycles until the next SysTick wrap; (mm_u64)-1 if SysTick is disabled. */
mm_u64 mm_scs_systick_cycles_until_fire(const struct mm_scs *scs);

/* Debug: total SysTick expirations observed (wraps). */
mm_u64 mm_scs_systick_wrap_count(const struct mm_scs *scs);

#endif /* M33MU_SCS_H */
