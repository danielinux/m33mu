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

        if (mm_execute_decoded(&ctx) != MM_EXEC_OK) {
            return 1;
        }
        if (done) {
            return 1;
        }
    }
    return 0;
}

static int test_verify_authenticity_bitmask(void)
{
    static const mm_u8 code[] = {
        0x01, 0x22, /* movs r2, #1 */
        0x08, 0x9b, /* ldr r3, [sp, #32] */
        0x9a, 0x40, /* lsls r2, r3 */
        0x09, 0x9b, /* ldr r3, [sp, #36] */
        0x1a, 0x40, /* ands r2, r3 */
        0x01, 0x21, /* movs r1, #1 */
        0x08, 0x9b, /* ldr r3, [sp, #32] */
        0x01, 0xfa, 0x03, 0xf3, /* lsl.w r3, r1, r3 */
        0x9a, 0x42  /* cmp r2, r3 */
    };
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_gdb_stub gdb;
    struct mm_cpu cpu;
    mm_u8 ram[128];
    mm_u32 bit = 1u;
    mm_u32 mask = 0xffffffffu;

    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    memset(&scs, 0, sizeof(scs));
    memset(&gdb, 0, sizeof(gdb));
    memset(&cpu, 0, sizeof(cpu));

    memcpy(&ram[32], &bit, sizeof(bit));
    memcpy(&ram[36], &mask, sizeof(mask));

    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[13] = 0x20000000u;
    cpu.r[15] = 1u;

    if (run_sequence(code, sizeof(code), &cpu, &map, &scs, &gdb) != 0) {
        printf("exec_verify_authenticity_test: execution failed\n");
        return 1;
    }
    if (cpu.r[1] != 1u) {
        printf("exec_verify_authenticity_test: r1 mismatch got=0x%08lx\n", (unsigned long)cpu.r[1]);
        return 1;
    }
    if (cpu.r[2] != 2u) {
        printf("exec_verify_authenticity_test: r2 mismatch got=0x%08lx\n", (unsigned long)cpu.r[2]);
        return 1;
    }
    if (cpu.r[3] != 2u) {
        printf("exec_verify_authenticity_test: r3 mismatch got=0x%08lx\n", (unsigned long)cpu.r[3]);
        return 1;
    }
    if ((cpu.xpsr & (1u << 30)) == 0u) {
        printf("exec_verify_authenticity_test: Z flag not set\n");
        return 1;
    }
    return 0;
}

int main(void)
{
    if (test_verify_authenticity_bitmask() != 0) {
        printf("FAIL: verify_authenticity_bitmask\n");
        return 1;
    }
    printf("PASS: verify_authenticity_bitmask\n");
    return 0;
}
