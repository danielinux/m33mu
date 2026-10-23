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

#include "m33mu/scs.h"
#include "m33mu/vector.h"
#include "m33mu/trace.h"
#include "rp2350/rp2350_mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/system_reset.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct mm_scs_mmio {
    struct mm_scs *scs;
    struct mm_nvic *nvic;
    const struct mm_scs_mux *mux;
    enum mm_sec_state sec;
};

#define SCS_PAGE_SIZE 0x1000u
#define SCS_SCB_OFFSET 0x0D00u /* SCB window starts at 0xE000ED00 inside SCS page */
#define PMU_PAGE_SIZE 0x1000u
#define PMU_PMCCNTR_OFFSET 0xE00u
#define MVFR0_FPV5_SP_D16 0x10110021u
#define MVFR1_FPV5_SP_D16 0x11000011u
#define CCR_STKALIGN (1u << 9)
#define MPU_TYPE_8REGIONS 0x00000800u
#define SYST_CSR 0x10u
#define SYST_RVR 0x14u
#define SYST_CVR 0x18u
#define SYST_CALIB 0x1Cu
#define SYST_LOAD_MASK 0x00FFFFFFu
#define NVIC_REG_BLOCK_END (0x100u + 4u * NVIC_WORDS)
#define NVIC_IPR_END (0x400u + MM_MAX_IRQ)
#define NVIC_ICTR 0x004u

static mm_bool g_meminfo_enabled = MM_FALSE;
#define NVIC_WORDS ((MM_MAX_IRQ + 31u) / 32u)
static mm_u32 g_nvic_enable_log_first[NVIC_WORDS];
static mm_u32 g_nvic_enable_log_second[NVIC_WORDS];
static mm_bool g_nvic_enable_log_first_set[NVIC_WORDS];
static mm_bool g_nvic_enable_log_second_set[NVIC_WORDS];

static mm_bool nvic_enable_log_suppressed(mm_u32 idx, mm_u32 value);

static mm_bool nvic_trace_enabled(void)
{
    static mm_bool init = MM_FALSE;
    static mm_bool enabled = MM_FALSE;
    const char *env;
    if (init) {
        return enabled;
    }
    env = getenv("M33MU_NVIC_TRACE");
    enabled = (env != 0 && env[0] != '\0') ? MM_TRUE : MM_FALSE;
    init = MM_TRUE;
    return enabled;
}

static void scs_select_ctx(struct mm_scs_mmio *ctx, struct mm_scs **scs_out, struct mm_nvic **nvic_out)
{
    struct mm_scs *scs = ctx->scs;
    struct mm_nvic *nvic = ctx->nvic;
    if (ctx->mux != 0 && ctx->mux->active_core != 0 && ctx->mux->core_count > 1u) {
        mm_u32 idx = *ctx->mux->active_core;
        if (idx >= ctx->mux->core_count) {
            idx = 0;
        }
        scs = ctx->mux->scs[idx];
        nvic = ctx->mux->nvic[idx];
    }
    if (scs_out) *scs_out = scs;
    if (nvic_out) *nvic_out = nvic;
}

static enum mm_sec_state scs_effective_sec(const struct mm_scs_mmio *ctx, const struct mm_scs *scs)
{
    enum mm_sec_state eff_sec = ctx->sec;

    if (eff_sec == MM_SECURE) {
        /* For the 0xE000_E000 SCS window, bank by the initiator's security. */
        eff_sec = scs->last_access_sec;
    }
    return eff_sec;
}

static mm_bool scs_finish_read_subword(mm_u32 offset, mm_u32 size_bytes, mm_u32 val, mm_u32 *value_out)
{
    mm_u32 shift;
    mm_u32 mask;

    shift = (offset & 0x3u) * 8u;
    if (size_bytes == 1u) {
        mask = 0xFFu;
        *value_out = (val >> shift) & mask;
        return MM_TRUE;
    }
    if (size_bytes == 2u) {
        mask = 0xFFFFu;
        *value_out = (val >> shift) & mask;
        return MM_TRUE;
    }
    if (size_bytes == 4u) {
        *value_out = val;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_u32 *scs_systick_ctrl_ptr(struct mm_scs *scs, enum mm_sec_state sec)
{
    return (sec == MM_NONSECURE) ? &scs->systick_ctrl_ns : &scs->systick_ctrl_s;
}

static mm_u32 *scs_systick_load_ptr(struct mm_scs *scs, enum mm_sec_state sec)
{
    return (sec == MM_NONSECURE) ? &scs->systick_load_ns : &scs->systick_load_s;
}

static mm_u32 *scs_systick_val_ptr(struct mm_scs *scs, enum mm_sec_state sec)
{
    return (sec == MM_NONSECURE) ? &scs->systick_val_ns : &scs->systick_val_s;
}

static mm_u32 *scs_systick_calib_ptr(struct mm_scs *scs, enum mm_sec_state sec)
{
    return (sec == MM_NONSECURE) ? &scs->systick_calib_ns : &scs->systick_calib_s;
}

static mm_bool *scs_systick_countflag_ptr(struct mm_scs *scs, enum mm_sec_state sec)
{
    return (sec == MM_NONSECURE) ? &scs->systick_countflag_ns : &scs->systick_countflag_s;
}

static mm_u64 *scs_systick_wraps_ptr(struct mm_scs *scs, enum mm_sec_state sec)
{
    return (sec == MM_NONSECURE) ? &scs->systick_wraps_ns : &scs->systick_wraps_s;
}

static mm_bool *scs_pend_st_ptr(struct mm_scs *scs, enum mm_sec_state sec)
{
    return (sec == MM_NONSECURE) ? &scs->pend_st_ns : &scs->pend_st_s;
}

static const mm_u32 *scs_systick_ctrl_ptr_const(const struct mm_scs *scs, enum mm_sec_state sec)
{
    return (sec == MM_NONSECURE) ? &scs->systick_ctrl_ns : &scs->systick_ctrl_s;
}

static const mm_u32 *scs_systick_load_ptr_const(const struct mm_scs *scs, enum mm_sec_state sec)
{
    return (sec == MM_NONSECURE) ? &scs->systick_load_ns : &scs->systick_load_s;
}

static const mm_u32 *scs_systick_val_ptr_const(const struct mm_scs *scs, enum mm_sec_state sec)
{
    return (sec == MM_NONSECURE) ? &scs->systick_val_ns : &scs->systick_val_s;
}

static const mm_u64 *scs_systick_wraps_ptr_const(const struct mm_scs *scs, enum mm_sec_state sec)
{
    return (sec == MM_NONSECURE) ? &scs->systick_wraps_ns : &scs->systick_wraps_s;
}

static const mm_bool *scs_pend_st_ptr_const(const struct mm_scs *scs, enum mm_sec_state sec)
{
    return (sec == MM_NONSECURE) ? &scs->pend_st_ns : &scs->pend_st_s;
}

static mm_bool scs_read_systick(struct mm_scs *scs,
                                enum mm_sec_state sec,
                                mm_u32 aligned,
                                mm_bool clear_countflag,
                                mm_bool noisy,
                                mm_u32 *value_out)
{
    mm_u32 *ctrl = scs_systick_ctrl_ptr(scs, sec);
    mm_u32 *load = scs_systick_load_ptr(scs, sec);
    mm_u32 *val = scs_systick_val_ptr(scs, sec);
    mm_u32 *calib = scs_systick_calib_ptr(scs, sec);
    mm_bool *countflag = scs_systick_countflag_ptr(scs, sec);

    switch (aligned) {
    case SYST_CSR:
        *value_out = *ctrl & 0x7u;
        if (*countflag) {
            *value_out |= (1u << 16);
        }
        if (clear_countflag) {
            *countflag = MM_FALSE;
        }
        if (noisy && scs->trace_enabled) {
            printf("[SYSTICK_CTRL_READ] sec=%d ctrl=0x%08lx\n",
                   (int)sec, (unsigned long)*value_out);
        }
        return MM_TRUE;
    case SYST_RVR:
        *value_out = *load;
        if (noisy && scs->trace_enabled) {
            printf("[SYSTICK_LOAD_READ] sec=%d load=0x%06lx\n",
                   (int)sec, (unsigned long)*value_out);
        }
        return MM_TRUE;
    case SYST_CVR:
        *value_out = *val;
        if (noisy && scs->trace_enabled) {
            printf("[SYSTICK_VAL_READ] sec=%d val=0x%06lx\n",
                   (int)sec, (unsigned long)*value_out);
        }
        return MM_TRUE;
    case SYST_CALIB:
        *value_out = *calib;
        return MM_TRUE;
    default:
        return MM_FALSE;
    }
}

static mm_bool scs_read_nvic(struct mm_nvic *nvic, enum mm_sec_state eff_sec,
                             mm_u32 offset, mm_u32 aligned, mm_u32 size_bytes,
                             mm_bool noisy, mm_u32 *value_out)
{
    mm_u32 word = 0;
    mm_u32 idx = 0;
    mm_bool matched = MM_FALSE;

    if (nvic == 0) {
        return MM_FALSE;
    }
    idx = (aligned - 0x100u) / 4u;
    if (aligned >= 0x100u && aligned < NVIC_REG_BLOCK_END) {
        if (idx < NVIC_WORDS) {
            word = nvic->enable_mask[idx];
            matched = MM_TRUE;
        }
    } else if (aligned >= 0x180u && aligned < (0x180u + 4u * NVIC_WORDS)) {
        idx = (aligned - 0x180u) / 4u;
        if (idx < NVIC_WORDS) {
            word = nvic->enable_mask[idx];
            matched = MM_TRUE;
        }
    } else if (aligned >= 0x200u && aligned < (0x200u + 4u * NVIC_WORDS)) {
        idx = (aligned - 0x200u) / 4u;
        if (idx < NVIC_WORDS) {
            word = nvic->pending_mask[idx];
            matched = MM_TRUE;
        }
    } else if (aligned >= 0x280u && aligned < (0x280u + 4u * NVIC_WORDS)) {
        idx = (aligned - 0x280u) / 4u;
        if (idx < NVIC_WORDS) {
            word = nvic->pending_mask[idx];
            matched = MM_TRUE;
        }
    } else if (aligned >= 0x300u && aligned < (0x300u + 4u * NVIC_WORDS)) {
        idx = (aligned - 0x300u) / 4u;
        if (idx < NVIC_WORDS) {
            word = nvic->active_mask[idx];
            matched = MM_TRUE;
        }
    } else if (aligned >= 0x380u && aligned < (0x380u + 4u * NVIC_WORDS)) {
        idx = (aligned - 0x380u) / 4u;
        if (idx < NVIC_WORDS) {
            word = (eff_sec == MM_SECURE) ? nvic->itns_mask[idx] : 0u;
            matched = MM_TRUE;
        }
    }
    if (matched) {
        if (eff_sec == MM_NONSECURE && idx < NVIC_WORDS) {
            word &= nvic->itns_mask[idx];
        }
        *value_out = word;
        return MM_TRUE;
    }

    if (offset >= 0x400u && offset < NVIC_IPR_END) {
        idx = offset - 0x400u;
        if (size_bytes == 1u) {
            if (noisy) {
                printf("[NVIC_IPR_READ] off=0x%03lx idx=%lu -> 0x%02lx\n",
                       (unsigned long)offset,
                       (unsigned long)idx,
                       (unsigned long)((idx < MM_MAX_IRQ) ? nvic->priority[idx] : 0xffu));
            }
            *value_out = (idx < MM_MAX_IRQ) ? nvic->priority[idx] : 0xffu;
            return MM_TRUE;
        }
        if (size_bytes == 4u && (idx % 4u) == 0u) {
            mm_u32 v = 0;
            mm_u32 i;
            for (i = 0; i < 4u; ++i) {
                mm_u32 pidx = idx + i;
                mm_u8 p = (pidx < MM_MAX_IRQ) ? nvic->priority[pidx] : 0xffu;
                v |= ((mm_u32)p) << (i * 8u);
            }
            *value_out = v;
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

static mm_u32 scs_read_scb_reg(const struct mm_scs_mmio *ctx, struct mm_scs *scs, enum mm_sec_state eff_sec, mm_u32 reg_off)
{
    mm_u32 val = 0;
    const mm_u32 *cfsr = mm_scs_cfsr_ptr_const(scs, eff_sec);
    const mm_u32 *mmfar = mm_scs_mmfar_ptr_const(scs, eff_sec);
    const mm_u32 *bfar = mm_scs_bfar_ptr_const(scs, eff_sec);
    (void)ctx;

    switch (reg_off) {
    case 0x0: val = scs->cpuid; break;
    case 0x4:
        val = (eff_sec == MM_NONSECURE) ? scs->icsr_ns : scs->icsr_s;
        if (scs->pend_sv) val |= (1u << 28);
        if (*scs_pend_st_ptr_const(scs, eff_sec)) val |= (1u << 26);
        break;
    case 0x8: val = (eff_sec == MM_NONSECURE) ? scs->vtor_ns : scs->vtor_s; break;
    case 0xC: val = (eff_sec == MM_NONSECURE) ? scs->aircr_ns : scs->aircr_s; break;
    case 0x10: val = (eff_sec == MM_NONSECURE) ? scs->scr_ns : scs->scr_s; break;
    case 0x14: val = scs->ccr; break;
    case 0x18: val = (eff_sec == MM_NONSECURE) ? scs->shpr1_ns : scs->shpr1_s; break;
    case 0x1C: val = (eff_sec == MM_NONSECURE) ? scs->shpr2_ns : scs->shpr2_s; break;
    case 0x20: val = (eff_sec == MM_NONSECURE) ? scs->shpr3_ns : scs->shpr3_s; break;
    case 0x24: val = (eff_sec == MM_NONSECURE) ? scs->shcsr_ns : scs->shcsr_s; break;
    case 0x28: val = *cfsr; break;
    case 0x2C: val = scs->hfsr; break;
    case 0x30: val = scs->dfsr; break;
    case 0x34: val = *mmfar; break;
    case 0x38: val = *bfar; break;
    case 0x3C: val = scs->afsr; break;
    case 0x88:
        val = (eff_sec == MM_NONSECURE) ? scs->cpacr_ns : scs->cpacr_s;
        if (!scs->fpu_present) {
            val &= ~0x00F00000u;
        }
        break;
    case 0x8C: val = (eff_sec == MM_SECURE) ? scs->nsacr : 0u; break;
    case 0x234: val = scs->fpu_present ? scs->fpccr : 0u; break;
    case 0x238: val = scs->fpu_present ? scs->fpcar : 0u; break;
    case 0x23C: val = scs->fpu_present ? scs->fpdscr : 0u; break;
    case 0x240: val = scs->fpu_present ? scs->mvfr0 : 0u; break;
    case 0x244: val = scs->fpu_present ? scs->mvfr1 : 0u; break;
    case 0x248: val = scs->fpu_present ? scs->mvfr2 : 0u; break;
    case 0x90: val = scs->mpu_type; break;
    case 0x94: val = (eff_sec == MM_NONSECURE) ? scs->mpu_ctrl_ns : scs->mpu_ctrl_s; break;
    case 0x98: val = (eff_sec == MM_NONSECURE) ? scs->mpu_rnr_ns : scs->mpu_rnr_s; break;
    case 0x9C: {
        mm_u32 r = (eff_sec == MM_NONSECURE) ? (scs->mpu_rnr_ns & 0x7u) : (scs->mpu_rnr_s & 0x7u);
        val = (eff_sec == MM_NONSECURE) ? scs->mpu_rbar_ns[r] : scs->mpu_rbar_s[r];
        break;
    }
    case 0xA0: {
        mm_u32 r = (eff_sec == MM_NONSECURE) ? (scs->mpu_rnr_ns & 0x7u) : (scs->mpu_rnr_s & 0x7u);
        val = (eff_sec == MM_NONSECURE) ? scs->mpu_rlar_ns[r] : scs->mpu_rlar_s[r];
        break;
    }
    case 0xC0: val = (eff_sec == MM_NONSECURE) ? scs->mpu_mair0_ns : scs->mpu_mair0_s; break;
    case 0xC4: val = (eff_sec == MM_NONSECURE) ? scs->mpu_mair1_ns : scs->mpu_mair1_s; break;
    case 0xD0: val = (eff_sec == MM_SECURE) ? scs->sau_ctrl : 0u; break;
    case 0xD4: val = (eff_sec == MM_SECURE) ? scs->sau_type : 0u; break;
    case 0xD8: val = (eff_sec == MM_SECURE) ? scs->sau_rnr : 0u; break;
    case 0xDC:
        if (eff_sec != MM_SECURE) { val = 0; break; }
        val = scs->sau_rbar[scs->sau_rnr & 0x7u];
        break;
    case 0xE0:
        if (eff_sec != MM_SECURE) { val = 0; break; }
        val = scs->sau_rlar[scs->sau_rnr & 0x7u];
        break;
    case 0xE4: val = (eff_sec == MM_SECURE) ? scs->sau_sfsr : 0u; break;
    case 0xE8: val = (eff_sec == MM_SECURE) ? scs->sau_sfar : 0u; break;
    default: val = 0; break;
    }
    return val;
}

static mm_bool scs_write_systick(struct mm_scs *scs,
                                 enum mm_sec_state sec,
                                 mm_u32 offset,
                                 mm_u32 aligned,
                                 mm_u32 size_bytes,
                                 mm_u32 value)
{
    mm_u32 shift = (offset & 0x3u) * 8u;
    mm_u32 mask = (size_bytes == 1u) ? 0xFFu : 0xFFFFu;
    mm_u32 *ctrl = scs_systick_ctrl_ptr(scs, sec);
    mm_u32 *load = scs_systick_load_ptr(scs, sec);
    mm_u32 *val = scs_systick_val_ptr(scs, sec);
    mm_bool *countflag = scs_systick_countflag_ptr(scs, sec);

    switch (aligned) {
    case SYST_CSR: {
        mm_u32 cur = *ctrl;
        mm_u32 v = value;
        if (size_bytes == 1u || size_bytes == 2u) {
            v = (cur & ~(mask << shift)) | ((value & mask) << shift);
        }
        *ctrl = v & 0x7u;
        if (scs->trace_enabled) {
            printf("[SYSTICK_CTRL_WRITE] sec=%d ctrl=0x%08lx\n",
                   (int)sec, (unsigned long)*ctrl);
        }
        return MM_TRUE;
    }
    case SYST_RVR:
        *load = ((size_bytes == 4u) ? value : ((value & mask) << shift)) & SYST_LOAD_MASK;
        if (scs->trace_enabled) {
            printf("[SYSTICK_LOAD_WRITE] sec=%d load=0x%06lx\n",
                   (int)sec, (unsigned long)*load);
        }
        return MM_TRUE;
    case SYST_CVR:
        *val = 0;
        *countflag = MM_FALSE;
        if (scs->trace_enabled) {
            printf("[SYSTICK_VAL_WRITE] sec=%d val cleared\n", (int)sec);
        }
        return MM_TRUE;
    default:
        return MM_FALSE;
    }
}

static mm_bool scs_write_nvic(struct mm_nvic *nvic, enum mm_sec_state eff_sec,
                              mm_u32 offset, mm_u32 aligned, mm_u32 size_bytes, mm_u32 value)
{
    if (nvic != 0 && size_bytes == 4u) {
        mm_u32 idx;
        mm_u32 v = value;

        if (aligned >= 0x100u && aligned < NVIC_REG_BLOCK_END) {
            idx = (aligned - 0x100u) / 4u;
            if (idx < NVIC_WORDS) {
                mm_u32 old_enable = nvic->enable_mask[idx];
                if (eff_sec == MM_NONSECURE) v &= nvic->itns_mask[idx];
                nvic->enable_mask[idx] |= v;
                mm_nvic_notify_enable_mask(idx, old_enable, nvic->enable_mask[idx]);
                if (nvic_trace_enabled() &&
                    old_enable != nvic->enable_mask[idx] &&
                    !nvic_enable_log_suppressed(idx, nvic->enable_mask[idx])) {
                    printf("[NVIC_ISER_WRITE] sec=%d idx=%lu val=0x%08lx enable=0x%08lx\n",
                           (int)eff_sec, (unsigned long)idx,
                           (unsigned long)value, (unsigned long)nvic->enable_mask[idx]);
                }
                return MM_TRUE;
            }
        } else if (aligned >= 0x180u && aligned < (0x180u + 4u * NVIC_WORDS)) {
            idx = (aligned - 0x180u) / 4u;
            if (idx < NVIC_WORDS) {
                mm_u32 old_enable = nvic->enable_mask[idx];
                if (eff_sec == MM_NONSECURE) v &= nvic->itns_mask[idx];
                nvic->enable_mask[idx] &= ~v;
                mm_nvic_notify_enable_mask(idx, old_enable, nvic->enable_mask[idx]);
                if (nvic_trace_enabled() &&
                    old_enable != nvic->enable_mask[idx] &&
                    !nvic_enable_log_suppressed(idx, nvic->enable_mask[idx])) {
                    printf("[NVIC_ICER_WRITE] sec=%d idx=%lu val=0x%08lx enable=0x%08lx\n",
                           (int)eff_sec, (unsigned long)idx,
                           (unsigned long)value, (unsigned long)nvic->enable_mask[idx]);
                }
                return MM_TRUE;
            }
        } else if (aligned >= 0x200u && aligned < (0x200u + 4u * NVIC_WORDS)) {
            idx = (aligned - 0x200u) / 4u;
            if (idx < NVIC_WORDS) {
                mm_u32 old_pending = nvic->pending_mask[idx];
                if (eff_sec == MM_NONSECURE) v &= nvic->itns_mask[idx];
                nvic->pending_mask[idx] |= v;
                if (nvic_trace_enabled() && old_pending != nvic->pending_mask[idx]) {
                    printf("[NVIC_ISPR_WRITE] sec=%d idx=%lu val=0x%08lx pending=0x%08lx itns=0x%08lx\n",
                           (int)eff_sec, (unsigned long)idx, (unsigned long)value,
                           (unsigned long)nvic->pending_mask[idx], (unsigned long)nvic->itns_mask[idx]);
                }
                return MM_TRUE;
            }
        } else if (aligned >= 0x280u && aligned < (0x280u + 4u * NVIC_WORDS)) {
            idx = (aligned - 0x280u) / 4u;
            if (idx < NVIC_WORDS) {
                mm_u32 old_pending = nvic->pending_mask[idx];
                if (eff_sec == MM_NONSECURE) v &= nvic->itns_mask[idx];
                nvic->pending_mask[idx] &= ~v;
                if (nvic_trace_enabled() && old_pending != nvic->pending_mask[idx]) {
                    printf("[NVIC_ICPR_WRITE] sec=%d idx=%lu val=0x%08lx pending=0x%08lx\n",
                           (int)eff_sec, (unsigned long)idx,
                           (unsigned long)value, (unsigned long)nvic->pending_mask[idx]);
                }
                return MM_TRUE;
            }
        } else if (aligned >= 0x380u && aligned < (0x380u + 4u * NVIC_WORDS)) {
            idx = (aligned - 0x380u) / 4u;
            if (idx < NVIC_WORDS) {
                mm_u32 old_itns = nvic->itns_mask[idx];
                if (eff_sec == MM_SECURE) {
                    nvic->itns_mask[idx] = value;
                }
                if (nvic_trace_enabled() && old_itns != nvic->itns_mask[idx]) {
                    printf("[NVIC_ITNS_WRITE] sec=%d idx=%lu val=0x%08lx itns=0x%08lx\n",
                           (int)eff_sec, (unsigned long)idx,
                           (unsigned long)value, (unsigned long)nvic->itns_mask[idx]);
                }
                return MM_TRUE;
            }
        }
    }
    if (nvic != 0 && offset >= 0x400u && offset < NVIC_IPR_END) {
        mm_u32 idx = offset - 0x400u;
        if (size_bytes == 1u) {
            if (idx < MM_MAX_IRQ) {
                mm_u8 old_prio = nvic->priority[idx];
                mm_u8 new_prio = (mm_u8)(value & 0xFFu);
                nvic->priority[idx] = new_prio;
                if (nvic_trace_enabled() && old_prio != new_prio) {
                    printf("[NVIC_IPR_WRITE] off=0x%03lx idx=%lu val=0x%02lx\n",
                           (unsigned long)offset, (unsigned long)idx, (unsigned long)new_prio);
                }
            }
            return MM_TRUE;
        }
        if (size_bytes == 4u && (idx % 4u) == 0u) {
            mm_u32 i;
            mm_bool changed = MM_FALSE;
            for (i = 0; i < 4u; ++i) {
                mm_u32 pidx = idx + i;
                mm_u8 p = (mm_u8)((value >> (i * 8u)) & 0xFFu);
                if (pidx < MM_MAX_IRQ) {
                    if (nvic->priority[pidx] != p) changed = MM_TRUE;
                    nvic->priority[pidx] = p;
                }
            }
            if (nvic_trace_enabled() && changed) {
                printf("[NVIC_IPR_WRITE] off=0x%03lx idx=%lu val=0x%08lx\n",
                       (unsigned long)offset, (unsigned long)idx, (unsigned long)value);
            }
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

static mm_bool scs_write_scb(struct mm_scs_mmio *ctx, struct mm_scs *scs, enum mm_sec_state eff_sec, mm_u32 reg_off, mm_u32 value)
{
    mm_u32 *cfsr = mm_scs_cfsr_ptr(scs, eff_sec);
    mm_u32 *mmfar = mm_scs_mmfar_ptr(scs, eff_sec);
    mm_u32 *bfar = mm_scs_bfar_ptr(scs, eff_sec);
    (void)ctx;

    switch (reg_off) {
    /* TODO: gate on privilege */
    case 0x4: {
        mm_u32 v = value;
        if (v & (1u << 28)) scs->pend_sv = MM_TRUE;
        if (v & (1u << 27)) scs->pend_sv = MM_FALSE;
        if (v & (1u << 26)) *scs_pend_st_ptr(scs, eff_sec) = MM_TRUE;
        if (v & (1u << 25)) *scs_pend_st_ptr(scs, eff_sec) = MM_FALSE;
        if (eff_sec == MM_NONSECURE) scs->icsr_ns = v & ~(0xFu << 25);
        else scs->icsr_s = v & ~(0xFu << 25);
        return MM_TRUE;
    }
    case 0x8:
        if (eff_sec == MM_NONSECURE) {
            scs->vtor_ns = value;
            if (mm_trace_enabled()) {
                printf("[VTOR_NS_WRITE] vtor_ns=0x%08lx\n", (unsigned long)value);
            }
        } else {
            scs->vtor_s = value;
            if (mm_trace_enabled()) {
                printf("[VTOR_S_WRITE] vtor_s=0x%08lx\n", (unsigned long)value);
            }
        }
        return MM_TRUE;
    case 0xC:
        if (((value >> 16) & 0xFFFFu) == 0x05FAu) {
            if (eff_sec == MM_NONSECURE) scs->aircr_ns = value;
            else scs->aircr_s = value;
            if (value & (1u << 2)) {
                mm_system_request_reset();
            }
            return MM_TRUE;
        }
        return MM_FALSE;
    case 0x10:
        if (eff_sec == MM_NONSECURE) scs->scr_ns = value;
        else scs->scr_s = value;
        return MM_TRUE;
    case 0x14: scs->ccr = value; return MM_TRUE;
    case 0x18:
        if (eff_sec == MM_NONSECURE) scs->shpr1_ns = value;
        else scs->shpr1_s = value;
        return MM_TRUE;
    case 0x1C:
        if (eff_sec == MM_NONSECURE) scs->shpr2_ns = value;
        else scs->shpr2_s = value;
        return MM_TRUE;
    case 0x20:
        if (eff_sec == MM_NONSECURE) scs->shpr3_ns = value;
        else scs->shpr3_s = value;
        return MM_TRUE;
    case 0x24:
        if (eff_sec == MM_NONSECURE) scs->shcsr_ns = value;
        else scs->shcsr_s = value;
        return MM_TRUE;
    case 0x28: *cfsr &= ~value; return MM_TRUE;
    case 0x2C: scs->hfsr = value; return MM_TRUE;
    case 0x30: scs->dfsr = value; return MM_TRUE;
    case 0x34: *mmfar = value; return MM_TRUE;
    case 0x38: *bfar = value; return MM_TRUE;
    case 0x3C: scs->afsr = value; return MM_TRUE;
    default:
        break;
    }
    return MM_FALSE;
}

static mm_bool nvic_enable_log_suppressed(mm_u32 idx, mm_u32 value)
{
    if (idx >= NVIC_WORDS) {
        return MM_FALSE;
    }
    if (g_nvic_enable_log_first_set[idx] && g_nvic_enable_log_first[idx] == value) {
        return MM_TRUE;
    }
    if (g_nvic_enable_log_second_set[idx] && g_nvic_enable_log_second[idx] == value) {
        return MM_TRUE;
    }
    if (!g_nvic_enable_log_first_set[idx]) {
        g_nvic_enable_log_first[idx] = value;
        g_nvic_enable_log_first_set[idx] = MM_TRUE;
    } else if (!g_nvic_enable_log_second_set[idx]) {
        g_nvic_enable_log_second[idx] = value;
        g_nvic_enable_log_second_set[idx] = MM_TRUE;
    } else {
        g_nvic_enable_log_second[idx] = value;
    }
    return MM_FALSE;
}

void mm_scs_set_meminfo(mm_bool enabled)
{
    g_meminfo_enabled = enabled ? MM_TRUE : MM_FALSE;
}

void mm_scs_init(struct mm_scs *scs, mm_u32 cpuid_const)
{
    int i;
    scs->cpuid = cpuid_const;
    scs->icsr_s = 0;
    scs->icsr_ns = 0;
    scs->vtor_s = 0;
    scs->vtor_ns = 0;
    scs->scr_s = 0;
    scs->scr_ns = 0;
    scs->ccr = CCR_STKALIGN;
    scs->aircr_s = 0;
    scs->aircr_ns = 0;
    scs->shpr1_s = scs->shpr2_s = scs->shpr3_s = 0;
    scs->shpr1_ns = scs->shpr2_ns = scs->shpr3_ns = 0;
    scs->shcsr_s = scs->shcsr_ns = 0;
    scs->cfsr_s = 0;
    scs->cfsr_ns = 0;
    scs->hfsr = 0;
    scs->dfsr = 0;
    scs->mmfar_s = 0;
    scs->mmfar_ns = 0;
    scs->bfar_s = 0;
    scs->bfar_ns = 0;
    scs->afsr = 0;
    scs->cpacr_s = 0;
    scs->cpacr_ns = 0;
    scs->nsacr = 0;
    scs->fpccr = 0;
    scs->fpcar = 0;
    scs->fpdscr = 0;
    scs->mvfr0 = 0;
    scs->mvfr1 = 0;
    scs->mvfr2 = 0;
    scs->fpu_present = MM_FALSE;
    scs->mpu_type = MPU_TYPE_8REGIONS; /* 8 regions, ARMv8‑M MPU */
    scs->mpu_ctrl_s = 0;
    scs->mpu_ctrl_ns = 0;
    scs->mpu_rnr_s = 0;
    scs->mpu_rnr_ns = 0;
    for (i = 0; i < 8; ++i) {
        scs->mpu_rbar_s[i] = 0;
        scs->mpu_rbar_ns[i] = 0;
        scs->mpu_rlar_s[i] = 0;
        scs->mpu_rlar_ns[i] = 0;
    }
    scs->mpu_mair0_s = 0;
    scs->mpu_mair0_ns = 0;
    scs->mpu_mair1_s = 0;
    scs->mpu_mair1_ns = 0;
    scs->sau_type = 0x00000007u; /* 8 regions supported */
    scs->sau_ctrl = 0;
    scs->sau_rnr = 0;
    for (i = 0; i < 8; ++i) {
        scs->sau_rbar[i] = 0;
        scs->sau_rlar[i] = 0;
    }
    scs->sau_sfsr = 0;
    scs->sau_sfar = 0;
    scs->securefault_pending = MM_FALSE;
    scs->last_access_sec = MM_SECURE;
    scs->systick_ctrl_s = 0;
    scs->systick_ctrl_ns = 0;
    scs->systick_load_s = 0;
    scs->systick_load_ns = 0;
    scs->systick_val_s = 0;
    scs->systick_val_ns = 0;
    scs->systick_calib_s = 0;
    scs->systick_calib_ns = 0;
    scs->systick_countflag_s = MM_FALSE;
    scs->systick_countflag_ns = MM_FALSE;
    scs->systick_wraps_s = 0;
    scs->systick_wraps_ns = 0;
    scs->pend_sv = MM_FALSE;
    scs->pend_st_s = MM_FALSE;
    scs->pend_st_ns = MM_FALSE;
    {
        const char *env = getenv("M33MU_SYSTICK_TRACE");
        scs->trace_enabled = (env != 0 && env[0] != '\0') ? MM_TRUE : MM_FALSE;
    }
}

static mm_u32 exception_active_mask(mm_u32 exc_num)
{
    switch (exc_num) {
    case MM_VECT_MEMMANAGE: return 1u << 0;
    case MM_VECT_BUSFAULT: return 1u << 1;
    case MM_VECT_USAGEFAULT: return 1u << 3;
    case MM_VECT_SECUREFAULT: return 1u << 4;
    case MM_VECT_SVCALL: return 1u << 7;
    case MM_VECT_PENDSV: return 1u << 10;
    case MM_VECT_SYSTICK: return 1u << 11;
    default: return 0u;
    }
}

void mm_scs_exception_set_active(struct mm_scs *scs, enum mm_sec_state sec,
                                 mm_u32 exc_num)
{
    mm_u32 mask = exception_active_mask(exc_num);
    if (scs == 0 || mask == 0u) {
        return;
    }
    if (sec == MM_NONSECURE) {
        scs->shcsr_ns |= mask;
    } else {
        scs->shcsr_s |= mask;
    }
}

void mm_scs_exception_clear_active(struct mm_scs *scs, enum mm_sec_state sec,
                                   mm_u32 exc_num)
{
    mm_u32 mask = exception_active_mask(exc_num);
    if (scs == 0 || mask == 0u) {
        return;
    }
    if (sec == MM_NONSECURE) {
        scs->shcsr_ns &= ~mask;
    } else {
        scs->shcsr_s &= ~mask;
    }
}

void mm_scs_set_vectactive(struct mm_scs *scs, enum mm_sec_state sec,
                           mm_u32 exc_num)
{
    mm_u32 *icsr;
    if (scs == 0) {
        return;
    }
    icsr = (sec == MM_NONSECURE) ? &scs->icsr_ns : &scs->icsr_s;
    *icsr = (*icsr & ~0x1FFu) | (exc_num & 0x1FFu);
}

void mm_scs_set_fpu_present(struct mm_scs *scs, mm_bool present)
{
    if (scs == 0) {
        return;
    }
    scs->fpu_present = present ? MM_TRUE : MM_FALSE;
    if (scs->fpu_present) {
        scs->mvfr0 = MVFR0_FPV5_SP_D16;
        scs->mvfr1 = MVFR1_FPV5_SP_D16;
        scs->mvfr2 = 0;
    } else {
        scs->cpacr_s &= ~0x00F00000u;
        scs->cpacr_ns &= ~0x00F00000u;
        scs->fpccr = 0;
        scs->fpcar = 0;
        scs->fpdscr = 0;
        scs->mvfr0 = 0;
        scs->mvfr1 = 0;
        scs->mvfr2 = 0;
    }
}

/* Cortex-M55 PMU (0xE000_3000): plain register file; PMCCNTR increments so
 * ARM_PMU_Get_CCNTR-based timing yields a monotonic cycle count. */
static mm_u32 g_pmu_regs[PMU_PAGE_SIZE / 4u];
static mm_u32 g_pmu_ccntr = 0u;

static mm_bool pmu_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    mm_u32 word_off;
    mm_u32 val;
    (void)opaque;
    if (value_out == 0 || offset >= PMU_PAGE_SIZE) {
        return MM_FALSE;
    }
    word_off = offset & ~0x3u;
    if (word_off == PMU_PMCCNTR_OFFSET) {
        val = ++g_pmu_ccntr;
    } else {
        val = g_pmu_regs[word_off >> 2u];
    }
    return scs_finish_read_subword(offset, size_bytes, val, value_out);
}

static mm_bool pmu_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 word_off;
    mm_u32 shift;
    mm_u32 mask;
    (void)opaque;
    if (offset >= PMU_PAGE_SIZE) {
        return MM_FALSE;
    }
    word_off = offset & ~0x3u;
    if (word_off == PMU_PMCCNTR_OFFSET) {
        return MM_TRUE; /* W1C / reset: write-ignored */
    }
    shift = (offset & 0x3u) * 8u;
    if (size_bytes == 1u) {
        mask = 0xFFu;
    } else if (size_bytes == 2u) {
        mask = 0xFFFFu;
    } else {
        mask = 0xFFFFFFFFu;
    }
    g_pmu_regs[word_off >> 2u] =
        (g_pmu_regs[word_off >> 2u] & ~(mask << shift)) | ((value & mask) << shift);
    return MM_TRUE;
}

mm_bool mm_scs_register_pmu_region(struct mmio_bus *bus, mm_u32 base)
{
    struct mmio_region reg;
    memset(&reg, 0, sizeof(reg));
    reg.base = base;
    reg.size = PMU_PAGE_SIZE;
    reg.opaque = 0;
    reg.read = pmu_read;
    reg.write = pmu_write;
    reg.magic = MMIO_REGION_MAGIC;
    reg.flags = MMIO_REGION_F_EXT;
    reg.name = "PMU";
    reg.version = 1u;
    return mmio_bus_register_region(bus, &reg);
}

static mm_bool scs_read_internal(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out,
                                 mm_bool clear_countflag, mm_bool noisy)
{
    struct mm_scs_mmio *ctx = (struct mm_scs_mmio *)opaque;
    struct mm_scs *scs = 0;
    struct mm_nvic *nvic = 0;
    mm_u32 aligned;
    mm_u32 reg_off;
    mm_u32 val = 0;

    if (value_out == 0) {
        return MM_FALSE;
    }
    scs_select_ctx(ctx, &scs, &nvic);
    if (scs == 0) {
        return MM_FALSE;
    }

    if (offset >= SCS_PAGE_SIZE) {
        return MM_FALSE;
    }

    /* RAZ/WI for everything outside the SCB window, but handle SysTick + NVIC. */
    if (offset < SCS_SCB_OFFSET) {
        enum mm_sec_state eff_sec = scs_effective_sec(ctx, scs);
        aligned = offset & ~0x3u;
        if (aligned == NVIC_ICTR) {
            return scs_finish_read_subword(offset, size_bytes, NVIC_WORDS - 1u, value_out);
        }
        if (scs_read_systick(scs, eff_sec, aligned, clear_countflag, noisy, &val)) {
            return scs_finish_read_subword(offset, size_bytes, val, value_out);
        }
        if (scs_read_nvic(nvic, eff_sec, offset, aligned, size_bytes, noisy, value_out)) {
            return MM_TRUE;
        }
        *value_out = 0;
        return MM_TRUE;
    }

    aligned = offset & ~0x3u;
    reg_off = aligned - SCS_SCB_OFFSET;
    val = scs_read_scb_reg(ctx, scs, scs_effective_sec(ctx, scs), reg_off);
    return scs_finish_read_subword(offset, size_bytes, val, value_out);
}

static mm_bool scs_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    return scs_read_internal(opaque, offset, size_bytes, value_out, MM_TRUE, MM_TRUE);
}

static mmio_peek_result_t scs_peek(void *opaque, mm_u32 offset, mm_u32 size_bytes, void *dst)
{
    mm_u32 val = 0;
    mm_u8 *out = (mm_u8 *)dst;
    if (dst == 0 || size_bytes == 0u || size_bytes > 4u) {
        return MMIO_PEEK_UNSUPPORTED;
    }
    if (!scs_read_internal(opaque, offset, size_bytes, &val, MM_FALSE, MM_FALSE)) {
        return MMIO_PEEK_UNSUPPORTED;
    }
    if (size_bytes == 1u) {
        out[0] = (mm_u8)(val & 0xffu);
    } else if (size_bytes == 2u) {
        out[0] = (mm_u8)(val & 0xffu);
        out[1] = (mm_u8)((val >> 8) & 0xffu);
    } else {
        out[0] = (mm_u8)(val & 0xffu);
        out[1] = (mm_u8)((val >> 8) & 0xffu);
        out[2] = (mm_u8)((val >> 16) & 0xffu);
        out[3] = (mm_u8)((val >> 24) & 0xffu);
    }
    return MMIO_PEEK_OK;
}

static mm_bool scs_save(void *opaque, struct mm_snapshot_writer *w)
{
    struct mm_scs_mmio *ctx = (struct mm_scs_mmio *)opaque;
    struct mm_scs *scs = 0;
    struct mm_nvic *nvic = 0;
    if (w == 0) {
        return MM_FALSE;
    }
    scs_select_ctx(ctx, &scs, &nvic);
    if (scs == 0) {
        return MM_FALSE;
    }
    if (!mm_snapshot_write(w, scs, (mm_u32)sizeof(*scs))) {
        return MM_FALSE;
    }
    if (nvic != 0) {
        if (!mm_snapshot_write(w, nvic, (mm_u32)sizeof(*nvic))) {
            return MM_FALSE;
        }
    }
    return MM_TRUE;
}

static mm_bool scs_load(void *opaque, struct mm_snapshot_reader *r)
{
    struct mm_scs_mmio *ctx = (struct mm_scs_mmio *)opaque;
    struct mm_scs *scs = 0;
    struct mm_nvic *nvic = 0;
    mm_u32 remaining;
    if (r == 0) {
        return MM_FALSE;
    }
    scs_select_ctx(ctx, &scs, &nvic);
    if (scs == 0) {
        return MM_FALSE;
    }
    remaining = r->size - r->offset;
    if (remaining < (mm_u32)sizeof(*scs)) {
        return MM_FALSE;
    }
    if (!mm_snapshot_read(r, scs, (mm_u32)sizeof(*scs))) {
        return MM_FALSE;
    }
    remaining = r->size - r->offset;
    if (nvic != 0) {
        if (remaining < (mm_u32)sizeof(*nvic)) {
            return MM_FALSE;
        }
        if (!mm_snapshot_read(r, nvic, (mm_u32)sizeof(*nvic))) {
            return MM_FALSE;
        }
    }
    return MM_TRUE;
}

static mm_bool scs_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct mm_scs_mmio *ctx = (struct mm_scs_mmio *)opaque;
    struct mm_scs *scs = 0;
    struct mm_nvic *nvic = 0;
    enum mm_sec_state eff_sec;
    mm_u32 aligned;
    mm_u32 reg_off;
    mm_u32 shift;
    mm_u32 mask;
    mm_u32 cur;

    scs_select_ctx(ctx, &scs, &nvic);
    if (scs == 0) {
        return MM_FALSE;
    }
    if (offset >= SCS_PAGE_SIZE) {
        return MM_FALSE;
    }

    /* Outside SCB window: handle SysTick/NVIC, otherwise WI. */
    if (offset < SCS_SCB_OFFSET) {
        enum mm_sec_state eff_sec = scs_effective_sec(ctx, scs);
        aligned = offset & ~0x3u;
        if (scs_write_systick(scs, eff_sec, offset, aligned, size_bytes, value)) {
            return MM_TRUE;
        }
        if (scs_write_nvic(nvic, eff_sec, offset, aligned, size_bytes, value)) {
            return MM_TRUE;
        }
        return MM_TRUE;
    }

    aligned = offset & ~0x3u;
    reg_off = aligned - SCS_SCB_OFFSET;
    eff_sec = scs_effective_sec(ctx, scs);

    if (size_bytes == 1u || size_bytes == 2u) {
        /* RMW for subword writes; fall back to WI on unknown offsets. */
        if (!scs_read(ctx, aligned, 4u, &cur)) {
            cur = 0;
        }
        shift = (offset & 0x3u) * 8u;
        mask = (size_bytes == 1u) ? 0xFFu : 0xFFFFu;
        cur = (cur & ~(mask << shift)) | ((value & mask) << shift);
        value = cur;
        size_bytes = 4u;
    }

    if (size_bytes != 4u) {
        return MM_FALSE;
    }

    if (scs_write_scb(ctx, scs, scs_effective_sec(ctx, scs), reg_off, value)) {
        return MM_TRUE;
    }

    switch (reg_off) {
    case 0x88: {
        mm_u32 mask = 0x00F0FFFFu;
        mm_u32 v = value & mask;
        mm_u32 old_s = scs->cpacr_s;
        mm_u32 old_ns = scs->cpacr_ns;
        if (!scs->fpu_present) {
            v &= ~0x00F00000u;
        }
        if (eff_sec == MM_NONSECURE) {
            scs->cpacr_ns = v;
        } else {
            scs->cpacr_s = v;
        }
        if (scs->fpu_present && (old_s != scs->cpacr_s || old_ns != scs->cpacr_ns)) {
            mm_bool s_en = ((scs->cpacr_s >> 20) & 0x3u) != 0u && ((scs->cpacr_s >> 22) & 0x3u) != 0u;
            mm_bool ns_en = ((scs->cpacr_ns >> 20) & 0x3u) != 0u && ((scs->cpacr_ns >> 22) & 0x3u) != 0u;
            fprintf(stderr, "[FPU] CPACR_S=%s CPACR_NS=%s\n",
                    s_en ? "Enabled" : "Disabled",
                    ns_en ? "Enabled" : "Disabled");
        }
        return MM_TRUE;
    }
    case 0x8C:
        if (eff_sec == MM_SECURE) {
            mm_u32 old = scs->nsacr;
            scs->nsacr = value & 0x00000CFFu;
            if (scs->fpu_present && old != scs->nsacr) {
                mm_bool ns_en = ((scs->nsacr >> 10) & 0x1u) != 0u && ((scs->nsacr >> 11) & 0x1u) != 0u;
                fprintf(stderr, "[FPU] NSACR=%s\n", ns_en ? "Enabled" : "Disabled");
            }
        }
        return MM_TRUE;
    case 0x234:
        if (scs->fpu_present) scs->fpccr = value;
        return MM_TRUE;
    case 0x238:
        if (scs->fpu_present) scs->fpcar = value;
        return MM_TRUE;
    case 0x23C:
        if (scs->fpu_present) scs->fpdscr = value;
        return MM_TRUE;
    /* MPU (banked) */
    case 0x94:
        if (eff_sec == MM_NONSECURE) scs->mpu_ctrl_ns = value;
        else scs->mpu_ctrl_s = value;
        if (g_meminfo_enabled) {
            printf("[MEMINFO] MPU_CTRL_%s=0x%08lx\n",
                   (eff_sec == MM_NONSECURE) ? "NS" : "S",
                   (unsigned long)value);
        }
        return MM_TRUE;
    case 0x98:
        if (eff_sec == MM_NONSECURE) scs->mpu_rnr_ns = value & 0x7u;
        else scs->mpu_rnr_s = value & 0x7u;
        if (g_meminfo_enabled) {
            printf("[MEMINFO] MPU_RNR_%s=%lu\n",
                   (eff_sec == MM_NONSECURE) ? "NS" : "S",
                   (unsigned long)(value & 0x7u));
        }
        return MM_TRUE;
    case 0x9C:
        if (eff_sec == MM_NONSECURE) scs->mpu_rbar_ns[scs->mpu_rnr_ns & 0x7u] = value;
        else scs->mpu_rbar_s[scs->mpu_rnr_s & 0x7u] = value;
        if (g_meminfo_enabled) {
            mm_u32 idx = (eff_sec == MM_NONSECURE) ? (scs->mpu_rnr_ns & 0x7u) : (scs->mpu_rnr_s & 0x7u);
            printf("[MEMINFO] MPU_RBAR_%s[%lu]=0x%08lx (XN=%lu BASE=0x%08lx)\n",
                   (eff_sec == MM_NONSECURE) ? "NS" : "S",
                   (unsigned long)idx,
                   (unsigned long)value,
                   (unsigned long)(value & 0x1u),
                   (unsigned long)(value & 0xFFFFFFE0u));
        }
        return MM_TRUE;
    case 0xA0:
        if (eff_sec == MM_NONSECURE) scs->mpu_rlar_ns[scs->mpu_rnr_ns & 0x7u] = value;
        else scs->mpu_rlar_s[scs->mpu_rnr_s & 0x7u] = value;
        if (g_meminfo_enabled) {
            mm_u32 idx = (eff_sec == MM_NONSECURE) ? (scs->mpu_rnr_ns & 0x7u) : (scs->mpu_rnr_s & 0x7u);
            mm_u32 base;
            mm_u32 limit;
            mm_u32 end;
            mm_u32 rbar = (eff_sec == MM_NONSECURE) ? scs->mpu_rbar_ns[idx] : scs->mpu_rbar_s[idx];
            base = rbar & 0xFFFFFFE0u;
            limit = value & 0xFFFFFFE0u;
            end = limit | 0x1Fu;
            printf("[MEMINFO] MPU_RLAR_%s[%lu]=0x%08lx (EN=%lu LIMIT=0x%08lx RANGE=0x%08lx..0x%08lx)\n",
                   (eff_sec == MM_NONSECURE) ? "NS" : "S",
                   (unsigned long)idx,
                   (unsigned long)value,
                   (unsigned long)(value & 0x1u),
                   (unsigned long)limit,
                   (unsigned long)base,
                   (unsigned long)end);
        }
        return MM_TRUE;
    case 0xC0:
        if (eff_sec == MM_NONSECURE) scs->mpu_mair0_ns = value;
        else scs->mpu_mair0_s = value;
        if (g_meminfo_enabled) {
            printf("[MEMINFO] MPU_MAIR0_%s=0x%08lx\n",
                   (eff_sec == MM_NONSECURE) ? "NS" : "S",
                   (unsigned long)value);
        }
        return MM_TRUE;
    case 0xC4:
        if (eff_sec == MM_NONSECURE) scs->mpu_mair1_ns = value;
        else scs->mpu_mair1_s = value;
        if (g_meminfo_enabled) {
            printf("[MEMINFO] MPU_MAIR1_%s=0x%08lx\n",
                   (eff_sec == MM_NONSECURE) ? "NS" : "S",
                   (unsigned long)value);
        }
        return MM_TRUE;
    /* SAU (secure only; ignore non‑secure writes) */
    case 0xD0: /* SAU_CTRL (new layout) */
        if (eff_sec == MM_SECURE) {
            scs->sau_ctrl = value;
            if (g_meminfo_enabled) {
                printf("[MEMINFO] SAU_CTRL=0x%08lx (EN=%lu ALLNS=%lu)\n",
                       (unsigned long)value,
                       (unsigned long)((value & 0x1u) != 0u),
                       (unsigned long)((value & 0x2u) != 0u));
            }
        } else if (g_meminfo_enabled) {
            printf("[MEMINFO] SAU_CTRL write ignored (NS) value=0x%08lx\n",
                   (unsigned long)value);
        }
        return MM_TRUE;
    case 0xD4: /* SAU_TYPE is read-only. */
        return MM_TRUE;
    case 0xD8: /* SAU_RNR */
        if (eff_sec == MM_SECURE) {
            scs->sau_rnr = value & 0x7u;
            if (g_meminfo_enabled) {
                printf("[MEMINFO] SAU_RNR=%lu\n",
                       (unsigned long)(value & 0x7u));
            }
        } else if (g_meminfo_enabled) {
            printf("[MEMINFO] SAU_RNR write ignored (NS) value=0x%08lx\n",
                   (unsigned long)value);
        }
        return MM_TRUE;
    case 0xDC: /* SAU_RBAR */
        if (eff_sec == MM_SECURE) {
            mm_u32 idx = scs->sau_rnr & 0x7u;
            scs->sau_rbar[idx] = value;
            if (g_meminfo_enabled) {
                printf("[MEMINFO] SAU_RBAR[%lu]=0x%08lx (BASE=0x%08lx)\n",
                       (unsigned long)idx,
                       (unsigned long)value,
                       (unsigned long)(value & 0xFFFFFFE0u));
            }
        } else if (g_meminfo_enabled) {
            printf("[MEMINFO] SAU_RBAR write ignored (NS) value=0x%08lx\n",
                   (unsigned long)value);
        }
        return MM_TRUE;
    case 0xE0: /* SAU_RLAR */
        if (eff_sec == MM_SECURE) {
            mm_u32 idx = scs->sau_rnr & 0x7u;
            scs->sau_rlar[idx] = value;
            if (g_meminfo_enabled) {
                mm_u32 base = scs->sau_rbar[idx] & 0xFFFFFFE0u;
                mm_u32 limit = value & 0xFFFFFFE0u;
                mm_u32 end = limit | 0x1Fu;
                printf("[MEMINFO] SAU_RLAR[%lu]=0x%08lx (EN=%lu NSC=%lu LIMIT=0x%08lx RANGE=0x%08lx..0x%08lx)\n",
                       (unsigned long)idx,
                       (unsigned long)value,
                       (unsigned long)((value & 0x1u) != 0u),
                       (unsigned long)((value & 0x2u) != 0u),
                       (unsigned long)limit,
                       (unsigned long)base,
                       (unsigned long)end);
            }
        } else if (g_meminfo_enabled) {
            printf("[MEMINFO] SAU_RLAR write ignored (NS) value=0x%08lx\n",
                   (unsigned long)value);
        }
        return MM_TRUE;
    case 0xE4: /* SAU_SFSR */
        if (eff_sec == MM_SECURE) {
            scs->sau_sfsr = value;
            if (g_meminfo_enabled) {
                printf("[MEMINFO] SAU_SFSR=0x%08lx\n", (unsigned long)value);
            }
        } else if (g_meminfo_enabled) {
            printf("[MEMINFO] SAU_SFSR write ignored (NS) value=0x%08lx\n",
                   (unsigned long)value);
        }
        return MM_TRUE;
    case 0xE8: /* SAU_SFAR */
        if (eff_sec == MM_SECURE) {
            scs->sau_sfar = value;
            if (g_meminfo_enabled) {
                printf("[MEMINFO] SAU_SFAR=0x%08lx\n", (unsigned long)value);
            }
        } else if (g_meminfo_enabled) {
            printf("[MEMINFO] SAU_SFAR write ignored (NS) value=0x%08lx\n",
                   (unsigned long)value);
        }
        return MM_TRUE;
    default:
        /* Writes to unimplemented SCS offsets are ignored. */
        return MM_TRUE;
    }
}

mm_u32 mm_scs_systick_advance(struct mm_scs *scs, mm_u64 cycles)
{
    mm_u32 total_wraps = 0u;
    enum mm_sec_state sec;

    if (scs == 0 || cycles == 0u) {
        return 0u;
    }

    for (sec = MM_NONSECURE; sec <= MM_SECURE; ++sec) {
        mm_u32 *ctrl = scs_systick_ctrl_ptr(scs, sec);
        mm_u32 *load = scs_systick_load_ptr(scs, sec);
        mm_u32 *val = scs_systick_val_ptr(scs, sec);
        mm_bool *countflag = scs_systick_countflag_ptr(scs, sec);
        mm_u64 *wraps_total = scs_systick_wraps_ptr(scs, sec);
        mm_bool *pend_st = scs_pend_st_ptr(scs, sec);
        mm_bool enable = (*ctrl & 0x1u) != 0u;
        mm_bool tickint = (*ctrl & 0x2u) != 0u;
        mm_u32 load_val;
        mm_u32 cur;
        mm_u64 remaining;
        mm_u64 wraps = 0u;

        if (!enable) {
            continue;
        }
        load_val = *load & SYST_LOAD_MASK;
        if (load_val == 0u) {
            continue;
        }

        cur = *val & SYST_LOAD_MASK;
        if (cur == 0u) {
            cur = load_val;
        }

        remaining = cycles;
        if (remaining < (mm_u64)cur) {
            cur = (mm_u32)(cur - remaining);
            remaining = 0;
        } else {
            remaining -= (mm_u64)cur;
            wraps += 1u;
            wraps += remaining / (mm_u64)load_val;
            {
                mm_u32 rem = (mm_u32)(remaining % (mm_u64)load_val);
                cur = (rem == 0u) ? load_val : (load_val - rem);
            }
        }

        *val = cur & SYST_LOAD_MASK;
        if (scs->trace_enabled && cycles > 1u) {
            mm_u32 end_cur = *val & SYST_LOAD_MASK;
            printf("[SYSTICK_FAST] sec=%d delta=%llu wraps=%lu end=0x%06lx\n",
                   (int)sec,
                   (unsigned long long)cycles,
                   (unsigned long)wraps,
                   (unsigned long)end_cur);
        }
        if (wraps > 0u) {
            *wraps_total += wraps;
            *countflag = MM_TRUE;
            total_wraps += (mm_u32)wraps;
            if (scs->trace_enabled) {
                printf("[SYSTICK_WRAP] sec=%d wraps=%lu total=%llu val=0x%06lx load=0x%06lx tickint=%d\n",
                       (int)sec,
                       (unsigned long)wraps,
                       (unsigned long long)*wraps_total,
                       (unsigned long)*val,
                       (unsigned long)load_val,
                       tickint ? 1 : 0);
            }
            if (tickint) {
                *pend_st = MM_TRUE;
            }
        } else if (scs->trace_enabled) {
            printf("[SYSTICK_STEP] sec=%d val=0x%06lx\n",
                   (int)sec, (unsigned long)*val);
        }
    }
    return total_wraps;
}

mm_u64 mm_scs_systick_cycles_until_fire(const struct mm_scs *scs)
{
    mm_u64 best = (mm_u64)-1;
    enum mm_sec_state sec;
    if (scs == 0) {
        return (mm_u64)-1;
    }
    for (sec = MM_NONSECURE; sec <= MM_SECURE; ++sec) {
        const mm_u32 *ctrl = scs_systick_ctrl_ptr_const(scs, sec);
        const mm_u32 *load = scs_systick_load_ptr_const(scs, sec);
        const mm_u32 *val = scs_systick_val_ptr_const(scs, sec);
        mm_bool enable = (*ctrl & 0x1u) != 0u;
        mm_u32 load_val;
        mm_u32 cur;
        if (!enable) {
            continue;
        }
        load_val = *load & SYST_LOAD_MASK;
        if (load_val == 0u) {
            continue;
        }
        cur = *val & SYST_LOAD_MASK;
        if (cur == 0u) {
            cur = load_val;
        }
        if ((mm_u64)cur < best) {
            best = (mm_u64)cur;
        }
    }
    return best;
}

mm_u64 mm_scs_systick_wrap_count(const struct mm_scs *scs)
{
    if (scs == 0) {
        return 0;
    }
    return *scs_systick_wraps_ptr_const(scs, MM_SECURE) +
           *scs_systick_wraps_ptr_const(scs, MM_NONSECURE);
}

void mm_scs_systick_step(struct mm_scs *scs)
{
    (void)mm_scs_systick_advance(scs, 1u);
}

mm_bool mm_scs_register_regions(struct mm_scs *scs, struct mmio_bus *bus, mm_u32 base_secure, mm_u32 base_nonsecure, struct mm_nvic *nvic)
{
    static struct mm_scs_mmio ctx_secure;
    static struct mm_scs_mmio ctx_nonsecure;
    struct mmio_region reg_s;
    struct mmio_region reg_ns;
    mm_u32 page_base_secure;
    mm_u32 page_base_ns;

    ctx_secure.scs = scs;
    ctx_secure.nvic = nvic;
    ctx_secure.mux = 0;
    ctx_secure.sec = MM_SECURE;

    /* Convert the SCB base passed by the caller (0xE000ED00) to the SCS page base. */
    page_base_secure = base_secure - SCS_SCB_OFFSET;
    memset(&reg_s, 0, sizeof(reg_s));
    reg_s.base = page_base_secure;
    reg_s.size = SCS_PAGE_SIZE;
    reg_s.opaque = &ctx_secure;
    reg_s.read = scs_read;
    reg_s.write = scs_write;
    reg_s.magic = MMIO_REGION_MAGIC;
    reg_s.flags = MMIO_REGION_F_EXT;
    reg_s.name = "SCS";
    reg_s.version = 1u;
    reg_s.peek = scs_peek;
    reg_s.save = scs_save;
    reg_s.load = scs_load;

    if (!mmio_bus_register_region(bus, &reg_s)) return MM_FALSE;

    if (base_nonsecure != base_secure) {
        ctx_nonsecure.scs = scs;
        ctx_nonsecure.nvic = nvic;
        ctx_nonsecure.mux = 0;
        ctx_nonsecure.sec = MM_NONSECURE;
        page_base_ns = base_nonsecure - SCS_SCB_OFFSET;
        memset(&reg_ns, 0, sizeof(reg_ns));
        reg_ns.base = page_base_ns;
        reg_ns.size = SCS_PAGE_SIZE;
        reg_ns.opaque = &ctx_nonsecure;
        reg_ns.read = scs_read;
        reg_ns.write = scs_write;
        reg_ns.magic = MMIO_REGION_MAGIC;
        reg_ns.flags = MMIO_REGION_F_EXT;
        reg_ns.peek = scs_peek;
        if (!mmio_bus_register_region(bus, &reg_ns)) return MM_FALSE;
    }

    return MM_TRUE;
}

mm_bool mm_scs_register_regions_multi(const struct mm_scs_mux *mux, struct mmio_bus *bus, mm_u32 base_secure, mm_u32 base_nonsecure)
{
    static struct mm_scs_mmio ctx_secure;
    static struct mm_scs_mmio ctx_nonsecure;
    struct mmio_region reg_s;
    struct mmio_region reg_ns;
    mm_u32 page_base_secure;
    mm_u32 page_base_ns;

    if (mux == 0) {
        return MM_FALSE;
    }

    ctx_secure.scs = 0;
    ctx_secure.nvic = 0;
    ctx_secure.mux = mux;
    ctx_secure.sec = MM_SECURE;

    page_base_secure = base_secure - SCS_SCB_OFFSET;
    memset(&reg_s, 0, sizeof(reg_s));
    reg_s.base = page_base_secure;
    reg_s.size = SCS_PAGE_SIZE;
    reg_s.opaque = &ctx_secure;
    reg_s.read = scs_read;
    reg_s.write = scs_write;
    reg_s.magic = MMIO_REGION_MAGIC;
    reg_s.flags = MMIO_REGION_F_EXT;
    reg_s.name = "SCS";
    reg_s.version = 1u;
    reg_s.peek = scs_peek;
    reg_s.save = scs_save;
    reg_s.load = scs_load;
    if (!mmio_bus_register_region(bus, &reg_s)) return MM_FALSE;

    if (base_nonsecure != base_secure) {
        ctx_nonsecure.scs = 0;
        ctx_nonsecure.nvic = 0;
        ctx_nonsecure.mux = mux;
        ctx_nonsecure.sec = MM_NONSECURE;
        page_base_ns = base_nonsecure - SCS_SCB_OFFSET;
        memset(&reg_ns, 0, sizeof(reg_ns));
        reg_ns.base = page_base_ns;
        reg_ns.size = SCS_PAGE_SIZE;
        reg_ns.opaque = &ctx_nonsecure;
        reg_ns.read = scs_read;
        reg_ns.write = scs_write;
        reg_ns.magic = MMIO_REGION_MAGIC;
        reg_ns.flags = MMIO_REGION_F_EXT;
        reg_ns.peek = scs_peek;
        if (!mmio_bus_register_region(bus, &reg_ns)) return MM_FALSE;
    }

    return MM_TRUE;
}
