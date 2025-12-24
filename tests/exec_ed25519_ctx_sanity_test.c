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

#include <stdio.h>
#include <string.h>
#include "m33mu/fetch.h"
#include "m33mu/decode.h"
#include "m33mu/memmap.h"
#include "m33mu/scs.h"
#include "m33mu/cpu.h"
#include "m33mu/execute.h"

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
    (void)ufsr_bits;
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

static void setup_ram_map(struct mm_memmap *map, mm_u8 *ram, size_t ram_len)
{
    struct mmio_region regions[1];
    struct mm_target_cfg cfg;

    memset(regions, 0, sizeof(regions));
    mm_memmap_init(map, regions, 1u);

    memset(&cfg, 0, sizeof(cfg));
    cfg.ram_base_s = 0x20000000u;
    cfg.ram_size_s = (mm_u32)ram_len;
    cfg.ram_base_ns = 0x20000000u;
    cfg.ram_size_ns = (mm_u32)ram_len;

    (void)mm_memmap_configure_ram(map, &cfg, ram, MM_FALSE);
}

static int run_sequence(const mm_u8 *code, size_t code_len,
                        struct mm_cpu *cpu, struct mm_memmap *map,
                        struct mm_scs *scs, struct mm_gdb_stub *gdb)
{
    struct mm_mem mem;
    mm_u8 it_pattern = 0;
    mm_u8 it_remaining = 0;
    mm_u8 it_cond = 0;
    mm_bool done = MM_FALSE;

    mem.buffer = code;
    mem.length = code_len;
    mem.base = 0;

    while ((cpu->r[15] & ~1u) < code_len) {
        struct mm_fetch_result fetch = mm_fetch_t32(cpu, &mem);
        struct mm_decoded dec;
        struct mm_execute_ctx ctx;

        if (fetch.fault) {
            printf("exec_ed25519_ctx_sanity_test: fetch fault pc=0x%08lx addr=0x%08lx\n",
                   (unsigned long)fetch.pc_fetch,
                   (unsigned long)fetch.fault_addr);
            return 1;
        }
        dec = mm_decode_t32(&fetch);

        memset(&ctx, 0, sizeof(ctx));
        ctx.cpu = cpu;
        ctx.map = map;
        ctx.scs = scs;
        ctx.gdb = gdb;
        ctx.fetch = &fetch;
        ctx.dec = &dec;
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

        if (dec.undefined) {
            printf("exec_ed25519_ctx_sanity_test: undefined decode pc=0x%08lx raw=0x%08lx\n",
                   (unsigned long)fetch.pc_fetch,
                   (unsigned long)dec.raw);
            return 1;
        }
        if (mm_execute_decoded(&ctx) != MM_EXEC_OK) {
            printf("exec_ed25519_ctx_sanity_test: exec failed pc=0x%08lx raw=0x%08lx kind=%u len=%u\n",
                   (unsigned long)fetch.pc_fetch,
                   (unsigned long)dec.raw,
                   (unsigned)dec.kind,
                   (unsigned)dec.len);
            return 1;
        }
        if (done) {
            printf("exec_ed25519_ctx_sanity_test: done set pc=0x%08lx raw=0x%08lx kind=%u\n",
                   (unsigned long)fetch.pc_fetch,
                   (unsigned long)dec.raw,
                   (unsigned)dec.kind);
            return 1;
        }
    }
    return 0;
}

static int test_ed25519_ctx_sanity_sequence(void)
{
    static const mm_u8 code[] = {
        0x8a, 0xb0,             /* sub sp, #0x28 */
        0x00, 0x23,             /* movs r3, #0 */
        0x04, 0x93,             /* str r3, [sp, #16] */
        0x03, 0x93,             /* str r3, [sp, #12] */
        0x9d, 0xf8, 0x20, 0x31  /* ldrb.w r3, [sp, #0x120] */
    };
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_gdb_stub gdb;
    struct mm_cpu cpu;
    mm_u8 ram[0x400];
    mm_u32 val;
    mm_u32 sp_start = 0x20000080u;
    mm_u32 sp_after = 0x20000058u;
    mm_u32 addr_ctx0 = sp_after + 16u;
    mm_u32 addr_ctx1 = sp_after + 12u;
    mm_u32 addr_ctxlen = sp_after + 0x120u;

    memset(ram, 0xCD, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    memset(&scs, 0, sizeof(scs));
    memset(&gdb, 0, sizeof(gdb));
    memset(&cpu, 0, sizeof(cpu));

    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.msp_s = sp_start;
    cpu.r[13] = sp_start;
    cpu.r[15] = 1u;

    if (!mm_memmap_write(&map, cpu.sec_state, addr_ctxlen, 1u, 0x5au)) {
        printf("exec_ed25519_ctx_sanity_test: setup write failed\n");
        return 1;
    }

    if (run_sequence(code, sizeof(code), &cpu, &map, &scs, &gdb) != 0) {
        printf("exec_ed25519_ctx_sanity_test: execution failed\n");
        return 1;
    }

    if (cpu.r[13] != sp_after) {
        printf("exec_ed25519_ctx_sanity_test: sp mismatch got=0x%08lx\n",
               (unsigned long)cpu.r[13]);
        return 1;
    }
    if (!mm_memmap_read(&map, cpu.sec_state, addr_ctx0, 4u, &val)) {
        printf("exec_ed25519_ctx_sanity_test: read failed (ctx0)\n");
        return 1;
    }
    if (val != 0u) {
        printf("exec_ed25519_ctx_sanity_test: ctx0 store mismatch got=0x%08lx\n",
               (unsigned long)val);
        return 1;
    }
    if (!mm_memmap_read(&map, cpu.sec_state, addr_ctx1, 4u, &val)) {
        printf("exec_ed25519_ctx_sanity_test: read failed (ctx1)\n");
        return 1;
    }
    if (val != 0u) {
        printf("exec_ed25519_ctx_sanity_test: ctx1 store mismatch got=0x%08lx\n",
               (unsigned long)val);
        return 1;
    }
    if (cpu.r[3] != 0x5au) {
        printf("exec_ed25519_ctx_sanity_test: ldrb mismatch got=0x%08lx\n",
               (unsigned long)cpu.r[3]);
        return 1;
    }

    return 0;
}

int main(void)
{
    if (test_ed25519_ctx_sanity_sequence() != 0) {
        return 1;
    }
    return 0;
}
