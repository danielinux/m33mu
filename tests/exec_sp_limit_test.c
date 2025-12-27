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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 */

#include <stdio.h>
#include <string.h>
#include "m33mu/execute.h"
#include "m33mu/cpu.h"
#include "m33mu/memmap.h"
#include "m33mu/scs.h"

#define UFSR_STKOF (1u << 20)

static mm_u32 g_ufsr_bits;
static int g_ufsr_count;

static mm_bool stub_handle_pc_write(struct mm_cpu *cpu,
                                    struct mm_memmap *map,
                                    mm_u32 value,
                                    mm_u8 *it_pattern,
                                    mm_u8 *it_remaining,
                                    mm_u8 *it_cond)
{
    (void)cpu;
    (void)map;
    (void)value;
    (void)it_pattern;
    (void)it_remaining;
    (void)it_cond;
    return MM_TRUE;
}

static mm_bool stub_raise_mem_fault(struct mm_cpu *cpu,
                                    struct mm_memmap *map,
                                    struct mm_scs *scs,
                                    mm_u32 fault_pc,
                                    mm_u32 fault_xpsr,
                                    mm_u32 addr,
                                    mm_bool is_exec)
{
    (void)cpu;
    (void)map;
    (void)scs;
    (void)fault_pc;
    (void)fault_xpsr;
    (void)addr;
    (void)is_exec;
    return MM_FALSE;
}

static mm_bool stub_raise_usage_fault(struct mm_cpu *cpu,
                                      struct mm_memmap *map,
                                      struct mm_scs *scs,
                                      mm_u32 fault_pc,
                                      mm_u32 fault_xpsr,
                                      mm_u32 ufsr_bits)
{
    (void)cpu;
    (void)map;
    (void)scs;
    (void)fault_pc;
    (void)fault_xpsr;
    g_ufsr_bits = ufsr_bits;
    g_ufsr_count++;
    return MM_FALSE;
}

static mm_bool stub_exc_return_unstack(struct mm_cpu *cpu,
                                       struct mm_memmap *map,
                                       mm_u32 exc_ret)
{
    (void)cpu;
    (void)map;
    (void)exc_ret;
    return MM_FALSE;
}

static mm_bool stub_enter_exception(struct mm_cpu *cpu,
                                    struct mm_memmap *map,
                                    struct mm_scs *scs,
                                    mm_u32 exc_num,
                                    mm_u32 return_pc,
                                    mm_u32 xpsr_in)
{
    (void)cpu;
    (void)map;
    (void)scs;
    (void)exc_num;
    (void)return_pc;
    (void)xpsr_in;
    return MM_FALSE;
}

static int run_decoded(struct mm_cpu *cpu,
                       struct mm_decoded *dec,
                       struct mm_fetch_result *fetch,
                       mm_bool *done_out)
{
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_gdb_stub gdb;
    struct mm_execute_ctx ctx;
    mm_u8 it_pattern = 0;
    mm_u8 it_remaining = 0;
    mm_u8 it_cond = 0;
    mm_bool done = MM_FALSE;

    memset(&map, 0, sizeof(map));
    memset(&scs, 0, sizeof(scs));
    memset(&gdb, 0, sizeof(gdb));
    memset(&ctx, 0, sizeof(ctx));

    ctx.cpu = cpu;
    ctx.map = &map;
    ctx.scs = &scs;
    ctx.gdb = &gdb;
    ctx.fetch = fetch;
    ctx.dec = dec;
    ctx.opt_dump = MM_FALSE;
    ctx.opt_gdb = MM_FALSE;
    ctx.it_pattern = &it_pattern;
    ctx.it_remaining = &it_remaining;
    ctx.it_cond = &it_cond;
    ctx.done = &done;
    ctx.handle_pc_write = stub_handle_pc_write;
    ctx.raise_mem_fault = stub_raise_mem_fault;
    ctx.raise_usage_fault = stub_raise_usage_fault;
    ctx.exc_return_unstack = stub_exc_return_unstack;
    ctx.enter_exception = stub_enter_exception;

    {
        enum mm_exec_status status = mm_execute_decoded(&ctx);
        if (status != MM_EXEC_OK && status != MM_EXEC_CONTINUE) {
            return 1;
        }
    }
    if (done_out) {
        *done_out = done;
    }
    return 0;
}

static int test_msplim_fault(void)
{
    struct mm_cpu cpu;
    struct mm_decoded dec;
    struct mm_fetch_result fetch;
    mm_bool done = MM_FALSE;

    memset(&cpu, 0, sizeof(cpu));
    memset(&dec, 0, sizeof(dec));
    memset(&fetch, 0, sizeof(fetch));

    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.control_s = 0u;
    cpu.msp_s = 0x20001000u;
    cpu.r[13] = 0x20001000u;
    cpu.msplim_s = 0x20001000u;

    dec.kind = MM_OP_SUB_SP_IMM;
    dec.rd = 13u;
    dec.imm = 4u;
    dec.len = 2u;
    fetch.pc_fetch = 0x100u;

    g_ufsr_bits = 0u;
    g_ufsr_count = 0;

    if (run_decoded(&cpu, &dec, &fetch, &done) != 0) {
        return 1;
    }
    if (!done) {
        printf("msplim: expected done flag on fault\n");
        return 1;
    }
    if (g_ufsr_count != 1 || g_ufsr_bits != UFSR_STKOF) {
        printf("msplim: expected UFSR_STKOF, got bits=0x%lx count=%d\n",
               (unsigned long)g_ufsr_bits, g_ufsr_count);
        return 1;
    }
    if (cpu.msp_s != 0x20001000u || cpu.r[13] != 0x20001000u) {
        printf("msplim: SP changed on fault msp=0x%lx r13=0x%lx\n",
               (unsigned long)cpu.msp_s, (unsigned long)cpu.r[13]);
        return 1;
    }
    return 0;
}

static int test_psplim_ok(void)
{
    struct mm_cpu cpu;
    struct mm_decoded dec;
    struct mm_fetch_result fetch;
    mm_bool done = MM_FALSE;

    memset(&cpu, 0, sizeof(cpu));
    memset(&dec, 0, sizeof(dec));
    memset(&fetch, 0, sizeof(fetch));

    cpu.sec_state = MM_NONSECURE;
    cpu.mode = MM_THREAD;
    cpu.control_ns = 0x2u;
    cpu.psp_ns = 0x20001000u;
    cpu.r[13] = 0x20001000u;
    cpu.psplim_ns = 0x20000000u;

    dec.kind = MM_OP_SUB_SP_IMM;
    dec.rd = 13u;
    dec.imm = 4u;
    dec.len = 2u;
    fetch.pc_fetch = 0x200u;

    g_ufsr_bits = 0u;
    g_ufsr_count = 0;

    if (run_decoded(&cpu, &dec, &fetch, &done) != 0) {
        return 1;
    }
    if (done) {
        printf("psplim: unexpected done flag on valid update\n");
        return 1;
    }
    if (g_ufsr_count != 0) {
        printf("psplim: unexpected usage fault count=%d bits=0x%lx\n",
               g_ufsr_count, (unsigned long)g_ufsr_bits);
        return 1;
    }
    if (cpu.psp_ns != 0x20000ffcu || cpu.r[13] != 0x20000ffcu) {
        printf("psplim: SP not updated psp=0x%lx r13=0x%lx\n",
               (unsigned long)cpu.psp_ns, (unsigned long)cpu.r[13]);
        return 1;
    }
    return 0;
}

int main(void)
{
    if (test_msplim_fault() != 0) return 1;
    if (test_psplim_ok() != 0) return 1;
    return 0;
}
