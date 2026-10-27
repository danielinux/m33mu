/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2025  Daniele Lacamera <root@danielinux.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "m33mu/cpu.h"
#include "m33mu/decode.h"
#include "m33mu/execute.h"
#include "m33mu/fetch.h"
#include "m33mu/memmap.h"
#include "m33mu/scs.h"

static mm_bool stub_handle_pc_write(struct mm_cpu *cpu,
                                    struct mm_memmap *map,
                                    struct mm_scs *scs,
                                    mm_u32 value,
                                    mm_u8 *it_pattern,
                                    mm_u8 *it_remaining,
                                    mm_u8 *it_cond)
{
    (void)cpu;
    (void)map;
    (void)scs;
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
    (void)ufsr_bits;
    return MM_FALSE;
}

static mm_bool stub_exc_return_unstack(struct mm_cpu *cpu,
                                       struct mm_memmap *map,
                                       struct mm_scs *scs,
                                       mm_u32 exc_ret)
{
    (void)cpu;
    (void)map;
    (void)scs;
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

static int run_sequence(const mm_u8 *code, size_t code_len, struct mm_cpu *cpu)
{
    struct mm_mem mem;
    struct mm_memmap map;
    struct mmio_region regions[1];
    struct mm_scs scs;
    mm_u8 it_pattern = 0;
    mm_u8 it_remaining = 0;
    mm_u8 it_cond = 0;
    mm_bool done = MM_FALSE;

    memset(&map, 0, sizeof(map));
    memset(regions, 0, sizeof(regions));
    memset(&scs, 0, sizeof(scs));
    mm_memmap_init(&map, regions, 1u);

    mem.buffer = code;
    mem.length = code_len;
    mem.base = 0;

    while ((cpu->r[15] & ~1u) < code_len) {
        struct mm_fetch_result fetch;
        struct mm_decoded dec;
        struct mm_execute_ctx ctx;

        fetch = mm_fetch_t32(cpu, &mem);
        if (fetch.fault) {
            return 1;
        }
        dec = mm_decode_t32(&fetch);

        if (!mm_it_should_execute(cpu, &dec, &it_pattern, &it_remaining, &it_cond) &&
            dec.kind != MM_OP_IT) {
            mm_it_advance(cpu, &dec, &it_pattern, &it_remaining, &it_cond);
            continue;
        }

        memset(&ctx, 0, sizeof(ctx));
        ctx.cpu = cpu;
        ctx.map = &map;
        ctx.scs = &scs;
        ctx.fetch = &fetch;
        ctx.dec = &dec;
        ctx.it_pattern = &it_pattern;
        ctx.it_remaining = &it_remaining;
        ctx.it_cond = &it_cond;
        ctx.done = &done;
        ctx.handle_pc_write = stub_handle_pc_write;
        ctx.raise_mem_fault = stub_raise_mem_fault;
        ctx.raise_usage_fault = stub_raise_usage_fault;
        ctx.exc_return_unstack = stub_exc_return_unstack;
        ctx.enter_exception = stub_enter_exception;

        if (mm_execute_decoded(&ctx) != MM_EXEC_OK) {
            return 1;
        }
        if (done) {
            return 1;
        }

        if (it_remaining > 0u && dec.kind != MM_OP_IT) {
            mm_it_advance(cpu, &dec, &it_pattern, &it_remaining, &it_cond);
        }
    }

    return 0;
}

#define XPSR_N (1u << 31)
#define XPSR_Z (1u << 30)
#define XPSR_C (1u << 29)

static int run(const char *name, const mm_u8 *code, size_t len, struct mm_cpu *cpu)
{
    memset(cpu, 0, sizeof(*cpu));
    cpu->r[15] = 1u;
    cpu->xpsr = 0x01000000u;
    if (run_sequence(code, len, cpu) != 0) {
        printf("exec_movs_w_flags_test: %s: sequence execution failed\n", name);
        return 1;
    }
    return 0;
}

int main(void)
{
    /* libgcc __aeabi_i2f: ANDS.W leaves Z set for a positive input, then
     * MOVS.W ip, r0 must clear it or the IT EQ early return is taken. */
    static const mm_u8 i2f[] = {
        0x7f, 0x20,             /* movs   r0, #0x7f                */
        0x10, 0xf0, 0x00, 0x43, /* ands.w r3, r0, #0x80000000 (Z, C) */
        0x5f, 0xea, 0x00, 0x0c, /* movs.w ip, r0                   */
        0x08, 0xbf,             /* it     eq                       */
        0x01, 0x22,             /* moveq  r2, #1                   */
    };
    /* MOV.W without S keeps every flag. */
    static const mm_u8 mov_w[] = {
        0x7f, 0x20,             /* movs   r0, #0x7f                */
        0x10, 0xf0, 0x00, 0x43, /* ands.w r3, r0, #0x80000000      */
        0x4f, 0xea, 0x00, 0x0c, /* mov.w  ip, r0                   */
    };
    /* MOVS.W of a negative value sets N and keeps C. */
    static const mm_u8 negative[] = {
        0x01, 0x20,             /* movs   r0, #1                   */
        0xc0, 0x07,             /* lsls   r0, r0, #31              */
        0x00, 0x21,             /* movs   r1, #0                   */
        0x00, 0x29,             /* cmp    r1, #0 (Z, C)            */
        0x5f, 0xea, 0x00, 0x0c, /* movs.w ip, r0                   */
    };
    struct mm_cpu cpu;

    if (run("i2f", i2f, sizeof(i2f), &cpu) != 0) return 1;
    if (cpu.r[2] != 0u || (cpu.xpsr & (XPSR_N | XPSR_Z | XPSR_C)) != XPSR_C || cpu.r[12] != 0x7fu) {
        printf("exec_movs_w_flags_test: MOVS.W ip, r0: r2=%lu xpsr=0x%08lx\n",
               (unsigned long)cpu.r[2], (unsigned long)cpu.xpsr);
        return 1;
    }
    if (run("mov.w", mov_w, sizeof(mov_w), &cpu) != 0) return 1;
    if ((cpu.xpsr & (XPSR_N | XPSR_Z | XPSR_C)) != (XPSR_Z | XPSR_C) || cpu.r[12] != 0x7fu) {
        printf("exec_movs_w_flags_test: MOV.W changed flags: xpsr=0x%08lx\n", (unsigned long)cpu.xpsr);
        return 1;
    }
    if (run("negative", negative, sizeof(negative), &cpu) != 0) return 1;
    if ((cpu.xpsr & (XPSR_N | XPSR_Z | XPSR_C)) != (XPSR_N | XPSR_C) || cpu.r[12] != 0x80000000u) {
        printf("exec_movs_w_flags_test: MOVS.W negative: xpsr=0x%08lx\n", (unsigned long)cpu.xpsr);
        return 1;
    }
    return 0;
}
