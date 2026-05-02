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

#include "m33mu/execute.h"
#include "m33mu/exec_helpers.h"
#include "m33mu/exception.h"
#include "m33mu/table_branch.h"
#include "m33mu/tz.h"
#include "m33mu/tt.h"
#include "m33mu/target_hal.h"
#include "m33mu/mem_prot.h"
#include "m33mu/dsp_helpers.h"
#include "rp2350/rp2350_mmio.h"
#include "rp2350/rp2350_coproc.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int g_stack_trace = -1;
static int g_splim_trace = -1;
static int g_pop_trace = -1;
static int g_psp_trace = -1;
static int g_msp_trace = -1;
static mm_bool svc_stack_trace_enabled(void);
static int g_ctrl_trace = -1;
static int g_svc_stack_trace = -1;

static mm_bool stack_trace_enabled(void)
{
    if (g_stack_trace < 0) {
        const char *v = getenv("M33MU_STACK_TRACE");
        g_stack_trace = (v && v[0] != '\0') ? 1 : 0;
    }
    return g_stack_trace ? MM_TRUE : MM_FALSE;
}

static mm_bool splim_trace_enabled(void)
{
    if (g_splim_trace < 0) {
        const char *v = getenv("M33MU_SPLIM_TRACE");
        g_splim_trace = (v && v[0] != '\0') ? 1 : 0;
    }
    return g_splim_trace ? MM_TRUE : MM_FALSE;
}

static mm_bool pop_trace_enabled(void)
{
    if (g_pop_trace < 0) {
        const char *v = getenv("M33MU_POP_TRACE");
        g_pop_trace = (v && v[0] != '\0') ? 1 : 0;
    }
    return g_pop_trace ? MM_TRUE : MM_FALSE;
}

static mm_bool psp_trace_enabled(void)
{
    if (g_psp_trace < 0) {
        const char *v = getenv("M33MU_PSP_TRACE");
        g_psp_trace = (v && v[0] != '\0') ? 1 : 0;
    }
    return g_psp_trace ? MM_TRUE : MM_FALSE;
}

static mm_bool msp_trace_enabled(void)
{
    if (g_msp_trace < 0) {
        const char *v = getenv("M33MU_MSP_TRACE");
        g_msp_trace = (v && v[0] != '\0') ? 1 : 0;
    }
    return g_msp_trace ? MM_TRUE : MM_FALSE;
}

static mm_bool ctrl_trace_enabled(void)
{
    if (g_ctrl_trace < 0) {
        const char *v = getenv("M33MU_CTRL_TRACE");
        g_ctrl_trace = (v && v[0] != '\0') ? 1 : 0;
    }
    return g_ctrl_trace ? MM_TRUE : MM_FALSE;
}

static mm_bool svc_stack_trace_enabled(void)
{
    if (g_svc_stack_trace < 0) {
        const char *v = getenv("M33MU_SVC_STACK_TRACE");
        g_svc_stack_trace = (v && v[0] != '\0') ? 1 : 0;
    }
    return g_svc_stack_trace ? MM_TRUE : MM_FALSE;
}

#define CCR_DIV_0_TRP (1u << 4)
#define UFSR_DIVBYZERO (1u << 25)
#define UFSR_NOCP (1u << 19)
#define UFSR_STKOF (1u << 20)
#define UFSR_UNALIGNED (1u << 24)

#define CPACR_CP10_SHIFT 20u
#define CPACR_CP11_SHIFT 22u
#define FPCCR_LSPACT (1u << 0)

static mm_u8 exec_preempt_priority_value(mm_u8 prio, mm_u8 prigroup)
{
    mm_u8 sub_bits;
    if (prigroup > 7u) {
        prigroup = 7u;
    }
    sub_bits = (mm_u8)(prigroup + 1u);
    if (sub_bits >= 8u) {
        return 0u;
    }
    return (mm_u8)(prio & (mm_u8)(0xFFu << sub_bits));
}

static mm_u8 exec_prigroup_for_sec(const struct mm_scs *scs, enum mm_sec_state sec)
{
    mm_u32 aircr;
    if (scs == 0) {
        return 0u;
    }
    aircr = (sec == MM_NONSECURE) ? scs->aircr_ns : scs->aircr_s;
    return (mm_u8)((aircr >> 8) & 0x7u);
}

static mm_u8 exec_system_exception_priority(const struct mm_cpu *cpu,
                                            const struct mm_scs *scs,
                                            mm_u32 exc_num)
{
    enum mm_sec_state sec;
    if (cpu == 0 || scs == 0) {
        return 0xFFu;
    }
    sec = cpu->sec_state;
    switch (exc_num) {
    case MM_VECT_MEMMANAGE:
        return (mm_u8)((((sec == MM_NONSECURE) ? scs->shpr1_ns : scs->shpr1_s) >> 0) & 0xFFu);
    case MM_VECT_BUSFAULT:
        return (mm_u8)((((sec == MM_NONSECURE) ? scs->shpr1_ns : scs->shpr1_s) >> 8) & 0xFFu);
    case MM_VECT_USAGEFAULT:
        return (mm_u8)((((sec == MM_NONSECURE) ? scs->shpr1_ns : scs->shpr1_s) >> 16) & 0xFFu);
    case MM_VECT_SECUREFAULT:
        return (mm_u8)((((sec == MM_NONSECURE) ? scs->shpr1_ns : scs->shpr1_s) >> 24) & 0xFFu);
    case MM_VECT_SVCALL:
        return (mm_u8)((((sec == MM_NONSECURE) ? scs->shpr2_ns : scs->shpr2_s) >> 24) & 0xFFu);
    case MM_VECT_PENDSV:
        return (mm_u8)((((sec == MM_NONSECURE) ? scs->shpr3_ns : scs->shpr3_s) >> 16) & 0xFFu);
    case MM_VECT_SYSTICK:
        return (mm_u8)((((sec == MM_NONSECURE) ? scs->shpr3_ns : scs->shpr3_s) >> 24) & 0xFFu);
    case MM_VECT_NMI:
    case MM_VECT_HARDFAULT:
        return 0u;
    default:
        return 0xFFu;
    }
}

static mm_bool svc_can_preempt_current(const struct mm_cpu *cpu,
                                       const struct mm_scs *scs,
                                       const struct mm_nvic *nvic)
{
    mm_u32 active_exc;
    mm_u8 current_prio;
    mm_u8 svc_prio;
    mm_u8 prigroup;
    if (cpu == 0 || scs == 0) {
        return MM_TRUE;
    }
    if (cpu->mode != MM_HANDLER) {
        return MM_TRUE;
    }
    active_exc = cpu->xpsr & 0x1FFu;
    if (active_exc == 0u) {
        return MM_TRUE;
    }
    if (active_exc >= 16u) {
        if (nvic == 0) {
            return MM_TRUE;
        }
        current_prio = nvic->priority[active_exc - 16u];
    } else {
        current_prio = exec_system_exception_priority(cpu, scs, active_exc);
    }
    svc_prio = exec_system_exception_priority(cpu, scs, MM_VECT_SVCALL);
    prigroup = exec_prigroup_for_sec(scs, cpu->sec_state);
    return exec_preempt_priority_value(svc_prio, prigroup) <
           exec_preempt_priority_value(current_prio, prigroup) ? MM_TRUE : MM_FALSE;
}

static mm_u32 cpacr_for_sec(const struct mm_scs *scs, enum mm_sec_state sec)
{
    if (scs == 0) {
        return 0u;
    }
    return (sec == MM_NONSECURE) ? scs->cpacr_ns : scs->cpacr_s;
}

static mm_bool fpu_access_allowed(const struct mm_cpu *cpu, const struct mm_scs *scs)
{
    mm_u32 cp10;
    mm_u32 cp11;
    if (cpu == 0 || scs == 0 || !scs->fpu_present) {
        return MM_FALSE;
    }
    if (cpu->sec_state == MM_NONSECURE) {
        if (((scs->nsacr >> 10) & 0x1u) == 0u || ((scs->nsacr >> 11) & 0x1u) == 0u) {
            return MM_FALSE;
        }
    }
    cp10 = (cpacr_for_sec(scs, cpu->sec_state) >> CPACR_CP10_SHIFT) & 0x3u;
    cp11 = (cpacr_for_sec(scs, cpu->sec_state) >> CPACR_CP11_SHIFT) & 0x3u;
    if (cp10 == 0u || cp11 == 0u) {
        return MM_FALSE;
    }
    if (!mm_cpu_get_privileged(cpu) && (cp10 != 3u || cp11 != 3u)) {
        return MM_FALSE;
    }
    return MM_TRUE;
}

static mm_bool dcp_access_allowed(const struct mm_cpu *cpu, const struct mm_scs *scs, mm_u8 coproc)
{
    if (cpu == 0) {
        return MM_FALSE;
    }
    (void)scs;
    if (!mm_rp2350_active()) {
        return MM_FALSE;
    }
    if (coproc == 5u && cpu->sec_state != MM_SECURE) {
        return MM_FALSE;
    }
    return MM_TRUE;
}

static mm_bool fpu_check_or_fault(struct mm_execute_ctx *ctx)
{
    int i;
    int idx;
    mm_u32 sp;
    enum mm_sec_state sec;
    if (ctx == 0 || ctx->cpu == 0 || ctx->scs == 0) {
        return MM_FALSE;
    }
    if (fpu_access_allowed(ctx->cpu, ctx->scs)) {
        if (ctx->cpu->mode == MM_HANDLER &&
            ctx->cpu->exc_depth > 0) {
            idx = ctx->cpu->exc_depth - 1;
            if (idx < MM_EXC_STACK_MAX &&
                ctx->cpu->exc_fp_reserved[idx] &&
                !ctx->cpu->exc_fp_saved[idx]) {
                sp = ctx->cpu->exc_sp[idx] + 32u;
                sec = ctx->cpu->exc_sec[idx];
                for (i = 0; i < 16; ++i) {
                    if (!mm_memmap_write(ctx->map, sec, sp + (mm_u32)(i * 4u), 4u, ctx->cpu->s[i])) {
                        if (ctx->raise_mem_fault != 0 && ctx->fetch != 0) {
                            if (!ctx->raise_mem_fault(ctx->cpu, ctx->map, ctx->scs,
                                                     ctx->fetch->pc_fetch, ctx->cpu->xpsr,
                                                     sp + (mm_u32)(i * 4u), MM_FALSE) &&
                                ctx->done != 0) {
                                *ctx->done = MM_TRUE;
                            }
                        }
                        return MM_FALSE;
                    }
                }
                if (!mm_memmap_write(ctx->map, sec, sp + (16u * 4u), 4u, ctx->cpu->fpscr) ||
                    !mm_memmap_write(ctx->map, sec, sp + (17u * 4u), 4u, 0u)) {
                    if (ctx->raise_mem_fault != 0 && ctx->fetch != 0) {
                        if (!ctx->raise_mem_fault(ctx->cpu, ctx->map, ctx->scs,
                                                 ctx->fetch->pc_fetch, ctx->cpu->xpsr,
                                                 sp + (16u * 4u), MM_FALSE) &&
                            ctx->done != 0) {
                            *ctx->done = MM_TRUE;
                        }
                    }
                    return MM_FALSE;
                }
                ctx->cpu->exc_fp_saved[idx] = MM_TRUE;
                ctx->scs->fpccr &= ~FPCCR_LSPACT;
            }
        }
        return MM_TRUE;
    }
    if (ctx->raise_usage_fault != 0 && ctx->fetch != 0) {
        if (!ctx->raise_usage_fault(ctx->cpu, ctx->map, ctx->scs,
                                    ctx->fetch->pc_fetch, ctx->cpu->xpsr, UFSR_NOCP)) {
            if (ctx->done) {
                *ctx->done = MM_TRUE;
            }
        }
    }
    if (ctx->done) {
        *ctx->done = MM_TRUE;
    }
    return MM_FALSE;
}

static void fpu_mark_active(struct mm_cpu *cpu)
{
    if (cpu == 0) {
        return;
    }
    cpu->fp_active = MM_TRUE;
    if (cpu->sec_state == MM_NONSECURE) {
        cpu->control_ns |= (1u << 2);
    } else {
        cpu->control_s |= (1u << 2);
    }
}

static float fpu_u32_to_f32(mm_u32 v)
{
    union { mm_u32 u; float f; } u;
    u.u = v;
    return u.f;
}

static mm_u32 exec_compose_apsr(const struct mm_cpu *cpu)
{
    mm_u32 val;
    if (cpu == 0) {
        return 0u;
    }
    val = cpu->xpsr & 0xF0000000u;
    if (cpu->q_flag) {
        val |= (1u << 27);
    }
    val |= ((mm_u32)cpu->ge_flags & 0xFu) << 16;
    return val;
}

static mm_u32 exec_compose_xpsr(const struct mm_cpu *cpu)
{
    mm_u32 val;
    if (cpu == 0) {
        return 0u;
    }
    val = cpu->xpsr & ~(((mm_u32)1u << 27) | ((mm_u32)0xFu << 16));
    if (cpu->q_flag) {
        val |= (1u << 27);
    }
    val |= ((mm_u32)cpu->ge_flags & 0xFu) << 16;
    return val;
}

static mm_u32 fpu_f32_to_u32(float f)
{
    union { mm_u32 u; float f; } u;
    u.f = f;
    return u.u;
}

static double fpu_round_to_int(float a, mm_u32 fpscr, mm_bool round)
{
    mm_u32 rmode;
    if (!round) {
        return (double)truncf(a);
    }
    rmode = (fpscr >> 22) & 0x3u;
    switch (rmode) {
    case 0x1u:
        return (double)ceilf(a);
    case 0x2u:
        return (double)floorf(a);
    case 0x3u:
        return (double)truncf(a);
    case 0x0u:
    default:
        return (double)nearbyintf(a);
    }
}

static mm_u32 fpu_vcvt_s32_from_f32(const struct mm_cpu *cpu, float a, mm_bool round)
{
    double v;

    if (a != a) {
        return 0u;
    }
    v = fpu_round_to_int(a, cpu ? cpu->fpscr : 0u, round);
    if (v > 2147483647.0) {
        return 0x7fffffffu;
    }
    if (v < -2147483648.0) {
        return 0x80000000u;
    }
    return (mm_u32)(mm_i32)v;
}

static mm_u32 fpu_vcvt_u32_from_f32(const struct mm_cpu *cpu, float a, mm_bool round)
{
    double v;

    if (a != a) {
        return 0u;
    }
    v = fpu_round_to_int(a, cpu ? cpu->fpscr : 0u, round);
    if (v <= 0.0) {
        return 0u;
    }
    if (v > 4294967295.0) {
        return 0xffffffffu;
    }
    return (mm_u32)v;
}

static mm_u32 fpu_vmov_imm_to_u32(mm_u8 imm8)
{
    mm_u32 sign = (mm_u32)((imm8 >> 7) & 0x1u);
    mm_u32 exp = (mm_u32)(((imm8 >> 4) & 0x7u) ^ 0x4u);
    mm_u32 mant = (mm_u32)(imm8 & 0x0fu) << 19;
    exp = exp + 124u;
    return (sign << 31) | (exp << 23) | mant;
}

static mm_bool exec_read_signed(struct mm_memmap *map,
                                enum mm_sec_state sec,
                                mm_u32 addr,
                                mm_u32 size,
                                mm_u32 *value_out)
{
    mm_u32 value = 0;
    if (!mm_memmap_read(map, sec, addr, size, &value)) {
        return MM_FALSE;
    }
    if (size == 1u && (value & 0x80u) != 0u) {
        value |= 0xffffff80u;
    } else if (size == 2u && (value & 0x8000u) != 0u) {
        value |= 0xffff0000u;
    }
    *value_out = value;
    return MM_TRUE;
}

static void fpu_set_fpscr_nzcv(struct mm_cpu *cpu, mm_u32 nzcv)
{
    if (cpu == 0) {
        return;
    }
    cpu->fpscr = (cpu->fpscr & ~0xF0000000u) | (nzcv & 0xF0000000u);
}

static void fpu_cmp_update_flags(struct mm_cpu *cpu, float a, float b)
{
    mm_u32 nzcv = 0;
    if (a != a || b != b) {
        nzcv = 0x30000000u; /* V=1, C=1 for unordered */
    } else if (a == b) {
        nzcv = 0x40000000u | 0x20000000u; /* Z=1, C=1 */
    } else if (a < b) {
        nzcv = 0x80000000u; /* N=1 */
    } else {
        nzcv = 0x20000000u; /* C=1 */
    }
    fpu_set_fpscr_nzcv(cpu, nzcv);
}

static mm_bool exec_set_active_sp(struct mm_cpu *cpu,
                                  struct mm_memmap *map,
                                  struct mm_scs *scs,
                                  const struct mm_fetch_result *fetch,
                                  mm_u32 value,
                                  mm_bool (*raise_usage_fault)(struct mm_cpu *, struct mm_memmap *,
                                                              struct mm_scs *, mm_u32, mm_u32, mm_u32),
                                  mm_bool *done)
{
    mm_u32 splim;
    mm_u32 sp_old;
    if (cpu == 0) {
        return MM_FALSE;
    }
    sp_old = mm_cpu_get_active_sp(cpu);
    if (svc_stack_trace_enabled() && (cpu->xpsr & 0x1ffu) == MM_VECT_SVCALL) {
        printf("[SP_SET] pc=0x%08lx old=0x%08lx new=0x%08lx mode=%d sec=%d ipsr=%lu ctrl_s=0x%08lx ctrl_ns=0x%08lx\n",
               (unsigned long)(fetch ? fetch->pc_fetch : 0u),
               (unsigned long)sp_old,
               (unsigned long)value,
               (int)cpu->mode,
               (int)cpu->sec_state,
               (unsigned long)(cpu->xpsr & 0x1ffu),
               (unsigned long)cpu->control_s,
               (unsigned long)cpu->control_ns);
    }
    splim = mm_cpu_get_active_splim(cpu);
    if (splim != 0u && value < splim) {
        if (raise_usage_fault != 0 && fetch != 0) {
            if (!raise_usage_fault(cpu, map, scs, fetch->pc_fetch, cpu->xpsr, UFSR_STKOF)) {
                if (done != 0) {
                    *done = MM_TRUE;
                }
            }
        }
        if (done != 0) {
            *done = MM_TRUE;
        }
        return MM_FALSE;
    }
    mm_cpu_set_active_sp(cpu, value);
    /* Keep r13 in sync with the active SP for subsequent SP-relative ops. */
    cpu->r[13] = mm_cpu_get_active_sp(cpu);
    return MM_TRUE;
}

static void it_mask_to_pattern(mm_u8 cond, mm_u8 mask, mm_u8 *pattern_out, mm_u8 *remaining_out)
{
    mm_u8 pattern = 0;
    mm_u8 remaining = 0;
    mm_u8 i;

    if (mask != 0u) {
        for (i = 0; i < 4u; ++i) {
            if ((mask & (1u << i)) != 0u) {
                remaining = (mm_u8)(4u - i);
                break;
            }
        }
        if (remaining != 0u) {
            pattern = 1u;
            for (i = 1u; i < remaining; ++i) {
                mm_u8 bit = (mm_u8)((mask >> (4u - i)) & 1u);
                mm_u8 pat_bit = (bit != 0u) ? 0u : 1u;
                pattern |= (mm_u8)(pat_bit << i);
            }
            if ((cond & 1u) != 0u && remaining > 1u) {
                mm_u8 flip = (mm_u8)(((1u << remaining) - 1u) & 0x0eu);
                pattern ^= flip;
            }
        }
    }

    if (pattern_out) {
        *pattern_out = pattern;
    }
    if (remaining_out) {
        *remaining_out = remaining;
    }
}

static mm_u32 shift_reg_operand(mm_u32 rm_val, mm_u32 packed_shift, mm_u32 xpsr, mm_bool *carry_out)
{
    mm_u8 type = (mm_u8)((packed_shift >> 5) & 0x3u);
    mm_u8 imm5 = (mm_u8)(packed_shift & 0x1fu);
    mm_bool carry_in = (xpsr & (1u << 29)) != 0u;
    return mm_shift_c_imm(rm_val, type, imm5, carry_in, carry_out);
}

mm_u8 itstate_get(mm_u32 xpsr)
{
    mm_u8 hi6 = (mm_u8)((xpsr >> 10) & 0x3fu);
    mm_u8 lo2 = (mm_u8)((xpsr >> 25) & 0x3u);
    return (mm_u8)((hi6 << 2) | lo2);
}

mm_u32 itstate_set(mm_u32 xpsr, mm_u8 itstate)
{
    mm_u32 v = xpsr & ~((0x3u << 25) | (0x3fu << 10));
    v |= ((mm_u32)(itstate & 0x3u) << 25);
    v |= ((mm_u32)((itstate >> 2) & 0x3fu) << 10);
    return v;
}

mm_u8 itstate_advance(mm_u8 itstate)
{
    mm_u8 mask = (mm_u8)(itstate & 0x0fu);
    if (mask == 0u) {
        return 0u;
    }
    mask = (mm_u8)(mask << 1);
    itstate = (mm_u8)((itstate & 0xF0u) | (mask & 0x0fu));
    if ((itstate & 0x0fu) == 0u) {
        itstate = 0u;
    }
    return itstate;
}

static void itstate_clear(mm_u32 *xpsr, mm_u8 *it_pattern, mm_u8 *it_remaining, mm_u8 *it_cond)
{
    if (xpsr != 0) {
        *xpsr = itstate_set(*xpsr, 0u);
    }
    if (it_pattern != 0) *it_pattern = 0;
    if (it_remaining != 0) *it_remaining = 0;
    if (it_cond != 0) *it_cond = 0;
}

void itstate_sync_from_xpsr(mm_u32 xpsr, mm_u8 *pattern_out, mm_u8 *remaining_out, mm_u8 *cond_out)
{
    mm_u8 raw = itstate_get(xpsr);
    mm_u8 mask = (mm_u8)(raw & 0x0fu);
    mm_u8 cond = (mm_u8)(raw >> 4);
    mm_u8 pattern = 0;
    mm_u8 remaining = 0;
    it_mask_to_pattern(cond, mask, &pattern, &remaining);
    if (pattern_out) {
        *pattern_out = pattern;
    }
    if (remaining_out) {
        *remaining_out = remaining;
    }
    if (cond_out) {
        *cond_out = cond;
    }
}

mm_bool mm_it_should_execute(struct mm_cpu *cpu,
                             const struct mm_decoded *dec,
                             mm_u8 *it_pattern,
                             mm_u8 *it_remaining,
                             mm_u8 *it_cond)
{
    mm_bool execute_it = MM_TRUE;
    if (cpu == 0 || dec == 0 || it_remaining == 0) {
        return MM_TRUE;
    }
    if (*it_remaining > 0u && itstate_get(cpu->xpsr) == 0u) {
        if (it_pattern) {
            *it_pattern = 0;
        }
        *it_remaining = 0;
        if (it_cond) {
            *it_cond = 0;
        }
    }
    if (*it_remaining > 0u && dec->kind != MM_OP_IT) {
        mm_bool cond_true = MM_FALSE;
        mm_bool take = MM_FALSE;
        mm_bool n = (cpu->xpsr & (1u << 31)) != 0u;
        mm_bool z = (cpu->xpsr & (1u << 30)) != 0u;
        mm_bool c = (cpu->xpsr & (1u << 29)) != 0u;
        mm_bool v = (cpu->xpsr & (1u << 28)) != 0u;
        mm_u8 cond = (it_cond != 0) ? *it_cond : 0u;
        switch (cond) {
            case MM_COND_EQ: cond_true = z; break;
            case MM_COND_NE: cond_true = !z; break;
            case MM_COND_CS: cond_true = c; break;
            case MM_COND_CC: cond_true = !c; break;
            case MM_COND_MI: cond_true = n; break;
            case MM_COND_PL: cond_true = !n; break;
            case MM_COND_VS: cond_true = v; break;
            case MM_COND_VC: cond_true = !v; break;
            case MM_COND_HI: cond_true = c && !z; break;
            case MM_COND_LS: cond_true = !c || z; break;
            case MM_COND_GE: cond_true = (n == v); break;
            case MM_COND_LT: cond_true = (n != v); break;
            case MM_COND_GT: cond_true = !z && (n == v); break;
            case MM_COND_LE: cond_true = z || (n != v); break;
            case MM_COND_AL: cond_true = MM_TRUE; break;
            default: cond_true = MM_FALSE; break;
        }
        take = ((it_pattern != 0 && ((*it_pattern & 0x1u) != 0u)) ? cond_true : !cond_true);
        execute_it = take;
    }
    return execute_it;
}

void mm_it_advance(struct mm_cpu *cpu,
                   const struct mm_decoded *dec,
                   mm_u8 *it_pattern,
                   mm_u8 *it_remaining,
                   mm_u8 *it_cond)
{
    mm_u8 raw;
    if (cpu == 0 || dec == 0 || it_remaining == 0) {
        return;
    }
    if (*it_remaining > 0u && dec->kind != MM_OP_IT) {
        raw = itstate_get(cpu->xpsr);
        if (it_pattern) {
            *it_pattern >>= 1;
        }
        (*it_remaining)--;
        raw = itstate_advance(raw);
        cpu->xpsr = itstate_set(cpu->xpsr, raw);
    }
    (void)it_cond;
}

enum mm_exec_status mm_execute_decoded(struct mm_execute_ctx *ctx)
{
    mm_u8 itstate_val = 0;
    mm_u32 pc_before_exec = 0;
    mm_bool (*handle_pc_write)(struct mm_cpu *, struct mm_memmap *, struct mm_scs *, mm_u32, mm_u8 *, mm_u8 *, mm_u8 *);
    mm_bool (*raise_mem_fault)(struct mm_cpu *, struct mm_memmap *, struct mm_scs *, mm_u32, mm_u32, mm_u32, mm_bool);
    mm_bool (*raise_usage_fault)(struct mm_cpu *, struct mm_memmap *, struct mm_scs *, mm_u32, mm_u32, mm_u32);
    mm_bool (*enter_exception)(struct mm_cpu *, struct mm_memmap *, struct mm_scs *, mm_u32, mm_u32, mm_u32);
    mm_bool opt_gdb;

    if (ctx == 0 || ctx->cpu == 0 || ctx->map == 0 || ctx->scs == 0 || ctx->fetch == 0 || ctx->dec == 0 ||
        ctx->it_pattern == 0 || ctx->it_remaining == 0 || ctx->it_cond == 0 || ctx->done == 0) {
        return MM_EXEC_CONTINUE;
    }

    handle_pc_write = ctx->handle_pc_write;
    raise_mem_fault = ctx->raise_mem_fault;
    raise_usage_fault = ctx->raise_usage_fault;
    enter_exception = ctx->enter_exception;
    opt_gdb = ctx->opt_gdb;

#define cpu (*ctx->cpu)
#define map (*ctx->map)
#define scs (*ctx->scs)
#define gdb (*ctx->gdb)
#define f (*ctx->fetch)
#define d (*ctx->dec)
#define it_pattern (*ctx->it_pattern)
#define it_remaining (*ctx->it_remaining)
#define it_cond (*ctx->it_cond)
#define done (*ctx->done)
#define EXEC_SET_SP(value_expr) do { \
    if (!exec_set_active_sp(&cpu, &map, &scs, &f, (value_expr), raise_usage_fault, &done)) { \
        return MM_EXEC_CONTINUE; \
    } \
} while (0)
#define EXEC_RAISE_UNDEF() do { \
    if (getenv("M33MU_UNDEF_TRACE")) { \
        printf("[UNDEF_EXEC] pc=0x%08lx len=%u raw=0x%08lx kind=%u\n", \
               (unsigned long)f.pc_fetch, \
               (unsigned int)d.len, \
               (unsigned long)d.raw, \
               (unsigned int)d.kind); \
    } \
    if (!raise_usage_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, (1u << 16))) { \
        done = MM_TRUE; \
    } \
    return MM_EXEC_CONTINUE; \
} while (0)

                    pc_before_exec = cpu.r[15];
                    /* Snapshot last executed instruction for fetch diagnostics. */
                    switch (d.kind) {
                        case MM_OP_IT:
                            it_cond = (mm_u8)((d.imm >> 4) & 0x0fu);
                            it_mask_to_pattern(it_cond, (mm_u8)(d.imm & 0x0fu), &it_pattern, &it_remaining);
                            itstate_val = (mm_u8)((it_cond << 4) | (d.imm & 0x0fu));
                            cpu.xpsr = itstate_set(cpu.xpsr, itstate_val);
                            break;
                        case MM_OP_NOP:
                        case MM_OP_NOP_W:
                            /* NOP instructions - no operation */
                            break;
                        case MM_OP_DSB:
                        case MM_OP_DMB:
                        case MM_OP_ISB:
                            /* Barriers are modeled as no-ops for now. */
                            break;
                        case MM_OP_MCR_MRC: {
                                               mm_u8 op1 = (mm_u8)(d.imm & 0x7u);
                                               mm_u8 op2 = (mm_u8)((d.imm >> 3) & 0x7u);
                                               mm_u8 opcode = (mm_u8)((d.imm >> 6) & 0x1u); /* 0=MCR, 1=MRC */
                                               mm_u8 peek = (mm_u8)((d.imm >> 7) & 0x1u);
                                               mm_u8 coproc = d.ra;
                                               if (coproc == 0u) {
                                                   if (opcode == 0u) {
                                                       (void)mm_rp2350_cp0_mcr(cpu.sec_state, op1, d.rn, d.rm, op2, cpu.r[d.rd]);
                                                   } else {
                                                       mm_u32 val = 0u;
                                                       if (!mm_rp2350_cp0_mrc(cpu.sec_state, op1, d.rn, d.rm, op2, &val)) {
                                                           val = 0u;
                                                       }
                                                       if (d.rd == 15u) {
                                                           cpu.xpsr = mm_xpsr_write_nzcvq(cpu.xpsr, val);
                                                       } else {
                                                           cpu.r[d.rd] = val;
                                                       }
                                                   }
                                               } else if (coproc == 4u || coproc == 5u) {
                                                   if (!dcp_access_allowed(&cpu, &scs, coproc)) {
                                                       if (!raise_usage_fault(&cpu, &map, &scs,
                                                                               f.pc_fetch, cpu.xpsr, UFSR_NOCP)) {
                                                           done = MM_TRUE;
                                                       }
                                                       return MM_EXEC_CONTINUE;
                                                   }
                                                   if (opcode != 0u) {
                                                       mm_u32 val = 0u;
                                                       if (!mm_rp2350_dcp_mrc(cpu.sec_state, coproc, op1, d.rn, d.rm, op2, peek != 0u, &val)) {
                                                           val = 0u;
                                                       }
                                                       if (d.rd == 15u) {
                                                           cpu.xpsr = mm_xpsr_write_nzcvq(cpu.xpsr, val);
                                                       } else {
                                                           cpu.r[d.rd] = val;
                                                       }
                                                   }
                                               } else if (coproc == 7u) {
                                                   if (opcode != 0u) {
                                                       if (d.rd == 15u) {
                                                           cpu.xpsr = mm_xpsr_write_nzcvq(cpu.xpsr, 0u);
                                                       } else {
                                                           cpu.r[d.rd] = 0u;
                                                       }
                                                   }
                                               } else {
                                                   (void)op1;
                                               }
                                           } break;
                        case MM_OP_MCRR_MRRC: {
                                                mm_u8 op1 = (mm_u8)(d.imm & 0x0fu);
                                                mm_u8 opcode = (mm_u8)((d.imm >> 8) & 0x1u); /* 0=MCRR, 1=MRRC */
                                                mm_u8 peek = (mm_u8)((d.imm >> 9) & 0x1u);
                                               mm_u8 coproc = d.ra;
                                               if (coproc == 0u) {
                                                    if (opcode == 0u) {
                                                        (void)mm_rp2350_cp0_mcrr(cpu.sec_state, op1, d.rm, cpu.r[d.rd], cpu.r[d.rn]);
                                                    } else {
                                                        mm_u32 lo = 0u;
                                                        mm_u32 hi = 0u;
                                                        if (!mm_rp2350_cp0_mrrc(cpu.sec_state, op1, d.rm, &lo, &hi)) {
                                                            lo = 0u;
                                                            hi = 0u;
                                                        }
                                                        cpu.r[d.rd] = lo;
                                                        cpu.r[d.rn] = hi;
                                                    }
                                               } else if (coproc == 4u || coproc == 5u) {
                                                    if (!dcp_access_allowed(&cpu, &scs, coproc)) {
                                                        if (!raise_usage_fault(&cpu, &map, &scs,
                                                                               f.pc_fetch, cpu.xpsr, UFSR_NOCP)) {
                                                            done = MM_TRUE;
                                                        }
                                                        return MM_EXEC_CONTINUE;
                                                    }
                                                    if (opcode == 0u) {
                                                        (void)mm_rp2350_dcp_mcrr(cpu.sec_state, coproc, op1, d.rm, cpu.r[d.rd], cpu.r[d.rn]);
                                                    } else {
                                                        mm_u32 lo = 0u;
                                                        mm_u32 hi = 0u;
                                                        if (!mm_rp2350_dcp_mrrc(cpu.sec_state, coproc, op1, d.rm, peek != 0u, &lo, &hi)) {
                                                            lo = 0u;
                                                            hi = 0u;
                                                        }
                                                        cpu.r[d.rd] = lo;
                                                        cpu.r[d.rn] = hi;
                                                    }
                                                } else if (coproc == 7u && opcode == 1u) {
                                                    EXEC_RAISE_UNDEF();
                                                } else {
                                                    (void)peek;
                                                }
                                            } break;
                        case MM_OP_STC:
                        case MM_OP_STC2:
                        case MM_OP_LDC:
                        case MM_OP_LDC2:
                        case MM_OP_CDP: {
                                            mm_u8 op1 = (mm_u8)(d.imm & 0x0fu);
                                            mm_u8 op2 = (mm_u8)((d.imm >> 4) & 0x7u);
                                            mm_u8 peek = (mm_u8)((d.imm >> 7) & 0x1u);
                                            mm_u8 coproc = d.ra;
                                            if (coproc == 4u || coproc == 5u) {
                                                if (!dcp_access_allowed(&cpu, &scs, coproc)) {
                                                    if (!raise_usage_fault(&cpu, &map, &scs,
                                                                           f.pc_fetch, cpu.xpsr, UFSR_NOCP)) {
                                                        done = MM_TRUE;
                                                    }
                                                    return MM_EXEC_CONTINUE;
                                                }
                                                (void)mm_rp2350_dcp_cdp(cpu.sec_state, coproc, op1, op2, d.rd, d.rn, d.rm);
                                            }
                                            (void)peek;
                                         } break;
                        case MM_OP_VADD: {
                                             float a;
                                             float b;
                                             if (!fpu_check_or_fault(ctx)) {
                                                 return MM_EXEC_CONTINUE;
                                             }
                                             if (d.rd >= 32u || d.rn >= 32u || d.rm >= 32u) {
                                                 EXEC_RAISE_UNDEF();
                                             }
                                             a = fpu_u32_to_f32(cpu.s[d.rn]);
                                             b = fpu_u32_to_f32(cpu.s[d.rm]);
                                             cpu.s[d.rd] = fpu_f32_to_u32(a + b);
                                             fpu_mark_active(&cpu);
                                             break;
                                         }
                        case MM_OP_VSUB: {
                                             float a;
                                             float b;
                                             if (!fpu_check_or_fault(ctx)) {
                                                 return MM_EXEC_CONTINUE;
                                             }
                                             if (d.rd >= 32u || d.rn >= 32u || d.rm >= 32u) {
                                                 EXEC_RAISE_UNDEF();
                                             }
                                             a = fpu_u32_to_f32(cpu.s[d.rn]);
                                             b = fpu_u32_to_f32(cpu.s[d.rm]);
                                             cpu.s[d.rd] = fpu_f32_to_u32(a - b);
                                             fpu_mark_active(&cpu);
                                             break;
                                         }
                        case MM_OP_VMUL: {
                                             float a;
                                             float b;
                                             if (!fpu_check_or_fault(ctx)) {
                                                 return MM_EXEC_CONTINUE;
                                             }
                                             if (d.rd >= 32u || d.rn >= 32u || d.rm >= 32u) {
                                                 EXEC_RAISE_UNDEF();
                                             }
                                             a = fpu_u32_to_f32(cpu.s[d.rn]);
                                             b = fpu_u32_to_f32(cpu.s[d.rm]);
                                             cpu.s[d.rd] = fpu_f32_to_u32(a * b);
                                             fpu_mark_active(&cpu);
                                             break;
                                         }
                        case MM_OP_VDIV: {
                                             float a;
                                             float b;
                                             if (!fpu_check_or_fault(ctx)) {
                                                 return MM_EXEC_CONTINUE;
                                             }
                                             if (d.rd >= 32u || d.rn >= 32u || d.rm >= 32u) {
                                                 EXEC_RAISE_UNDEF();
                                             }
                                             a = fpu_u32_to_f32(cpu.s[d.rn]);
                                             b = fpu_u32_to_f32(cpu.s[d.rm]);
                                             cpu.s[d.rd] = fpu_f32_to_u32(a / b);
                                             fpu_mark_active(&cpu);
                                             break;
                                         }
                        case MM_OP_VNEG: {
                                             float a;
                                             if (!fpu_check_or_fault(ctx)) {
                                                 return MM_EXEC_CONTINUE;
                                             }
                                             if (d.rd >= 32u || d.rm >= 32u) {
                                                 EXEC_RAISE_UNDEF();
                                             }
                                             a = fpu_u32_to_f32(cpu.s[d.rm]);
                                             cpu.s[d.rd] = fpu_f32_to_u32(-a);
                                             fpu_mark_active(&cpu);
                                             break;
                                         }
                        case MM_OP_VABS: {
                                             if (!fpu_check_or_fault(ctx)) {
                                                 return MM_EXEC_CONTINUE;
                                             }
                                             if (d.rd >= 32u || d.rm >= 32u) {
                                                 EXEC_RAISE_UNDEF();
                                             }
                                             cpu.s[d.rd] = cpu.s[d.rm] & ~0x80000000u;
                                             fpu_mark_active(&cpu);
                                             break;
                                         }
                        case MM_OP_VMLA: {
                                             float acc;
                                             float a;
                                             float b;
                                             if (!fpu_check_or_fault(ctx)) {
                                                 return MM_EXEC_CONTINUE;
                                             }
                                             if (d.rd >= 32u || d.rn >= 32u || d.rm >= 32u) {
                                                 EXEC_RAISE_UNDEF();
                                             }
                                             acc = fpu_u32_to_f32(cpu.s[d.rd]);
                                             a = fpu_u32_to_f32(cpu.s[d.rn]);
                                             b = fpu_u32_to_f32(cpu.s[d.rm]);
                                             cpu.s[d.rd] = fpu_f32_to_u32(acc + (a * b));
                                             fpu_mark_active(&cpu);
                                             break;
                                         }
                        case MM_OP_VMLS: {
                                             float acc;
                                             float a;
                                             float b;
                                             if (!fpu_check_or_fault(ctx)) {
                                                 return MM_EXEC_CONTINUE;
                                             }
                                             if (d.rd >= 32u || d.rn >= 32u || d.rm >= 32u) {
                                                 EXEC_RAISE_UNDEF();
                                             }
                                             acc = fpu_u32_to_f32(cpu.s[d.rd]);
                                             a = fpu_u32_to_f32(cpu.s[d.rn]);
                                             b = fpu_u32_to_f32(cpu.s[d.rm]);
                                             cpu.s[d.rd] = fpu_f32_to_u32(acc - (a * b));
                                             fpu_mark_active(&cpu);
                                             break;
                                         }
                        case MM_OP_VCMP:
                        case MM_OP_VCMPE: {
                                             float a;
                                             float b;
                                             if (!fpu_check_or_fault(ctx)) {
                                                 return MM_EXEC_CONTINUE;
                                             }
                                             if (d.rd >= 32u || d.rm >= 32u) {
                                                 EXEC_RAISE_UNDEF();
                                             }
                                             a = fpu_u32_to_f32(cpu.s[d.rd]);
                                             if (d.imm != 0u) {
                                                 b = 0.0f;
                                             } else {
                                                 b = fpu_u32_to_f32(cpu.s[d.rm]);
                                             }
                                             fpu_cmp_update_flags(&cpu, a, b);
                                             fpu_mark_active(&cpu);
                                             break;
                                         }
                        case MM_OP_VCVT_S32_F32: {
                                                     float a;
                                                     if (!fpu_check_or_fault(ctx)) {
                                                         return MM_EXEC_CONTINUE;
                                                     }
                                                      if (d.rd >= 32u || d.rm >= 32u) {
                                                          EXEC_RAISE_UNDEF();
                                                      }
                                                      a = fpu_u32_to_f32(cpu.s[d.rm]);
                                                      cpu.s[d.rd] = fpu_vcvt_s32_from_f32(&cpu, a, MM_FALSE);
                                                      fpu_mark_active(&cpu);
                                                      break;
                                                  }
                        case MM_OP_VCVTR_S32_F32: {
                                                      float a;
                                                      if (!fpu_check_or_fault(ctx)) {
                                                          return MM_EXEC_CONTINUE;
                                                      }
                                                      if (d.rd >= 32u || d.rm >= 32u) {
                                                          EXEC_RAISE_UNDEF();
                                                      }
                                                      a = fpu_u32_to_f32(cpu.s[d.rm]);
                                                      cpu.s[d.rd] = fpu_vcvt_s32_from_f32(&cpu, a, MM_TRUE);
                                                      fpu_mark_active(&cpu);
                                                      break;
                                                  }
                        case MM_OP_VCVT_U32_F32: {
                                                     float a;
                                                     if (!fpu_check_or_fault(ctx)) {
                                                         return MM_EXEC_CONTINUE;
                                                     }
                                                      if (d.rd >= 32u || d.rm >= 32u) {
                                                          EXEC_RAISE_UNDEF();
                                                      }
                                                      a = fpu_u32_to_f32(cpu.s[d.rm]);
                                                      cpu.s[d.rd] = fpu_vcvt_u32_from_f32(&cpu, a, MM_FALSE);
                                                      fpu_mark_active(&cpu);
                                                      break;
                                                  }
                        case MM_OP_VCVTR_U32_F32: {
                                                      float a;
                                                      if (!fpu_check_or_fault(ctx)) {
                                                          return MM_EXEC_CONTINUE;
                                                      }
                                                      if (d.rd >= 32u || d.rm >= 32u) {
                                                          EXEC_RAISE_UNDEF();
                                                      }
                                                      a = fpu_u32_to_f32(cpu.s[d.rm]);
                                                      cpu.s[d.rd] = fpu_vcvt_u32_from_f32(&cpu, a, MM_TRUE);
                                                      fpu_mark_active(&cpu);
                                                      break;
                                                  }
                        case MM_OP_VCVT_F32_S32: {
                                                     mm_i32 a;
                                                     if (!fpu_check_or_fault(ctx)) {
                                                         return MM_EXEC_CONTINUE;
                                                     }
                                                     if (d.rd >= 32u || d.rm >= 32u) {
                                                         EXEC_RAISE_UNDEF();
                                                     }
                                                     a = (mm_i32)cpu.s[d.rm];
                                                     cpu.s[d.rd] = fpu_f32_to_u32((float)a);
                                                     fpu_mark_active(&cpu);
                                                     break;
                                                 }
                        case MM_OP_VCVT_F32_U32: {
                                                     mm_u32 a;
                                                     if (!fpu_check_or_fault(ctx)) {
                                                         return MM_EXEC_CONTINUE;
                                                     }
                                                     if (d.rd >= 32u || d.rm >= 32u) {
                                                         EXEC_RAISE_UNDEF();
                                                     }
                                                     a = cpu.s[d.rm];
                                                     cpu.s[d.rd] = fpu_f32_to_u32((float)a);
                                                     fpu_mark_active(&cpu);
                                                     break;
                                                 }
                        case MM_OP_VSQRT: {
                                              float a;
                                              if (!fpu_check_or_fault(ctx)) {
                                                  return MM_EXEC_CONTINUE;
                                              }
                                              if (d.rd >= 32u || d.rm >= 32u) {
                                                  EXEC_RAISE_UNDEF();
                                              }
                                              a = fpu_u32_to_f32(cpu.s[d.rm]);
                                              cpu.s[d.rd] = fpu_f32_to_u32(sqrtf(a));
                                              fpu_mark_active(&cpu);
                                              break;
                                          }
                        case MM_OP_VMOV_SR: {
                                                 if (!fpu_check_or_fault(ctx)) {
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                                 if (d.rd >= 32u || d.rn >= 16u) {
                                                     EXEC_RAISE_UNDEF();
                                                 }
                                                 cpu.s[d.rd] = cpu.r[d.rn];
                                                 fpu_mark_active(&cpu);
                                                 break;
                                             }
                        case MM_OP_VMOV_RS: {
                                                 if (!fpu_check_or_fault(ctx)) {
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                                 if (d.rn >= 32u || d.rd >= 16u) {
                                                     EXEC_RAISE_UNDEF();
                                                 }
                                                 cpu.r[d.rd] = cpu.s[d.rn];
                                                 fpu_mark_active(&cpu);
                                                 break;
                                             }
                        case MM_OP_VMOV_IMM: {
                                                  if (!fpu_check_or_fault(ctx)) {
                                                      return MM_EXEC_CONTINUE;
                                                  }
                                                  if (d.rd >= 32u) {
                                                      EXEC_RAISE_UNDEF();
                                                  }
                                                  cpu.s[d.rd] = fpu_vmov_imm_to_u32((mm_u8)(d.imm & 0xffu));
                                                  fpu_mark_active(&cpu);
                                                  break;
                                              }
                        case MM_OP_VMOV_SRR: {
                                                 if (!fpu_check_or_fault(ctx)) {
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                                 if (d.rd >= 32u || d.rn >= 32u || d.rm >= 15u || d.ra >= 15u) {
                                                     EXEC_RAISE_UNDEF();
                                                 }
                                                 cpu.s[d.rd] = cpu.r[d.rm];
                                                 cpu.s[d.rn] = cpu.r[d.ra];
                                                 fpu_mark_active(&cpu);
                                                 break;
                                             }
                        case MM_OP_VMOV_RSS: {
                                                 if (!fpu_check_or_fault(ctx)) {
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                                 if (d.rd >= 15u || d.rn >= 15u || d.rm >= 32u || d.ra >= 32u) {
                                                     EXEC_RAISE_UNDEF();
                                                 }
                                                 cpu.r[d.rd] = cpu.s[d.rm];
                                                 cpu.r[d.rn] = cpu.s[d.ra];
                                                 fpu_mark_active(&cpu);
                                                 break;
                                             }
                        case MM_OP_VMOV_DRR: {
                                                 if (!fpu_check_or_fault(ctx)) {
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                                 if (d.rd >= 16u || d.rm >= 15u || d.ra >= 15u) {
                                                     EXEC_RAISE_UNDEF();
                                                 }
                                                 cpu.s[d.rd * 2u] = cpu.r[d.rm];
                                                 cpu.s[d.rd * 2u + 1u] = cpu.r[d.ra];
                                                 fpu_mark_active(&cpu);
                                                 break;
                                             }
                        case MM_OP_VMOV_RDD: {
                                                 if (!fpu_check_or_fault(ctx)) {
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                                 if (d.rm >= 16u || d.rd >= 15u || d.rn >= 15u) {
                                                     EXEC_RAISE_UNDEF();
                                                 }
                                                 cpu.r[d.rd] = cpu.s[d.rm * 2u];
                                                 cpu.r[d.rn] = cpu.s[d.rm * 2u + 1u];
                                                 fpu_mark_active(&cpu);
                                                 break;
                                             }
                        case MM_OP_VMRS: {
                                              if (!fpu_check_or_fault(ctx)) {
                                                  return MM_EXEC_CONTINUE;
                                              }
                                              if (d.rd == 15u) {
                                                  cpu.xpsr = mm_xpsr_write_nzcvq(cpu.xpsr, cpu.fpscr);
                                              } else {
                                                  cpu.r[d.rd] = cpu.fpscr;
                                              }
                                              fpu_mark_active(&cpu);
                                              break;
                                          }
                        case MM_OP_VMSR: {
                                              if (!fpu_check_or_fault(ctx)) {
                                                  return MM_EXEC_CONTINUE;
                                              }
                                              if (d.rd == 15u) {
                                                  cpu.fpscr = (cpu.fpscr & ~0xF0000000u) | (cpu.xpsr & 0xF0000000u);
                                              } else {
                                                  cpu.fpscr = cpu.r[d.rd];
                                              }
                                              fpu_mark_active(&cpu);
                                              break;
                                          }
                        case MM_OP_VLDR: {
                                              mm_u32 base = (d.rn == 15u) ? ((f.pc_fetch + 4u) & ~3u) : cpu.r[d.rn];
                                              mm_u32 offset = d.imm & 0x0FFFFFFFu;
                                              mm_bool u = (d.imm & 0x80000000u) != 0u;
                                              mm_bool w = (d.imm & 0x40000000u) != 0u;
                                              mm_bool p = (d.imm & 0x20000000u) != 0u;
                                              mm_bool is_double = (d.imm & MM_VFP_LS_DOUBLE) != 0u;
                                              mm_u32 addr = base;
                                              mm_u32 val = 0u;
                                              if (!fpu_check_or_fault(ctx)) {
                                                  return MM_EXEC_CONTINUE;
                                              }
                                              if ((!is_double && d.rd >= 32u) || (is_double && d.rd >= 16u)) {
                                                  EXEC_RAISE_UNDEF();
                                              }
                                              if (p) {
                                                  addr = u ? (base + offset) : (base - offset);
                                              }
                                              if (is_double) {
                                                  mm_u32 hi = 0u;
                                                  mm_u32 s_idx = (mm_u32)d.rd * 2u;
                                                  if (!mm_memmap_read(&map, cpu.sec_state, addr, 4u, &val) ||
                                                      !mm_memmap_read(&map, cpu.sec_state, addr + 4u, 4u, &hi)) {
                                                      if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                      return MM_EXEC_CONTINUE;
                                                  }
                                                  cpu.s[s_idx] = val;
                                                  cpu.s[s_idx + 1u] = hi;
                                              } else {
                                                  if (!mm_memmap_read(&map, cpu.sec_state, addr, 4u, &val)) {
                                                      if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                      return MM_EXEC_CONTINUE;
                                                  }
                                                  cpu.s[d.rd] = val;
                                              }
                                              if ((w || !p) && d.rn != 15u) {
                                                  base = u ? (base + offset) : (base - offset);
                                                  if (d.rn == 13u) {
                                                      EXEC_SET_SP(base);
                                                  } else {
                                                      cpu.r[d.rn] = base;
                                                  }
                                              }
                                              fpu_mark_active(&cpu);
                                              break;
                                          }
                        case MM_OP_VSTR: {
                                              mm_u32 base = (d.rn == 15u) ? ((f.pc_fetch + 4u) & ~3u) : cpu.r[d.rn];
                                              mm_u32 offset = d.imm & 0x0FFFFFFFu;
                                              mm_bool u = (d.imm & 0x80000000u) != 0u;
                                              mm_bool w = (d.imm & 0x40000000u) != 0u;
                                              mm_bool p = (d.imm & 0x20000000u) != 0u;
                                              mm_bool is_double = (d.imm & MM_VFP_LS_DOUBLE) != 0u;
                                              mm_u32 addr = base;
                                              if (!fpu_check_or_fault(ctx)) {
                                                  return MM_EXEC_CONTINUE;
                                              }
                                              if ((!is_double && d.rd >= 32u) || (is_double && d.rd >= 16u)) {
                                                  EXEC_RAISE_UNDEF();
                                              }
                                              if (p) {
                                                  addr = u ? (base + offset) : (base - offset);
                                              }
                                              if (is_double) {
                                                  mm_u32 s_idx = (mm_u32)d.rd * 2u;
                                                  if (!mm_memmap_write(&map, cpu.sec_state, addr, 4u, cpu.s[s_idx]) ||
                                                      !mm_memmap_write(&map, cpu.sec_state, addr + 4u, 4u, cpu.s[s_idx + 1u])) {
                                                      if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                      return MM_EXEC_CONTINUE;
                                                  }
                                              } else {
                                                  if (!mm_memmap_write(&map, cpu.sec_state, addr, 4u, cpu.s[d.rd])) {
                                                      if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                      return MM_EXEC_CONTINUE;
                                                  }
                                              }
                                              if ((w || !p) && d.rn != 15u) {
                                                  base = u ? (base + offset) : (base - offset);
                                                  if (d.rn == 13u) {
                                                      EXEC_SET_SP(base);
                                                  } else {
                                                      cpu.r[d.rn] = base;
                                                  }
                                              }
                                              fpu_mark_active(&cpu);
                                              break;
                                          }
                        case MM_OP_VLDM:
                        case MM_OP_VSTM: {
                                              mm_u32 base = cpu.r[d.rn];
                                              mm_u32 count = d.imm & 0xffu;
                                              mm_bool u = (d.imm & 0x80000000u) != 0u;
                                              mm_bool w = (d.imm & 0x40000000u) != 0u;
                                              mm_bool p = (d.imm & 0x20000000u) != 0u;
                                              mm_bool is_double = (d.imm & MM_VFP_LS_DOUBLE) != 0u;
                                              mm_bool load = (d.kind == MM_OP_VLDM);
                                              mm_u32 addr;
                                              mm_u32 ireg;
                                              if (!fpu_check_or_fault(ctx)) {
                                                  return MM_EXEC_CONTINUE;
                                              }
                                              if (count == 0u) {
                                                  EXEC_RAISE_UNDEF();
                                              }
                                              if (is_double) {
                                                  if ((count & 1u) != 0u) {
                                                      EXEC_RAISE_UNDEF();
                                                  }
                                                  count >>= 1;
                                              }
                                              if (is_double) {
                                                  if (d.rd >= 16u || (d.rd + count) > 16u) {
                                                      EXEC_RAISE_UNDEF();
                                                  }
                                              } else {
                                                  if (d.rd >= 32u || (d.rd + count) > 32u) {
                                                      EXEC_RAISE_UNDEF();
                                                  }
                                              }
                                              if (u) {
                                                  addr = p ? (base + (is_double ? 8u : 4u)) : base;
                                              } else {
                                                  mm_u32 step = is_double ? 8u : 4u;
                                                  addr = p ? (base - (count * step)) : (base - ((count - 1u) * step));
                                              }
                                              for (ireg = 0; ireg < count; ++ireg) {
                                                  mm_u32 cur_addr = addr + (ireg * (is_double ? 8u : 4u));
                                                  if (load) {
                                                      if (is_double) {
                                                          mm_u32 lo = 0u;
                                                          mm_u32 hi = 0u;
                                                          if (!mm_memmap_read(&map, cpu.sec_state, cur_addr, 4u, &lo)) {
                                                              if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, cur_addr, MM_FALSE)) done = MM_TRUE;
                                                              return MM_EXEC_CONTINUE;
                                                          }
                                                          if (!mm_memmap_read(&map, cpu.sec_state, cur_addr + 4u, 4u, &hi)) {
                                                              if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, cur_addr + 4u, MM_FALSE)) done = MM_TRUE;
                                                              return MM_EXEC_CONTINUE;
                                                          }
                                                          cpu.s[(d.rd + ireg) * 2u] = lo;
                                                          cpu.s[(d.rd + ireg) * 2u + 1u] = hi;
                                                      } else {
                                                          mm_u32 val = 0u;
                                                          if (!mm_memmap_read(&map, cpu.sec_state, cur_addr, 4u, &val)) {
                                                              if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, cur_addr, MM_FALSE)) done = MM_TRUE;
                                                              return MM_EXEC_CONTINUE;
                                                          }
                                                          cpu.s[d.rd + ireg] = val;
                                                      }
                                                  } else {
                                                      if (is_double) {
                                                          mm_u32 lo = cpu.s[(d.rd + ireg) * 2u];
                                                          mm_u32 hi = cpu.s[(d.rd + ireg) * 2u + 1u];
                                                          if (!mm_memmap_write(&map, cpu.sec_state, cur_addr, 4u, lo)) {
                                                              if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, cur_addr, MM_FALSE)) done = MM_TRUE;
                                                              return MM_EXEC_CONTINUE;
                                                          }
                                                          if (!mm_memmap_write(&map, cpu.sec_state, cur_addr + 4u, 4u, hi)) {
                                                              if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, cur_addr + 4u, MM_FALSE)) done = MM_TRUE;
                                                              return MM_EXEC_CONTINUE;
                                                          }
                                                      } else {
                                                          if (!mm_memmap_write(&map, cpu.sec_state, cur_addr, 4u, cpu.s[d.rd + ireg])) {
                                                              if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, cur_addr, MM_FALSE)) done = MM_TRUE;
                                                              return MM_EXEC_CONTINUE;
                                                          }
                                                      }
                                                  }
                                              }
                                              if (w) {
                                                  mm_u32 step = is_double ? 8u : 4u;
                                                  mm_u32 new_base = u ? (base + (count * step)) : (base - (count * step));
                                                  if (d.rn == 13u) {
                                                      EXEC_SET_SP(new_base);
                                                  } else {
                                                      cpu.r[d.rn] = new_base;
                                                  }
                                              }
                                              fpu_mark_active(&cpu);
                                              break;
                                          }
                        case MM_OP_B_UNCOND:
                        case MM_OP_B_UNCOND_WIDE:
                            {
                                mm_u32 target = (f.pc_fetch + 4u + d.imm) | 1u;
                                cpu.r[15] = target;
                                itstate_clear(&cpu.xpsr, &it_pattern, &it_remaining, &it_cond);
                            }
                            break;
                        case MM_OP_B_COND:
                        case MM_OP_B_COND_WIDE: {
                                                    mm_bool take = MM_FALSE;
                                                    mm_bool n = (cpu.xpsr & (1u << 31)) != 0u;
                                                    mm_bool z = (cpu.xpsr & (1u << 30)) != 0u;
                                                    mm_bool c = (cpu.xpsr & (1u << 29)) != 0u;
                                                    mm_bool v = (cpu.xpsr & (1u << 28)) != 0u;
                                                    switch (d.cond) {
                                                        case MM_COND_EQ: take = z; break;
                                                        case MM_COND_NE: take = !z; break;
                                                        case MM_COND_CS: take = c; break;
                                                        case MM_COND_CC: take = !c; break;
                                                        case MM_COND_MI: take = n; break;
                                                        case MM_COND_PL: take = !n; break;
                                                        case MM_COND_VS: take = v; break;
                                                        case MM_COND_VC: take = !v; break;
                                                        case MM_COND_HI: take = c && !z; break;
                                                        case MM_COND_LS: take = !c || z; break;
                                                        case MM_COND_GE: take = (n == v); break;
                                                        case MM_COND_LT: take = (n != v); break;
                                                        case MM_COND_GT: take = !z && (n == v); break;
                                                        case MM_COND_LE: take = z || (n != v); break;
                                                        case MM_COND_AL: take = MM_TRUE; break;
                                                        default: take = MM_FALSE; break;
                                                    }

                                                    if (take) {
                                                        mm_u32 target = (f.pc_fetch + 4u + d.imm) | 1u;
                                                        cpu.r[15] = target;
                                                        itstate_clear(&cpu.xpsr, &it_pattern, &it_remaining, &it_cond);
                                                    }
                                                } break;
                        case MM_OP_CBZ:
                        case MM_OP_CBNZ: {
                                             /* CBZ/CBNZ T1 use PC+4 as the branch base. */
                                             mm_bool zero = (cpu.r[d.rn] == 0u);
                                             mm_bool take = (d.kind == MM_OP_CBZ) ? zero : (!zero);
                                             if (take) {
                                                 mm_u32 target = (f.pc_fetch + 4u + d.imm) | 1u;
                                                 cpu.r[15] = target;
                                                 itstate_clear(&cpu.xpsr, &it_pattern, &it_remaining, &it_cond);
                                             } else {
                                                 /* Fall-through already handled by PC increment in fetch/decode. */
                                             }
                                         } break;
                                       case MM_OP_BX: {
                                           mm_u32 target = cpu.r[d.rm];
                                           if ((target & 0xffffff00u) == 0xffffff00u) {
                                               if (svc_stack_trace_enabled() && (cpu.xpsr & 0x1ffu) == MM_VECT_SVCALL) {
                                                   printf("[BX_EXC_RETURN] pc=0x%08lx rm=%u target=0x%08lx lr=0x%08lx sp=0x%08lx sec=%d mode=%d ipsr=%lu\n",
                                                          (unsigned long)f.pc_fetch,
                                                          (unsigned)d.rm,
                                                          (unsigned long)target,
                                                          (unsigned long)cpu.r[14],
                                                          (unsigned long)mm_cpu_get_active_sp(&cpu),
                                                          (int)cpu.sec_state,
                                                          (int)cpu.mode,
                                                          (unsigned long)(cpu.xpsr & 0x1ffu));
                                               }
                                               if (!handle_pc_write(&cpu, &map, &scs, target, &it_pattern, &it_remaining, &it_cond)) {
                                                   done = MM_TRUE;
                                               }
                                               return MM_EXEC_CONTINUE;
                                           }
                                           if (d.rm == 14u && cpu.sec_state == MM_NONSECURE &&
                                                   cpu.tz_depth > 0 && target == MM_TZ_FNC_RETURN) {
                                               mm_u32 secure_sp;
                                               /* Return from Secure->Non-secure BLXNS callback. */
                                               cpu.tz_depth--;
                                               cpu.sec_state = cpu.tz_ret_sec[cpu.tz_depth];
                                               cpu.mode = cpu.tz_ret_mode[cpu.tz_depth];
                                               secure_sp = mm_cpu_get_active_sp(&cpu);
                                               mm_cpu_set_active_sp(&cpu, secure_sp + 8u);
                                               cpu.r[15] = cpu.tz_ret_pc[cpu.tz_depth] | 1u;
                                               cpu.r[14] = cpu.tz_ret_pc[cpu.tz_depth] | 1u;
                                               EXEC_SET_SP(mm_cpu_get_active_sp(&cpu));
                                               itstate_clear(&cpu.xpsr, &it_pattern, &it_remaining, &it_cond);
                                           } else {
                                               if (svc_stack_trace_enabled() && (cpu.xpsr & 0x1ffu) == MM_VECT_SVCALL) {
                                                   printf("[BX_PC_WRITE] pc=0x%08lx rm=%u target=0x%08lx lr=0x%08lx sp=0x%08lx sec=%d mode=%d ipsr=%lu\n",
                                                          (unsigned long)f.pc_fetch,
                                                          (unsigned)d.rm,
                                                          (unsigned long)target,
                                                          (unsigned long)cpu.r[14],
                                                          (unsigned long)mm_cpu_get_active_sp(&cpu),
                                                          (int)cpu.sec_state,
                                                          (int)cpu.mode,
                                                          (unsigned long)(cpu.xpsr & 0x1ffu));
                                               }
                                               if (!handle_pc_write(&cpu, &map, &scs, target, &it_pattern, &it_remaining, &it_cond)) {
                                                   done = MM_TRUE;
                                               }
                                           }
                                       } break;
                        case MM_OP_BLX: {
                                           mm_u32 target = cpu.r[d.rm];
                                           cpu.r[14] = (f.pc_fetch + d.len) | 1u;
                                           if (!handle_pc_write(&cpu, &map, &scs, target, &it_pattern, &it_remaining, &it_cond)) {
                                               done = MM_TRUE;
                                           }
                                       } break;
                        case MM_OP_SG:
                                       mm_tz_exec_sg(&cpu, &scs, f.pc_fetch);
                                       break;
                        case MM_OP_BXNS:
                                       mm_tz_exec_bxns(&cpu, cpu.r[d.rm]);
                                       break;
                        case MM_OP_BLXNS:
                                       /* Return address is the next instruction (fetch already advanced PC state). */
                                       mm_tz_exec_blxns(&cpu, cpu.r[d.rm], (f.pc_fetch + d.len));
                                       break;
                        case MM_OP_BL:
                                       {
                                           mm_u32 target = (f.pc_fetch + 4u + d.imm) | 1u;
                                           cpu.r[14] = (f.pc_fetch + 4u) | 1u;
                                           cpu.r[15] = target;
                                           itstate_clear(&cpu.xpsr, &it_pattern, &it_remaining, &it_cond);
                                       }
                                       break;
                        case MM_OP_MOV_IMM: {
                                       mm_bool setflags = MM_FALSE;
                                       mm_bool carry_out = MM_FALSE;
                                       if (d.len == 2u) {
                                           /* Thumb-1 MOVS should not clobber flags inside IT blocks. */
                                           setflags = (it_remaining == 0u) ? MM_TRUE : MM_FALSE;
                                       } else if (d.len == 4u && ((d.raw >> 20) & 1u) != 0u) {
                                           setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                           if (setflags) {
                                               mm_u32 imm12 = (((d.raw >> 26) & 1u) << 11) |
                                                              (((d.raw >> 12) & 0x7u) << 8) |
                                                              (d.raw & 0xffu);
                                               mm_u32 imm32 = 0;
                                               mm_bool carry_in = (cpu.xpsr & (1u << 29)) != 0u;
                                               mm_thumb_expand_imm12_c(imm12, carry_in, &imm32, &carry_out);
                                           }
                                       }
                                       cpu.r[d.rd] = d.imm;
                                       if (setflags) {
                                           mm_u32 res = cpu.r[d.rd];
                                           if (d.len == 2u) {
                                               cpu.xpsr &= ~(0xC0000000u);
                                           } else {
                                               cpu.xpsr &= ~(0xE0000000u);
                                           }
                                           if (res == 0u) cpu.xpsr |= (1u << 30);
                                           if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                           if (d.len == 4u && carry_out) cpu.xpsr |= (1u << 29);
                                       }
                                       break;
                        }
                        case MM_OP_MOVW:
                                       /* MOVW writes a zero-extended 16-bit immediate into Rd. */
                                       cpu.r[d.rd] = d.imm & 0xffffu;
                                       break;
                        case MM_OP_MOVT:
                                       cpu.r[d.rd] = (cpu.r[d.rd] & 0x0000ffffu) | ((d.imm & 0xffffu) << 16);
                                       break;
                        case MM_OP_ADD_IMM:
                                       /* TODO: check the boundaries of memory of the operators */
                                       {
                                           mm_u32 lhs = cpu.r[d.rn];
                                           mm_bool setflags = MM_FALSE;
                                       if (d.len == 4u && d.rn == 15u) {
                                           /* ADD (immediate) with PC base is the ADR form. */
                                           lhs = (f.pc_fetch + 4u) & ~3u;
                                       }
                                       if (d.len == 2u) {
                                           /* Thumb-1 ADD (imm) behaves better if it does not clobber flags inside IT. */
                                           setflags = (it_remaining == 0u) ? MM_TRUE : MM_FALSE;
                                       } else if (((d.raw >> 20) & 1u) != 0u) {
                                           setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                       }
                                           if (setflags) {
                                               mm_u32 res;
                                               mm_bool cflag;
                                               mm_bool vflag;
                                               mm_add_with_carry(lhs, d.imm, MM_FALSE, &res, &cflag, &vflag);
                                               cpu.r[d.rd] = res;
                                               cpu.xpsr &= ~(0xF0000000u);
                                               if (res == 0u) cpu.xpsr |= (1u << 30);
                                               if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                               if (cflag) cpu.xpsr |= (1u << 29);
                                               if (vflag) cpu.xpsr |= (1u << 28);
                                           } else {
                                               cpu.r[d.rd] = lhs + d.imm;
                                           }
                                           if (d.rd == 13u) {
                                               EXEC_SET_SP(cpu.r[13]);
                                           }
                                       }
                                       break;
                        case MM_OP_RSB_IMM: {
                                               mm_u32 res;
                                               mm_bool cflag;
                                               mm_bool vflag;
                                               mm_bool setflags = MM_FALSE;
                                               if ((d.raw & (1u << 20)) != 0u) {
                                                   setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                               }
                                               mm_add_with_carry(d.imm, ~cpu.r[d.rn], MM_TRUE, &res, &cflag, &vflag);
                                               cpu.r[d.rd] = res;
                                               if (setflags) {
                                                   cpu.xpsr &= ~(0xF0000000u);
                                                   if (res == 0u) cpu.xpsr |= (1u << 30);
                                                   if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                   if (cflag) cpu.xpsr |= (1u << 29);
                                                   if (vflag) cpu.xpsr |= (1u << 28);
                                               }
                                           } break;
                        case MM_OP_ADD_SP_IMM:
                                       if (d.rd == 13u) {
                                           mm_u32 sp_before = cpu.r[13];
                                           if (svc_stack_trace_enabled()) {
                                               printf("[ADD_SP_IMM] pc=0x%08lx sp_before=0x%08lx imm=0x%08lx\n",
                                                      (unsigned long)f.pc_fetch,
                                                      (unsigned long)sp_before,
                                                      (unsigned long)d.imm);
                                           }
                                           EXEC_SET_SP(sp_before + d.imm);
                                           if (svc_stack_trace_enabled()) {
                                               printf("[ADD_SP_IMM] pc=0x%08lx sp_after=0x%08lx\n",
                                                      (unsigned long)f.pc_fetch,
                                                      (unsigned long)mm_cpu_get_active_sp(&cpu));
                                           }
                                       } else {
                                           cpu.r[d.rd] = cpu.r[13] + d.imm;
                                       }
                                       break;
                        case MM_OP_ADD_REG:
                                       if ((d.raw & 0xfe000000u) == 0xea000000u) {
                                           mm_u32 rhs = shift_reg_operand(cpu.r[d.rm], d.imm, cpu.xpsr, NULL);
                                           if ((d.raw & (1u << 20)) != 0u) {
                                               mm_bool setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                               mm_u32 res;
                                               mm_bool cflag;
                                               mm_bool vflag;
                                               mm_add_with_carry(cpu.r[d.rn], rhs, MM_FALSE, &res, &cflag, &vflag);
                                               cpu.r[d.rd] = res;
                                               if (setflags) {
                                                   cpu.xpsr &= ~(0xF0000000u);
                                                   if (res == 0u) cpu.xpsr |= (1u << 30);
                                                   if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                   if (cflag) cpu.xpsr |= (1u << 29);
                                                   if (vflag) cpu.xpsr |= (1u << 28);
                                               }
                                           } else {
                                               cpu.r[d.rd] = cpu.r[d.rn] + rhs;
                                           }
                                           if (d.rd == 13u) {
                                               EXEC_SET_SP(cpu.r[13]);
                                           }
                                       } else {
                                           mm_bool setflags = MM_FALSE;
                                           if (d.len == 2u) {
                                               if ((d.raw & 0xfc00u) == 0x4400u) {
                                                   setflags = MM_FALSE;
                                               } else {
                                                   /* Thumb-1 ADDS aliases inside IT must not clobber flags. */
                                                   setflags = (it_remaining == 0u) ? MM_TRUE : MM_FALSE;
                                               }
                                           }
                                           {
                                               mm_u32 lhs = (d.rn == 15u) ? mm_pc_operand(&f) : cpu.r[d.rn];
                                               mm_u32 rhs = (d.rm == 15u) ? mm_pc_operand(&f) : cpu.r[d.rm];

                                               if (setflags) {
                                                   mm_u32 res;
                                                   mm_bool cflag;
                                                   mm_bool vflag;
                                                   mm_add_with_carry(lhs, rhs, MM_FALSE, &res, &cflag, &vflag);
                                                   cpu.r[d.rd] = res;
                                                   cpu.xpsr &= ~(0xF0000000u);
                                                   if (res == 0u) cpu.xpsr |= (1u << 30);
                                                   if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                   if (cflag) cpu.xpsr |= (1u << 29);
                                                   if (vflag) cpu.xpsr |= (1u << 28);
                                               } else {
                                                   cpu.r[d.rd] = lhs + rhs;
                                               }
                                           }
                                       }
                                       break;
                        case MM_OP_LSL_REG: {
                                                mm_u32 val = cpu.r[d.rn];
                                                mm_u32 sh = cpu.r[d.rm] & 0xffu;
                                                mm_bool carry_in = (cpu.xpsr & (1u << 29)) != 0u;
                                                struct mm_shift_result r = mm_lsl(val, (mm_u8)sh, carry_in);
                                                mm_bool setflags = MM_FALSE;
                                                if (d.len == 2u) {
                                                    setflags = (it_remaining == 0u) ? MM_TRUE : MM_FALSE;
                                                } else if (d.len == 4u && ((d.raw >> 20) & 1u) != 0u) {
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                }
                                                cpu.r[d.rd] = r.value;
                                                if (setflags) {
                                                    cpu.xpsr &= ~(0xE0000000u);
                                                    if (r.value == 0u) cpu.xpsr |= (1u << 30);
                                                    if (r.value & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                    if (r.carry_out) cpu.xpsr |= (1u << 29);
                                                }
                                            } break;
                        case MM_OP_LSL_IMM: {
                                                mm_u32 val = cpu.r[d.rm];
                                                mm_u32 sh = d.imm & 0x1fu;
                                                mm_u32 res;
                                                mm_bool carry = (cpu.xpsr & (1u << 29)) != 0u;
                                                mm_bool setflags = MM_FALSE;
                                                if (sh == 0u) {
                                                    res = val;
                                                } else {
                                                    carry = (val >> (32u - sh)) & 0x1u;
                                                    res = val << sh;
                                                }
                                                if (d.len == 2u) {
                                                    /* Thumb-1 LSLS aliases inside IT must not clobber flags. */
                                                    setflags = (it_remaining == 0u) ? MM_TRUE : MM_FALSE;
                                                } else if (d.len == 4u && ((d.raw >> 20) & 1u) != 0u) {
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                }
                                                cpu.r[d.rd] = res;
                                                if (setflags) {
                                                    cpu.xpsr &= ~(0xE0000000u);
                                                    if (res == 0u) cpu.xpsr |= (1u << 30);
                                                    if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                    if (carry) cpu.xpsr |= (1u << 29);
                                                }
                                            } break;
                        case MM_OP_LSR_REG: {
                                                mm_u32 val = cpu.r[d.rn];
                                                mm_u32 sh = cpu.r[d.rm] & 0xffu;
                                                mm_bool carry_in = (cpu.xpsr & (1u << 29)) != 0u;
                                                struct mm_shift_result r = mm_lsr(val, (mm_u8)sh, carry_in);
                                                mm_bool setflags = MM_FALSE;
                                                if (d.len == 2u) {
                                                    setflags = (it_remaining == 0u) ? MM_TRUE : MM_FALSE;
                                                } else if (d.len == 4u && ((d.raw >> 20) & 1u) != 0u) {
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                }
                                                cpu.r[d.rd] = r.value;
                                                if (setflags) {
                                                    cpu.xpsr &= ~(0xE0000000u);
                                                    if (r.value == 0u) cpu.xpsr |= (1u << 30);
                                                    if (r.value & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                    if (r.carry_out) cpu.xpsr |= (1u << 29);
                                                }
                                            } break;
                        case MM_OP_LSR_IMM: {
                                                mm_u32 val = cpu.r[d.rm];
                                                mm_u32 sh = d.imm & 0x1fu;
                                                mm_u32 res;
                                                mm_bool carry = (cpu.xpsr & (1u << 29)) != 0u;
                                                mm_bool setflags = MM_FALSE;
                                                if (sh == 0u) {
                                                    carry = (val >> 31) & 0x1u;
                                                    res = 0;
                                                } else {
                                                    carry = (val >> (sh - 1u)) & 0x1u;
                                                    res = val >> sh;
                                                }
                                                if (d.len == 2u) {
                                                    setflags = (it_remaining == 0u) ? MM_TRUE : MM_FALSE;
                                                } else if (d.len == 4u && ((d.raw >> 20) & 1u) != 0u) {
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                }
                                                cpu.r[d.rd] = res;
                                                if (setflags) {
                                                    cpu.xpsr &= ~(0xE0000000u);
                                                    if (res == 0u) cpu.xpsr |= (1u << 30);
                                                    if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                    if (carry) cpu.xpsr |= (1u << 29);
                                                }
                                            } break;
                        case MM_OP_ROR_IMM: {
                                                mm_u32 val = cpu.r[d.rm];
                                                mm_u32 sh = d.imm & 0x1fu;
                                                mm_bool carry_in = (cpu.xpsr & (1u << 29)) != 0u;
                                                mm_bool carry_out = carry_in;
                                                mm_u32 res = mm_shift_c_imm(val, 3u, (mm_u8)sh, carry_in, &carry_out);
                                                mm_bool setflags = MM_FALSE;
                                                if (d.len == 4u && ((d.raw >> 20) & 1u) != 0u) {
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                }
                                                cpu.r[d.rd] = res;
                                                if (setflags) {
                                                    cpu.xpsr &= ~(0xE0000000u);
                                                    if (res == 0u) cpu.xpsr |= (1u << 30);
                                                    if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                    if (carry_out) cpu.xpsr |= (1u << 29);
                                                }
                                            } break;
                        case MM_OP_ASR_REG: {
                                                mm_u32 val = cpu.r[d.rn];
                                                mm_u32 sh = cpu.r[d.rm] & 0xffu;
                                                mm_bool carry_in = (cpu.xpsr & (1u << 29)) != 0u;
                                                struct mm_shift_result r = mm_asr(val, (mm_u8)sh, carry_in);
                                                mm_bool setflags = MM_FALSE;
                                                if (d.len == 2u) {
                                                    setflags = (it_remaining == 0u) ? MM_TRUE : MM_FALSE;
                                                } else if (d.len == 4u && ((d.raw >> 20) & 1u) != 0u) {
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                }
                                                cpu.r[d.rd] = r.value;
                                                if (setflags) {
                                                    cpu.xpsr &= ~(0xE0000000u);
                                                    if (r.value == 0u) cpu.xpsr |= (1u << 30);
                                                    if (r.value & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                    if (r.carry_out) cpu.xpsr |= (1u << 29);
                                                }
                                            } break;
                        case MM_OP_ASR_IMM: {
                                                mm_u32 val = cpu.r[d.rm];
                                                mm_u32 sh = d.imm & 0x1fu;
                                                mm_u32 res;
                                                mm_bool carry = (cpu.xpsr & (1u << 29)) != 0u;
                                                mm_bool setflags = MM_FALSE;
                                                if (sh == 0u) {
                                                    carry = (val >> 31) & 0x1u;
                                                    res = (val & 0x80000000u) ? 0xffffffffu : 0u;
                                                } else {
                                                    carry = (val >> (sh - 1u)) & 0x1u;
                                                    res = (mm_u32)(((mm_i32)val) >> sh);
                                                }
                                                if (d.len == 2u) {
                                                    setflags = (it_remaining == 0u) ? MM_TRUE : MM_FALSE;
                                                } else if (d.len == 4u && ((d.raw >> 20) & 1u) != 0u) {
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                }
                                                cpu.r[d.rd] = res;
                                                if (setflags) {
                                                    cpu.xpsr &= ~(0xE0000000u);
                                                    if (res == 0u) cpu.xpsr |= (1u << 30);
                                                    if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                    if (carry) cpu.xpsr |= (1u << 29);
                                                }
                                            } break;
                        case MM_OP_ROR_REG: {
                                                mm_u32 val = cpu.r[d.rn];
                                                mm_u32 sh = cpu.r[d.rm] & 0xffu;
                                                mm_bool setflags = MM_FALSE;
                                                mm_bool carry_in = (cpu.xpsr & (1u << 29)) != 0u;
                                                mm_bool carry_out = carry_in;
                                                mm_u32 res = mm_ror_reg_shift_c(val, sh, carry_in, &carry_out);
                                                if (d.len == 2u) {
                                                    /* Thumb-1 RORS aliases inside IT must not clobber flags. */
                                                    setflags = (it_remaining == 0u) ? MM_TRUE : MM_FALSE;
                                                } else if (d.len == 4u && ((d.raw >> 20) & 1u) != 0u) {
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                }
                                                cpu.r[d.rd] = res;
                                                if (setflags) {
                                                    cpu.xpsr &= ~(0xE0000000u);
                                                    if (res == 0u) cpu.xpsr |= (1u << 30);
                                                    if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                    if (carry_out) cpu.xpsr |= (1u << 29);
                                                }
                                            } break;
                        case MM_OP_ROR_REG_NF: {
                                                 mm_u32 val = cpu.r[d.rn];
                                                 mm_u32 sh = cpu.r[d.rm] & 0xffu;
                                                 mm_bool carry_in = (cpu.xpsr & (1u << 29)) != 0u;
                                                 mm_bool carry_out = carry_in;
                                                 mm_u32 res = mm_ror_reg_shift_c(val, sh, carry_in, &carry_out);
                                                 cpu.r[d.rd] = res;
                                             } break;
                        case MM_OP_NEG: {
                                            mm_u32 res;
                                            mm_bool cflag;
                                            mm_bool vflag;
                                            mm_bool setflags = (d.len == 2u && it_remaining > 0u) ? MM_FALSE : MM_TRUE;
                                            mm_add_with_carry(0u, ~cpu.r[d.rm], MM_TRUE, &res, &cflag, &vflag);
                                            cpu.r[d.rd] = res;
                                            if (setflags) {
                                                cpu.xpsr &= ~(0xF0000000u);
                                                if (res == 0u) cpu.xpsr |= (1u << 30);
                                                if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                if (cflag) cpu.xpsr |= (1u << 29);
                                                if (vflag) cpu.xpsr |= (1u << 28);
                                            }
                                        } break;
                        case MM_OP_SBCS_REG: {
                                                 mm_bool reg_form = ((d.raw & 0xfe000000u) == 0xea000000u);
                                                 mm_bool setflags = MM_FALSE;
                                                 if (reg_form) {
                                                     if (((d.raw >> 20) & 1u) != 0u) {
                                                         setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                     }
                                                     {
                                                         mm_u32 rhs = shift_reg_operand(cpu.r[d.rm], d.imm, cpu.xpsr, NULL);
                                                         cpu.r[d.rd] = mm_sbcs_reg(cpu.r[d.rn], rhs, &cpu.xpsr, setflags);
                                                     }
                                                 } else {
                                                     setflags = (it_remaining == 0u) ? MM_TRUE : MM_FALSE;
                                                     cpu.r[d.rd] = mm_sbcs_reg(cpu.r[d.rn], cpu.r[d.rm], &cpu.xpsr, setflags);
                                                 }
                                             } break;
                        case MM_OP_ADCS_REG: {
                                                 mm_bool reg_form = ((d.raw & 0xfe000000u) == 0xea000000u);
                                                 mm_bool setflags = MM_FALSE;
                                                 if (d.len == 2u) {
                                                     setflags = (it_remaining == 0u) ? MM_TRUE : MM_FALSE;
                                                 } else if (d.len == 4u && ((d.raw >> 20) & 1u) != 0u) {
                                                     setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                 }
                                                 if (reg_form) {
                                                     mm_u32 rhs = shift_reg_operand(cpu.r[d.rm], d.imm, cpu.xpsr, NULL);
                                                     cpu.r[d.rd] = mm_adcs_reg(cpu.r[d.rn], rhs, &cpu.xpsr, setflags);
                                                 } else {
                                                     cpu.r[d.rd] = mm_adcs_reg(cpu.r[d.rn], cpu.r[d.rm], &cpu.xpsr, setflags);
                                                 }
                                             } break;
                        case MM_OP_ADC_IMM: {
                                                mm_u32 res;
                                                mm_bool cflag;
                                                mm_bool vflag;
                                                mm_bool carry_in = (cpu.xpsr & (1u << 29)) != 0u;
                                                mm_bool setflags = MM_FALSE;
                                                mm_add_with_carry(cpu.r[d.rn], d.imm, carry_in, &res, &cflag, &vflag);
                                                cpu.r[d.rd] = res;
                                                if (d.len == 4u && ((d.raw >> 20) & 1u) != 0u) {
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                }
                                                if (setflags) {
                                                    cpu.xpsr &= ~(0xF0000000u);
                                                    if (res == 0u) cpu.xpsr |= (1u << 30);
                                                    if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                    if (cflag) cpu.xpsr |= (1u << 29);
                                                    if (vflag) cpu.xpsr |= (1u << 28);
                                                }
                                            } break;
                        case MM_OP_AND_REG: {
                                                mm_u32 lhs = cpu.r[d.rn];
                                                mm_u32 rhs;
                                                mm_bool reg_form = ((d.raw & 0xfe000000u) == 0xea000000u);
                                                mm_u32 res;
                                                mm_bool setflags = MM_FALSE;
                                                mm_bool carry_out = (cpu.xpsr & (1u << 29)) != 0u;
                                                if (d.len == 2u) {
                                                    /* Thumb-1 ANDS aliases inside IT must not clobber flags. */
                                                    setflags = (it_remaining == 0u) ? MM_TRUE : MM_FALSE;
                                                } else if (d.len == 4u && ((d.raw >> 20) & 1u) != 0u) {
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                }
                                                if (reg_form) {
                                                    rhs = shift_reg_operand(cpu.r[d.rm], d.imm, cpu.xpsr, &carry_out);
                                                } else if (d.len == 4u) {
                                                    /* Thumb-2 data-processing (modified immediate) AND. */
                                                    rhs = d.imm;
                                                    if (setflags) {
                                                        mm_u32 imm12 = (((d.raw >> 26) & 1u) << 11) |
                                                                       (((d.raw >> 12) & 0x7u) << 8) |
                                                                       (d.raw & 0xffu);
                                                        mm_u32 imm32 = 0;
                                                        mm_bool carry_in = (cpu.xpsr & (1u << 29)) != 0u;
                                                        mm_thumb_expand_imm12_c(imm12, carry_in, &imm32, &carry_out);
                                                    }
                                                } else {
                                                    /* 16-bit ANDS (register) */
                                                    rhs = cpu.r[d.rm];
                                                }
                                                res = lhs & rhs;
                                                if (d.rd == 15u) {
                                                    if (!handle_pc_write(&cpu, &map, &scs, res, &it_pattern, &it_remaining, &it_cond)) {
                                                        done = MM_TRUE;
                                                    }
                                                } else {
                                                    cpu.r[d.rd] = res;
                                                }
                                                if (setflags) {
                                                    cpu.xpsr &= ~(0xE0000000u);
                                                    if (res == 0u) cpu.xpsr |= (1u << 30);
                                                    if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                    if (carry_out) cpu.xpsr |= (1u << 29);
                                                }
                                            } break;
                        case MM_OP_EOR_REG: {
                                                mm_u32 lhs = cpu.r[d.rn];
                                                mm_u32 rhs;
                                                mm_bool reg_form = ((d.raw & 0xfe000000u) == 0xea000000u);
                                                mm_bool setflags = MM_FALSE;
                                                mm_bool carry_out = (cpu.xpsr & (1u << 29)) != 0u;
                                                if (d.len == 2u) {
                                                    /* Thumb-1 EORS aliases inside IT must not clobber flags. */
                                                    setflags = (it_remaining == 0u) ? MM_TRUE : MM_FALSE;
                                                } else if (d.len == 4u && ((d.raw >> 20) & 1u) != 0u) {
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                }
                                                if (reg_form) {
                                                    rhs = shift_reg_operand(cpu.r[d.rm], d.imm, cpu.xpsr, &carry_out);
                                                } else if (d.len == 4u) {
                                                    /* Thumb-2 data-processing (modified immediate) EOR. */
                                                    rhs = d.imm;
                                                    if (setflags) {
                                                        mm_u32 imm12 = (((d.raw >> 26) & 1u) << 11) |
                                                                       (((d.raw >> 12) & 0x7u) << 8) |
                                                                       (d.raw & 0xffu);
                                                        mm_u32 imm32 = 0;
                                                        mm_bool carry_in = (cpu.xpsr & (1u << 29)) != 0u;
                                                        mm_thumb_expand_imm12_c(imm12, carry_in, &imm32, &carry_out);
                                                    }
                                                } else {
                                                    /* 16-bit EORS (register) */
                                                    rhs = cpu.r[d.rm];
                                                }
                                                {
                                                    mm_u32 res = lhs ^ rhs;
                                                    if (d.rd == 15u) {
                                                        if (!handle_pc_write(&cpu, &map, &scs, res, &it_pattern, &it_remaining, &it_cond)) {
                                                            done = MM_TRUE;
                                                        }
                                                    } else {
                                                        cpu.r[d.rd] = res;
                                                    }
                                                    if (setflags) {
                                                        cpu.xpsr &= ~(0xE0000000u);
                                                        if (res == 0u) cpu.xpsr |= (1u << 30);
                                                        if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                        if (carry_out) cpu.xpsr |= (1u << 29);
                                                    }
                                                }
                                            } break;
                        case MM_OP_TEQ_REG: {
                                                mm_u32 lhs = cpu.r[d.rn];
                                                mm_u32 rhs;
                                                mm_bool carry_out = (cpu.xpsr & (1u << 29)) != 0u;
                                                rhs = shift_reg_operand(cpu.r[d.rm], d.imm, cpu.xpsr, &carry_out);
                                                {
                                                    mm_u32 res = lhs ^ rhs;
                                                    cpu.xpsr &= ~(0xE0000000u);
                                                    if (res == 0u) cpu.xpsr |= (1u << 30);
                                                    if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                    if (carry_out) cpu.xpsr |= (1u << 29);
                                                }
                                            } break;
                        case MM_OP_TEQ_IMM: {
                                                mm_u32 imm12 = (((d.raw >> 26) & 1u) << 11) |
                                                               (((d.raw >> 12) & 0x7u) << 8) |
                                                               (d.raw & 0xffu);
                                                mm_u32 imm32 = 0;
                                                mm_bool carry_out = (cpu.xpsr & (1u << 29)) != 0u;
                                                mm_u32 res;
                                                mm_thumb_expand_imm12_c(imm12, carry_out, &imm32, &carry_out);
                                                res = cpu.r[d.rn] ^ imm32;
                                                cpu.xpsr &= ~(0xE0000000u);
                                                if (res == 0u) cpu.xpsr |= (1u << 30);
                                                if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                if (carry_out) cpu.xpsr |= (1u << 29);
                                            } break;
                        case MM_OP_TST_REG: {
                                                mm_u32 rhs = cpu.r[d.rm];
                                                mm_bool carry_out = (cpu.xpsr & (1u << 29)) != 0u;
                                                if ((d.raw & 0xfe000000u) == 0xea000000u) {
                                                    rhs = shift_reg_operand(cpu.r[d.rm], d.imm, cpu.xpsr, &carry_out);
                                                }
                                                {
                                                    mm_u32 res = cpu.r[d.rn] & rhs;
                                                    cpu.xpsr &= ~(0xE0000000u);
                                                    if (res == 0u) cpu.xpsr |= (1u << 30);
                                                    if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                    if (carry_out) cpu.xpsr |= (1u << 29);
                                                }
                                            } break;
                        case MM_OP_TST_IMM: {
                                                mm_bool carry_out = (cpu.xpsr & (1u << 29)) != 0u;
                                                mm_u32 res = cpu.r[d.rn] & d.imm;
                                                if (d.len == 4u) {
                                                    mm_u32 imm12 = (((d.raw >> 26) & 1u) << 11) |
                                                                   (((d.raw >> 12) & 0x7u) << 8) |
                                                                   (d.raw & 0xffu);
                                                    mm_u32 imm32 = 0;
                                                    mm_bool carry_in = (cpu.xpsr & (1u << 29)) != 0u;
                                                    mm_thumb_expand_imm12_c(imm12, carry_in, &imm32, &carry_out);
                                                }
                                                cpu.xpsr &= ~(0xE0000000u);
                                                if (res == 0u) cpu.xpsr |= (1u << 30);
                                                if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                if (carry_out) cpu.xpsr |= (1u << 29);
                                            } break;
                        case MM_OP_ORR_REG: {
                                                mm_u32 lhs;
                                                mm_u32 rhs;
                                                mm_bool reg_form = ((d.raw & 0xfe000000u) == 0xea000000u);
                                                mm_bool setflags = MM_FALSE;
                                                mm_bool carry_out = (cpu.xpsr & (1u << 29)) != 0u;
                                                if (d.len == 2u) {
                                                    /* Thumb-1 ORRS aliases inside IT must not clobber flags. */
                                                    setflags = (it_remaining == 0u) ? MM_TRUE : MM_FALSE;
                                                } else if (d.len == 4u && ((d.raw >> 20) & 1u) != 0u) {
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                }
                                                if (d.rn == 15u) {
                                                    /* ORR (immediate) alias MOV (immediate) uses Rn=1111. */
                                                    lhs = 0;
                                                } else {
                                                    lhs = cpu.r[d.rn];
                                                }
                                                if (reg_form) {
                                                    rhs = shift_reg_operand(cpu.r[d.rm], d.imm, cpu.xpsr, &carry_out);
                                                } else if (d.len == 4u) {
                                                    /* Thumb-2 data-processing (modified immediate) ORR. */
                                                    rhs = d.imm;
                                                    if (setflags) {
                                                        mm_u32 imm12 = (((d.raw >> 26) & 1u) << 11) |
                                                                       (((d.raw >> 12) & 0x7u) << 8) |
                                                                       (d.raw & 0xffu);
                                                        mm_u32 imm32 = 0;
                                                        mm_bool carry_in = (cpu.xpsr & (1u << 29)) != 0u;
                                                        mm_thumb_expand_imm12_c(imm12, carry_in, &imm32, &carry_out);
                                                    }
                                                } else {
                                                    /* 16-bit ORRS (register) */
                                                    rhs = cpu.r[d.rm];
                                                }
                                                cpu.r[d.rd] = lhs | rhs;
                                                if (setflags) {
                                                    mm_u32 res = cpu.r[d.rd];
                                                    cpu.xpsr &= ~(0xE0000000u);
                                                    if (res == 0u) cpu.xpsr |= (1u << 30);
                                                    if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                    if (carry_out) cpu.xpsr |= (1u << 29);
                                                }
                                            } break;
                        case MM_OP_ORN_REG:
                        case MM_OP_ORN_IMM: {
                                                mm_u32 lhs = cpu.r[d.rn];
                                                mm_u32 rhs;
                                                mm_bool reg_form = (d.kind == MM_OP_ORN_REG);
                                                mm_bool setflags = MM_FALSE;
                                                mm_bool carry_out = (cpu.xpsr & (1u << 29)) != 0u;
                                                if (d.len == 4u && ((d.raw >> 20) & 1u) != 0u) {
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                }
                                                if (reg_form) {
                                                    rhs = shift_reg_operand(cpu.r[d.rm], d.imm, cpu.xpsr, &carry_out);
                                                } else {
                                                    rhs = d.imm;
                                                    if (setflags) {
                                                        mm_u32 imm12 = (((d.raw >> 26) & 1u) << 11) |
                                                                       (((d.raw >> 12) & 0x7u) << 8) |
                                                                       (d.raw & 0xffu);
                                                        mm_u32 imm32 = 0;
                                                        mm_bool carry_in = (cpu.xpsr & (1u << 29)) != 0u;
                                                        mm_thumb_expand_imm12_c(imm12, carry_in, &imm32, &carry_out);
                                                    }
                                                }
                                                cpu.r[d.rd] = lhs | (~rhs);
                                                if (setflags) {
                                                    mm_u32 res = cpu.r[d.rd];
                                                    cpu.xpsr &= ~(0xE0000000u);
                                                    if (res == 0u) cpu.xpsr |= (1u << 30);
                                                    if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                    if (carry_out) cpu.xpsr |= (1u << 29);
                                                }
                                            } break;
                        case MM_OP_BIC_REG: {
                                                mm_u32 lhs = cpu.r[d.rn];
                                                mm_u32 rhs;
                                                mm_bool reg_form = ((d.raw & 0xfe000000u) == 0xea000000u);
                                                mm_bool setflags = MM_FALSE;
                                                mm_bool carry_out = (cpu.xpsr & (1u << 29)) != 0u;
                                                if (d.len == 2u) {
                                                    /* Thumb-1 BICS aliases inside IT must not clobber flags. */
                                                    setflags = (it_remaining == 0u) ? MM_TRUE : MM_FALSE;
                                                } else if (d.len == 4u && ((d.raw >> 20) & 1u) != 0u) {
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                }
                                                if (reg_form) {
                                                    rhs = shift_reg_operand(cpu.r[d.rm], d.imm, cpu.xpsr, &carry_out);
                                                } else if (d.len == 4u) {
                                                    /* Thumb-2 data-processing (modified immediate) BIC. */
                                                    rhs = d.imm;
                                                    if (setflags) {
                                                        mm_u32 imm12 = (((d.raw >> 26) & 1u) << 11) |
                                                                       (((d.raw >> 12) & 0x7u) << 8) |
                                                                       (d.raw & 0xffu);
                                                        mm_u32 imm32 = 0;
                                                        mm_bool carry_in = (cpu.xpsr & (1u << 29)) != 0u;
                                                        mm_thumb_expand_imm12_c(imm12, carry_in, &imm32, &carry_out);
                                                    }
                                                } else {
                                                    /* 16-bit BICS (register) */
                                                    rhs = cpu.r[d.rm];
                                                }
                                                cpu.r[d.rd] = lhs & (~rhs);
                                                if (setflags) {
                                                    mm_u32 res = cpu.r[d.rd];
                                                    cpu.xpsr &= ~(0xE0000000u);
                                                    if (res == 0u) cpu.xpsr |= (1u << 30);
                                                    if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                    if (carry_out) cpu.xpsr |= (1u << 29);
                                                }
                                            } break;
                        case MM_OP_MUL: {
                                            mm_u32 lhs = cpu.r[d.rd];
                                            mm_u32 rhs = cpu.r[d.rm];
                                            mm_u32 res = lhs * rhs;
                                            cpu.r[d.rd] = res;
                                            if (it_remaining == 0u) {
                                                cpu.xpsr &= ~(0xC0000000u);
                                                if (res == 0u) cpu.xpsr |= (1u << 30);
                                                if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                            }
                                        } break;
                        case MM_OP_REV: {
                                            mm_u32 val = cpu.r[d.rm];
                                            cpu.r[d.rd] = mm_bswap32(val);
                                        } break;
                        case MM_OP_REV16: {
                                              mm_u32 val = cpu.r[d.rm];
                                              cpu.r[d.rd] = mm_rev16(val);
                                          } break;
                        case MM_OP_REVSH: {
                                              mm_u32 val = cpu.r[d.rm];
                                              cpu.r[d.rd] = mm_revsh(val);
                                          } break;
                        case MM_OP_UBFX: {
                                             mm_u32 imm3 = (d.raw >> 12) & 0x7u;
                                             mm_u32 imm2 = (d.raw >> 6) & 0x3u;
                                             mm_u32 lsb = (imm3 << 2) | imm2;
                                             mm_u32 width = (d.raw & 0x1fu) + 1u;
                                             if (d.rd == 15u || d.rn == 15u || lsb >= 32u || width == 0u || (lsb + width) > 32u) {
                                                 EXEC_RAISE_UNDEF();
                                             }
                                             cpu.r[d.rd] = mm_ubfx(cpu.r[d.rn], (mm_u8)lsb, (mm_u8)width);
                                         } break;
                        case MM_OP_SBFX: {
                                             mm_u32 imm3 = (d.raw >> 12) & 0x7u;
                                             mm_u32 imm2 = (d.raw >> 6) & 0x3u;
                                             mm_u32 lsb = (imm3 << 2) | imm2;
                                             mm_u32 width = (d.raw & 0x1fu) + 1u;
                                             if (d.rd == 15u || d.rn == 15u || lsb >= 32u || width == 0u || (lsb + width) > 32u) {
                                                 EXEC_RAISE_UNDEF();
                                             }
                                             cpu.r[d.rd] = mm_sbfx(cpu.r[d.rn], (mm_u8)lsb, (mm_u8)width);
                                         } break;
                        case MM_OP_BFI: {
                                            mm_u32 imm3 = (d.raw >> 12) & 0x7u;
                                            mm_u32 imm2 = (d.raw >> 6) & 0x3u;
                                            mm_u32 lsb = (imm3 << 2) | imm2;
                                            mm_u32 msb = d.raw & 0x1fu;
                                            mm_u32 width;
                                            if (msb < lsb) {
                                                EXEC_RAISE_UNDEF();
                                            }
                                            width = (msb - lsb) + 1u;
                                            if (d.rd == 15u || d.rn == 15u || lsb >= 32u || width == 0u || (lsb + width) > 32u) {
                                                EXEC_RAISE_UNDEF();
                                            }
                                            cpu.r[d.rd] = mm_bfi(cpu.r[d.rd], cpu.r[d.rn], (mm_u8)lsb, (mm_u8)width);
                                        } break;
                        case MM_OP_BFC: {
                                            mm_u32 imm3 = (d.raw >> 12) & 0x7u;
                                            mm_u32 imm2 = (d.raw >> 6) & 0x3u;
                                            mm_u32 lsb = (imm3 << 2) | imm2;
                                            mm_u32 msb = d.raw & 0x1fu;
                                            mm_u32 width;
                                            if (msb < lsb) {
                                                EXEC_RAISE_UNDEF();
                                            }
                                            width = (msb - lsb) + 1u;
                                            if (d.rd == 15u || lsb >= 32u || width == 0u || (lsb + width) > 32u) {
                                                EXEC_RAISE_UNDEF();
                                            }
                                            cpu.r[d.rd] = mm_bfc(cpu.r[d.rd], (mm_u8)lsb, (mm_u8)width);
                                        } break;
                        case MM_OP_UDIV: {
                                             mm_u32 divisor = cpu.r[d.rm];
                                             if (divisor == 0u) {
                                                 if ((scs.ccr & CCR_DIV_0_TRP) != 0u) {
                                                     if (!raise_usage_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, UFSR_DIVBYZERO)) done = MM_TRUE;
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                                 cpu.r[d.rd] = 0u;
                                             } else {
                                                 cpu.r[d.rd] = cpu.r[d.rn] / divisor;
                                             }
                                         } break;
                        case MM_OP_SDIV: {
                                             mm_u32 divisor_u = cpu.r[d.rm];
                                             if (divisor_u == 0u) {
                                                 if ((scs.ccr & CCR_DIV_0_TRP) != 0u) {
                                                     if (!raise_usage_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, UFSR_DIVBYZERO)) done = MM_TRUE;
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                                 cpu.r[d.rd] = 0u;
                                             } else {
                                                 mm_i32 dividend = (mm_i32)cpu.r[d.rn];
                                                 mm_i32 divisor = (mm_i32)divisor_u;
                                                 if (dividend == (mm_i32)0x80000000u && divisor == -1) {
                                                     cpu.r[d.rd] = 0x80000000u;
                                                 } else {
                                                     mm_i32 quot = dividend / divisor;
                                                     cpu.r[d.rd] = (mm_u32)quot;
                                                 }
                                             }
                                         } break;
                        case MM_OP_UMULL:
                        case MM_OP_UMLAL: {
                                              mm_u32 lo;
                                              mm_u32 hi;
                                              mm_u64 acc;
                                              mm_umul64(cpu.r[d.rn], cpu.r[d.rm], &lo, &hi);
                                              if (d.kind == MM_OP_UMLAL) {
                                                  acc = ((mm_u64)cpu.r[d.ra] << 32) | cpu.r[d.rd];
                                                  acc += ((mm_u64)hi << 32) | lo;
                                                  lo = (mm_u32)acc;
                                                  hi = (mm_u32)(acc >> 32);
                                              }
                                              cpu.r[d.rd] = lo;
                                              cpu.r[d.ra] = hi;
                                          } break;
                        case MM_OP_UMAAL: {
                                              mm_u64 acc;
                                              acc = (mm_u64)cpu.r[d.rn] * (mm_u64)cpu.r[d.rm];
                                              acc += (mm_u64)cpu.r[d.rd];
                                              acc += (mm_u64)cpu.r[d.ra];
                                              cpu.r[d.rd] = (mm_u32)acc;
                                              cpu.r[d.ra] = (mm_u32)(acc >> 32);
                                          } break;
                        case MM_OP_SMULL:
                        case MM_OP_SMLAL: {
                                              mm_u32 lo;
                                              mm_u32 hi;
                                              mm_u64 acc;
                                              mm_smul64(cpu.r[d.rn], cpu.r[d.rm], &lo, &hi);
                                              if (d.kind == MM_OP_SMLAL) {
                                                  acc = ((mm_u64)cpu.r[d.ra] << 32) | cpu.r[d.rd];
                                                  acc += ((mm_u64)hi << 32) | lo;
                                                  lo = (mm_u32)acc;
                                                  hi = (mm_u32)(acc >> 32);
                                              }
                                              cpu.r[d.rd] = lo;
                                              cpu.r[d.ra] = hi;
                                          } break;
                        case MM_OP_MLA: {
                                            mm_u32 prod = cpu.r[d.rn] * cpu.r[d.rm];
                                            cpu.r[d.rd] = prod + cpu.r[d.ra];
                                        } break;
                        case MM_OP_SMLA: {
                                            mm_u32 rn_val = cpu.r[d.rn];
                                            mm_u32 rm_val = cpu.r[d.rm];
                                            mm_i32 rn_half = (mm_i16)(((d.imm & 0x2u) != 0u) ? (rn_val >> 16) : (rn_val & 0xffffu));
                                            mm_i32 rm_half = (mm_i16)(((d.imm & 0x1u) != 0u) ? (rm_val >> 16) : (rm_val & 0xffffu));
                                            mm_i32 prod = rn_half * rm_half;
                                            mm_i64 acc64 = (mm_i64)prod + (mm_i64)(mm_i32)cpu.r[d.ra];
                                            mm_i32 acc = (mm_i32)acc64;
                                            cpu.r[d.rd] = (mm_u32)acc;
                                            if (acc64 != (mm_i64)acc) {
                                                cpu.q_flag = MM_TRUE;
                                            }
                                        } break;
                        case MM_OP_MLS: {
                                            mm_u32 prod = cpu.r[d.rn] * cpu.r[d.rm];
                                            cpu.r[d.rd] = cpu.r[d.ra] - prod;
                                        } break;
                        case MM_OP_SMLAD:
                        case MM_OP_SMLADX: {
                                              mm_u32 rn_val = cpu.r[d.rn];
                                              mm_u32 rm_val = cpu.r[d.rm];
                                              mm_i16 rn_lo = (mm_i16)(rn_val & 0xffffu);
                                              mm_i16 rn_hi = (mm_i16)(rn_val >> 16);
                                              mm_i16 rm_lo = (mm_i16)(rm_val & 0xffffu);
                                              mm_i16 rm_hi = (mm_i16)(rm_val >> 16);
                                              mm_i32 prod1, prod2;
                                              mm_i64 sum;
                                              if (d.kind == MM_OP_SMLADX) {
                                                  prod1 = (mm_i32)rn_lo * (mm_i32)rm_hi;
                                                  prod2 = (mm_i32)rn_hi * (mm_i32)rm_lo;
                                              } else {
                                                  prod1 = (mm_i32)rn_lo * (mm_i32)rm_lo;
                                                  prod2 = (mm_i32)rn_hi * (mm_i32)rm_hi;
                                              }
                                              sum = (mm_i64)prod1 + (mm_i64)prod2 + (mm_i64)(mm_i32)cpu.r[d.ra];
                                              cpu.r[d.rd] = (mm_u32)sum;
                                              if (sum != (mm_i64)(mm_i32)cpu.r[d.rd]) {
                                                  cpu.q_flag = MM_TRUE;
                                              }
                                          } break;
                        case MM_OP_SMLALD:
                        case MM_OP_SMLALDX: {
                                               mm_u32 rn_val = cpu.r[d.rn];
                                               mm_u32 rm_val = cpu.r[d.rm];
                                               mm_i16 rn_lo = (mm_i16)(rn_val & 0xffffu);
                                               mm_i16 rn_hi = (mm_i16)(rn_val >> 16);
                                               mm_i16 rm_lo = (mm_i16)(rm_val & 0xffffu);
                                               mm_i16 rm_hi = (mm_i16)(rm_val >> 16);
                                               mm_i32 prod1, prod2;
                                               mm_i64 accum, sum;
                                               if (d.kind == MM_OP_SMLALDX) {
                                                   prod1 = (mm_i32)rn_lo * (mm_i32)rm_hi;
                                                   prod2 = (mm_i32)rn_hi * (mm_i32)rm_lo;
                                               } else {
                                                   prod1 = (mm_i32)rn_lo * (mm_i32)rm_lo;
                                                   prod2 = (mm_i32)rn_hi * (mm_i32)rm_hi;
                                               }
                                               accum = ((mm_i64)cpu.r[d.ra] << 32) | (mm_i64)cpu.r[d.rd];
                                               sum = accum + (mm_i64)prod1 + (mm_i64)prod2;
                                               cpu.r[d.rd] = (mm_u32)(sum & 0xffffffffLL);
                                               cpu.r[d.ra] = (mm_u32)(sum >> 32);
                                           } break;
                        case MM_OP_PKHBT:
                        case MM_OP_PKHTB: {
                                             mm_u32 rn_val = cpu.r[d.rn];
                                             mm_u32 rm_val = cpu.r[d.rm];
                                             mm_u32 result;
                                             if (d.kind == MM_OP_PKHBT) {
                                                 mm_u32 shifted = rm_val << d.imm;
                                                 result = (rn_val & 0xffffu) | (shifted & 0xffff0000u);
                                             } else {
                                                 mm_u32 shifted;
                                                 if (d.imm == 0u) {
                                                     shifted = (mm_u32)((mm_i32)rm_val >> 31);
                                                 } else {
                                                     shifted = (mm_u32)((mm_i32)rm_val >> d.imm);
                                                 }
                                                 result = (rn_val & 0xffff0000u) | (shifted & 0xffffu);
                                             }
                                             cpu.r[d.rd] = result;
                                         } break;
                        case MM_OP_QADD:
                        case MM_OP_QSUB: {
                                            mm_u32 result;
                                            if (d.kind == MM_OP_QADD) {
                                                result = mm_qadd(cpu.r[d.rn], cpu.r[d.rm], &cpu.q_flag);
                                            } else {
                                                result = mm_qsub(cpu.r[d.rn], cpu.r[d.rm], &cpu.q_flag);
                                            }
                                            cpu.r[d.rd] = result;
                                        } break;
                        case MM_OP_QDADD:
                        case MM_OP_QDSUB: {
                                             mm_u32 result;
                                             if (d.kind == MM_OP_QDADD) {
                                                 result = mm_qdadd(cpu.r[d.rn], cpu.r[d.rm], &cpu.q_flag);
                                             } else {
                                                 result = mm_qdsub(cpu.r[d.rn], cpu.r[d.rm], &cpu.q_flag);
                                             }
                                             cpu.r[d.rd] = result;
                                         } break;
                        case MM_OP_SMLSD:
                        case MM_OP_SMLSDX: {
                                              mm_u32 rn_val = cpu.r[d.rn];
                                              mm_u32 rm_val = cpu.r[d.rm];
                                              mm_i16 rn_lo = (mm_i16)(rn_val & 0xffffu);
                                              mm_i16 rn_hi = (mm_i16)(rn_val >> 16);
                                              mm_i16 rm_lo = (mm_i16)(rm_val & 0xffffu);
                                              mm_i16 rm_hi = (mm_i16)(rm_val >> 16);
                                              mm_i32 prod1, prod2;
                                              mm_i64 diff;
                                              if (d.kind == MM_OP_SMLSDX) {
                                                  prod1 = (mm_i32)rn_lo * (mm_i32)rm_hi;
                                                  prod2 = (mm_i32)rn_hi * (mm_i32)rm_lo;
                                              } else {
                                                  prod1 = (mm_i32)rn_lo * (mm_i32)rm_lo;
                                                  prod2 = (mm_i32)rn_hi * (mm_i32)rm_hi;
                                              }
                                              diff = (mm_i64)prod1 - (mm_i64)prod2 + (mm_i64)(mm_i32)cpu.r[d.ra];
                                              cpu.r[d.rd] = (mm_u32)diff;
                                              if (diff != (mm_i64)(mm_i32)cpu.r[d.rd]) {
                                                  cpu.q_flag = MM_TRUE;
                                              }
                                          } break;
                        case MM_OP_SMULBB: {
                                              mm_u32 rn_val = cpu.r[d.rn];
                                              mm_u32 rm_val = cpu.r[d.rm];
                                              mm_u8 xy = (mm_u8)(d.imm & 0x3u);
                                              mm_i16 rn_half = (xy & 0x2u) ? (mm_i16)(rn_val >> 16) : (mm_i16)(rn_val & 0xffffu);
                                              mm_i16 rm_half = (xy & 0x1u) ? (mm_i16)(rm_val >> 16) : (mm_i16)(rm_val & 0xffffu);
                                              mm_i32 result = (mm_i32)rn_half * (mm_i32)rm_half;
                                              cpu.r[d.rd] = (mm_u32)result;
                                          } break;
                        case MM_OP_SSAT:
                        case MM_OP_USAT: {
                                            mm_u32 imm = d.imm;
                                            mm_u8 sat_to = (mm_u8)(imm & 0xffu);
                                            mm_u8 shift_imm = (mm_u8)((imm >> 8) & 0x1fu);
                                            mm_u8 shift_type = (mm_u8)((imm >> 16) & 0x3u);
                                            mm_i32 operand = (mm_i32)cpu.r[d.rn];
                                            mm_i64 shifted;
                                            mm_u32 result;
                                            if (shift_type == 0x0u) {
                                                shifted = (mm_i64)operand << shift_imm;
                                            } else if (shift_type == 0x2u) {
                                                if (shift_imm == 0u) {
                                                    shifted = operand >> 31;
                                                } else {
                                                    shifted = operand >> shift_imm;
                                                }
                                            } else {
                                                shifted = operand;
                                            }
                                            if (d.kind == MM_OP_SSAT) {
                                                mm_u32 n = sat_to + 1;
                                                result = (mm_u32)mm_sat_s32(shifted, n, &cpu.q_flag);
                                            } else {
                                                result = mm_sat_u32(shifted, sat_to, &cpu.q_flag);
                                            }
                                            cpu.r[d.rd] = result;
                                        } break;
                        case MM_OP_SMMUL:
                        case MM_OP_SMMLA:
                        case MM_OP_SMMLS:
                        case MM_OP_SMMLSR: {
                                              mm_i64 prod = (mm_i64)(mm_i32)cpu.r[d.rn] * (mm_i64)(mm_i32)cpu.r[d.rm];
                                              mm_i64 result;
                                              if (d.kind == MM_OP_SMMLA) {
                                                  result = prod + (mm_i64)((mm_u64)(mm_u32)cpu.r[d.ra] << 32);
                                              } else if (d.kind == MM_OP_SMMLS || d.kind == MM_OP_SMMLSR) {
                                                  result = (mm_i64)((mm_u64)(mm_u32)cpu.r[d.ra] << 32);
                                                  result = result - prod;
                                              } else {
                                                  result = prod;
                                              }
                                              if (d.imm != 0u || d.kind == MM_OP_SMMLSR) {
                                                  result += 0x80000000LL;
                                              }
                                              cpu.r[d.rd] = (mm_u32)(result >> 32);
                                          } break;
                        case MM_OP_SMLAWB:
                        case MM_OP_SMLAWT:
                        case MM_OP_SMULWB:
                        case MM_OP_SMULWT: {
                                              mm_i32 rn_val = (mm_i32)cpu.r[d.rn];
                                              mm_u32 rm_val = cpu.r[d.rm];
                                              mm_i16 rm_half;
                                              mm_i64 product;
                                              mm_i32 result;
                                              if (d.kind == MM_OP_SMLAWT || d.kind == MM_OP_SMULWT) {
                                                  rm_half = (mm_i16)(rm_val >> 16);
                                              } else {
                                                  rm_half = (mm_i16)rm_val;
                                              }
                                              product = (mm_i64)rn_val * (mm_i64)rm_half;
                                              result = (mm_i32)(product >> 16);
                                              if (d.kind == MM_OP_SMLAWB || d.kind == MM_OP_SMLAWT) {
                                                  mm_i32 ra_val = (mm_i32)cpu.r[d.ra];
                                                  mm_i64 sum = (mm_i64)result + (mm_i64)ra_val;
                                                  result = (mm_i32)sum;
                                                  if (sum != (mm_i64)result) {
                                                      cpu.q_flag = 1;
                                                  }
                                              }
                                              cpu.r[d.rd] = (mm_u32)result;
                                          } break;
                        case MM_OP_MUL_W: {
                                              mm_u32 res = cpu.r[d.rn] * cpu.r[d.rm];
                                              mm_bool setflags = ((d.imm & 1u) != 0u && it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                              cpu.r[d.rd] = res;
                                              if (setflags) {
                                                  mm_u32 xpsr = cpu.xpsr;
                                                  xpsr &= ~((1u << 31) | (1u << 30)); /* clear N,Z */
                                                  if (res == 0u) {
                                                      xpsr |= (1u << 30);
                                                  }
                                                  if ((res & 0x80000000u) != 0u) {
                                                      xpsr |= (1u << 31);
                                                  }
                                                  cpu.xpsr = xpsr;
                                              }
                                          } break;
                        case MM_OP_TBB:
                        case MM_OP_TBH: {
                                            mm_u32 target_pc = 0;
                                            mm_u32 fault_addr = 0;
                                            mm_bool is_tbh = (d.kind == MM_OP_TBH) ? MM_TRUE : MM_FALSE;
                                            mm_u32 rn_val;
                                            mm_u32 rm_val;
                                            const char *tb_trace = getenv("M33MU_TB_TRACE");

                                            if (d.rn == 15u) {
                                                rn_val = (f.pc_fetch + 4u) & ~1u;
                                            } else {
                                                rn_val = cpu.r[d.rn];
                                            }
                                            rm_val = cpu.r[d.rm];

                                            if (!mm_table_branch_target(&map, cpu.sec_state, f.pc_fetch, rn_val, rm_val, is_tbh, &target_pc, &fault_addr)) {
                                                if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, fault_addr, MM_FALSE)) done = MM_TRUE;
                                                return MM_EXEC_CONTINUE;
                                            }
                                            if (tb_trace != 0 && tb_trace[0] != '\0') {
                                                printf("[TB_TRACE] kind=%s pc_fetch=0x%08lx rn=%u rn_val=0x%08lx rm=%u rm_val=0x%08lx target=0x%08lx\n",
                                                       is_tbh ? "TBH" : "TBB",
                                                       (unsigned long)f.pc_fetch,
                                                       (unsigned)d.rn,
                                                       (unsigned long)rn_val,
                                                       (unsigned)d.rm,
                                                       (unsigned long)rm_val,
                                                       (unsigned long)target_pc);
                                            }
                                            if (!handle_pc_write(&cpu, &map, &scs, target_pc, &it_pattern, &it_remaining, &it_cond)) {
                                                done = MM_TRUE;
                                            }
                                            return MM_EXEC_CONTINUE;
                                        } break;
                        case MM_OP_UXTB: {
                                             mm_u32 val = cpu.r[d.rm];
                                             mm_u32 rot = d.imm & 0x1fu;
                                             mm_u32 ext;
                                             if (rot != 0u) {
                                                 val = (val >> rot) | (val << (32u - rot));
                                             }
                                             ext = val & 0xffu;
                                             if ((d.imm & 0x80000000u) != 0u && d.rn != 15u) {
                                                 cpu.r[d.rd] = cpu.r[d.rn] + ext;
                                             } else {
                                                 cpu.r[d.rd] = ext;
                                             }
                                         } break;
                        case MM_OP_SXTB: {
                                             mm_u32 val = cpu.r[d.rm];
                                             mm_u32 rot = d.imm & 0x1fu;
                                             mm_u32 ext;
                                             if (rot != 0u) {
                                                 val = (val >> rot) | (val << (32u - rot));
                                             }
                                             ext = (mm_u32)(mm_i32)(mm_i8)(val & 0xffu);
                                             if ((d.imm & 0x80000000u) != 0u && d.rn != 15u) {
                                                 cpu.r[d.rd] = cpu.r[d.rn] + ext;
                                             } else {
                                                 cpu.r[d.rd] = ext;
                                             }
                                         } break;
                        case MM_OP_SXTH: {
                                             mm_u32 val = cpu.r[d.rm];
                                             mm_u8 rot = (mm_u8)(d.imm & 0x1fu);
                                             mm_u32 ext;
                                             if (rot != 0u) {
                                                 val = (val >> rot) | (val << (32u - rot));
                                             }
                                             ext = (mm_u32)(mm_i32)(mm_i16)(val & 0xffffu);
                                             if ((d.imm & 0x80000000u) != 0u && d.rn != 15u) {
                                                 cpu.r[d.rd] = cpu.r[d.rn] + ext;
                                             } else {
                                                 cpu.r[d.rd] = ext;
                                             }
                                         } break;
                        case MM_OP_UXTH: {
                                             mm_u32 val = cpu.r[d.rm];
                                             mm_u8 rot = (mm_u8)(d.imm & 0x1fu);
                                             mm_u32 ext;
                                             if (rot != 0u) {
                                                 val = (val >> rot) | (val << (32u - rot));
                                             }
                                             ext = val & 0xffffu;
                                             if ((d.imm & 0x80000000u) != 0u && d.rn != 15u) {
                                                 cpu.r[d.rd] = cpu.r[d.rn] + ext;
                                             } else {
                                                 cpu.r[d.rd] = ext;
                                             }
                                         } break;
                        case MM_OP_MRS: {
                                            mm_u32 sysm = d.imm & 0xffu;
                                            mm_u32 val = 0;
                                            if (d.rd == 15u) {
                                                break;
                                            }
                                            switch (sysm) {
                                                case 0x00: /* APSR */
                                                    val = exec_compose_apsr(&cpu);
                                                    break;
                                                case 0x03: val = exec_compose_xpsr(&cpu); break; /* XPSR */
                                                case 0x05: val = cpu.xpsr & 0x1ffu; break; /* IPSR */
                                                case 0x08: val = mm_cpu_get_msp(&cpu, cpu.sec_state); break; /* MSP */
                                                case 0x09: val = (cpu.sec_state == MM_NONSECURE) ? cpu.psp_ns : cpu.psp_s; break; /* PSP */
                                                case 0x0a: val = (cpu.sec_state == MM_NONSECURE) ? cpu.msplim_ns : cpu.msplim_s; break; /* MSPLIM */
                                                case 0x0b: val = (cpu.sec_state == MM_NONSECURE) ? cpu.psplim_ns : cpu.psplim_s; break; /* PSPLIM */
                                                case 0x10: val = (cpu.sec_state == MM_NONSECURE) ? cpu.primask_ns : cpu.primask_s; break; /* PRIMASK */
                                                case 0x11: val = (cpu.sec_state == MM_NONSECURE) ? cpu.basepri_ns : cpu.basepri_s; break; /* BASEPRI */
                                                case 0x12: val = (cpu.sec_state == MM_NONSECURE) ? cpu.faultmask_ns : cpu.faultmask_s; break; /* FAULTMASK */
                                                case 0x88: val = cpu.msp_ns; break;
                                                case 0x89: val = cpu.psp_ns; break;
                                                case 0x8a: val = cpu.msplim_ns; break;
                                                case 0x8b: val = cpu.psplim_ns; break;
                                                case 0x90: val = cpu.primask_ns; break;
                                                case 0x91: val = cpu.basepri_ns; break;
                                                case 0x92: val = cpu.faultmask_ns; break;
                                                case 0x94: val = cpu.control_ns; break;
                                                case 0x14: val = (cpu.sec_state == MM_NONSECURE) ? cpu.control_ns : cpu.control_s; break;
                                                default: val = 0; break;
                                            }
                                            cpu.r[d.rd] = val;
                                        } break;
                        case MM_OP_MSR: {
                                            mm_u32 sysm = d.imm & 0xffu;
                                            mm_u32 mask = (d.imm >> 8) & 0xfu;
                                            mm_u32 val = cpu.r[d.rm];
                                            mm_bool unpriv = !mm_cpu_get_privileged(&cpu);
                                            switch (sysm) {
                                                case 0x00: /* APSR */
                                                    /* Only NZCVQ and GE flags can be written via APSR */
                                                    if (mask & 0x8) { /* _nzcvq */
                                                        cpu.xpsr = mm_xpsr_write_nzcvq(cpu.xpsr, val);
                                                        cpu.q_flag = (val & (1u << 27)) ? MM_TRUE : MM_FALSE;
                                                    }
                                                    if (mask & 0x4) { /* _g */
                                                        cpu.ge_flags = (mm_u8)((val >> 16) & 0xfu);
                                                        cpu.xpsr = (cpu.xpsr & ~((mm_u32)0xFu << 16)) |
                                                                   ((((mm_u32)cpu.ge_flags) & 0xFu) << 16);
                                                    }
                                                    break;
                                                case 0x08: /* MSP */
                                                    if (cpu.sec_state == MM_NONSECURE && cpu.mode == MM_THREAD && unpriv) {
                                                        break;
                                                    }
                                                    mm_cpu_set_msp(&cpu, cpu.sec_state, val);
                                                    if (msp_trace_enabled()) {
                                                        printf("[MSP] MSP %s=0x%08lx pc=0x%08lx mode=%d\n",
                                                               (cpu.sec_state == MM_NONSECURE) ? "NS" : "S",
                                                               (unsigned long)val,
                                                               (unsigned long)f.pc_fetch,
                                                               (int)cpu.mode);
                                                    }
                                                    break;
                                                case 0x09: /* PSP */
                                                    mm_cpu_set_psp(&cpu, cpu.sec_state, val);
                                                    if (psp_trace_enabled()) {
                                                        printf("[PSP] PSP %s=0x%08lx pc=0x%08lx mode=%d\n",
                                                               (cpu.sec_state == MM_NONSECURE) ? "NS" : "S",
                                                               (unsigned long)val,
                                                               (unsigned long)f.pc_fetch,
                                                               (int)cpu.mode);
                                                    }
                                                    break;
                                                case 0x0a: /* MSPLIM */
                                                    if (cpu.sec_state == MM_NONSECURE && cpu.mode == MM_THREAD && unpriv) {
                                                        break;
                                                    }
                                                    if (cpu.sec_state == MM_NONSECURE) {
                                                        cpu.msplim_ns = val & ~0x7u;
                                                    } else {
                                                        cpu.msplim_s = val & ~0x7u;
                                                    }
                                                    if (splim_trace_enabled()) {
                                                        printf("[SPLIM] MSPLIM %s=0x%08lx\n",
                                                               (cpu.sec_state == MM_NONSECURE) ? "NS" : "S",
                                                               (unsigned long)(val & ~0x7u));
                                                    }
                                                    break;
                                                case 0x0b: /* PSPLIM */
                                                    if (cpu.sec_state == MM_NONSECURE && cpu.mode == MM_THREAD && unpriv) {
                                                        break;
                                                    }
                                                    if (cpu.sec_state == MM_NONSECURE) {
                                                        cpu.psplim_ns = val & ~0x7u;
                                                    } else {
                                                        cpu.psplim_s = val & ~0x7u;
                                                    }
                                                    if (splim_trace_enabled()) {
                                                        printf("[SPLIM] PSPLIM %s=0x%08lx pc=0x%08lx mode=%d\n",
                                                               (cpu.sec_state == MM_NONSECURE) ? "NS" : "S",
                                                               (unsigned long)(val & ~0x7u),
                                                               (unsigned long)f.pc_fetch,
                                                               (int)cpu.mode);
                                                    }
                                                    break;
                                                case 0x88: /* MSP_NS */
                                                    if (cpu.sec_state == MM_NONSECURE && cpu.mode == MM_THREAD && unpriv) {
                                                        break;
                                                    }
                                                    mm_cpu_set_msp(&cpu, MM_NONSECURE, val);
                                                    if (msp_trace_enabled()) {
                                                        printf("[MSP] MSP NS=0x%08lx pc=0x%08lx mode=%d\n",
                                                               (unsigned long)val,
                                                               (unsigned long)f.pc_fetch,
                                                               (int)cpu.mode);
                                                    }
                                                    break;
                                                case 0x89: /* PSP_NS */
                                                    mm_cpu_set_psp(&cpu, MM_NONSECURE, val);
                                                    if (psp_trace_enabled()) {
                                                        printf("[PSP] PSP NS=0x%08lx pc=0x%08lx mode=%d\n",
                                                               (unsigned long)val,
                                                               (unsigned long)f.pc_fetch,
                                                               (int)cpu.mode);
                                                    }
                                                    break;
                                                case 0x8a: /* MSPLIM_NS */
                                                    if (cpu.sec_state == MM_NONSECURE && cpu.mode == MM_THREAD && unpriv) {
                                                        break;
                                                    }
                                                    cpu.msplim_ns = val & ~0x7u;
                                                    if (splim_trace_enabled()) {
                                                        printf("[SPLIM] MSPLIM NS=0x%08lx\n", (unsigned long)(val & ~0x7u));
                                                    }
                                                    break;
                                                case 0x8b: /* PSPLIM_NS */
                                                    if (cpu.sec_state == MM_NONSECURE && cpu.mode == MM_THREAD && unpriv) {
                                                        break;
                                                    }
                                                    cpu.psplim_ns = val & ~0x7u;
                                                    if (splim_trace_enabled()) {
                                                        printf("[SPLIM] PSPLIM NS=0x%08lx pc=0x%08lx mode=%d\n",
                                                               (unsigned long)(val & ~0x7u),
                                                               (unsigned long)f.pc_fetch,
                                                               (int)cpu.mode);
                                                    }
                                                    break;
                                                case 0x10: /* PRIMASK */
                                                    if (cpu.sec_state == MM_NONSECURE) cpu.primask_ns = val & 1u;
                                                    else cpu.primask_s = val & 1u;
                                                    break;
                                                case 0x11: /* BASEPRI */
                                                    if (cpu.sec_state == MM_NONSECURE) cpu.basepri_ns = val & 0xffu;
                                                    else cpu.basepri_s = val & 0xffu;
                                                    break;
                                                case 0x12: /* FAULTMASK */
                                                    if (cpu.sec_state == MM_NONSECURE) cpu.faultmask_ns = val & 1u;
                                                    else cpu.faultmask_s = val & 1u;
                                                    break;
                                                case 0x90: /* PRIMASK_NS */
                                                    cpu.primask_ns = val & 1u;
                                                    break;
                                                case 0x91: /* BASEPRI_NS */
                                                    cpu.basepri_ns = val & 0xffu;
                                                    break;
                                                case 0x92: /* FAULTMASK_NS */
                                                    cpu.faultmask_ns = val & 1u;
                                                    break;
                                                case 0x14: /* CONTROL */
                                                    if (cpu.sec_state == MM_NONSECURE) {
                                                        mm_cpu_set_control(&cpu, MM_NONSECURE, val);
                                                    } else {
                                                        mm_cpu_set_control(&cpu, MM_SECURE, val);
                                                    }
                                                    if (ctrl_trace_enabled()) {
                                                        printf("[CONTROL] CONTROL %s=0x%08lx pc=0x%08lx mode=%d\n",
                                                               (cpu.sec_state == MM_NONSECURE) ? "NS" : "S",
                                                               (unsigned long)val,
                                                               (unsigned long)f.pc_fetch,
                                                               (int)cpu.mode);
                                                    }
                                                    break;
                                                case 0x94: /* CONTROL_NS */
                                                    mm_cpu_set_control(&cpu, MM_NONSECURE, val);
                                                    if (ctrl_trace_enabled()) {
                                                        printf("[CONTROL] CONTROL NS=0x%08lx pc=0x%08lx mode=%d\n",
                                                               (unsigned long)val,
                                                               (unsigned long)f.pc_fetch,
                                                               (int)cpu.mode);
                                                    }
                                                    break;
                                                default:
                                                    break;
                                            }
                                            /* MSR does not affect PC; fall through with normal PC increment. */
                                        } break;
                        case MM_OP_MVN_IMM: {
                                                mm_bool setflags = MM_FALSE;
                                                mm_bool carry_in = (cpu.xpsr & (1u << 29)) != 0u;
                                                mm_bool carry_out = carry_in;
                                                mm_u32 imm32 = 0;
                                                mm_u32 res;
                                                if ((d.raw & (1u << 20)) != 0u) {
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                }
                                                mm_thumb_expand_imm12_c(d.imm, carry_in, &imm32, &carry_out);
                                                res = ~imm32;
                                                cpu.r[d.rd] = res;
                                                if (setflags) {
                                                    /* Update N,Z,C; V unchanged. */
                                                    cpu.xpsr &= ~(0xE0000000u);
                                                    if (res == 0u) cpu.xpsr |= (1u << 30);
                                                    if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                    if (carry_out) cpu.xpsr |= (1u << 29);
                                                }
                                            } break;
                        case MM_OP_MVN_REG: {
                                                if (d.len == 2u) {
                                                    /* Thumb-1 MVNS aliases inside IT must not clobber flags. */
                                                    mm_bool setflags = (it_remaining == 0u) ? MM_TRUE : MM_FALSE;
                                                    mm_u32 rm_val = cpu.r[d.rm];
                                                    cpu.r[d.rd] = mm_mvn_reg(rm_val, &cpu.xpsr, setflags);
                                                } else {
                                                    mm_bool setflags = ((((d.raw >> 20) & 1u) != 0u) && it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                    mm_u8 rd = (mm_u8)((d.raw >> 8) & 0x0fu);
                                                    mm_u8 rm = (mm_u8)(d.raw & 0x0fu);
                                                    mm_u8 imm3 = (mm_u8)((d.raw >> 12) & 0x7u);
                                                    mm_u8 imm2 = (mm_u8)((d.raw >> 6) & 0x3u);
                                                    mm_u8 type = (mm_u8)((d.raw >> 4) & 0x3u);
                                                    mm_u8 imm5 = (mm_u8)((imm3 << 2) | imm2);
                                                    mm_bool carry_in = (cpu.xpsr & (1u << 29)) != 0u;
                                                    mm_bool carry_out = carry_in;
                                                    mm_u32 shifted = mm_shift_c_imm(cpu.r[rm], type, imm5, carry_in, &carry_out);
                                                    mm_u32 res = ~shifted;
                                                    cpu.r[rd] = res;
                                                    if (setflags) {
                                                        cpu.xpsr &= ~(0xE0000000u);
                                                        if (res == 0u) cpu.xpsr |= (1u << 30);
                                                        if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                        if (carry_out) cpu.xpsr |= (1u << 29);
                                                    }
                                                }
                                            } break;
                        case MM_OP_CPS: {
                                            mm_bool disable = (d.imm & 0x10u) != 0u;
                                            mm_bool affect_f = (d.imm & 0x01u) != 0u;
                                            mm_bool affect_i = (d.imm & 0x02u) != 0u;
                                            if (cpu.mode == MM_THREAD && !mm_cpu_get_privileged(&cpu)) {
                                                break;
                                            }
                                            if (affect_i) {
                                                if (cpu.sec_state == MM_NONSECURE) {
                                                    cpu.primask_ns = disable ? 1u : 0u;
                                                } else {
                                                    cpu.primask_s = disable ? 1u : 0u;
                                                }
                                            }
                                            if (affect_f) {
                                                if (cpu.sec_state == MM_NONSECURE) {
                                                    cpu.faultmask_ns = disable ? 1u : 0u;
                                                } else {
                                                    cpu.faultmask_s = disable ? 1u : 0u;
                                                }
                                            }
                                        } break;
                        case MM_OP_SUB_IMM:
                                        {
                                            mm_u32 lhs = (d.rn == 15u) ? mm_pc_operand(&f) : cpu.r[d.rn];
                                            mm_bool setflags = MM_FALSE;
                                            if (d.len == 2u) {
                                                /* Thumb-1 SUBS aliases inside IT must not clobber flags. */
                                                setflags = (it_remaining == 0u) ? MM_TRUE : MM_FALSE;
                                            } else if (((d.raw >> 20) & 1u) != 0u) {
                                                setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                            }
                                            if (setflags) {
                                                mm_u32 res;
                                                mm_bool cflag;
                                                mm_bool vflag;
                                                /* Thumb SUB (immediate) updates flags (SUBS). */
                                                mm_add_with_carry(lhs, ~d.imm, MM_TRUE, &res, &cflag, &vflag);
                                                if (d.rd == 15u) {
                                                    if (!handle_pc_write(&cpu, &map, &scs, res, &it_pattern, &it_remaining, &it_cond)) {
                                                        done = MM_TRUE;
                                                    }
                                                } else {
                                                    cpu.r[d.rd] = res;
                                                    if (d.rd == 13u) {
                                                        EXEC_SET_SP(cpu.r[13]);
                                                    }
                                                }
                                                cpu.xpsr &= ~(0xF0000000u);
                                                if (res == 0u) cpu.xpsr |= (1u << 30);
                                                if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                if (cflag) cpu.xpsr |= (1u << 29);
                                                if (vflag) cpu.xpsr |= (1u << 28);
                                            } else {
                                                mm_u32 res = lhs - d.imm;
                                                if (d.rd == 15u) {
                                                    if (!handle_pc_write(&cpu, &map, &scs, res, &it_pattern, &it_remaining, &it_cond)) {
                                                        done = MM_TRUE;
                                                    }
                                                } else {
                                                    cpu.r[d.rd] = res;
                                                    if (d.rd == 13u) {
                                                        EXEC_SET_SP(cpu.r[13]);
                                                    }
                                                }
                                            }
                                        }
                                        break;
                        case MM_OP_SUB_IMM_NF:
                                        {
                                            mm_u32 lhs = (d.rn == 15u) ? mm_pc_operand(&f) : cpu.r[d.rn];
                                            mm_u32 res = lhs - d.imm;
                                            if (d.rd == 15u) {
                                                if (!handle_pc_write(&cpu, &map, &scs, res, &it_pattern, &it_remaining, &it_cond)) {
                                                    done = MM_TRUE;
                                                }
                                            } else {
                                                cpu.r[d.rd] = res;
                                                if (d.rd == 13u) {
                                                    EXEC_SET_SP(cpu.r[13]);
                                                }
                                            }
                                        }
                                        break;
                        case MM_OP_SUB_REG:
                                        if ((d.raw & 0xfe000000u) == 0xea000000u) {
                                            mm_u32 rhs = shift_reg_operand(cpu.r[d.rm], d.imm, cpu.xpsr, NULL);
                                            mm_u32 res;
                                            if ((d.raw & (1u << 20)) != 0u) {
                                                mm_bool setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                mm_bool cflag;
                                                mm_bool vflag;
                                                mm_add_with_carry(cpu.r[d.rn], ~rhs, MM_TRUE, &res, &cflag, &vflag);
                                                if (setflags) {
                                                    cpu.xpsr &= ~(0xF0000000u);
                                                    if (res == 0u) cpu.xpsr |= (1u << 30);
                                                    if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                    if (cflag) cpu.xpsr |= (1u << 29);
                                                    if (vflag) cpu.xpsr |= (1u << 28);
                                                }
                                            } else {
                                                res = cpu.r[d.rn] - rhs;
                                            }
                                            if (d.rd == 15u) {
                                                if (!handle_pc_write(&cpu, &map, &scs, res, &it_pattern, &it_remaining, &it_cond)) {
                                                    done = MM_TRUE;
                                                }
                                            } else {
                                                cpu.r[d.rd] = res;
                                                if (d.rd == 13u) {
                                                    EXEC_SET_SP(cpu.r[13]);
                                                }
                                            }
                                        } else {
                                            mm_bool setflags = (d.len == 2u) ? ((it_remaining == 0u) ? MM_TRUE : MM_FALSE) : MM_FALSE;
                                            mm_u32 res;
                                            if (setflags) {
                                                mm_bool cflag;
                                                mm_bool vflag;
                                                /* SUBS Rd,Rn,Rm (Thumb-1) updates flags. */
                                                mm_add_with_carry(cpu.r[d.rn], ~cpu.r[d.rm], MM_TRUE, &res, &cflag, &vflag);
                                                cpu.xpsr &= ~(0xF0000000u);
                                                if (res == 0u) cpu.xpsr |= (1u << 30);
                                                if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                if (cflag) cpu.xpsr |= (1u << 29);
                                                if (vflag) cpu.xpsr |= (1u << 28);
                                            } else {
                                                res = cpu.r[d.rn] - cpu.r[d.rm];
                                            }
                                            if (d.rd == 15u) {
                                                if (!handle_pc_write(&cpu, &map, &scs, res, &it_pattern, &it_remaining, &it_cond)) {
                                                    done = MM_TRUE;
                                                }
                                            } else {
                                                cpu.r[d.rd] = res;
                                                if (d.rd == 13u) {
                                                    EXEC_SET_SP(cpu.r[13]);
                                                }
                                            }
                                        }
                                        break;
                        case MM_OP_RSB_REG: {
                                               mm_u32 rhs = shift_reg_operand(cpu.r[d.rm], d.imm, cpu.xpsr, NULL);
                                               if ((d.raw & (1u << 20)) != 0u) {
                                                   mm_bool setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                   mm_u32 res;
                                                   mm_bool cflag;
                                                   mm_bool vflag;
                                                   mm_add_with_carry(rhs, ~cpu.r[d.rn], MM_TRUE, &res, &cflag, &vflag);
                                                   cpu.r[d.rd] = res;
                                                   if (setflags) {
                                                       cpu.xpsr &= ~(0xF0000000u);
                                                       if (res == 0u) cpu.xpsr |= (1u << 30);
                                                       if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                       if (cflag) cpu.xpsr |= (1u << 29);
                                                       if (vflag) cpu.xpsr |= (1u << 28);
                                                   }
                                               } else {
                                                   cpu.r[d.rd] = rhs - cpu.r[d.rn];
                                               }
                                           } break;
                        case MM_OP_SUB_SP_IMM:
                                        if (d.rd == 13u) {
                                            mm_u32 sp_before = cpu.r[13];
                                            if (svc_stack_trace_enabled()) {
                                                printf("[SUB_SP_IMM] pc=0x%08lx sp_before=0x%08lx imm=0x%08lx\n",
                                                       (unsigned long)f.pc_fetch,
                                                       (unsigned long)sp_before,
                                                       (unsigned long)d.imm);
                                            }
                                            EXEC_SET_SP(sp_before - d.imm);
                                            if (svc_stack_trace_enabled()) {
                                                printf("[SUB_SP_IMM] pc=0x%08lx sp_after=0x%08lx\n",
                                                       (unsigned long)f.pc_fetch,
                                                       (unsigned long)mm_cpu_get_active_sp(&cpu));
                                            }
                                            if (svc_stack_trace_enabled() &&
                                                f.pc_fetch >= 0x08013518u &&
                                                f.pc_fetch <= 0x0801351eu) {
                                                printf("[SUB_SP_IMM_PRO] pc=0x%08lx sp_before=0x%08lx imm=0x%08lx sp_after=0x%08lx\n",
                                                       (unsigned long)f.pc_fetch,
                                                       (unsigned long)sp_before,
                                                       (unsigned long)d.imm,
                                                       (unsigned long)mm_cpu_get_active_sp(&cpu));
                                            }
                                        } else {
                                            cpu.r[d.rd] = cpu.r[13] - d.imm;
                                        }
                                        break;
                        case MM_OP_MOV_REG:
                                        if (d.rd == 15u) {
                                            if (!handle_pc_write(&cpu, &map, &scs, cpu.r[d.rm], &it_pattern, &it_remaining, &it_cond)) {
                                                done = MM_TRUE;
                                            }
                                        } else if (d.rd == 13u) {
                                            if (svc_stack_trace_enabled() && d.rm == 7u) {
                                                printf("[MOV_SP_R7] pc=0x%08lx sp_before=0x%08lx r7=0x%08lx\n",
                                                       (unsigned long)f.pc_fetch,
                                                       (unsigned long)mm_cpu_get_active_sp(&cpu),
                                                       (unsigned long)cpu.r[7]);
                                            }
                                            EXEC_SET_SP(cpu.r[d.rm]);
                                            if (svc_stack_trace_enabled() && d.rm == 7u) {
                                                printf("[MOV_SP_R7] pc=0x%08lx sp_after=0x%08lx r7=0x%08lx\n",
                                                       (unsigned long)f.pc_fetch,
                                                       (unsigned long)mm_cpu_get_active_sp(&cpu),
                                                       (unsigned long)cpu.r[7]);
                                            }
                                        } else {
                                            cpu.r[d.rd] = cpu.r[d.rm];
                                        }
                                        break;
                        case MM_OP_ADR:
                                        cpu.r[d.rd] = mm_adr_value(&f, d.imm);
                                        break;
                        case MM_OP_CMP_IMM: {
                                                mm_u32 res;
                                                mm_bool cflag;
                                                mm_bool vflag;
                                                mm_add_with_carry(cpu.r[d.rn], ~d.imm, MM_TRUE, &res, &cflag, &vflag);
                                                cpu.xpsr &= ~(0xF0000000u);
                                                if (res == 0u) cpu.xpsr |= (1u << 30);
                                                if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                if (cflag) cpu.xpsr |= (1u << 29);
                                                if (vflag) cpu.xpsr |= (1u << 28);
                                            } break;
                        case MM_OP_CMN_IMM: {
                                                mm_u32 res;
                                                mm_bool cflag;
                                                mm_bool vflag;
                                                mm_add_with_carry(cpu.r[d.rn], d.imm, MM_FALSE, &res, &cflag, &vflag);
                                                cpu.xpsr &= ~(0xF0000000u);
                                                if (res == 0u) cpu.xpsr |= (1u << 30);
                                                if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                if (cflag) cpu.xpsr |= (1u << 29);
                                                if (vflag) cpu.xpsr |= (1u << 28);
                                            } break;
                        case MM_OP_SBC_IMM:
                        case MM_OP_SBC_IMM_NF: {
                                                mm_u32 res;
                                                mm_bool cflag;
                                                mm_bool vflag;
                                                mm_bool carry_in = (cpu.xpsr & (1u << 29)) != 0u;
                                                mm_add_with_carry(cpu.r[d.rn], ~d.imm, carry_in, &res, &cflag, &vflag);
                                                cpu.r[d.rd] = res;
                                                if (d.kind == MM_OP_SBC_IMM) {
                                                    cpu.xpsr &= ~(0xF0000000u);
                                                    if (res == 0u) cpu.xpsr |= (1u << 30);
                                                    if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                    if (cflag) cpu.xpsr |= (1u << 29);
                                                    if (vflag) cpu.xpsr |= (1u << 28);
                                                }
                                            } break;
                        case MM_OP_CMP_REG: {
                                                mm_u32 res;
                                                mm_bool cflag;
                                                mm_bool vflag;
                                                mm_bool reg_form = ((d.raw & 0xfe000000u) == 0xea000000u);
                                                mm_u32 rhs = cpu.r[d.rm];
                                                if (reg_form) {
                                                    rhs = shift_reg_operand(cpu.r[d.rm], d.imm, cpu.xpsr, NULL);
                                                }
                                                mm_add_with_carry(cpu.r[d.rn], ~rhs, MM_TRUE, &res, &cflag, &vflag);
                                                cpu.xpsr &= ~(0xF0000000u);
                                                if (res == 0u) cpu.xpsr |= (1u << 30);
                                                if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                if (cflag) cpu.xpsr |= (1u << 29);
                                                if (vflag) cpu.xpsr |= (1u << 28);
                                            } break;
                        case MM_OP_CMN_REG: {
                                                mm_u32 res;
                                                mm_bool cflag;
                                                mm_bool vflag;
                                                mm_bool reg_form = ((d.raw & 0xfe000000u) == 0xea000000u);
                                                mm_u32 rhs = cpu.r[d.rm];
                                                if (reg_form) {
                                                    rhs = shift_reg_operand(cpu.r[d.rm], d.imm, cpu.xpsr, NULL);
                                                }
                                                mm_add_with_carry(cpu.r[d.rn], rhs, MM_FALSE, &res, &cflag, &vflag);
                                                cpu.xpsr &= ~(0xF0000000u);
                                                if (res == 0u) cpu.xpsr |= (1u << 30);
                                                if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                if (cflag) cpu.xpsr |= (1u << 29);
                                                if (vflag) cpu.xpsr |= (1u << 28);
                                            } break;
                        case MM_OP_BKPT: {
                                            mm_u32 ret_pc = f.pc_fetch + 2u;
                                            if (ctx->bkpt_hit != 0) {
                                                *ctx->bkpt_hit = MM_TRUE;
                                            }
                                            if (ctx->bkpt_imm != 0) {
                                                *ctx->bkpt_imm = d.imm;
                                            }
                                            if (ctx->opt_expect_bkpt) {
                                                done = MM_TRUE;
                                                break;
                                            }
                                            scs.dfsr |= (1u << 1); /* BKPT */
                                            if (opt_gdb) {
                                                mm_gdb_stub_notify_stop(&gdb, 5);
                                            } else if (enter_exception == 0 ||
                                                       !enter_exception(&cpu, &map, &scs, MM_VECT_DEBUGMON, ret_pc, cpu.xpsr)) {
                                                done = MM_TRUE;
                                            } else {
                                                itstate_sync_from_xpsr(cpu.xpsr, &it_pattern, &it_remaining, &it_cond);
                                            }
                                        } break;
                        case MM_OP_LDR_LITERAL: {
                                                    static int ldr_lit_trace = -1;
                                                    mm_u32 val = 0;
                                                    mm_u32 addr = ((f.pc_fetch + 4u) & ~3u) + d.imm;
                                                    if (!mm_memmap_read(&map, cpu.sec_state, addr, 4u, &val)) {
                                                        if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_TRUE)) done = MM_TRUE;
                                                        return MM_EXEC_CONTINUE;
                                                    }
                                                    if (ldr_lit_trace < 0) {
                                                        ldr_lit_trace = (getenv("M33MU_LDR_LITERAL_TRACE") != 0) ? 1 : 0;
                                                    }
                                                    if (ldr_lit_trace) {
                                                        printf("[LDR_LITERAL] pc=0x%08lx imm=0x%08lx addr=0x%08lx val=0x%08lx\n",
                                                               (unsigned long)f.pc_fetch,
                                                               (unsigned long)d.imm,
                                                               (unsigned long)addr,
                                                               (unsigned long)val);
                                                    }
                                                    if (d.rd == 15u) {
                                                        if (!handle_pc_write(&cpu, &map, &scs, val, &it_pattern, &it_remaining, &it_cond)) {
                                                            done = MM_TRUE;
                                                        }
                                                        return MM_EXEC_CONTINUE;
                                                    }
                                                    cpu.r[d.rd] = val;
                                                } break;
                        case MM_OP_LDR_IMM:
                        case MM_OP_LDRT: {
                                                mm_u32 val = 0;
                                                mm_u32 addr = cpu.r[d.rn] + d.imm;
                                                if (!mm_memmap_read(&map, cpu.sec_state, addr, 4u, &val)) {
                                                    if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                    return MM_EXEC_CONTINUE;
                                                }
                                                if (d.rd == 15u) {
                                                    if (!handle_pc_write(&cpu, &map, &scs, val, &it_pattern, &it_remaining, &it_cond)) {
                                                        done = MM_TRUE;
                                                    }
                                                    return MM_EXEC_CONTINUE;
                                                }
                                                cpu.r[d.rd] = val;
                                            } break;
                        case MM_OP_LDR_REG: {
                                                mm_u32 val = 0;
                                                mm_u32 addr = cpu.r[d.rn] + (cpu.r[d.rm] << (d.imm & 0x3u));
                                                if (!mm_memmap_read(&map, cpu.sec_state, addr, 4u, &val)) {
                                                    if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                    return MM_EXEC_CONTINUE;
                                                }
                                                if (d.rd == 15u) {
                                                    if (!handle_pc_write(&cpu, &map, &scs, val, &it_pattern, &it_remaining, &it_cond)) {
                                                        done = MM_TRUE;
                                                    }
                                                    return MM_EXEC_CONTINUE;
                                                }
                                                cpu.r[d.rd] = val;
                                            } break;
                        case MM_OP_LDREX: {
                                              mm_u32 val = 0;
                                              mm_u32 addr = cpu.r[d.rn] + d.imm;
                                              if ((addr & 0x3u) != 0u) {
                                                  if (!raise_usage_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, UFSR_UNALIGNED)) done = MM_TRUE;
                                                  return MM_EXEC_CONTINUE;
                                              }
                                              if (!mm_memmap_read(&map, cpu.sec_state, addr, 4u, &val)) {
                                                  if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                  return MM_EXEC_CONTINUE;
                                              }
                                              if (d.rd != 15u) {
                                                  cpu.r[d.rd] = val;
                                              }
                                              mm_cpu_excl_set(&cpu, cpu.sec_state, addr, 4u);
                                          } break;
                        case MM_OP_LDREXB: {
                                              mm_u8 val = 0;
                                              mm_u32 addr = cpu.r[d.rn];
                                              if (!mm_memmap_read8(&map, cpu.sec_state, addr, &val)) {
                                                  if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                  return MM_EXEC_CONTINUE;
                                              }
                                              if (d.rd != 15u) {
                                                  cpu.r[d.rd] = (mm_u32)val;
                                              }
                                              mm_cpu_excl_set(&cpu, cpu.sec_state, addr, 1u);
                                          } break;
                        case MM_OP_LDREXH: {
                                              mm_u32 val = 0;
                                              mm_u32 addr = cpu.r[d.rn];
                                              if ((addr & 0x1u) != 0u) {
                                                  if (!raise_usage_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, UFSR_UNALIGNED)) done = MM_TRUE;
                                                  return MM_EXEC_CONTINUE;
                                              }
                                              if (!mm_memmap_read(&map, cpu.sec_state, addr, 2u, &val)) {
                                                  if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                  return MM_EXEC_CONTINUE;
                                              }
                                              if (d.rd != 15u) {
                                                  cpu.r[d.rd] = val & 0xffffu;
                                              }
                                              mm_cpu_excl_set(&cpu, cpu.sec_state, addr, 2u);
                                          } break;
                        case MM_OP_CLREX: {
                                              mm_cpu_excl_clear(&cpu);
                                          } break;
                        case MM_OP_STREX: {
                                              mm_u32 addr = cpu.r[d.rn] + d.imm;
                                              mm_bool ok;
                                              if ((addr & 0x3u) != 0u) {
                                                  if (!raise_usage_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, UFSR_UNALIGNED)) done = MM_TRUE;
                                                  return MM_EXEC_CONTINUE;
                                              }
                                              ok = mm_cpu_excl_check_and_clear(&cpu, cpu.sec_state, addr, 4u);
                                              if (ok) {
                                                  if (!mm_memmap_write(&map, cpu.sec_state, addr, 4u, cpu.r[d.rm])) {
                                                      if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                      return MM_EXEC_CONTINUE;
                                                  }
                                              }
                                              if (d.rd != 15u) {
                                                  cpu.r[d.rd] = ok ? 0u : 1u;
                                              }
                                          } break;
                        case MM_OP_STREXB: {
                                              mm_u32 addr = cpu.r[d.rn];
                                              mm_bool ok = mm_cpu_excl_check_and_clear(&cpu, cpu.sec_state, addr, 1u);
                                              if (ok) {
                                                  if (!mm_memmap_write8(&map, cpu.sec_state, addr, (mm_u8)(cpu.r[d.rm] & 0xffu))) {
                                                      if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                      return MM_EXEC_CONTINUE;
                                                  }
                                              }
                                              if (d.rd != 15u) {
                                                  cpu.r[d.rd] = ok ? 0u : 1u;
                                              }
                                          } break;
                        case MM_OP_STREXH: {
                                              mm_u32 addr = cpu.r[d.rn];
                                              mm_bool ok;
                                              if ((addr & 0x1u) != 0u) {
                                                  if (!raise_usage_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, UFSR_UNALIGNED)) done = MM_TRUE;
                                                  return MM_EXEC_CONTINUE;
                                              }
                                              ok = mm_cpu_excl_check_and_clear(&cpu, cpu.sec_state, addr, 2u);
                                              if (ok) {
                                                  if (!mm_memmap_write(&map, cpu.sec_state, addr, 2u, cpu.r[d.rm] & 0xffffu)) {
                                                      if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                      return MM_EXEC_CONTINUE;
                                                  }
                                              }
                                              if (d.rd != 15u) {
                                                  cpu.r[d.rd] = ok ? 0u : 1u;
                                              }
                                          } break;
                        case MM_OP_STR_IMM:
                        case MM_OP_STRT: {
                                                mm_u32 base = cpu.r[d.rn];
                                                mm_u32 addr = base + d.imm;
                                                if (!mm_memmap_write(&map, cpu.sec_state, addr, 4u, cpu.r[d.rd])) {
                                                    if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                    return MM_EXEC_CONTINUE;
                                                }
                                            } break;
                        case MM_OP_STR_REG: {
                                                mm_u32 addr = cpu.r[d.rn] + (cpu.r[d.rm] << (d.imm & 0x3u));
                                                if (!mm_memmap_write(&map, cpu.sec_state, addr, 4u, cpu.r[d.rd])) {
                                                    if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                    return MM_EXEC_CONTINUE;
                                                }
                                            } break;
                        case MM_OP_LDR_POST_IMM: {
                                                     mm_u32 val = 0;
                                                     mm_u32 addr = cpu.r[d.rn];
                                                     mm_u32 new_rn;
                                                     if (!mm_memmap_read(&map, cpu.sec_state, addr, 4u, &val)) {
                                                         if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                         return MM_EXEC_CONTINUE;
                                                     }
                                                     new_rn = addr + d.imm;
                                                     if (d.rd == 15u) {
                                                         if (!handle_pc_write(&cpu, &map, &scs, val, &it_pattern, &it_remaining, &it_cond)) {
                                                             done = MM_TRUE;
                                                         }
                                                     } else {
                                                         cpu.r[d.rd] = val;
                                                     }
                                                     if (d.rn == 13u) {
                                                         EXEC_SET_SP(new_rn);
                                                     } else {
                                                         cpu.r[d.rn] = new_rn;
                                                     }
                                                 } break;
                        case MM_OP_LDR_PRE_IMM: {
                                                     mm_u32 val = 0;
                                                     mm_u32 addr = cpu.r[d.rn] + d.imm;
                                                     if (!mm_memmap_read(&map, cpu.sec_state, addr, 4u, &val)) {
                                                         if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                         return MM_EXEC_CONTINUE;
                                                     }
                                                     if (d.rd == 15u) {
                                                         if (!handle_pc_write(&cpu, &map, &scs, val, &it_pattern, &it_remaining, &it_cond)) {
                                                             done = MM_TRUE;
                                                         }
                                                     } else {
                                                         cpu.r[d.rd] = val;
                                                     }
                                                     if (d.rn == 13u) {
                                                         EXEC_SET_SP(addr);
                                                     } else {
                                                         cpu.r[d.rn] = addr;
                                                     }
                                                 } break;
                        case MM_OP_LDRB_POST_IMM: {
                                                      mm_u32 val = 0;
                                                      mm_u32 addr = cpu.r[d.rn];
                                                      if (!mm_memmap_read(&map, cpu.sec_state, addr, 1u, &val)) {
                                                          if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                          return MM_EXEC_CONTINUE;
                                                      }
                                                      cpu.r[d.rd] = val & 0xffu;
                                                      if (d.rn == 13u) {
                                                          EXEC_SET_SP(addr + d.imm);
                                                      } else {
                                                          cpu.r[d.rn] = addr + d.imm;
                                                      }
                                                  } break;
                        case MM_OP_STRB_POST_IMM: {
                                                      mm_u32 addr = cpu.r[d.rn];
                                                      if (!mm_memmap_write(&map, cpu.sec_state, addr, 1u, cpu.r[d.rd])) {
                                                          if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                          return MM_EXEC_CONTINUE;
                                                      }
                                                      if (d.rn == 13u) {
                                                          EXEC_SET_SP(addr + d.imm);
                                                      } else {
                                                          cpu.r[d.rn] = addr + d.imm;
                                                      }
                                                  } break;
                        case MM_OP_LDRB_PRE_IMM: {
                                                     mm_u32 addr = cpu.r[d.rn] + d.imm;
                                                     mm_u32 val = 0;
                                                     if (!mm_memmap_read(&map, cpu.sec_state, addr, 1u, &val)) {
                                                         if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                         return MM_EXEC_CONTINUE;
                                                     }
                                                     cpu.r[d.rd] = val & 0xffu;
                                                     if (d.rn == 13u) {
                                                         EXEC_SET_SP(addr);
                                                     } else {
                                                         cpu.r[d.rn] = addr;
                                                     }
                                                 } break;
                        case MM_OP_STRB_PRE_IMM: {
                                                     mm_u32 addr = cpu.r[d.rn] + d.imm;
                                                     if (!mm_memmap_write(&map, cpu.sec_state, addr, 1u, cpu.r[d.rd])) {
                                                         if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                         return MM_EXEC_CONTINUE;
                                                     }
                                                     if (d.rn == 13u) {
                                                         EXEC_SET_SP(addr);
                                                     } else {
                                                         cpu.r[d.rn] = addr;
                                                     }
                                                 } break;
                        case MM_OP_STR_POST_IMM: {
                                                     mm_u32 addr = cpu.r[d.rn];
                                                     if (!mm_memmap_write(&map, cpu.sec_state, addr, 4u, cpu.r[d.rd])) {
                                                         if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                         return MM_EXEC_CONTINUE;
                                                     }
                                                     if (d.rn == 13u) {
                                                         EXEC_SET_SP(addr + d.imm);
                                                     } else {
                                                         cpu.r[d.rn] = addr + d.imm;
                                                     }
                                                 } break;
                        case MM_OP_STR_PRE_IMM: {
                                                    mm_u32 addr = cpu.r[d.rn] + d.imm;
                                                    if (!mm_memmap_write(&map, cpu.sec_state, addr, 4u, cpu.r[d.rd])) {
                                                        if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                        return MM_EXEC_CONTINUE;
                                                    }
                                                     if (d.rn == 13u) {
                                                         EXEC_SET_SP(addr);
                                                     } else {
                                                         cpu.r[d.rn] = addr;
                                                     }
                                                 } break;
                        case MM_OP_STRB_REG: {
                                                 mm_u32 offset = cpu.r[d.rm] << (d.imm & 0x3u);
                                                 mm_u32 addr = cpu.r[d.rn] + offset;
                                                 if (!mm_memmap_write(&map, cpu.sec_state, addr, 1u, cpu.r[d.rd])) {
                                                     if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                             } break;
                        case MM_OP_LDRB_REG: {
                                                 mm_u32 val = 0;
                                                 mm_u32 offset = cpu.r[d.rm] << (d.imm & 0x3u);
                                                 mm_u32 addr = cpu.r[d.rn] + offset;
                                                 if (!mm_memmap_read(&map, cpu.sec_state, addr, 1u, &val)) {
                                                     if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                                 cpu.r[d.rd] = val & 0xffu;
                                             } break;
                        case MM_OP_LDRB_IMM:
                        case MM_OP_LDRBT: {
                                                 mm_u32 addr = cpu.r[d.rn] + d.imm;
                                                 mm_u32 val = 0;
                                                 if (!mm_memmap_read(&map, cpu.sec_state, addr, 1u, &val)) {
                                                     if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                                 cpu.r[d.rd] = val & 0xffu;
                                             } break;
                        case MM_OP_LDRSB_IMM:
                        case MM_OP_LDRSBT: {
                                                  mm_u32 addr = cpu.r[d.rn] + d.imm;
                                                  mm_u32 val = 0;
                                                  if (!exec_read_signed(&map, cpu.sec_state, addr, 1u, &val)) {
                                                      if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                      return MM_EXEC_CONTINUE;
                                                  }
                                                  cpu.r[d.rd] = val;
                                              } break;
                        case MM_OP_LDRSB_POST_IMM: {
                                                      mm_u32 base = cpu.r[d.rn];
                                                      mm_u32 val = 0;
                                                      if (!mm_memmap_read(&map, cpu.sec_state, base, 1u, &val)) {
                                                          if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, base, MM_FALSE)) done = MM_TRUE;
                                                          return MM_EXEC_CONTINUE;
                                                      }
                                                      cpu.r[d.rd] = (val & 0x80u) ? (val | 0xffffff80u) : (val & 0xffu);
                                                      if (d.rn == 13u) {
                                                          EXEC_SET_SP(base + d.imm);
                                                      } else {
                                                          cpu.r[d.rn] = base + d.imm;
                                                      }
                                                  } break;
                        case MM_OP_LDRSH_IMM:
                        case MM_OP_LDRSHT: {
                                                  mm_u32 addr = cpu.r[d.rn] + d.imm;
                                                  mm_u32 val = 0;
                                                  if (!exec_read_signed(&map, cpu.sec_state, addr, 2u, &val)) {
                                                      if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                      return MM_EXEC_CONTINUE;
                                                  }
                                                  cpu.r[d.rd] = val;
                                              } break;
                        case MM_OP_CLZ: {
                                            cpu.r[d.rd] = mm_clz(cpu.r[d.rm]);
                                        } break;
                        case MM_OP_RBIT: {
                                             cpu.r[d.rd] = mm_rbit(cpu.r[d.rm]);
                                         } break;
                        case MM_OP_TT:
                        case MM_OP_TTT:
                        case MM_OP_TTA:
                        case MM_OP_TTAT: {
                                             mm_u32 addr = cpu.r[d.rn];
                                             mm_bool alt = (d.kind == MM_OP_TTA || d.kind == MM_OP_TTAT);
                                             mm_bool forceunpriv = (d.kind == MM_OP_TTT || d.kind == MM_OP_TTAT);
                                             cpu.r[d.rd] = mm_tt_resp(&cpu, &scs, addr, alt, forceunpriv);
                                         } break;
                        case MM_OP_LDRSH_REG: {
                                                  mm_u32 addr = cpu.r[d.rn] + (cpu.r[d.rm] << (d.imm & 0x3u));
                                                  mm_u32 val = 0;
                                                  if (!mm_memmap_read(&map, cpu.sec_state, addr, 2u, &val)) {
                                                      if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                      return MM_EXEC_CONTINUE;
                                                  }
                                                  if ((val & 0x8000u) != 0u) {
                                                      val |= 0xffff0000u;
                                                  }
                                                  cpu.r[d.rd] = val;
                                              } break;
                        case MM_OP_STRB_IMM:
                        case MM_OP_STRBT: {
                                                 mm_u32 addr = cpu.r[d.rn] + d.imm;
                                                 if (!mm_memmap_write(&map, cpu.sec_state, addr, 1u, cpu.r[d.rd])) {
                                                     if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                             } break;
                        case MM_OP_LDRH_IMM:
                        case MM_OP_LDRHT: {
                                                 mm_u32 addr = cpu.r[d.rn] + d.imm;
                                                 mm_u32 val = 0;
                                                 if (!mm_memmap_read(&map, cpu.sec_state, addr, 2u, &val)) {
                                                     if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                                 cpu.r[d.rd] = val & 0xffffu;
                                             } break;
                        case MM_OP_LDRH_PRE_IMM: {
                                                    mm_u32 addr = cpu.r[d.rn] + d.imm;
                                                    mm_u32 val = 0;
                                                    if (!mm_memmap_read(&map, cpu.sec_state, addr, 2u, &val)) {
                                                        if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                        return MM_EXEC_CONTINUE;
                                                    }
                                                    cpu.r[d.rd] = val & 0xffffu;
                                                    if (d.rn == 13u) {
                                                        EXEC_SET_SP(addr);
                                                    } else {
                                                        cpu.r[d.rn] = addr;
                                                    }
                                                } break;
                        case MM_OP_LDRH_POST_IMM: {
                                                     mm_u32 base = cpu.r[d.rn];
                                                     mm_u32 val = 0;
                                                     if (!mm_memmap_read(&map, cpu.sec_state, base, 2u, &val)) {
                                                         if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, base, MM_FALSE)) done = MM_TRUE;
                                                         return MM_EXEC_CONTINUE;
                                                     }
                                                     cpu.r[d.rd] = val & 0xffffu;
                                                     if (d.rn == 13u) {
                                                         EXEC_SET_SP(base + d.imm);
                                                     } else {
                                                         cpu.r[d.rn] = base + d.imm;
                                                     }
                                                 } break;
                        case MM_OP_LDRH_REG: {
                                                 mm_u32 addr = cpu.r[d.rn] + (cpu.r[d.rm] << (d.imm & 0x3u));
                                                 mm_u32 val = 0;
                                                 if (!mm_memmap_read(&map, cpu.sec_state, addr, 2u, &val)) {
                                                     if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                                 cpu.r[d.rd] = val & 0xffffu;
                                             } break;
                        case MM_OP_LDRSB_REG: {
                                                  mm_u32 addr = cpu.r[d.rn] + (cpu.r[d.rm] << (d.imm & 0x3u));
                                                  mm_u32 val = 0;
                                                  if (!mm_memmap_read(&map, cpu.sec_state, addr, 1u, &val)) {
                                                      if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                      return MM_EXEC_CONTINUE;
                                                  }
                                                  cpu.r[d.rd] = (val & 0x80u) ? (val | 0xffffff80u) : (val & 0xffu);
                                              } break;
                        case MM_OP_STRH_IMM:
                        case MM_OP_STRHT: {
                                                 mm_u32 addr = cpu.r[d.rn] + d.imm;
                                                 mm_u32 val = cpu.r[d.rd] & 0xffffu;
                                                 if (!mm_memmap_write(&map, cpu.sec_state, addr, 2u, val)) {
                                                     if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                             } break;
                        case MM_OP_STRH_PRE_IMM: {
                                                    mm_u32 addr = cpu.r[d.rn] + d.imm;
                                                    mm_u32 val = cpu.r[d.rd] & 0xffffu;
                                                    if (!mm_memmap_write(&map, cpu.sec_state, addr, 2u, val)) {
                                                        if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                        return MM_EXEC_CONTINUE;
                                                    }
                                                    if (d.rn == 13u) {
                                                        EXEC_SET_SP(addr);
                                                    } else {
                                                        cpu.r[d.rn] = addr;
                                                    }
                                                } break;
                        case MM_OP_STRH_POST_IMM: {
                                                     mm_u32 base = cpu.r[d.rn];
                                                     mm_u32 val = cpu.r[d.rd] & 0xffffu;
                                                     if (!mm_memmap_write(&map, cpu.sec_state, base, 2u, val)) {
                                                         if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, base, MM_FALSE)) done = MM_TRUE;
                                                         return MM_EXEC_CONTINUE;
                                                     }
                                                     if (d.rn == 13u) {
                                                         EXEC_SET_SP(base + d.imm);
                                                     } else {
                                                         cpu.r[d.rn] = base + d.imm;
                                                     }
                                                 } break;
                        case MM_OP_STRH_REG: {
                                                 mm_u32 addr = cpu.r[d.rn] + (cpu.r[d.rm] << (d.imm & 0x3u));
                                                 mm_u32 val = cpu.r[d.rd] & 0xffffu;
                                                 if (!mm_memmap_write(&map, cpu.sec_state, addr, 2u, val)) {
                                                     if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                             } break;
                        case MM_OP_LDRD:
                        case MM_OP_STRD: {
                                             mm_bool load = (d.kind == MM_OP_LDRD);
                                             mm_bool u = (d.imm & 0x80000000u) != 0u;
                                             mm_bool w = (d.imm & 0x40000000u) != 0u;
                                             mm_bool p = (d.imm & 0x20000000u) != 0u;
                                             mm_u32 imm = d.imm & 0x3ffu; /* lower bits hold imm<<2 */
                                             mm_u32 base = cpu.r[d.rn];
                                             mm_u32 addr = p ? (u ? (base + imm) : (base - imm)) : base;
                                             if ((addr & 0x3u) != 0u) {
                                                 if (!raise_usage_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, UFSR_UNALIGNED)) done = MM_TRUE;
                                                 return MM_EXEC_CONTINUE;
                                             }
                                             if (load) {
                                                 mm_u32 v1 = 0;
                                                 mm_u32 v2 = 0;
                                                 if (!mm_memmap_read(&map, cpu.sec_state, addr, 4u, &v1)) {
                                                     if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                                 if (!mm_memmap_read(&map, cpu.sec_state, addr + 4u, 4u, &v2)) {
                                                     if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr + 4u, MM_FALSE)) done = MM_TRUE;
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                                 cpu.r[d.rd] = v1;
                                                 cpu.r[d.rm] = v2;
                                             } else {
                                                 if (!mm_memmap_write(&map, cpu.sec_state, addr, 4u, cpu.r[d.rd])) {
                                                     if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                                 if (!mm_memmap_write(&map, cpu.sec_state, addr + 4u, 4u, cpu.r[d.rm])) {
                                                     if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr + 4u, MM_FALSE)) done = MM_TRUE;
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                             }
                                            if (w) {
                                                mm_u32 new_rn = u ? (base + imm) : (base - imm);
                                                if (d.rn == 13u) {
                                                    EXEC_SET_SP(new_rn);
                                                } else {
                                                    cpu.r[d.rn] = new_rn;
                                                }
                                            }
                                        } break;
                        case MM_OP_STM:
                        case MM_OP_LDM: {
                                            mm_u32 opc = (d.imm >> 24) & 0x3u; /* 01=IA, 10=DB */
                                            mm_u32 wbit = (d.imm >> 16) & 0x1u;
                                            mm_u32 mask = d.imm & 0xffffu;
                                            mm_u32 count = 0;
                                            mm_u32 reg;
                                            mm_u32 start;
                                            mm_u32 addr;
                                            mm_u32 base = (d.rn == 13u) ? mm_cpu_get_active_sp(&cpu) : cpu.r[d.rn];
                                            mm_bool exc_return_taken = MM_FALSE;

                                            if (stack_trace_enabled() && d.rn == 13u) {
                                                printf("[STACK_LDMSTM] kind=%s opc=%lu w=%lu mask=0x%04lx base=0x%08lx mode=%d sec=%d sp_active=0x%08lx pc=0x%08lx\n",
                                                       (d.kind == MM_OP_LDM) ? "LDM" : "STM",
                                                       (unsigned long)opc,
                                                       (unsigned long)wbit,
                                                       (unsigned long)mask,
                                                       (unsigned long)base,
                                                       (int)cpu.mode,
                                                       (int)cpu.sec_state,
                                                       (unsigned long)mm_cpu_get_active_sp(&cpu),
                                                       (unsigned long)f.pc_fetch);
                                            }

                                            for (reg = 0; reg < 16u; ++reg) {
                                                if (mask & (1u << reg)) {
                                                    count++;
                                                }
                                            }
                                            if (count == 0u) {
                                                break;
                                            }

                                            if (opc == 2u) { /* DB/FD: first transfer at Rn-4*count */
                                                start = base - 4u * count;
                                            } else { /* IA/EA default */
                                                start = base;
                                            }
                                            if ((start & 0x3u) != 0u) {
                                                if (!raise_usage_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, UFSR_UNALIGNED)) done = MM_TRUE;
                                                return MM_EXEC_CONTINUE;
                                            }

                                            if (stack_trace_enabled() && d.rn == 13u) {
                                                printf("[STACK_LDMSTM] start=0x%08lx count=%lu\n",
                                                       (unsigned long)start,
                                                       (unsigned long)count);
                                            }

                                            addr = start;
                                            for (reg = 0; reg < 16u; ++reg) {
                                                if ((mask & (1u << reg)) == 0u) {
                                                    continue;
                                                }
                                                if (d.kind == MM_OP_STM) {
                                                    mm_u32 val = (reg == 15u) ? (cpu.r[15] | 1u) : cpu.r[reg];
                                                    if (getenv("M33MU_UNDEF_TRACE") && d.rn == 13u && (reg == 14u || reg == 15u)) {
                                                        printf("[STACK_STORE] pc=0x%08lx addr=0x%08lx reg=%u val=0x%08lx\n",
                                                               (unsigned long)f.pc_fetch,
                                                               (unsigned long)addr,
                                                               (unsigned int)reg,
                                                               (unsigned long)val);
                                                    }
                                                    if (!mm_memmap_write(&map, cpu.sec_state, addr, 4u, val)) {
                                                        if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                        break;
                                                    }
                                                } else {
                                                    mm_u32 val = 0;
                                                    if (!mm_memmap_read(&map, cpu.sec_state, addr, 4u, &val)) {
                                                        if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                        break;
                                                    }
                                                if (reg == 15u) {
                                                    if (getenv("M33MU_UNDEF_TRACE") && d.rn == 13u) {
                                                        printf("[STACK_LOAD_PC] pc=0x%08lx addr=0x%08lx val=0x%08lx\n",
                                                               (unsigned long)f.pc_fetch,
                                                               (unsigned long)addr,
                                                               (unsigned long)val);
                                                    }
                                                    if (svc_stack_trace_enabled() && (cpu.xpsr & 0x1ffu) == MM_VECT_SVCALL) {
                                                        printf("[LDM_PC_WRITE] pc=0x%08lx sp=0x%08lx val=0x%08lx lr=0x%08lx sec=%d mode=%d ipsr=%lu\n",
                                                               (unsigned long)f.pc_fetch,
                                                               (unsigned long)mm_cpu_get_active_sp(&cpu),
                                                               (unsigned long)val,
                                                               (unsigned long)cpu.r[14],
                                                               (int)cpu.sec_state,
                                                               (int)cpu.mode,
                                                               (unsigned long)(cpu.xpsr & 0x1ffu));
                                                    }
                                                    if ((val & 0xffffff00u) == 0xffffff00u && wbit) {
                                                        mm_u32 sp_after = (opc == 2u) ? (base - 4u * count) : (base + 4u * count);
                                                        if (d.rn == 13u) {
                                                            EXEC_SET_SP(sp_after);
                                                        } else {
                                                            cpu.r[d.rn] = sp_after;
                                                        }
                                                    }
                                                    if (!handle_pc_write(&cpu, &map, &scs, val, &it_pattern, &it_remaining, &it_cond)) {
                                                        done = MM_TRUE;
                                                    }
                                                    exc_return_taken = (val & 0xffffff00u) == 0xffffff00u;
                                                } else {
                                                        cpu.r[reg] = val;
                                                    }
                                                }
                                                addr += 4u;
                                                if (exc_return_taken) {
                                                    break;
                                                }
                                            }


                                            if (exc_return_taken) {
                                                /* Exception return unstack already updated SP. */
                                                break;
                                            }
                                            if (wbit && !done) {
                                                mm_bool base_in_list = (mask & (1u << d.rn)) != 0u;
                                                if (d.kind == MM_OP_LDM && base_in_list) {
                                                    /* LDM with base in list: no writeback; base is loaded from memory. */
                                                    break;
                                                }
                                                /* Write-back: IA increments by 4*count; DB decrements by 4*count. */
                                                if (opc == 2u) {
                                                    if (d.rn == 13u) {
                                                        EXEC_SET_SP(base - 4u * count);
                                                    } else {
                                                        cpu.r[d.rn] = base - 4u * count;
                                                    }
                                                } else {
                                                    if (d.rn == 13u) {
                                                        EXEC_SET_SP(base + 4u * count);
                                                    } else {
                                                        cpu.r[d.rn] = base + 4u * count;
                                                    }
                                                }
                                            }
                                        } break;
                        case MM_OP_WFI:
                        case MM_OP_WFI_W:
                                        cpu.sleeping = MM_TRUE;
                                        cpu.sleep_wfe = MM_FALSE;
                                        break;
                        case MM_OP_WFE:
                        case MM_OP_WFE_W:
                                        if (cpu.event_reg) {
                                            cpu.event_reg = MM_FALSE;
                                        } else {
                                            cpu.sleeping = MM_TRUE;
                                            cpu.sleep_wfe = MM_TRUE;
                                        }
                                        break;
                        case MM_OP_SEV:
                        case MM_OP_SEV_W:
                        case MM_OP_SEVL_W:
                                        cpu.event_reg = MM_TRUE;
                                        break;
                        case MM_OP_YIELD:
                        case MM_OP_YIELD_W:
                                        /* Hint: currently no scheduler hook; treat as NOP. */
                                        break;
                        case MM_OP_SVC: {
                                            /* SVC is a 16-bit instruction; return to the following halfword. */
                                            mm_u32 ret_pc = f.pc_fetch + 2u;
                                            mm_u32 exc_num = MM_VECT_SVCALL;
                                            if (!svc_can_preempt_current(&cpu, &scs, ctx->nvic)) {
                                                exc_num = MM_VECT_HARDFAULT;
                                            }
                                            if (enter_exception == 0 ||
                                                !enter_exception(&cpu, &map, &scs, exc_num, ret_pc, cpu.xpsr)) {
                                                done = MM_TRUE;
                                            } else {
                                                /* Sync IT state to the handler xPSR after exception entry. */
                                                itstate_sync_from_xpsr(cpu.xpsr, &it_pattern, &it_remaining, &it_cond);
                                            }
                                        } break;
                        case MM_OP_PUSH: {
                                             mm_u32 sp = mm_cpu_get_active_sp(&cpu);
                                             mm_u16 mask = (mm_u16)d.imm;
                                             int reg;
                                             mm_u32 count = 0;
                                             mm_u32 addr;
                                             /* TODO: check the boundaries of memory of the operators */
                                             for (reg = 0; reg <= 7; ++reg) {
                                                 if ((mask & (1u << reg)) != 0u) {
                                                     count++;
                                                 }
                                             }
                                             if ((mask & 0x0100u) != 0u) {
                                                 count++;
                                             }
                                             addr = sp - (mm_u32)count * 4u;
                                            for (reg = 0; reg <= 7; ++reg) {
                                                if ((mask & (1u << reg)) == 0u) {
                                                    continue;
                                                }
                                                if (!mm_memmap_write(&map, cpu.sec_state, addr, 4u, cpu.r[reg])) {
                                                    if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                    break;
                                                 }
                                                 addr += 4u;
                                             }
                                             if (!done && (mask & 0x0100u) != 0u) {
                                                 if (!mm_memmap_write(&map, cpu.sec_state, addr, 4u, cpu.r[14])) {
                                                     if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                 } else {
                                                     addr += 4u;
                                                 }
                                             }
                                             if (!done) {
                                                 if (svc_stack_trace_enabled()) {
                                                     printf("[PUSH_SP] pc=0x%08lx sp_before=0x%08lx count=%lu\n",
                                                            (unsigned long)f.pc_fetch,
                                                            (unsigned long)sp,
                                                            (unsigned long)count);
                                                 }
                                                 EXEC_SET_SP(sp - (mm_u32)count * 4u);
                                                 if (svc_stack_trace_enabled()) {
                                                     printf("[PUSH_SP] pc=0x%08lx sp_after=0x%08lx\n",
                                                            (unsigned long)f.pc_fetch,
                                                            (unsigned long)mm_cpu_get_active_sp(&cpu));
                                                 }
                                             }
                                         } break;
                        case MM_OP_POP: {
                                            mm_u32 sp = mm_cpu_get_active_sp(&cpu);
                                            mm_u32 sp_start = sp;
                                            mm_u16 mask = (mm_u16)d.imm;
                                            int reg;
                                            mm_bool exc_return_taken = MM_FALSE;
                                            mm_u32 pop_count = 0;
                                            if (svc_stack_trace_enabled() && (mask & 0x0100u) != 0u) {
                                                mm_u32 w0 = 0, w1 = 0;
                                                mm_bool ok0 = mm_memmap_read(&map, cpu.sec_state, sp, 4u, &w0);
                                                mm_bool ok1 = mm_memmap_read(&map, cpu.sec_state, sp + 4u, 4u, &w1);
                                                printf("[POP_PRE] pc=0x%08lx sp=0x%08lx word0=%s0x%08lx word1=%s0x%08lx\n",
                                                       (unsigned long)f.pc_fetch,
                                                       (unsigned long)sp,
                                                       ok0 ? "" : "!!", (unsigned long)w0,
                                                       ok1 ? "" : "!!", (unsigned long)w1);
                                            }
                                            if (svc_stack_trace_enabled() && (cpu.xpsr & 0x1ffu) == MM_VECT_SVCALL) {
                                                for (reg = 0; reg < 16; ++reg) {
                                                    if (reg > 7 && reg != 15) {
                                                        continue;
                                                    }
                                                    if (reg == 15) {
                                                        if ((mask & 0x0100u) == 0u) continue;
                                                    } else {
                                                        if ((mask & (1u << reg)) == 0u) continue;
                                                    }
                                                    pop_count++;
                                                }
                                                printf("[POP_BEGIN] pc=0x%08lx sp=0x%08lx mask=0x%04lx count=%lu lr=0x%08lx sec=%d mode=%d ipsr=%lu\n",
                                                       (unsigned long)f.pc_fetch,
                                                       (unsigned long)sp_start,
                                                       (unsigned long)mask,
                                                       (unsigned long)pop_count,
                                                       (unsigned long)cpu.r[14],
                                                       (int)cpu.sec_state,
                                                       (int)cpu.mode,
                                                       (unsigned long)(cpu.xpsr & 0x1ffu));
                                                {
                                                    mm_u32 w0 = 0, w1 = 0, w2 = 0, w3 = 0;
                                                    mm_bool ok0 = mm_memmap_read(&map, cpu.sec_state, sp_start, 4u, &w0);
                                                    mm_bool ok1 = mm_memmap_read(&map, cpu.sec_state, sp_start + 4u, 4u, &w1);
                                                    mm_bool ok2 = mm_memmap_read(&map, cpu.sec_state, sp_start + 8u, 4u, &w2);
                                                    mm_bool ok3 = mm_memmap_read(&map, cpu.sec_state, sp_start + 12u, 4u, &w3);
                                                    printf("[POP_BEGIN] stack@sp: %s0x%08lx %s0x%08lx %s0x%08lx %s0x%08lx\n",
                                                           ok0 ? "" : "!!", (unsigned long)w0,
                                                           ok1 ? "" : "!!", (unsigned long)w1,
                                                           ok2 ? "" : "!!", (unsigned long)w2,
                                                           ok3 ? "" : "!!", (unsigned long)w3);
                                                }
                                            }
                                            /* TODO: check the boundaries of memory of the operators */
                                            for (reg = 0; reg < 16; ++reg) {
                                                mm_u32 val;
                                                if (reg > 7 && reg != 15) {
                                                    continue; /* POP encodes r0-r7 and optional PC only */
                                                }
                                                if (reg == 15) {
                                                    if ((mask & 0x0100u) == 0u) continue;
                                                } else {
                                                    if ((mask & (1u << reg)) == 0u) continue;
                                                }
                                                if (!mm_memmap_read(&map, cpu.sec_state, sp, 4u, &val)) {
                                                    if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, sp, MM_FALSE)) done = MM_TRUE;
                                                    break;
                                                }
                                                if (reg == 15) {
                                                    mm_bool is_exc_return = (val & 0xffffff00u) == 0xffffff00u;
                                                    if (is_exc_return) {
                                                        mm_u32 sp_after = sp + 4u;
                                                        EXEC_SET_SP(sp_after);
                                                        sp = sp_after;
                                                        if (!handle_pc_write(&cpu, &map, &scs, val, &it_pattern, &it_remaining, &it_cond)) {
                                                            done = MM_TRUE;
                                                        }
                                                        exc_return_taken = MM_TRUE;
                                                        break;
                                                    }
                                                    if (pop_trace_enabled()) {
                                                        mm_bool suspicious = MM_FALSE;
                                                        if ((val & 0xF0000000u) == 0u) {
                                                            suspicious = MM_TRUE;
                                                        } else if (map.flash.buffer != 0) {
                                                            if (val < map.flash.base || val >= (map.flash.base + map.flash.length)) {
                                                                suspicious = MM_TRUE;
                                                            }
                                                        }
                                                        if (suspicious) {
                                                            printf("[POP_PC] pc=0x%08lx sp=0x%08lx val=0x%08lx sec=%d mode=%d msp_ns=0x%08lx psp_ns=0x%08lx\n",
                                                                   (unsigned long)f.pc_fetch,
                                                                   (unsigned long)sp,
                                                                   (unsigned long)val,
                                                                   (int)cpu.sec_state,
                                                                   (int)cpu.mode,
                                                                   (unsigned long)cpu.msp_ns,
                                                                   (unsigned long)cpu.psp_ns);
                                                        }
                                                    }
                                                    if (svc_stack_trace_enabled() && (cpu.xpsr & 0x1ffu) == MM_VECT_SVCALL) {
                                                        printf("[POP_PC_WRITE] pc=0x%08lx sp=0x%08lx val=0x%08lx lr=0x%08lx sec=%d mode=%d ipsr=%lu\n",
                                                               (unsigned long)f.pc_fetch,
                                                               (unsigned long)sp,
                                                               (unsigned long)val,
                                                               (unsigned long)cpu.r[14],
                                                               (int)cpu.sec_state,
                                                               (int)cpu.mode,
                                                               (unsigned long)(cpu.xpsr & 0x1ffu));
                                                    }
                                                    if (!handle_pc_write(&cpu, &map, &scs, val, &it_pattern, &it_remaining, &it_cond)) {
                                                        done = MM_TRUE;
                                                    }
                                                } else {
                                                    cpu.r[reg] = val;
                                                }
                                                sp += 4u;
                                                if (exc_return_taken) {
                                                    break;
                                                }
                                            }
                                            if (!exc_return_taken) {
                                                EXEC_SET_SP(sp);
                                                if (svc_stack_trace_enabled() && (cpu.xpsr & 0x1ffu) == MM_VECT_SVCALL) {
                                                    printf("[POP_END] sp_start=0x%08lx sp_end=0x%08lx r13=0x%08lx msp_s=0x%08lx msp_ns=0x%08lx psp_s=0x%08lx psp_ns=0x%08lx\n",
                                                           (unsigned long)sp_start,
                                                           (unsigned long)sp,
                                                           (unsigned long)cpu.r[13],
                                                           (unsigned long)cpu.msp_s,
                                                           (unsigned long)cpu.msp_ns,
                                                           (unsigned long)cpu.psp_s,
                                                           (unsigned long)cpu.psp_ns);
                                                }
                                            }
                                        } break;
                        default:
                                        EXEC_RAISE_UNDEF();
                    }

                    if ((cpu.r[15] & 0xF0000000u) == 0xF0000000u) {
                        printf("[PC_HIGH] pc=0x%08lx prev_pc=0x%08lx fetch=0x%08lx lr=0x%08lx kind=%u\n",
                               (unsigned long)cpu.r[15],
                               (unsigned long)pc_before_exec,
                               (unsigned long)f.pc_fetch,
                               (unsigned long)cpu.r[14],
                               (unsigned)d.kind);
                    }
#undef cpu
#undef map
#undef scs
#undef gdb
#undef f
#undef d
#undef it_pattern
#undef it_remaining
#undef it_cond
#undef done
    return MM_EXEC_OK;
}
