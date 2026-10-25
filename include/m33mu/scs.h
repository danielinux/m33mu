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
    mm_u32 cfsr_s;
    mm_u32 cfsr_ns;
    mm_u32 hfsr;
    mm_u32 dfsr;
    mm_u32 mmfar_s;
    mm_u32 mmfar_ns;
    mm_u32 bfar_s;
    mm_u32 bfar_ns;
    mm_u32 afsr;
    mm_u32 cpacr_s;
    mm_u32 cpacr_ns;
    mm_u32 nsacr;
    mm_u32 fpccr;
    mm_u32 fpcar;
    mm_u32 fpdscr;
    mm_u32 mvfr0;
    mm_u32 mvfr1;
    mm_u32 mvfr2;
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
    /* SysTick (banked by security state on ARMv8-M). */
    mm_u32 systick_ctrl_s;
    mm_u32 systick_ctrl_ns;
    mm_u32 systick_load_s;
    mm_u32 systick_load_ns;
    mm_u32 systick_val_s;
    mm_u32 systick_val_ns;
    mm_u32 systick_calib_s;
    mm_u32 systick_calib_ns;
    mm_bool systick_countflag_s;
    mm_bool systick_countflag_ns;
    mm_u64 systick_wraps_s;  /* cumulative wraps for debug/diagnostics */
    mm_u64 systick_wraps_ns;
    mm_bool pend_sv;
    mm_bool pend_st_s;
    mm_bool pend_st_ns;
    mm_bool trace_enabled;
    mm_bool fpu_present;
};

struct mm_scs_mux {
    struct mm_scs *scs[2];
    struct mm_nvic *nvic[2];
    mm_u32 core_count;
    mm_u32 *active_core;
};

void mm_scs_init(struct mm_scs *scs, mm_u32 cpuid_const);
void mm_scs_set_fpu_present(struct mm_scs *scs, mm_bool present);

/* Registers MMIO regions for secure and non-secure SCS views. */
mm_bool mm_scs_register_regions(struct mm_scs *scs, struct mmio_bus *bus, mm_u32 base_secure, mm_u32 base_nonsecure, struct mm_nvic *nvic);
mm_bool mm_scs_register_pmu_region(struct mmio_bus *bus, mm_u32 base);
mm_bool mm_scs_register_regions_multi(const struct mm_scs_mux *mux, struct mmio_bus *bus, mm_u32 base_secure, mm_u32 base_nonsecure);

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

static inline mm_u32 *mm_scs_cfsr_ptr(struct mm_scs *scs, enum mm_sec_state sec)
{
    return (sec == MM_NONSECURE) ? &scs->cfsr_ns : &scs->cfsr_s;
}

static inline const mm_u32 *mm_scs_cfsr_ptr_const(const struct mm_scs *scs, enum mm_sec_state sec)
{
    return (sec == MM_NONSECURE) ? &scs->cfsr_ns : &scs->cfsr_s;
}

static inline mm_u32 *mm_scs_mmfar_ptr(struct mm_scs *scs, enum mm_sec_state sec)
{
    return (sec == MM_NONSECURE) ? &scs->mmfar_ns : &scs->mmfar_s;
}

static inline const mm_u32 *mm_scs_mmfar_ptr_const(const struct mm_scs *scs, enum mm_sec_state sec)
{
    return (sec == MM_NONSECURE) ? &scs->mmfar_ns : &scs->mmfar_s;
}

static inline mm_u32 *mm_scs_bfar_ptr(struct mm_scs *scs, enum mm_sec_state sec)
{
    return (sec == MM_NONSECURE) ? &scs->bfar_ns : &scs->bfar_s;
}

static inline const mm_u32 *mm_scs_bfar_ptr_const(const struct mm_scs *scs, enum mm_sec_state sec)
{
    return (sec == MM_NONSECURE) ? &scs->bfar_ns : &scs->bfar_s;
}

#endif /* M33MU_SCS_H */
