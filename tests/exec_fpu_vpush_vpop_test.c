/* m33mu -- ARMv8-M Emulator
 *
 * Verifies that VPUSH/VPOP encodings (handled by the existing VLDM/VSTM
 * decoder once Rn=SP and writeback are set) round-trip correctly through
 * a memory-backed stack.  Tests both single-precision (T1, 0xed2d0a*) and
 * double-precision (T2, 0xed2d0b*) forms.
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

static mm_bool stub_handle_pc_write(struct mm_cpu *cpu, struct mm_memmap *map,
                                    struct mm_scs *scs, mm_u32 value,
                                    mm_u8 *itp, mm_u8 *itr, mm_u8 *itc)
{ (void)cpu;(void)map;(void)scs;(void)value;(void)itp;(void)itr;(void)itc; return MM_TRUE; }
static mm_bool stub_mem(struct mm_cpu *c, struct mm_memmap *m, struct mm_scs *s,
                        mm_u32 pc, mm_u32 xp, mm_u32 a, mm_bool e)
{ (void)c;(void)m;(void)s;(void)pc;(void)xp;(void)a;(void)e; return MM_FALSE; }
static mm_bool stub_uf(struct mm_cpu *c, struct mm_memmap *m, struct mm_scs *s,
                       mm_u32 pc, mm_u32 xp, mm_u32 u)
{ (void)c;(void)m;(void)s;(void)pc;(void)xp;(void)u; return MM_FALSE; }
static mm_bool stub_ret(struct mm_cpu *c, struct mm_memmap *m, struct mm_scs *s, mm_u32 r)
{ (void)c;(void)m;(void)s;(void)r; return MM_FALSE; }
static mm_bool stub_enter(struct mm_cpu *c, struct mm_memmap *m, struct mm_scs *s,
                          mm_u32 n, mm_u32 rp, mm_u32 xp)
{ (void)c;(void)m;(void)s;(void)n;(void)rp;(void)xp; return MM_FALSE; }

static int run_decode_check(mm_u32 insn, enum mm_op_kind expected,
                            const char *name)
{
    struct mm_fetch_result fetch;
    struct mm_decoded dec;
    memset(&fetch, 0, sizeof(fetch));
    fetch.insn = insn;
    fetch.len = 4u;
    dec = mm_decode_t32(&fetch);
    if (dec.kind != expected || dec.undefined) {
        printf("%s: kind=%d expected=%d\n", name, (int)dec.kind, (int)expected);
        return 1;
    }
    return 0;
}

int main(void)
{
    /* Decode-side: assembler-emitted encodings should map to VLDM/VSTM. */
    /* vpush {s0-s7} → 0xed2d0a08 */
    if (run_decode_check(0xed2d0a08u, MM_OP_VSTM, "dec_vpush_s")) return 1;
    /* vpush {d0-d3} → 0xed2d0b08 (count=8 single-equivalents, 4 doubles) */
    if (run_decode_check(0xed2d0b08u, MM_OP_VSTM, "dec_vpush_d")) return 1;
    /* vpop {s0-s7} → 0xecbd0a08 */
    if (run_decode_check(0xecbd0a08u, MM_OP_VLDM, "dec_vpop_s")) return 1;
    /* vlstm r0 / vlldm ip (arm-none-eabi-as -march=armv8-m.main+fp): the
     * zero-count VSTM/VLDM space holds the Secure FP context instructions. */
    if (run_decode_check(0xec200a00u, MM_OP_VLSTM, "dec_vlstm_r0")) return 1;
    if (run_decode_check(0xec2d0a00u, MM_OP_VLSTM, "dec_vlstm_sp")) return 1;
    if (run_decode_check(0xec3c0a00u, MM_OP_VLLDM, "dec_vlldm_ip")) return 1;
    /* vpop {d0-d3} → 0xecbd0b08 */
    if (run_decode_check(0xecbd0b08u, MM_OP_VLDM, "dec_vpop_d")) return 1;

    /* Round-trip: push s0..s3, zero them, pop, check restored. */
    {
        struct mm_cpu cpu;
        struct mm_memmap map;
        struct mm_scs scs;
        struct mm_gdb_stub gdb;
        struct mm_fetch_result fetch;
        struct mm_decoded dec;
        struct mm_execute_ctx ctx;
        struct mm_target_cfg cfg;
        struct mmio_region regions[1];
        mm_u8 ram[256];
        mm_u8 itp = 0, itr = 0, itc = 0;
        mm_bool done = MM_FALSE;
        int i;

        memset(&cpu, 0, sizeof(cpu));
        memset(&map, 0, sizeof(map));
        memset(&scs, 0, sizeof(scs));
        memset(&gdb, 0, sizeof(gdb));
        memset(&fetch, 0, sizeof(fetch));
        memset(&dec, 0, sizeof(dec));
        memset(&ctx, 0, sizeof(ctx));
        memset(&cfg, 0, sizeof(cfg));
        memset(regions, 0, sizeof(regions));
        memset(ram, 0, sizeof(ram));

        mm_memmap_init(&map, regions, 1u);
        cfg.ram_base_s = 0x20000000u;
        cfg.ram_size_s = sizeof(ram);
        cfg.ram_base_ns = 0x20000000u;
        cfg.ram_size_ns = sizeof(ram);
        mm_memmap_configure_ram(&map, &cfg, ram, MM_FALSE);

        cpu.sec_state = MM_SECURE;
        cpu.mode = MM_THREAD;
        cpu.r[13] = 0x20000080u;   /* SP mid-RAM, room to grow down */
        for (i = 0; i < 4; ++i) {
            cpu.s[i] = 0x11110000u + (mm_u32)i;
        }
        scs.fpu_present = MM_TRUE;
        scs.cpacr_s = 0x00f00000u;

        ctx.cpu = &cpu; ctx.map = &map; ctx.scs = &scs; ctx.gdb = &gdb;
        ctx.fetch = &fetch; ctx.dec = &dec;
        ctx.it_pattern = &itp; ctx.it_remaining = &itr; ctx.it_cond = &itc;
        ctx.done = &done;
        ctx.handle_pc_write = stub_handle_pc_write;
        ctx.raise_mem_fault = stub_mem;
        ctx.raise_usage_fault = stub_uf;
        ctx.exc_return_unstack = stub_ret;
        ctx.enter_exception = stub_enter;

        /* VPUSH {s0-s3}: P=1, U=0, W=1, count=4, Rn=13.  Use d.imm flags
         * matching the VLDM/VSTM decoder layout: bits 31=U, 30=W, 29=P. */
        dec.kind = MM_OP_VSTM;
        dec.rn = 13u;
        dec.rd = 0u;
        dec.imm = 4u | (0u << 31) | (1u << 30) | (1u << 29);
        dec.len = 4u;

        if (mm_execute_decoded(&ctx) != MM_EXEC_OK || done) {
            printf("vpush exec failed\n");
            return 1;
        }
        if (cpu.r[13] != 0x20000080u - 16u) {
            printf("vpush sp wrong: got 0x%08lx expected 0x%08lx\n",
                   (unsigned long)cpu.r[13],
                   (unsigned long)(0x20000080u - 16u));
            return 1;
        }

        /* Clear registers, then VPOP {s0-s3}: P=0, U=1, W=1. */
        for (i = 0; i < 4; ++i) {
            cpu.s[i] = 0xdeadbeefu;
        }
        memset(&dec, 0, sizeof(dec));
        dec.kind = MM_OP_VLDM;
        dec.rn = 13u;
        dec.rd = 0u;
        dec.imm = 4u | (1u << 31) | (1u << 30) | (0u << 29);
        dec.len = 4u;

        if (mm_execute_decoded(&ctx) != MM_EXEC_OK || done) {
            printf("vpop exec failed\n");
            return 1;
        }
        if (cpu.r[13] != 0x20000080u) {
            printf("vpop sp wrong: got 0x%08lx expected 0x%08lx\n",
                   (unsigned long)cpu.r[13],
                   (unsigned long)0x20000080u);
            return 1;
        }
        for (i = 0; i < 4; ++i) {
            if (cpu.s[i] != 0x11110000u + (mm_u32)i) {
                printf("vpop s%d wrong: got 0x%08lx expected 0x%08lx\n",
                       i,
                       (unsigned long)cpu.s[i],
                       (unsigned long)(0x11110000u + (mm_u32)i));
                return 1;
            }
        }
    }

    printf("exec_fpu_vpush_vpop_test: OK\n");
    return 0;
}
