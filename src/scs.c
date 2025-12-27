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
#include <stdlib.h>
#include <stdio.h>

extern void mm_system_request_reset(void);

struct mm_scs_mmio {
    struct mm_scs *scs;
    struct mm_nvic *nvic;
    enum mm_sec_state sec;
};

#define SCS_PAGE_SIZE 0x1000u
#define SCS_SCB_OFFSET 0x0D00u /* SCB window starts at 0xE000ED00 inside SCS page */

static mm_bool g_meminfo_enabled = MM_FALSE;
static int g_sau_layout = 0; /* 0=unknown, 1=new(CTRL@0xD0/RNR@0xD4), 2=legacy(RNR@0xD8) */
#define NVIC_WORDS ((MM_MAX_IRQ + 31u) / 32u)
static mm_u32 g_nvic_enable_log_first[NVIC_WORDS];
static mm_u32 g_nvic_enable_log_second[NVIC_WORDS];
static mm_bool g_nvic_enable_log_first_set[NVIC_WORDS];
static mm_bool g_nvic_enable_log_second_set[NVIC_WORDS];

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

static void sau_set_layout(int layout)
{
    if (layout == 0 || layout == g_sau_layout) {
        return;
    }
    g_sau_layout = layout;
    if (g_meminfo_enabled) {
        printf("[MEMINFO] SAU_LAYOUT=%s\n", (layout == 2) ? "legacy" : "new");
    }
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
    scs->ccr = 0;
    scs->aircr_s = 0;
    scs->aircr_ns = 0;
    scs->shpr1_s = scs->shpr2_s = scs->shpr3_s = 0;
    scs->shpr1_ns = scs->shpr2_ns = scs->shpr3_ns = 0;
    scs->shcsr_s = scs->shcsr_ns = 0;
    scs->cfsr = 0;
    scs->hfsr = 0;
    scs->dfsr = 0;
    scs->mmfar = 0;
    scs->bfar = 0;
    scs->afsr = 0;
    scs->mpu_type = 0x00000800u; /* 8 regions, ARMv8‑M MPU */
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
    scs->systick_ctrl = 0;
    scs->systick_load = 0;
    scs->systick_val = 0;
    scs->systick_calib = 0;
    scs->systick_countflag = MM_FALSE;
    scs->systick_wraps = 0;
    scs->pend_sv = MM_FALSE;
    scs->pend_st = MM_FALSE;
    {
        const char *env = getenv("SYSTICK_TRACE");
        scs->trace_enabled = (env != 0 && env[0] != '\0') ? MM_TRUE : MM_FALSE;
    }
}

static mm_bool scs_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct mm_scs_mmio *ctx = (struct mm_scs_mmio *)opaque;
    struct mm_scs *scs = ctx->scs;
    enum mm_sec_state eff_sec;
    mm_u32 aligned;
    mm_u32 reg_off;
    mm_u32 val = 0;
    mm_u32 shift = 0;
    mm_u32 mask = 0;

    if (value_out == 0) {
        return MM_FALSE;
    }
    eff_sec = ctx->sec;
    if (eff_sec == MM_SECURE) {
        /* For the 0xE000_E000 SCS window, bank by the initiator's security. */
        eff_sec = scs->last_access_sec;
    }

    if (offset >= SCS_PAGE_SIZE) {
        return MM_FALSE;
    }

    /* RAZ/WI for everything outside the SCB window, but handle SysTick + NVIC. */
    if (offset < SCS_SCB_OFFSET) {
        aligned = offset & ~0x3u;
        switch (aligned) {
        case 0x10: /* SysTick CTRL */
            val = scs->systick_ctrl & 0x7u;
            if (scs->systick_countflag) val |= (1u << 16);
            scs->systick_countflag = MM_FALSE; /* COUNTFLAG clears on read */
            goto done_subword;
        case 0x14: val = scs->systick_load; goto done_subword;
        case 0x18: val = scs->systick_val; goto done_subword;
        case 0x1C: val = scs->systick_calib; goto done_subword;
        default:
            /* NVIC enable/pending/active + ITNS registers. */
            if (ctx->nvic != 0) {
                mm_u32 word = 0xffffffffu;
                mm_u32 idx = (aligned - 0x100u) / 4u;
                mm_u32 mask_ns = 0;
                if (aligned >= 0x100u && aligned < 0x108u) {
                    /* ISER0/1 */
                    if (idx < (MM_MAX_IRQ + 31u) / 32u) word = ctx->nvic->enable_mask[idx];
                } else if (aligned >= 0x180u && aligned < 0x188u) {
                    /* ICER0/1 reads current enable */
                    idx = (aligned - 0x180u) / 4u;
                    if (idx < (MM_MAX_IRQ + 31u) / 32u) word = ctx->nvic->enable_mask[idx];
                } else if (aligned >= 0x200u && aligned < 0x208u) {
                    /* ISPR0/1 */
                    idx = (aligned - 0x200u) / 4u;
                    if (idx < (MM_MAX_IRQ + 31u) / 32u) word = ctx->nvic->pending_mask[idx];
                } else if (aligned >= 0x280u && aligned < 0x288u) {
                    /* ICPR0/1 reads current pending */
                    idx = (aligned - 0x280u) / 4u;
                    if (idx < (MM_MAX_IRQ + 31u) / 32u) word = ctx->nvic->pending_mask[idx];
                } else if (aligned >= 0x300u && aligned < 0x308u) {
                    /* IABR0/1 */
                    idx = (aligned - 0x300u) / 4u;
                    if (idx < (MM_MAX_IRQ + 31u) / 32u) word = ctx->nvic->active_mask[idx];
                } else if (aligned >= 0x380u && aligned < 0x388u) {
                    /* ITNS0/1 (Secure only) */
                    idx = (aligned - 0x380u) / 4u;
                    if (idx < (MM_MAX_IRQ + 31u) / 32u) {
                        word = (eff_sec == MM_SECURE) ? ctx->nvic->itns_mask[idx] : 0u;
                    }
                }
                if (word != 0xffffffffu) {
                    if (eff_sec == MM_NONSECURE) {
                        idx = (aligned >= 0x380u && aligned < 0x388u) ? (aligned - 0x380u) / 4u : (aligned - 0x100u) / 4u;
                        if (idx < (MM_MAX_IRQ + 31u) / 32u) {
                            mask_ns = ctx->nvic->itns_mask[idx];
                            word &= mask_ns;
                        }
                    }
                    val = word;
                    goto done_subword;
                }
            }
            /* NVIC IPR priorities: 0xE000E400-0xE000E4FF */
            if (ctx->nvic != 0 && offset >= 0x400u && offset < 0x500u) {
                mm_u32 idx = offset - 0x400u;
                if (size_bytes == 1u) {
                    printf("[NVIC_IPR_READ] off=0x%03lx idx=%lu -> 0x%02lx\n",
                           (unsigned long)offset,
                           (unsigned long)idx,
                           (unsigned long)((idx < MM_MAX_IRQ) ? ctx->nvic->priority[idx] : 0xffu));
                    *value_out = (idx < MM_MAX_IRQ) ? ctx->nvic->priority[idx] : 0xffu;
                    return MM_TRUE;
                }
                if (size_bytes == 4u && (idx % 4u) == 0u) {
                    mm_u32 v = 0;
                    mm_u32 i;
                    for (i = 0; i < 4u; ++i) {
                        mm_u32 pidx = idx + i;
                        mm_u8 p = (pidx < MM_MAX_IRQ) ? ctx->nvic->priority[pidx] : 0xffu;
                        v |= ((mm_u32)p) << (i * 8u);
                    }
                    *value_out = v;
                    return MM_TRUE;
                }
            }
            *value_out = 0;
            return MM_TRUE;
        }
    }

    aligned = offset & ~0x3u;
    reg_off = aligned - SCS_SCB_OFFSET;

    switch (reg_off) {
    case 0x0: val = scs->cpuid; break;          /* CPUID */
    case 0x4: /* ICSR */
        val = (ctx->sec == MM_NONSECURE) ? scs->icsr_ns : scs->icsr_s;
        if (scs->pend_sv) val |= (1u << 28);
        if (scs->pend_st) val |= (1u << 26);
        break;
    case 0x8: val = (eff_sec == MM_NONSECURE) ? scs->vtor_ns : scs->vtor_s; break; /* VTOR */
    case 0xC: val = (eff_sec == MM_NONSECURE) ? scs->aircr_ns : scs->aircr_s; break; /* AIRCR */
    case 0x10: val = (eff_sec == MM_NONSECURE) ? scs->scr_ns : scs->scr_s; break;  /* SCR */
    case 0x14: val = scs->ccr; break;           /* CCR */
    case 0x18: val = (eff_sec == MM_NONSECURE) ? scs->shpr1_ns : scs->shpr1_s; break; /* SHPR1 */
    case 0x1C: val = (eff_sec == MM_NONSECURE) ? scs->shpr2_ns : scs->shpr2_s; break; /* SHPR2 */
    case 0x20: val = (eff_sec == MM_NONSECURE) ? scs->shpr3_ns : scs->shpr3_s; break; /* SHPR3 */
    case 0x24: val = (eff_sec == MM_NONSECURE) ? scs->shcsr_ns : scs->shcsr_s; break; /* SHCSR */
    case 0x28: val = scs->cfsr; break; /* CFSR (shared) */
    case 0x2C: val = scs->hfsr; break; /* HFSR */
    case 0x30: val = scs->dfsr; break; /* DFSR */
    case 0x34: val = scs->mmfar; break; /* MMFAR */
    case 0x38: val = scs->bfar; break; /* BFAR */
    case 0x3C: val = scs->afsr; break; /* AFSR */
    /* MPU (ARMv8‑M Mainline) */
    case 0x90: val = scs->mpu_type; break; /* MPU_TYPE */
    case 0x94: val = (eff_sec == MM_NONSECURE) ? scs->mpu_ctrl_ns : scs->mpu_ctrl_s; break; /* MPU_CTRL */
    case 0x98: val = (eff_sec == MM_NONSECURE) ? scs->mpu_rnr_ns : scs->mpu_rnr_s; break; /* MPU_RNR */
    case 0x9C: { /* MPU_RBAR */
        mm_u32 r = (eff_sec == MM_NONSECURE) ? (scs->mpu_rnr_ns & 0x7u) : (scs->mpu_rnr_s & 0x7u);
        val = (eff_sec == MM_NONSECURE) ? scs->mpu_rbar_ns[r] : scs->mpu_rbar_s[r];
        break;
    }
    case 0xA0: { /* MPU_RLAR */
        mm_u32 r = (eff_sec == MM_NONSECURE) ? (scs->mpu_rnr_ns & 0x7u) : (scs->mpu_rnr_s & 0x7u);
        val = (eff_sec == MM_NONSECURE) ? scs->mpu_rlar_ns[r] : scs->mpu_rlar_s[r];
        break;
    }
    case 0xC0: val = (eff_sec == MM_NONSECURE) ? scs->mpu_mair0_ns : scs->mpu_mair0_s; break; /* MPU_MAIR0 */
    case 0xC4: val = (eff_sec == MM_NONSECURE) ? scs->mpu_mair1_ns : scs->mpu_mair1_s; break; /* MPU_MAIR1 */
    /* SAU (Secure only) */
    case 0xCC: val = (eff_sec == MM_SECURE) ? scs->sau_type : 0u; break; /* SAU_TYPE */
    case 0xD0: val = (eff_sec == MM_SECURE) ? scs->sau_ctrl : 0u; break; /* SAU_CTRL (new layout) */
    case 0xD4:
        /* SAU_RNR (new layout) */
        if (eff_sec == MM_SECURE) val = scs->sau_rnr;
        else val = 0;
        break;
    case 0xD8:
        /* SAU_RNR (legacy) or SAU_RBAR (new); choose by layout if known. */
        if (eff_sec != MM_SECURE) { val = 0; break; }
        if (g_sau_layout == 2) {
            val = scs->sau_rnr;
        } else {
            val = scs->sau_rbar[scs->sau_rnr & 0x7u];
        }
        break;
    case 0xDC:
        /* SAU_RBAR (legacy) or SAU_RLAR (new) */
        if (eff_sec != MM_SECURE) { val = 0; break; }
        if (g_sau_layout == 2) {
            val = scs->sau_rbar[scs->sau_rnr & 0x7u];
        } else {
            val = scs->sau_rlar[scs->sau_rnr & 0x7u];
        }
        break;
    case 0xE0:
        /* SAU_RLAR (legacy) or SAU_SFSR (new) */
        if (eff_sec != MM_SECURE) { val = 0; break; }
        if (g_sau_layout == 2) {
            val = scs->sau_rlar[scs->sau_rnr & 0x7u];
        } else {
            val = scs->sau_sfsr;
        }
        break;
    case 0xE4:
        /* SAU_SFSR (legacy) or SAU_SFAR (new) */
        if (eff_sec != MM_SECURE) { val = 0; break; }
        if (g_sau_layout == 2) {
            val = scs->sau_sfsr;
        } else {
            val = scs->sau_sfar;
        }
        break;
    case 0xE8:
        /* SAU_SFAR (legacy) */
        if (eff_sec == MM_SECURE) val = scs->sau_sfar;
        else val = 0;
        break;
    default:
        /* RAZ/WI for unimplemented SCS slots. */
        val = 0;
        break;
    }

done_subword:
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

static mm_bool scs_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct mm_scs_mmio *ctx = (struct mm_scs_mmio *)opaque;
    struct mm_scs *scs = ctx->scs;
    enum mm_sec_state eff_sec;
    mm_u32 aligned;
    mm_u32 reg_off;
    mm_u32 shift;
    mm_u32 mask;
    mm_u32 cur;

    if (offset >= SCS_PAGE_SIZE) {
        return MM_FALSE;
    }
    eff_sec = ctx->sec;
    if (eff_sec == MM_SECURE) {
        eff_sec = scs->last_access_sec;
    }

    /* Outside SCB window: handle SysTick/NVIC, otherwise WI. */
    if (offset < SCS_SCB_OFFSET) {
        aligned = offset & ~0x3u;
        shift = (offset & 0x3u) * 8u;
        mask = (size_bytes == 1u) ? 0xFFu : 0xFFFFu;
        switch (aligned) {
        case 0x10: { /* SysTick CTRL */
            mm_u32 cur = scs->systick_ctrl;
            mm_u32 v = value;
            if (size_bytes == 1u || size_bytes == 2u) {
                v = (cur & ~(mask << shift)) | ((value & mask) << shift);
            }
            scs->systick_ctrl = v & 0x7u; /* keep ENABLE/TICKINT/CLKSOURCE */
            if (scs->trace_enabled) {
                printf("[SYSTICK_CTRL_WRITE] ctrl=0x%08lx\n", (unsigned long)scs->systick_ctrl);
            }
            return MM_TRUE;
        }
        case 0x14: /* LOAD */
            scs->systick_load = ((size_bytes == 4u) ? value : ((value & mask) << shift)) & 0x00FFFFFFu;
            if (scs->trace_enabled) {
                printf("[SYSTICK_LOAD_WRITE] load=0x%06lx\n", (unsigned long)scs->systick_load);
            }
            return MM_TRUE;
        case 0x18: /* VAL write clears current count */
            scs->systick_val = 0;
            scs->systick_countflag = MM_FALSE;
            if (scs->trace_enabled) {
                printf("[SYSTICK_VAL_WRITE] val cleared\n");
            }
            return MM_TRUE;
        default:
            if (ctx->nvic != 0 && size_bytes == 4u) {
                mm_u32 idx;
                mm_u32 v = value;
                if (aligned >= 0x100u && aligned < (0x100u + 4u * ((MM_MAX_IRQ + 31u) / 32u))) {
                    /* ISER0/1: set-enable */
                    idx = (aligned - 0x100u) / 4u;
                    if (idx < (MM_MAX_IRQ + 31u) / 32u) {
                        mm_u32 old_enable = ctx->nvic->enable_mask[idx];
                        if (eff_sec == MM_NONSECURE) v &= ctx->nvic->itns_mask[idx];
                        ctx->nvic->enable_mask[idx] |= v;
                        if (nvic_trace_enabled() &&
                            old_enable != ctx->nvic->enable_mask[idx] &&
                            !nvic_enable_log_suppressed(idx, ctx->nvic->enable_mask[idx])) {
                            printf("[NVIC_ISER_WRITE] sec=%d idx=%lu val=0x%08lx enable=0x%08lx\n",
                                   (int)eff_sec,
                                   (unsigned long)idx,
                                   (unsigned long)value,
                                   (unsigned long)ctx->nvic->enable_mask[idx]);
                        }
                        return MM_TRUE;
                    }
                } else if (aligned >= 0x180u && aligned < (0x180u + 4u * ((MM_MAX_IRQ + 31u) / 32u))) {
                    /* ICER0/1: clear-enable */
                    idx = (aligned - 0x180u) / 4u;
                    if (idx < (MM_MAX_IRQ + 31u) / 32u) {
                        mm_u32 old_enable = ctx->nvic->enable_mask[idx];
                        if (eff_sec == MM_NONSECURE) v &= ctx->nvic->itns_mask[idx];
                        ctx->nvic->enable_mask[idx] &= ~v;
                        if (nvic_trace_enabled() &&
                            old_enable != ctx->nvic->enable_mask[idx] &&
                            !nvic_enable_log_suppressed(idx, ctx->nvic->enable_mask[idx])) {
                            printf("[NVIC_ICER_WRITE] sec=%d idx=%lu val=0x%08lx enable=0x%08lx\n",
                                   (int)eff_sec,
                                   (unsigned long)idx,
                                   (unsigned long)value,
                                   (unsigned long)ctx->nvic->enable_mask[idx]);
                        }
                        return MM_TRUE;
                    }
                } else if (aligned >= 0x200u && aligned < (0x200u + 4u * ((MM_MAX_IRQ + 31u) / 32u))) {
                    /* ISPR0/1: set-pending */
                    idx = (aligned - 0x200u) / 4u;
                    if (idx < (MM_MAX_IRQ + 31u) / 32u) {
                        mm_u32 old_pending = ctx->nvic->pending_mask[idx];
                        if (eff_sec == MM_NONSECURE) v &= ctx->nvic->itns_mask[idx];
                        ctx->nvic->pending_mask[idx] |= v;
                        if (nvic_trace_enabled() && old_pending != ctx->nvic->pending_mask[idx]) {
                            printf("[NVIC_ISPR_WRITE] sec=%d idx=%lu val=0x%08lx pending=0x%08lx itns=0x%08lx\n",
                                   (int)eff_sec,
                                   (unsigned long)idx,
                                   (unsigned long)value,
                                   (unsigned long)ctx->nvic->pending_mask[idx],
                                   (unsigned long)ctx->nvic->itns_mask[idx]);
                        }
                        return MM_TRUE;
                    }
                } else if (aligned >= 0x280u && aligned < (0x280u + 4u * ((MM_MAX_IRQ + 31u) / 32u))) {
                    /* ICPR0/1: clear-pending */
                    idx = (aligned - 0x280u) / 4u;
                    if (idx < (MM_MAX_IRQ + 31u) / 32u) {
                        mm_u32 old_pending = ctx->nvic->pending_mask[idx];
                        if (eff_sec == MM_NONSECURE) v &= ctx->nvic->itns_mask[idx];
                        ctx->nvic->pending_mask[idx] &= ~v;
                        if (nvic_trace_enabled() && old_pending != ctx->nvic->pending_mask[idx]) {
                            printf("[NVIC_ICPR_WRITE] sec=%d idx=%lu val=0x%08lx pending=0x%08lx\n",
                                   (int)eff_sec,
                                   (unsigned long)idx,
                                   (unsigned long)value,
                                   (unsigned long)ctx->nvic->pending_mask[idx]);
                        }
                        return MM_TRUE;
                    }
                } else if (aligned >= 0x380u && aligned < (0x380u + 4u * ((MM_MAX_IRQ + 31u) / 32u))) {
                    /* ITNS0/1: Secure-only */
                    idx = (aligned - 0x380u) / 4u;
                    if (idx < (MM_MAX_IRQ + 31u) / 32u) {
                        mm_u32 old_itns = ctx->nvic->itns_mask[idx];
                        if (eff_sec == MM_SECURE) {
                            ctx->nvic->itns_mask[idx] = value;
                        }
                        if (idx == (74u / 32u)) {
                            mm_u32 bit = 1u << (74u % 32u);
                            if ((old_itns ^ ctx->nvic->itns_mask[idx]) & bit) {
                                printf("[NVIC_ITNS_WRITE_IRQ74] sec=%d itns=0x%08lx\n",
                                       (int)eff_sec,
                                       (unsigned long)ctx->nvic->itns_mask[idx]);
                            }
                        }
                        if (nvic_trace_enabled() && old_itns != ctx->nvic->itns_mask[idx]) {
                            printf("[NVIC_ITNS_WRITE] sec=%d idx=%lu val=0x%08lx itns=0x%08lx\n",
                                   (int)eff_sec,
                                   (unsigned long)idx,
                                   (unsigned long)value,
                                   (unsigned long)ctx->nvic->itns_mask[idx]);
                        }
                        return MM_TRUE;
                    }
                }
            }
            /* NVIC IPR priority bytes */
            if (ctx->nvic != 0 && offset >= 0x400u && offset < 0x500u) {
                mm_u32 idx = offset - 0x400u;
                if (size_bytes == 1u) {
                    if (idx < MM_MAX_IRQ) {
                        mm_u8 old_prio = ctx->nvic->priority[idx];
                        mm_u8 new_prio = (mm_u8)(value & 0xFFu);
                        ctx->nvic->priority[idx] = new_prio;
                        if (nvic_trace_enabled() && old_prio != new_prio) {
                            printf("[NVIC_IPR_WRITE] off=0x%03lx idx=%lu val=0x%02lx\n",
                                   (unsigned long)offset,
                                   (unsigned long)idx,
                                   (unsigned long)new_prio);
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
                            if (ctx->nvic->priority[pidx] != p) changed = MM_TRUE;
                            ctx->nvic->priority[pidx] = p;
                        }
                    }
                    if (nvic_trace_enabled() && changed) {
                        printf("[NVIC_IPR_WRITE] off=0x%03lx idx=%lu val=0x%08lx\n",
                               (unsigned long)offset,
                               (unsigned long)idx,
                               (unsigned long)value);
                    }
                    return MM_TRUE;
                }
            }
            return MM_TRUE;
        }
    }

    aligned = offset & ~0x3u;
    reg_off = aligned - SCS_SCB_OFFSET;

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

    switch (reg_off) {
    case 0x4: {
        mm_u32 v = value;
        if (v & (1u << 28)) scs->pend_sv = MM_TRUE;
        if (v & (1u << 27)) scs->pend_sv = MM_FALSE;
        if (v & (1u << 26)) scs->pend_st = MM_TRUE;
        if (v & (1u << 25)) scs->pend_st = MM_FALSE;
        if (ctx->sec == MM_NONSECURE) scs->icsr_ns = v & ~(0x3u << 25);
        else scs->icsr_s = v & ~(0x3u << 25);
        return MM_TRUE;
    }
    case 0x8:
        if (eff_sec == MM_NONSECURE) {
            scs->vtor_ns = value;
            printf("[VTOR_NS_WRITE] vtor_ns=0x%08lx\n", (unsigned long)value);
        } else {
            scs->vtor_s = value;
            printf("[VTOR_S_WRITE] vtor_s=0x%08lx\n", (unsigned long)value);
        }
        return MM_TRUE;
    case 0xC: {
        mm_bool key_ok = ((value >> 16) & 0xFFFFu) == 0x05FAu;
        if (key_ok) {
            if (ctx->sec == MM_NONSECURE) scs->aircr_ns = value;
            else scs->aircr_s = value;
            if (value & (1u << 2)) {
                mm_system_request_reset();
            }
            return MM_TRUE;
        }
        return MM_FALSE;
    }
    case 0x10:
        if (eff_sec == MM_NONSECURE) scs->scr_ns = value;
        else scs->scr_s = value;
        return MM_TRUE;
    case 0x14:
        scs->ccr = value;
        return MM_TRUE;
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
    case 0x28:
        /* CFSR bits are W1C (write-1-to-clear). */
        scs->cfsr &= ~value;
        return MM_TRUE;
    case 0x2C: scs->hfsr = value; return MM_TRUE;
    case 0x30: scs->dfsr = value; return MM_TRUE;
    case 0x34: scs->mmfar = value; return MM_TRUE;
    case 0x38: scs->bfar = value; return MM_TRUE;
    case 0x3C: scs->afsr = value; return MM_TRUE;
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
    case 0xD4: /* SAU_RNR (new layout) */
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
    case 0xD8: /* SAU_RNR (legacy) or SAU_RBAR (new) */
        if (eff_sec == MM_SECURE) {
            if ((value & ~0x7u) == 0u) {
                scs->sau_rnr = value & 0x7u;
                sau_set_layout(2);
                if (g_meminfo_enabled) {
                    printf("[MEMINFO] SAU_RNR=%lu\n",
                           (unsigned long)(value & 0x7u));
                }
            } else {
                mm_u32 idx = scs->sau_rnr & 0x7u;
                scs->sau_rbar[idx] = value;
                if (g_meminfo_enabled) {
                    printf("[MEMINFO] SAU_RBAR[%lu]=0x%08lx (BASE=0x%08lx)\n",
                           (unsigned long)idx,
                           (unsigned long)value,
                           (unsigned long)(value & 0xFFFFFFE0u));
                }
            }
        } else if (g_meminfo_enabled) {
            printf("[MEMINFO] SAU_RNR/RBAR write ignored (NS) value=0x%08lx\n",
                   (unsigned long)value);
        }
        return MM_TRUE;
    case 0xDC: /* SAU_RBAR (legacy) or SAU_RLAR (new) */
        if (eff_sec == MM_SECURE) {
            if ((value & 0x1Fu) == 0u) {
                mm_u32 idx = scs->sau_rnr & 0x7u;
                scs->sau_rbar[idx] = value;
                sau_set_layout(2);
                if (g_meminfo_enabled) {
                    printf("[MEMINFO] SAU_RBAR[%lu]=0x%08lx (BASE=0x%08lx)\n",
                           (unsigned long)idx,
                           (unsigned long)value,
                           (unsigned long)(value & 0xFFFFFFE0u));
                }
            } else {
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
            }
        } else if (g_meminfo_enabled) {
            printf("[MEMINFO] SAU_RBAR/RLAR write ignored (NS) value=0x%08lx\n",
                   (unsigned long)value);
        }
        return MM_TRUE;
    case 0xE0: /* SAU_RLAR (legacy) or SAU_SFSR (new) */
        if (eff_sec == MM_SECURE) {
            if (value <= 0xFFu) {
                scs->sau_sfsr = value;
                if (g_meminfo_enabled) {
                    printf("[MEMINFO] SAU_SFSR=0x%08lx\n", (unsigned long)value);
                }
            } else {
                mm_u32 idx = scs->sau_rnr & 0x7u;
                scs->sau_rlar[idx] = value;
                sau_set_layout(2);
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
            }
        } else if (g_meminfo_enabled) {
            printf("[MEMINFO] SAU_RLAR/SFSR write ignored (NS) value=0x%08lx\n",
                   (unsigned long)value);
        }
        return MM_TRUE;
    case 0xE4: /* SAU_SFSR (legacy) or SAU_SFAR (new) */
        if (eff_sec == MM_SECURE) {
            if (value <= 0xFFu) {
                scs->sau_sfsr = value;
                sau_set_layout(2);
                if (g_meminfo_enabled) {
                    printf("[MEMINFO] SAU_SFSR=0x%08lx\n", (unsigned long)value);
                }
            } else {
                scs->sau_sfar = value;
                if (g_meminfo_enabled) {
                    printf("[MEMINFO] SAU_SFAR=0x%08lx\n", (unsigned long)value);
                }
            }
        } else if (g_meminfo_enabled) {
            printf("[MEMINFO] SAU_SFSR/SFAR write ignored (NS) value=0x%08lx\n",
                   (unsigned long)value);
        }
        return MM_TRUE;
    case 0xE8: /* SAU_SFAR (legacy) */
        if (eff_sec == MM_SECURE) {
            scs->sau_sfar = value;
            sau_set_layout(2);
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
    mm_bool enable;
    mm_bool tickint;
    mm_u32 load;
    mm_u32 cur;
    mm_u64 remaining;
    mm_u64 wraps = 0;

    if (scs == 0 || cycles == 0u) {
        return 0u;
    }
    enable = (scs->systick_ctrl & 0x1u) != 0u;
    tickint = (scs->systick_ctrl & 0x2u) != 0u;
    if (!enable) {
        return 0u;
    }
    load = scs->systick_load & 0x00FFFFFFu;
    if (load == 0u) {
        return 0u;
    }

    cur = scs->systick_val & 0x00FFFFFFu;
    {
        /* For trace/debug: remember starting value when skipping many cycles. */
        mm_u32 start_cur = (cur == 0u) ? 0x00FFFFFFu : cur;
        (void)start_cur; /* used in tracing below */
    }
    if (cur == 0u) {
        /* Reload before first tick or after explicit VAL write. */
        cur = load;
    }

    remaining = cycles;
    if (remaining < (mm_u64)cur) {
        cur = (mm_u32)(cur - remaining);
        remaining = 0;
    } else {
        remaining -= (mm_u64)cur;
        wraps += 1;
        if (load != 0u) {
            wraps += remaining / (mm_u64)load;
            {
                mm_u32 rem = (mm_u32)(remaining % (mm_u64)load);
                cur = (rem == 0u) ? load : (load - rem);
            }
        }
    }

    scs->systick_val = cur & 0x00FFFFFFu;
    if (scs->trace_enabled && cycles > 1u) {
        mm_u32 end_cur = scs->systick_val & 0x00FFFFFFu;
        printf("[SYSTICK_FAST] delta=%llu wraps=%lu end=0x%06lx\n",
               (unsigned long long)cycles,
               (unsigned long)wraps,
               (unsigned long)end_cur);
    }
    if (wraps > 0u) {
        scs->systick_wraps += wraps;
        scs->systick_countflag = MM_TRUE;
        if (scs->trace_enabled) {
            printf("[SYSTICK_WRAP] wraps=%lu total=%llu val=0x%06lx load=0x%06lx tickint=%d\n",
                   (unsigned long)wraps,
                   (unsigned long long)scs->systick_wraps,
                   (unsigned long)scs->systick_val,
                   (unsigned long)load,
                   tickint ? 1 : 0);
        }
        if (tickint) {
            scs->pend_st = MM_TRUE;
        }
    } else if (scs->trace_enabled) {
        printf("[SYSTICK_STEP] val=0x%06lx\n", (unsigned long)scs->systick_val);
    }
    return (mm_u32)wraps;
}

mm_u64 mm_scs_systick_cycles_until_fire(const struct mm_scs *scs)
{
    mm_bool enable;
    mm_u32 load;
    mm_u32 cur;
    if (scs == 0) {
        return (mm_u64)-1;
    }
    enable = (scs->systick_ctrl & 0x1u) != 0u;
    if (!enable) {
        return (mm_u64)-1;
    }
    load = scs->systick_load & 0x00FFFFFFu;
    if (load == 0u) {
        return (mm_u64)-1;
    }
    cur = scs->systick_val & 0x00FFFFFFu;
    if (cur == 0u) {
        cur = load;
    }
    return (mm_u64)cur;
}

mm_u64 mm_scs_systick_wrap_count(const struct mm_scs *scs)
{
    if (scs == 0) {
        return 0;
    }
    return scs->systick_wraps;
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
    ctx_secure.sec = MM_SECURE;

    /* Convert the SCB base passed by the caller (0xE000ED00) to the SCS page base. */
    page_base_secure = base_secure - SCS_SCB_OFFSET;
    reg_s.base = page_base_secure;
    reg_s.size = SCS_PAGE_SIZE;
    reg_s.opaque = &ctx_secure;
    reg_s.read = scs_read;
    reg_s.write = scs_write;

    if (!mmio_bus_register_region(bus, &reg_s)) return MM_FALSE;

    if (base_nonsecure != base_secure) {
        ctx_nonsecure.scs = scs;
        ctx_nonsecure.nvic = nvic;
        ctx_nonsecure.sec = MM_NONSECURE;
        page_base_ns = base_nonsecure - SCS_SCB_OFFSET;
        reg_ns.base = page_base_ns;
        reg_ns.size = SCS_PAGE_SIZE;
        reg_ns.opaque = &ctx_nonsecure;
        reg_ns.read = scs_read;
        reg_ns.write = scs_write;
        if (!mmio_bus_register_region(bus, &reg_ns)) return MM_FALSE;
    }

    return MM_TRUE;
}
