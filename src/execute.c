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
#include "m33mu/target_hal.h"
#include "m33mu/mem_prot.h"
#include <stdio.h>
#include <stdlib.h>

static int g_stack_trace = -1;
static int g_splim_trace = -1;

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

#define CCR_DIV_0_TRP (1u << 4)
#define UFSR_DIVBYZERO (1u << 25)
#define UFSR_STKOF (1u << 20)

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
    if (cpu == 0) {
        return MM_FALSE;
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
    mm_u8 next;
    if (mask == 0u) {
        return 0u;
    }
    next = (mm_u8)((itstate & 0xE0u) | ((itstate & 0x1Fu) << 1));
    if ((next & 0x0fu) == 0u) {
        next = 0u;
    }
    return next;
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

enum mm_exec_status mm_execute_decoded(struct mm_execute_ctx *ctx)
{
    mm_u8 itstate_val = 0;
    mm_u32 pc_before_exec = 0;
    mm_bool (*handle_pc_write)(struct mm_cpu *, struct mm_memmap *, mm_u32, mm_u8 *, mm_u8 *, mm_u8 *);
    mm_bool (*raise_mem_fault)(struct mm_cpu *, struct mm_memmap *, struct mm_scs *, mm_u32, mm_u32, mm_u32, mm_bool);
    mm_bool (*raise_usage_fault)(struct mm_cpu *, struct mm_memmap *, struct mm_scs *, mm_u32, mm_u32, mm_u32);
    mm_bool (*exc_return_unstack)(struct mm_cpu *, struct mm_memmap *, mm_u32);
    mm_bool (*enter_exception)(struct mm_cpu *, struct mm_memmap *, struct mm_scs *, mm_u32, mm_u32, mm_u32);
    mm_bool opt_gdb;

    if (ctx == 0 || ctx->cpu == 0 || ctx->map == 0 || ctx->scs == 0 || ctx->fetch == 0 || ctx->dec == 0 ||
        ctx->it_pattern == 0 || ctx->it_remaining == 0 || ctx->it_cond == 0 || ctx->done == 0) {
        return MM_EXEC_CONTINUE;
    }

    handle_pc_write = ctx->handle_pc_write;
    raise_mem_fault = ctx->raise_mem_fault;
    raise_usage_fault = ctx->raise_usage_fault;
    exc_return_unstack = ctx->exc_return_unstack;
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

                    pc_before_exec = cpu.r[15];
                    switch (d.kind) {
                        case MM_OP_IT:
                            it_cond = (mm_u8)((d.imm >> 4) & 0x0fu);
                            it_mask_to_pattern(it_cond, (mm_u8)(d.imm & 0x0fu), &it_pattern, &it_remaining);
                            itstate_val = (mm_u8)((it_cond << 4) | (d.imm & 0x0fu));
                            cpu.xpsr = itstate_set(cpu.xpsr, itstate_val);
                            break;
                        case MM_OP_NOP:
                            break;
                        case MM_OP_DSB:
                        case MM_OP_DMB:
                        case MM_OP_ISB:
                            /* Barriers are modeled as no-ops for now. */
                            break;
                        case MM_OP_B_UNCOND:
                        case MM_OP_B_UNCOND_WIDE:
                            cpu.r[15] = (f.pc_fetch + 4u + d.imm) | 1u;
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
                                                        cpu.r[15] = (f.pc_fetch + 4u + d.imm) | 1u;
                                                    }
                                                } break;
                        case MM_OP_CBZ:
                        case MM_OP_CBNZ: {
                                             /* CBZ/CBNZ T1 use PC+4 as the branch base. */
                                             mm_bool zero = (cpu.r[d.rn] == 0u);
                                             mm_bool take = (d.kind == MM_OP_CBZ) ? zero : (!zero);
                                             if (take) {
                                                 cpu.r[15] = (f.pc_fetch + 4u + d.imm) | 1u;
                                             } else {
                                                 /* Fall-through already handled by PC increment in fetch/decode. */
                                             }
                                         } break;
                                       case MM_OP_BX: {
                                           mm_u32 target = cpu.r[d.rm];
                                           if (d.rm == 14u && (target & 0xffffff00u) == 0xffffff00u) {
                                               if (!exc_return_unstack(&cpu, &map, target)) {
                                                   printf("[BX] exc_return_unstack failed target=0x%08lx pc=0x%08lx lr=0x%08lx\n",
                                                          (unsigned long)target,
                                                          (unsigned long)cpu.r[15],
                                                          (unsigned long)cpu.r[14]);
                                                   done = MM_TRUE;
                                               } else {
                                                   itstate_sync_from_xpsr(cpu.xpsr, &it_pattern, &it_remaining, &it_cond);
                                               }
                                           } else if ((target & 0xF0000000u) == 0xF0000000u) {
                                               printf("[BX] suspicious target=0x%08lx pc=0x%08lx lr=0x%08lx rm=%u\n",
                                                      (unsigned long)target,
                                                      (unsigned long)cpu.r[15],
                                                      (unsigned long)cpu.r[14],
                                                      (unsigned)d.rm);
                                               cpu.r[15] = target | 1u;
                                           } else if (d.rm == 14u && cpu.sec_state == MM_NONSECURE &&
                                                   cpu.tz_depth > 0 && target == 0xDEAD0001u) {
                                               /* Return from Secure->Non-secure BLXNS callback. */
                                               cpu.tz_depth--;
                                               cpu.sec_state = cpu.tz_ret_sec[cpu.tz_depth];
                                               cpu.mode = cpu.tz_ret_mode[cpu.tz_depth];
                                               cpu.r[15] = cpu.tz_ret_pc[cpu.tz_depth] | 1u;
                                               cpu.r[14] = cpu.tz_ret_pc[cpu.tz_depth] | 1u;
                                               EXEC_SET_SP(mm_cpu_get_active_sp(&cpu));
                                           } else {
                                               cpu.r[15] = target | 1u;
                                           }
                                       } break;
                        case MM_OP_BLX: {
                                           mm_u32 target = cpu.r[d.rm];
                                           cpu.r[14] = (f.pc_fetch + d.len) | 1u;
                                           cpu.r[15] = target | 1u;
                                       } break;
                        case MM_OP_SG:
                                       mm_tz_exec_sg(&cpu);
                                       break;
                        case MM_OP_BXNS:
                                       mm_tz_exec_bxns(&cpu, cpu.r[d.rm]);
                                       break;
                        case MM_OP_BLXNS:
                                       /* Return address is the next instruction (fetch already advanced PC state). */
                                       mm_tz_exec_blxns(&cpu, cpu.r[d.rm], (f.pc_fetch + d.len));
                                       break;
                        case MM_OP_BL:
                                       cpu.r[14] = (f.pc_fetch + 4u) | 1u;
                                       cpu.r[15] = (f.pc_fetch + 4u + d.imm) | 1u;
                                       break;
                        case MM_OP_MOV_IMM:
                                       cpu.r[d.rd] = d.imm;
                                       break;
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
                                           mm_bool setflags = MM_FALSE;
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
                                               mm_add_with_carry(cpu.r[d.rn], d.imm, MM_FALSE, &res, &cflag, &vflag);
                                               cpu.r[d.rd] = res;
                                               cpu.xpsr &= ~(0xF0000000u);
                                               if (res == 0u) cpu.xpsr |= (1u << 30);
                                               if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                               if (cflag) cpu.xpsr |= (1u << 29);
                                               if (vflag) cpu.xpsr |= (1u << 28);
                                           } else {
                                               cpu.r[d.rd] = cpu.r[d.rn] + d.imm;
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
                                               mm_bool setflags = (d.raw & (1u << 20)) != 0u;
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
                                           EXEC_SET_SP(mm_cpu_get_active_sp(&cpu) + d.imm);
                                       } else {
                                           cpu.r[d.rd] = cpu.r[13] + d.imm;
                                       }
                                       break;
                        case MM_OP_ADD_REG:
                                       if ((d.raw & 0xfe000000u) == 0xea000000u) {
                                           mm_u32 rhs = shift_reg_operand(cpu.r[d.rm], d.imm, cpu.xpsr, NULL);
                                           if ((d.raw & (1u << 20)) != 0u) {
                                               mm_u32 res;
                                               mm_bool cflag;
                                               mm_bool vflag;
                                               mm_add_with_carry(cpu.r[d.rn], rhs, MM_FALSE, &res, &cflag, &vflag);
                                               cpu.r[d.rd] = res;
                                               cpu.xpsr &= ~(0xF0000000u);
                                               if (res == 0u) cpu.xpsr |= (1u << 30);
                                               if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                               if (cflag) cpu.xpsr |= (1u << 29);
                                               if (vflag) cpu.xpsr |= (1u << 28);
                                           } else {
                                               cpu.r[d.rd] = cpu.r[d.rn] + rhs;
                                           }
                                       } else {
                                           mm_bool setflags = MM_FALSE;
                                           if (d.len == 2u) {
                                               if ((d.raw & 0xfc00u) == 0x4400u) {
                                                   setflags = MM_FALSE;
                                               } else {
                                                   setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                               }
                                           }
                                           if (setflags) {
                                               mm_u32 res;
                                               mm_bool cflag;
                                               mm_bool vflag;
                                               mm_add_with_carry(cpu.r[d.rn], cpu.r[d.rm], MM_FALSE, &res, &cflag, &vflag);
                                               cpu.r[d.rd] = res;
                                               cpu.xpsr &= ~(0xF0000000u);
                                               if (res == 0u) cpu.xpsr |= (1u << 30);
                                               if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                               if (cflag) cpu.xpsr |= (1u << 29);
                                               if (vflag) cpu.xpsr |= (1u << 28);
                                           } else {
                                               cpu.r[d.rd] = cpu.r[d.rn] + cpu.r[d.rm];
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
                                                    cpu.xpsr &= ~(0xF0000000u);
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
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                } else if (d.len == 4u && ((d.raw >> 20) & 1u) != 0u) {
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                }
                                                cpu.r[d.rd] = res;
                                                if (setflags) {
                                                    cpu.xpsr &= ~(0xF0000000u);
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
                                                    cpu.xpsr &= ~(0xF0000000u);
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
                                                    cpu.xpsr &= ~(0xF0000000u);
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
                                                if (d.len == 2u) {
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                } else if (d.len == 4u && ((d.raw >> 20) & 1u) != 0u) {
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                }
                                                cpu.r[d.rd] = res;
                                                if (setflags) {
                                                    cpu.xpsr &= ~(0xF0000000u);
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
                                                mm_bool setflags = (d.len == 2u) ? ((it_remaining == 0u) ? MM_TRUE : MM_FALSE) : MM_FALSE;
                                                cpu.r[d.rd] = r.value;
                                                if (setflags) {
                                                    cpu.xpsr &= ~(0xF0000000u);
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
                                                    cpu.xpsr &= ~(0xF0000000u);
                                                    if (res == 0u) cpu.xpsr |= (1u << 30);
                                                    if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                    if (carry) cpu.xpsr |= (1u << 29);
                                                }
                                            } break;
                        case MM_OP_ROR_REG: {
                                                mm_u32 val = cpu.r[d.rn];
                                                mm_u32 sh = cpu.r[d.rm] & 0xffu;
                                                mm_bool setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                mm_bool carry_in = (cpu.xpsr & (1u << 29)) != 0u;
                                                mm_bool carry_out = carry_in;
                                                mm_u32 res = mm_ror_reg_shift_c(val, sh, carry_in, &carry_out);
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
                                            mm_add_with_carry(0u, ~cpu.r[d.rm], MM_TRUE, &res, &cflag, &vflag);
                                            cpu.r[d.rd] = res;
                                            cpu.xpsr &= ~(0xF0000000u);
                                            if (res == 0u) cpu.xpsr |= (1u << 30);
                                            if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                            if (cflag) cpu.xpsr |= (1u << 29);
                                            if (vflag) cpu.xpsr |= (1u << 28);
                                        } break;
                        case MM_OP_SBCS_REG: {
                                                 mm_bool reg_form = ((d.raw & 0xfe000000u) == 0xea000000u);
                                                 mm_bool setflags;
                                                 if (reg_form) {
                                                     setflags = (((d.raw >> 20) & 1u) != 0u) ? ((it_remaining <= 1u) ? MM_TRUE : MM_FALSE) : MM_FALSE;
                                                     {
                                                         mm_u32 rhs = shift_reg_operand(cpu.r[d.rm], d.imm, cpu.xpsr, NULL);
                                                         cpu.r[d.rd] = mm_sbcs_reg(cpu.r[d.rn], rhs, &cpu.xpsr, setflags);
                                                     }
                                                 } else {
                                                     setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                     cpu.r[d.rd] = mm_sbcs_reg(cpu.r[d.rn], cpu.r[d.rm], &cpu.xpsr, setflags);
                                                 }
                                             } break;
                        case MM_OP_ADCS_REG: {
                                                 mm_bool reg_form = ((d.raw & 0xfe000000u) == 0xea000000u);
                                                 mm_bool setflags = MM_FALSE;
                                                 if (d.len == 2u) {
                                                     setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
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
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
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
                                                cpu.r[d.rd] = res;
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
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
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
                                                cpu.r[d.rd] = lhs ^ rhs;
                                                if (setflags) {
                                                    mm_u32 res = cpu.r[d.rd];
                                                    cpu.xpsr &= ~(0xE0000000u);
                                                    if (res == 0u) cpu.xpsr |= (1u << 30);
                                                    if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                    if (carry_out) cpu.xpsr |= (1u << 29);
                                                }
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
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
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
                                                    setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
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
                                            cpu.xpsr &= ~(0xC0000000u);
                                            if (res == 0u) cpu.xpsr |= (1u << 30);
                                            if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
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
                                                 if (!raise_usage_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, (1u << 16))) done = MM_TRUE;
                                                 return MM_EXEC_CONTINUE;
                                             }
                                             cpu.r[d.rd] = mm_ubfx(cpu.r[d.rn], (mm_u8)lsb, (mm_u8)width);
                                         } break;
                        case MM_OP_SBFX: {
                                             mm_u32 imm3 = (d.raw >> 12) & 0x7u;
                                             mm_u32 imm2 = (d.raw >> 6) & 0x3u;
                                             mm_u32 lsb = (imm3 << 2) | imm2;
                                             mm_u32 width = (d.raw & 0x1fu) + 1u;
                                             if (d.rd == 15u || d.rn == 15u || lsb >= 32u || width == 0u || (lsb + width) > 32u) {
                                                 if (!raise_usage_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, (1u << 16))) done = MM_TRUE;
                                                 return MM_EXEC_CONTINUE;
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
                                                if (!raise_usage_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, (1u << 16))) done = MM_TRUE;
                                                return MM_EXEC_CONTINUE;
                                            }
                                            width = (msb - lsb) + 1u;
                                            if (d.rd == 15u || d.rn == 15u || lsb >= 32u || width == 0u || (lsb + width) > 32u) {
                                                if (!raise_usage_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, (1u << 16))) done = MM_TRUE;
                                                return MM_EXEC_CONTINUE;
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
                                                if (!raise_usage_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, (1u << 16))) done = MM_TRUE;
                                                return MM_EXEC_CONTINUE;
                                            }
                                            width = (msb - lsb) + 1u;
                                            if (d.rd == 15u || lsb >= 32u || width == 0u || (lsb + width) > 32u) {
                                                if (!raise_usage_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, (1u << 16))) done = MM_TRUE;
                                                return MM_EXEC_CONTINUE;
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
                                                 mm_i32 quot = dividend / divisor;
                                                 cpu.r[d.rd] = (mm_u32)quot;
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
                                            mm_i32 acc = prod + (mm_i32)cpu.r[d.ra];
                                            cpu.r[d.rd] = (mm_u32)acc;
                                        } break;
                        case MM_OP_MLS: {
                                            mm_u32 prod = cpu.r[d.rn] * cpu.r[d.rm];
                                            cpu.r[d.rd] = cpu.r[d.ra] - prod;
                                        } break;
                        case MM_OP_MUL_W: {
                                              mm_u32 res = cpu.r[d.rn] * cpu.r[d.rm];
                                              mm_bool setflags = (d.imm & 1u) ? MM_TRUE : MM_FALSE;
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
                                            if (!handle_pc_write(&cpu, &map, target_pc, &it_pattern, &it_remaining, &it_cond)) {
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
                                                case 0x08: val = mm_cpu_get_active_sp(&cpu); break; /* MSP */
                                                case 0x09: val = (cpu.sec_state == MM_NONSECURE) ? cpu.psp_ns : cpu.psp_s; break; /* PSP */
                                                case 0x0a: val = (cpu.sec_state == MM_NONSECURE) ? cpu.msplim_ns : cpu.msplim_s; break; /* MSPLIM */
                                                case 0x0b: val = (cpu.sec_state == MM_NONSECURE) ? cpu.psplim_ns : cpu.psplim_s; break; /* PSPLIM */
                                                case 0x88: val = cpu.msp_ns; break;
                                                case 0x89: val = cpu.psp_ns; break;
                                                case 0x8a: val = cpu.msplim_ns; break;
                                                case 0x8b: val = cpu.psplim_ns; break;
                                                case 0x94: val = cpu.control_ns; break;
                                                case 0x14: val = cpu.control_s; break;
                                                default: val = 0; break;
                                            }
                                            cpu.r[d.rd] = val;
                                        } break;
                        case MM_OP_MSR: {
                                            mm_u32 sysm = d.imm & 0xffu;
                                            mm_u32 mask = (d.imm >> 8) & 0xfu;
                                            mm_u32 val = cpu.r[d.rm];
                                            if (mask == 8u) {
                                                switch (sysm) {
                                                    case 0x08: /* MSP */
                                                        mm_cpu_set_msp(&cpu, cpu.sec_state, val);
                                                        break;
                                                    case 0x09: /* PSP */
                                                        mm_cpu_set_psp(&cpu, cpu.sec_state, val);
                                                        break;
                                                    case 0x0a: /* MSPLIM */
                                                        if (cpu.sec_state == MM_NONSECURE) {
                                                            cpu.msplim_ns = val;
                                                        } else {
                                                            cpu.msplim_s = val;
                                                        }
                                                        if (splim_trace_enabled()) {
                                                            printf("[SPLIM] MSPLIM %s=0x%08lx\n",
                                                                   (cpu.sec_state == MM_NONSECURE) ? "NS" : "S",
                                                                   (unsigned long)val);
                                                        }
                                                        break;
                                                    case 0x0b: /* PSPLIM */
                                                        if (cpu.sec_state == MM_NONSECURE) {
                                                            cpu.psplim_ns = val;
                                                        } else {
                                                            cpu.psplim_s = val;
                                                        }
                                                        if (splim_trace_enabled()) {
                                                            printf("[SPLIM] PSPLIM %s=0x%08lx\n",
                                                                   (cpu.sec_state == MM_NONSECURE) ? "NS" : "S",
                                                                   (unsigned long)val);
                                                        }
                                                        break;
                                                    case 0x88: /* MSP_NS */
                                                        mm_cpu_set_msp(&cpu, MM_NONSECURE, val);
                                                        break;
                                                    case 0x89: /* PSP_NS */
                                                        mm_cpu_set_psp(&cpu, MM_NONSECURE, val);
                                                        break;
                                                    case 0x8a: /* MSPLIM_NS */
                                                        cpu.msplim_ns = val;
                                                        if (splim_trace_enabled()) {
                                                            printf("[SPLIM] MSPLIM NS=0x%08lx\n", (unsigned long)val);
                                                        }
                                                        break;
                                                    case 0x8b: /* PSPLIM_NS */
                                                        cpu.psplim_ns = val;
                                                        if (splim_trace_enabled()) {
                                                            printf("[SPLIM] PSPLIM NS=0x%08lx\n", (unsigned long)val);
                                                        }
                                                        break;
                                                    case 0x14: /* CONTROL_S */
                                                        mm_cpu_set_control(&cpu, MM_SECURE, val);
                                                        break;
                                                    case 0x94: /* CONTROL_NS */
                                                        mm_cpu_set_control(&cpu, MM_NONSECURE, val);
                                                        break;
                                                    default:
                                                        break;
                                                }
                                            } else if (sysm == 0x00u) {
                                                /* APSR field write: honor NZCVQ group when selected.
                                                 * Only these bits are writable; IT/T/exception number remain unchanged. */
                                                if ((mask & 8u) != 0u) {
                                                    cpu.xpsr = mm_xpsr_write_nzcvq(cpu.xpsr, val);
                                                }
                                            }
                                            /* MSR does not affect PC; fall through with normal PC increment. */
                                        } break;
                        case MM_OP_MVN_IMM: {
                                                mm_bool setflags = (d.raw & (1u << 20)) != 0u; /* Thumb-2 MVN immediate S bit. */
                                                mm_bool carry_in = (cpu.xpsr & (1u << 29)) != 0u;
                                                mm_bool carry_out = carry_in;
                                                mm_u32 imm32 = 0;
                                                mm_u32 res;
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
                                                    mm_bool setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                                    mm_u32 rm_val = cpu.r[d.rm];
                                                    cpu.r[d.rd] = mm_mvn_reg(rm_val, &cpu.xpsr, setflags);
                                                } else {
                                                    mm_bool setflags = ((d.raw >> 20) & 1u) ? MM_TRUE : MM_FALSE;
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
                                            mm_bool affect_i = (d.imm & 0x02u) != 0u;
                                            if (affect_i) {
                                                if (cpu.sec_state == MM_NONSECURE) {
                                                    cpu.primask_ns = disable ? 1u : 0u;
                                                } else {
                                                    cpu.primask_s = disable ? 1u : 0u;
                                                }
                                            }
                                        } break;
                        case MM_OP_SUB_IMM:
                                        {
                                            mm_bool setflags = MM_FALSE;
                                            if (d.len == 2u) {
                                                setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                            } else if (((d.raw >> 20) & 1u) != 0u) {
                                                setflags = (it_remaining <= 1u) ? MM_TRUE : MM_FALSE;
                                            }
                                            if (setflags) {
                                                mm_u32 res;
                                                mm_bool cflag;
                                                mm_bool vflag;
                                                /* Thumb SUB (immediate) updates flags (SUBS). */
                                                mm_add_with_carry(cpu.r[d.rn], ~d.imm, MM_TRUE, &res, &cflag, &vflag);
                                                cpu.r[d.rd] = res;
                                                cpu.xpsr &= ~(0xF0000000u);
                                                if (res == 0u) cpu.xpsr |= (1u << 30);
                                                if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                if (cflag) cpu.xpsr |= (1u << 29);
                                                if (vflag) cpu.xpsr |= (1u << 28);
                                            } else {
                                                cpu.r[d.rd] = cpu.r[d.rn] - d.imm;
                                            }
                                            if (d.rd == 13u) {
                                                EXEC_SET_SP(cpu.r[13]);
                                            }
                                        }
                                        break;
                        case MM_OP_SUB_IMM_NF:
                                        cpu.r[d.rd] = cpu.r[d.rn] - d.imm;
                                        if (d.rd == 13u) {
                                            EXEC_SET_SP(cpu.r[13]);
                                        }
                                        break;
                        case MM_OP_SUB_REG:
                                        if ((d.raw & 0xfe000000u) == 0xea000000u) {
                                            mm_u32 rhs = shift_reg_operand(cpu.r[d.rm], d.imm, cpu.xpsr, NULL);
                                            if ((d.raw & (1u << 20)) != 0u) {
                                                mm_u32 res;
                                                mm_bool cflag;
                                                mm_bool vflag;
                                                mm_add_with_carry(cpu.r[d.rn], ~rhs, MM_TRUE, &res, &cflag, &vflag);
                                                cpu.r[d.rd] = res;
                                                cpu.xpsr &= ~(0xF0000000u);
                                                if (res == 0u) cpu.xpsr |= (1u << 30);
                                                if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                if (cflag) cpu.xpsr |= (1u << 29);
                                                if (vflag) cpu.xpsr |= (1u << 28);
                                            } else {
                                                cpu.r[d.rd] = cpu.r[d.rn] - rhs;
                                            }
                                        } else {
                                            mm_bool setflags = (d.len == 2u) ? ((it_remaining <= 1u) ? MM_TRUE : MM_FALSE) : MM_FALSE;
                                            if (setflags) {
                                                mm_u32 res;
                                                mm_bool cflag;
                                                mm_bool vflag;
                                                /* SUBS Rd,Rn,Rm (Thumb-1) updates flags. */
                                                mm_add_with_carry(cpu.r[d.rn], ~cpu.r[d.rm], MM_TRUE, &res, &cflag, &vflag);
                                                cpu.r[d.rd] = res;
                                                cpu.xpsr &= ~(0xF0000000u);
                                                if (res == 0u) cpu.xpsr |= (1u << 30);
                                                if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                if (cflag) cpu.xpsr |= (1u << 29);
                                                if (vflag) cpu.xpsr |= (1u << 28);
                                            } else {
                                                cpu.r[d.rd] = cpu.r[d.rn] - cpu.r[d.rm];
                                            }
                                        }
                                        break;
                        case MM_OP_RSB_REG: {
                                               mm_u32 rhs = shift_reg_operand(cpu.r[d.rm], d.imm, cpu.xpsr, NULL);
                                               if ((d.raw & (1u << 20)) != 0u) {
                                                   mm_u32 res;
                                                   mm_bool cflag;
                                                   mm_bool vflag;
                                                   mm_add_with_carry(rhs, ~cpu.r[d.rn], MM_TRUE, &res, &cflag, &vflag);
                                                   cpu.r[d.rd] = res;
                                                   cpu.xpsr &= ~(0xF0000000u);
                                                   if (res == 0u) cpu.xpsr |= (1u << 30);
                                                   if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                   if (cflag) cpu.xpsr |= (1u << 29);
                                                   if (vflag) cpu.xpsr |= (1u << 28);
                                               } else {
                                                   cpu.r[d.rd] = rhs - cpu.r[d.rn];
                                               }
                                           } break;
                        case MM_OP_SUB_SP_IMM:
                                        if (d.rd == 13u) {
                                            EXEC_SET_SP(mm_cpu_get_active_sp(&cpu) - d.imm);
                                        } else {
                                            cpu.r[d.rd] = cpu.r[13] - d.imm;
                                        }
                                        break;
                        case MM_OP_MOV_REG:
                                        if (d.rd == 15u) {
                                            if (!handle_pc_write(&cpu, &map, cpu.r[d.rm], &it_pattern, &it_remaining, &it_cond)) {
                                                done = MM_TRUE;
                                            }
                                        } else if (d.rd == 13u) {
                                            EXEC_SET_SP(cpu.r[d.rm]);
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
                                                mm_add_with_carry(cpu.r[d.rn], cpu.r[d.rm], MM_FALSE, &res, &cflag, &vflag);
                                                cpu.xpsr &= ~(0xF0000000u);
                                                if (res == 0u) cpu.xpsr |= (1u << 30);
                                                if (res & 0x80000000u) cpu.xpsr |= (1u << 31);
                                                if (cflag) cpu.xpsr |= (1u << 29);
                                                if (vflag) cpu.xpsr |= (1u << 28);
                                            } break;
                        case MM_OP_BKPT:
                                            if (opt_gdb) {
                                                mm_gdb_stub_notify_stop(&gdb, 5);
                                            } else {
                                                done = MM_TRUE;
                                            }
                                            break;
                        case MM_OP_LDR_LITERAL: {
                                                    mm_u32 val = 0;
                                                    mm_u32 addr = ((f.pc_fetch + 4u) & ~3u) + d.imm;
                                                    if (!mm_memmap_read(&map, cpu.sec_state, addr, 4u, &val)) {
                                                        if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_TRUE)) done = MM_TRUE;
                                                        return MM_EXEC_CONTINUE;
                                                    }
                                                    if (d.rd == 15u) {
                                                        if (!handle_pc_write(&cpu, &map, val, &it_pattern, &it_remaining, &it_cond)) {
                                                            done = MM_TRUE;
                                                        }
                                                        return MM_EXEC_CONTINUE;
                                                    }
                                                    cpu.r[d.rd] = val;
                                                } break;
                        case MM_OP_LDR_IMM: {
                                                mm_u32 val = 0;
                                                mm_u32 addr = cpu.r[d.rn] + d.imm;
                                                if (!mm_memmap_read(&map, cpu.sec_state, addr, 4u, &val)) {
                                                    if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                    return MM_EXEC_CONTINUE;
                                                }
                                                if (d.rd == 15u) {
                                                    if (!handle_pc_write(&cpu, &map, val, &it_pattern, &it_remaining, &it_cond)) {
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
                                                    if (!handle_pc_write(&cpu, &map, val, &it_pattern, &it_remaining, &it_cond)) {
                                                        done = MM_TRUE;
                                                    }
                                                    return MM_EXEC_CONTINUE;
                                                }
                                                cpu.r[d.rd] = val;
                                            } break;
                        case MM_OP_LDREX: {
                                              mm_u32 val = 0;
                                              mm_u32 addr = cpu.r[d.rn];
                                              if (!mm_memmap_read(&map, cpu.sec_state, addr, 4u, &val)) {
                                                  if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                  return MM_EXEC_CONTINUE;
                                              }
                                              if (d.rd != 15u) {
                                                  cpu.r[d.rd] = val;
                                              }
                                              mm_cpu_excl_set(&cpu, cpu.sec_state, addr, 4u);
                                          } break;
                        case MM_OP_CLREX: {
                                              mm_cpu_excl_clear(&cpu);
                                          } break;
                        case MM_OP_STREX: {
                                              mm_u32 addr = cpu.r[d.rn];
                                              mm_bool ok = mm_cpu_excl_check_and_clear(&cpu, cpu.sec_state, addr, 4u);
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
                        case MM_OP_STR_IMM: {
                                                mm_u32 addr = cpu.r[d.rn] + d.imm;
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
                                                         if (!handle_pc_write(&cpu, &map, val, &it_pattern, &it_remaining, &it_cond)) {
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
                                                         if (!handle_pc_write(&cpu, &map, val, &it_pattern, &it_remaining, &it_cond)) {
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
                                                 mm_u32 offset = cpu.r[d.rm] << (d.imm & 0x1fu);
                                                 mm_u32 addr = cpu.r[d.rn] + offset;
                                                 if (!mm_memmap_write(&map, cpu.sec_state, addr, 1u, cpu.r[d.rd])) {
                                                     if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                             } break;
                        case MM_OP_LDRB_REG: {
                                                 mm_u32 val = 0;
                                                 mm_u32 offset = cpu.r[d.rm] << (d.imm & 0x1fu);
                                                 mm_u32 addr = cpu.r[d.rn] + offset;
                                                 if (!mm_memmap_read(&map, cpu.sec_state, addr, 1u, &val)) {
                                                     if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                                 cpu.r[d.rd] = val & 0xffu;
                                             } break;
                        case MM_OP_LDRB_IMM: {
                                                 mm_u32 addr = cpu.r[d.rn] + d.imm;
                                                 mm_u32 val = 0;
                                                 if (!mm_memmap_read(&map, cpu.sec_state, addr, 1u, &val)) {
                                                     if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                                 cpu.r[d.rd] = val & 0xffu;
                                             } break;
                        case MM_OP_LDRSB_IMM: {
                                                  mm_u32 addr = cpu.r[d.rn] + d.imm;
                                                  mm_u32 val = 0;
                                                  if (!mm_memmap_read(&map, cpu.sec_state, addr, 1u, &val)) {
                                                      if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                      return MM_EXEC_CONTINUE;
                                                  }
                                                  cpu.r[d.rd] = (val & 0x80u) ? (val | 0xffffff80u) : (val & 0xffu);
                                              } break;
                        case MM_OP_LDRSH_IMM: {
                                                  mm_u32 addr = cpu.r[d.rn] + d.imm;
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
                                             /* TODO: model full CMSE attribute queries; return zero for now. */
                                             cpu.r[d.rd] = 0u;
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
                        case MM_OP_STRB_IMM: {
                                                 mm_u32 addr = cpu.r[d.rn] + d.imm;
                                                 if (!mm_memmap_write(&map, cpu.sec_state, addr, 1u, cpu.r[d.rd])) {
                                                     if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                     return MM_EXEC_CONTINUE;
                                                 }
                                             } break;
                        case MM_OP_LDRH_IMM: {
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
                                                    cpu.r[d.rn] = addr;
                                                } break;
                        case MM_OP_LDRH_POST_IMM: {
                                                     mm_u32 base = cpu.r[d.rn];
                                                     mm_u32 val = 0;
                                                     if (!mm_memmap_read(&map, cpu.sec_state, base, 2u, &val)) {
                                                         if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, base, MM_FALSE)) done = MM_TRUE;
                                                         return MM_EXEC_CONTINUE;
                                                     }
                                                     cpu.r[d.rd] = val & 0xffffu;
                                                     cpu.r[d.rn] = base + d.imm;
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
                                                  mm_u32 addr = cpu.r[d.rn] + cpu.r[d.rm];
                                                  mm_u32 val = 0;
                                                  if (!mm_memmap_read(&map, cpu.sec_state, addr, 1u, &val)) {
                                                      if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, addr, MM_FALSE)) done = MM_TRUE;
                                                      return MM_EXEC_CONTINUE;
                                                  }
                                                  cpu.r[d.rd] = (val & 0x80u) ? (val | 0xffffff80u) : (val & 0xffu);
                                              } break;
                        case MM_OP_STRH_IMM: {
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
                                                    cpu.r[d.rn] = addr;
                                                } break;
                        case MM_OP_STRH_POST_IMM: {
                                                     mm_u32 base = cpu.r[d.rn];
                                                     mm_u32 val = cpu.r[d.rd] & 0xffffu;
                                                     if (!mm_memmap_write(&map, cpu.sec_state, base, 2u, val)) {
                                                         if (!raise_mem_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, base, MM_FALSE)) done = MM_TRUE;
                                                         return MM_EXEC_CONTINUE;
                                                     }
                                                     cpu.r[d.rn] = base + d.imm;
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
                                            mm_u32 base = cpu.r[d.rn];
                                            mm_bool exc_return_taken = MM_FALSE;

                                            if (stack_trace_enabled() && d.rn == 13u) {
                                                printf("[STACK_LDMSTM] kind=%s opc=%lu w=%lu mask=0x%04lx base=0x%08lx mode=%d sec=%d sp_active=0x%08lx\n",
                                                       (d.kind == MM_OP_LDM) ? "LDM" : "STM",
                                                       (unsigned long)opc,
                                                       (unsigned long)wbit,
                                                       (unsigned long)mask,
                                                       (unsigned long)base,
                                                       (int)cpu.mode,
                                                       (int)cpu.sec_state,
                                                       (unsigned long)mm_cpu_get_active_sp(&cpu));
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
                                                        if (!handle_pc_write(&cpu, &map, val, &it_pattern, &it_remaining, &it_cond)) {
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


                                            if (wbit && !done && !exc_return_taken) {
                                                mm_bool base_in_list = (mask & (1u << d.rn)) != 0u;
                                                if (d.kind == MM_OP_LDM && base_in_list) {
                                                    /* LDM with base in list: no writeback; base is loaded from memory. */
                                                    break;
                                                }
                                                /* Write-back: IA increments by 4*count; DB decrements by 4*count. */
                                                if (opc == 2u) {
                                                    cpu.r[d.rn] = base - 4u * count;
                                                } else {
                                                    cpu.r[d.rn] = base + 4u * count;
                                                }
                                                if (d.rn == 13u) {
                                                    EXEC_SET_SP(cpu.r[13]);
                                                }
                                            }
                                        } break;
                        case MM_OP_WFI:
                                        cpu.sleeping = MM_TRUE;
                                        break;
                        case MM_OP_WFE:
                                        if (cpu.event_reg) {
                                            cpu.event_reg = MM_FALSE;
                                        } else {
                                            cpu.sleeping = MM_TRUE;
                                        }
                                        break;
                        case MM_OP_SEV:
                                        cpu.event_reg = MM_TRUE;
                                        break;
                        case MM_OP_YIELD:
                                        /* Hint: currently no scheduler hook; treat as NOP. */
                                        break;
                        case MM_OP_SVC: {
                                            mm_u32 ret_pc = f.pc_fetch + d.len;
                                            if (enter_exception == 0 ||
                                                !enter_exception(&cpu, &map, &scs, MM_VECT_SVCALL, ret_pc, cpu.xpsr)) {
                                                done = MM_TRUE;
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
                                                 EXEC_SET_SP(sp - (mm_u32)count * 4u);
                                             }
                                         } break;
                        case MM_OP_POP: {
                                            mm_u32 sp = mm_cpu_get_active_sp(&cpu);
                                            mm_u16 mask = (mm_u16)d.imm;
                                            int reg;
                                            mm_bool exc_return_taken = MM_FALSE;
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
                                                    if (!handle_pc_write(&cpu, &map, val, &it_pattern, &it_remaining, &it_cond)) {
                                                        done = MM_TRUE;
                                                    }
                                                    exc_return_taken = (val & 0xffffff00u) == 0xffffff00u;
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
                                            }
                                        } break;
                        default:
                                        if (!raise_usage_fault(&cpu, &map, &scs, f.pc_fetch, cpu.xpsr, (1u << 16))) {
                                            if (opt_gdb) {
                                                mm_gdb_stub_notify_stop(&gdb, 4);
                                            }
                                            done = MM_TRUE;
                                            return MM_EXEC_CONTINUE;
                                        }
                                        return MM_EXEC_CONTINUE;
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
