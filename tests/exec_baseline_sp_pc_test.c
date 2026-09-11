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

/*
 * The 16-bit high-register ADD/MOV group writing SP or PC.  Thumb-2 code
 * rarely uses these forms, so they were only exercised once an ARMv8-M
 * Baseline (Cortex-M23) target was added:
 *
 *  - Baseline has no wide "SUB SP, SP, #imm12" and its immediate form tops out
 *    at 508 bytes, so a large stack frame is built with
 *    "ldr rN, =-size; add sp, rN".  R13 in struct mm_cpu is only a mirror of
 *    the banked stack pointer, so that has to reach mm_cpu_set_active_sp().
 *  - Baseline switch dispatch is "mov pc, rN" through a table of even
 *    addresses.  MOV/ADD writing PC is ALUWritePC (ARM ARM B1.4.2), which
 *    ignores bit 0, unlike the BXWritePC of BX/BLX/POP/LDR which faults on an
 *    even target.
 */

#include <stdio.h>
#include <string.h>
#include "m33mu/cpu.h"
#include "m33mu/decode.h"
#include "m33mu/execute.h"
#include "m33mu/fetch.h"
#include "m33mu/mem.h"
#include "m33mu/memmap.h"
#include "m33mu/scs.h"
#include "m33mu/target.h"

static mm_u32 g_last_pc_write_value;
static int g_handle_pc_write_calls;
static mm_u32 g_last_ufsr_bits;
static int g_raise_usage_fault_calls;

static mm_bool stub_handle_pc_write(struct mm_cpu *cpu,
                                    struct mm_memmap *map,
                                    struct mm_scs *scs,
                                    mm_u32 value,
                                    mm_u8 *it_pattern,
                                    mm_u8 *it_remaining,
                                    mm_u8 *it_cond)
{
    (void)cpu; (void)map; (void)scs;
    (void)it_pattern; (void)it_remaining; (void)it_cond;
    g_last_pc_write_value = value;
    g_handle_pc_write_calls++;
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
    (void)cpu; (void)map; (void)scs;
    (void)fault_pc; (void)fault_xpsr; (void)addr; (void)is_exec;
    return MM_FALSE;
}

static mm_bool stub_raise_usage_fault(struct mm_cpu *cpu,
                                      struct mm_memmap *map,
                                      struct mm_scs *scs,
                                      mm_u32 fault_pc,
                                      mm_u32 fault_xpsr,
                                      mm_u32 ufsr_bits)
{
    (void)cpu; (void)map; (void)scs; (void)fault_pc; (void)fault_xpsr;
    g_last_ufsr_bits = ufsr_bits;
    g_raise_usage_fault_calls++;
    return MM_FALSE;
}

static mm_bool stub_exc_return_unstack(struct mm_cpu *cpu,
                                       struct mm_memmap *map,
                                       struct mm_scs *scs,
                                       mm_u32 exc_ret)
{
    (void)cpu; (void)map; (void)scs; (void)exc_ret;
    return MM_FALSE;
}

static mm_bool stub_enter_exception(struct mm_cpu *cpu,
                                    struct mm_memmap *map,
                                    struct mm_scs *scs,
                                    mm_u32 exc_num,
                                    mm_u32 return_pc,
                                    mm_u32 xpsr_in)
{
    (void)cpu; (void)map; (void)scs;
    (void)exc_num; (void)return_pc; (void)xpsr_in;
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

static int decode_halfword(mm_u16 hw, struct mm_decoded *out_dec)
{
    struct mm_mem mem;
    struct mm_cpu cpu;
    struct mm_fetch_result fetch;
    mm_u8 bytes[2];
    size_t i;

    bytes[0] = (mm_u8)(hw & 0xffu);
    bytes[1] = (mm_u8)(hw >> 8);
    mem.buffer = bytes;
    mem.length = sizeof(bytes);
    mem.base = 0;
    for (i = 0; i < 16; ++i) cpu.r[i] = 0;
    cpu.r[15] = 1u;
    cpu.xpsr = 0;

    fetch = mm_fetch_t32(&cpu, &mem);
    if (fetch.fault) return 1;
    *out_dec = mm_decode_t32(&fetch);
    return out_dec->undefined ? 1 : 0;
}

static int exec_one(struct mm_cpu *cpu,
                    struct mm_memmap *map,
                    struct mm_scs *scs,
                    struct mm_decoded *dec)
{
    struct mm_fetch_result fetch;
    struct mm_gdb_stub gdb;
    struct mm_execute_ctx ctx;
    mm_u8 it_pattern = 0u;
    mm_u8 it_remaining = 0u;
    mm_u8 it_cond = 0u;
    mm_bool done = MM_FALSE;

    memset(&fetch, 0, sizeof(fetch));
    memset(&gdb, 0, sizeof(gdb));
    memset(&ctx, 0, sizeof(ctx));
    fetch.pc_fetch = cpu->r[15] & ~1u;
    ctx.cpu = cpu;
    ctx.map = map;
    ctx.scs = scs;
    ctx.gdb = &gdb;
    ctx.fetch = &fetch;
    ctx.dec = dec;
    ctx.nvic = 0;
    ctx.it_pattern = &it_pattern;
    ctx.it_remaining = &it_remaining;
    ctx.it_cond = &it_cond;
    ctx.done = &done;
    ctx.handle_pc_write = stub_handle_pc_write;
    ctx.raise_mem_fault = stub_raise_mem_fault;
    ctx.raise_usage_fault = stub_raise_usage_fault;
    ctx.exc_return_unstack = stub_exc_return_unstack;
    ctx.enter_exception = stub_enter_exception;

    if (mm_execute_decoded(&ctx) != MM_EXEC_OK) return 1;
    return done ? 1 : 0;
}

static void reset_probes(void)
{
    g_last_pc_write_value = 0u;
    g_handle_pc_write_calls = 0;
    g_last_ufsr_bits = 0u;
    g_raise_usage_fault_calls = 0;
}

/* "add sp, r4" with a negative r4 is how Thumb-1 allocates a frame larger
 * than the 508-byte immediate form allows. */
static int test_add_sp_reg_updates_active_sp(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[128];
    mm_u32 sp;

    if (decode_halfword(0x44a5u, &dec) != 0) {   /* add sp, r4 */
        printf("add_sp_reg: decode failed\n");
        return 1;
    }
    if (dec.kind != MM_OP_ADD_REG || dec.rd != 13u || dec.rm != 4u) {
        printf("add_sp_reg: kind=%d rd=%u rm=%u\n",
               (int)dec.kind, (unsigned)dec.rd, (unsigned)dec.rm);
        return 1;
    }

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    mm_cpu_set_active_sp(&cpu, 0x20008000u);
    cpu.r[4] = (mm_u32)(-644);

    reset_probes();
    if (exec_one(&cpu, &map, &scs, &dec) != 0) {
        printf("add_sp_reg: execute failed\n");
        return 1;
    }

    sp = mm_cpu_get_active_sp(&cpu);
    if (sp != 0x20008000u - 644u) {
        printf("add_sp_reg: active sp=0x%08lx exp 0x%08lx\n",
               (unsigned long)sp, (unsigned long)(0x20008000u - 644u));
        return 1;
    }
    if (cpu.msp_s != sp) {
        printf("add_sp_reg: msp_s=0x%08lx not synced with sp=0x%08lx\n",
               (unsigned long)cpu.msp_s, (unsigned long)sp);
        return 1;
    }
    if (cpu.r[13] != sp) {
        printf("add_sp_reg: r13 mirror=0x%08lx sp=0x%08lx\n",
               (unsigned long)cpu.r[13], (unsigned long)sp);
        return 1;
    }
    return 0;
}

/* Same instruction shape, but selecting PSP, to prove the banked pointer and
 * not just msp_s is updated. */
static int test_add_sp_reg_updates_psp(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[128];

    if (decode_halfword(0x44a5u, &dec) != 0) return 1;

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.control_s = 2u;                 /* SPSEL: use PSP */
    cpu.r[15] = 1u;
    mm_cpu_set_active_sp(&cpu, 0x20007000u);
    cpu.r[4] = (mm_u32)(-64);

    reset_probes();
    if (exec_one(&cpu, &map, &scs, &dec) != 0) return 1;

    if (cpu.psp_s != 0x20007000u - 64u) {
        printf("add_sp_reg_psp: psp_s=0x%08lx exp 0x%08lx\n",
               (unsigned long)cpu.psp_s, (unsigned long)(0x20007000u - 64u));
        return 1;
    }
    return 0;
}

/* "mov pc, r2" is ALUWritePC: an even target is a normal branch, not a fault. */
static int test_mov_pc_reg_even_target_branches(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[128];

    if (decode_halfword(0x4697u, &dec) != 0) {   /* mov pc, r2 */
        printf("mov_pc: decode failed\n");
        return 1;
    }
    if (dec.kind != MM_OP_MOV_REG || dec.rd != 15u || dec.rm != 2u) {
        printf("mov_pc: kind=%d rd=%u rm=%u\n",
               (int)dec.kind, (unsigned)dec.rd, (unsigned)dec.rm);
        return 1;
    }

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    cpu.r[2] = 0x000010aeu;             /* even: a Thumb-1 jump-table entry */

    reset_probes();
    if (exec_one(&cpu, &map, &scs, &dec) != 0) {
        printf("mov_pc: execute reported done\n");
        return 1;
    }
    if (g_raise_usage_fault_calls != 0) {
        printf("mov_pc: raised usage fault bits=0x%08lx\n",
               (unsigned long)g_last_ufsr_bits);
        return 1;
    }
    if (g_handle_pc_write_calls != 1) {
        printf("mov_pc: pc writes=%d\n", g_handle_pc_write_calls);
        return 1;
    }
    if (g_last_pc_write_value != 0x000010afu) {
        printf("mov_pc: target=0x%08lx exp 0x000010af\n",
               (unsigned long)g_last_pc_write_value);
        return 1;
    }
    return 0;
}

static int test_mov_pc_reg_odd_target_preserved(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[128];

    if (decode_halfword(0x4697u, &dec) != 0) return 1;

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    cpu.r[2] = 0x000010afu;

    reset_probes();
    if (exec_one(&cpu, &map, &scs, &dec) != 0) return 1;
    if (g_last_pc_write_value != 0x000010afu) {
        printf("mov_pc_odd: target=0x%08lx\n",
               (unsigned long)g_last_pc_write_value);
        return 1;
    }
    return 0;
}

/* An EXC_RETURN magic value must reach handle_pc_write() untouched, or
 * exception return through "mov pc, lr" stops being recognised. */
static int test_mov_pc_reg_exc_return_untouched(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[128];

    if (decode_halfword(0x4697u, &dec) != 0) return 1;

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_HANDLER;
    cpu.r[15] = 1u;
    cpu.r[2] = 0xfffffffcu;             /* even EXC_RETURN */

    reset_probes();
    (void)exec_one(&cpu, &map, &scs, &dec);
    if (g_last_pc_write_value != 0xfffffffcu) {
        printf("mov_pc_excret: target=0x%08lx exp 0xfffffffc\n",
               (unsigned long)g_last_pc_write_value);
        return 1;
    }
    return 0;
}

/* BX keeps BXWritePC semantics: the even target must arrive unmodified so the
 * INVSTATE check downstream still sees it. */
static int test_bx_even_target_not_rewritten(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[128];

    if (decode_halfword(0x4710u, &dec) != 0) {   /* bx r2 */
        printf("bx: decode failed\n");
        return 1;
    }
    if (dec.kind != MM_OP_BX) {
        printf("bx: kind=%d\n", (int)dec.kind);
        return 1;
    }

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    cpu.r[dec.rm] = 0x000010aeu;

    reset_probes();
    (void)exec_one(&cpu, &map, &scs, &dec);
    if (g_handle_pc_write_calls == 1 && g_last_pc_write_value != 0x000010aeu) {
        printf("bx: target=0x%08lx exp 0x000010ae (unmodified)\n",
               (unsigned long)g_last_pc_write_value);
        return 1;
    }
    return 0;
}

int main(void)
{
    if (test_add_sp_reg_updates_active_sp() != 0) {
        printf("FAIL: add_sp_reg_updates_active_sp\n");
        return 1;
    }
    if (test_add_sp_reg_updates_psp() != 0) {
        printf("FAIL: add_sp_reg_updates_psp\n");
        return 1;
    }
    if (test_mov_pc_reg_even_target_branches() != 0) {
        printf("FAIL: mov_pc_reg_even_target_branches\n");
        return 1;
    }
    if (test_mov_pc_reg_odd_target_preserved() != 0) {
        printf("FAIL: mov_pc_reg_odd_target_preserved\n");
        return 1;
    }
    if (test_mov_pc_reg_exc_return_untouched() != 0) {
        printf("FAIL: mov_pc_reg_exc_return_untouched\n");
        return 1;
    }
    if (test_bx_even_target_not_rewritten() != 0) {
        printf("FAIL: bx_even_target_not_rewritten\n");
        return 1;
    }
    printf("exec_baseline_sp_pc_test: OK\n");
    return 0;
}
