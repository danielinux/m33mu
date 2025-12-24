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

static int decode_from_bytes(const mm_u8 *bytes, size_t len_bytes, struct mm_decoded *out_dec)
{
    struct mm_mem mem;
    struct mm_cpu cpu;
    struct mm_fetch_result fetch;
    size_t i;

    mem.buffer = bytes;
    mem.length = len_bytes;
    mem.base = 0;
    for (i = 0; i < 16; ++i) {
        cpu.r[i] = 0;
    }
    cpu.r[15] = 1u;
    cpu.xpsr = 0;

    fetch = mm_fetch_t32(&cpu, &mem);
    if (fetch.fault) {
        return 1;
    }
    *out_dec = mm_decode_t32(&fetch);
    return 0;
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

static int test_umull(void)
{
    static const mm_u8 code[] = { 0xa0, 0xfb, 0x04, 0xab }; /* umull sl, fp, r0, r4 */
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_gdb_stub gdb;
    struct mm_cpu cpu;
    mm_u8 ram[64];
    mm_u64 prod;
    struct mm_decoded dec;

    if (decode_from_bytes(code, sizeof(code), &dec) != 0) {
        printf("exec_longmul_test: decode UMULL failed\n");
        return 1;
    }
    if (dec.kind != MM_OP_UMULL || dec.rn != 0u || dec.rm != 4u || dec.rd != 10u || dec.ra != 11u) {
        printf("exec_longmul_test: decode UMULL mismatch kind=%d rn=%u rm=%u rd=%u ra=%u\n",
               (int)dec.kind, (unsigned)dec.rn, (unsigned)dec.rm, (unsigned)dec.rd, (unsigned)dec.ra);
        return 1;
    }

    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    memset(&scs, 0, sizeof(scs));
    memset(&gdb, 0, sizeof(gdb));
    memset(&cpu, 0, sizeof(cpu));

    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    cpu.r[0] = 0x12345678u;
    cpu.r[4] = 0x9abcdef0u;

    if (run_sequence(code, sizeof(code), &cpu, &map, &scs, &gdb) != 0) {
        printf("exec_longmul_test: UMULL execution failed\n");
        return 1;
    }

    prod = (mm_u64)cpu.r[0] * (mm_u64)cpu.r[4];
    if (cpu.r[10] != (mm_u32)prod || cpu.r[11] != (mm_u32)(prod >> 32)) {
        printf("exec_longmul_test: UMULL result mismatch lo=0x%08lx hi=0x%08lx\n",
               (unsigned long)cpu.r[10], (unsigned long)cpu.r[11]);
        return 1;
    }
    return 0;
}

static int test_umlal(void)
{
    static const mm_u8 code[] = { 0xe0, 0xfb, 0x04, 0x8c }; /* umlal r8, ip, r0, r4 */
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_gdb_stub gdb;
    struct mm_cpu cpu;
    mm_u8 ram[64];
    mm_u64 acc;
    struct mm_decoded dec;

    if (decode_from_bytes(code, sizeof(code), &dec) != 0) {
        printf("exec_longmul_test: decode UMLAL failed\n");
        return 1;
    }
    if (dec.kind != MM_OP_UMLAL || dec.rn != 0u || dec.rm != 4u || dec.rd != 8u || dec.ra != 12u) {
        printf("exec_longmul_test: decode UMLAL mismatch kind=%d rn=%u rm=%u rd=%u ra=%u\n",
               (int)dec.kind, (unsigned)dec.rn, (unsigned)dec.rm, (unsigned)dec.rd, (unsigned)dec.ra);
        return 1;
    }

    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    memset(&scs, 0, sizeof(scs));
    memset(&gdb, 0, sizeof(gdb));
    memset(&cpu, 0, sizeof(cpu));

    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    cpu.r[0] = 0x13579bdfu;
    cpu.r[4] = 0x2468ace0u;
    cpu.r[8] = 0x11111111u;
    cpu.r[12] = 0x22222222u;

    if (run_sequence(code, sizeof(code), &cpu, &map, &scs, &gdb) != 0) {
        printf("exec_longmul_test: UMLAL execution failed\n");
        return 1;
    }

    acc = ((mm_u64)0x22222222u << 32) | 0x11111111u;
    acc += (mm_u64)0x13579bdfu * (mm_u64)0x2468ace0u;
    if (cpu.r[8] != (mm_u32)acc || cpu.r[12] != (mm_u32)(acc >> 32)) {
        printf("exec_longmul_test: UMLAL result mismatch lo=0x%08lx hi=0x%08lx\n",
               (unsigned long)cpu.r[8], (unsigned long)cpu.r[12]);
        return 1;
    }
    return 0;
}

static int test_umaal(void)
{
    static const mm_u8 code[] = { 0xe0, 0xfb, 0x65, 0xbc }; /* umaal fp, ip, r0, r5 */
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_gdb_stub gdb;
    struct mm_cpu cpu;
    mm_u8 ram[64];
    mm_u64 acc;
    struct mm_decoded dec;

    if (decode_from_bytes(code, sizeof(code), &dec) != 0) {
        printf("exec_longmul_test: decode UMAAL failed\n");
        return 1;
    }
    if (dec.kind != MM_OP_UMAAL || dec.rn != 0u || dec.rm != 5u || dec.rd != 11u || dec.ra != 12u) {
        printf("exec_longmul_test: decode UMAAL mismatch kind=%d rn=%u rm=%u rd=%u ra=%u\n",
               (int)dec.kind, (unsigned)dec.rn, (unsigned)dec.rm, (unsigned)dec.rd, (unsigned)dec.ra);
        return 1;
    }

    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    memset(&scs, 0, sizeof(scs));
    memset(&gdb, 0, sizeof(gdb));
    memset(&cpu, 0, sizeof(cpu));

    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    cpu.r[0] = 0x0badf00du;
    cpu.r[5] = 0x00c0ffeeu;
    cpu.r[11] = 0x01020304u;
    cpu.r[12] = 0x05060708u;

    if (run_sequence(code, sizeof(code), &cpu, &map, &scs, &gdb) != 0) {
        printf("exec_longmul_test: UMAAL execution failed\n");
        return 1;
    }

    acc = (mm_u64)0x0badf00du * (mm_u64)0x00c0ffeeu;
    acc += (mm_u64)0x01020304u;
    acc += (mm_u64)0x05060708u;
    if (cpu.r[11] != (mm_u32)acc || cpu.r[12] != (mm_u32)(acc >> 32)) {
        printf("exec_longmul_test: UMAAL result mismatch lo=0x%08lx hi=0x%08lx\n",
               (unsigned long)cpu.r[11], (unsigned long)cpu.r[12]);
        return 1;
    }
    return 0;
}

int main(void)
{
    if (test_umull() != 0) return 1;
    if (test_umlal() != 0) return 1;
    if (test_umaal() != 0) return 1;
    return 0;
}
