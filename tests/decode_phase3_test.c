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
#include "m33mu/fetch.h"
#include "m33mu/decode.h"
#include "m33mu/mem.h"
#include "m33mu/cpu.h"

static int decode_insn(mm_u16 hw1, mm_u16 hw2, struct mm_decoded *out_dec)
{
    mm_u8 bytes[4];
    struct mm_mem mem;
    struct mm_cpu cpu;
    struct mm_fetch_result fetch;
    size_t i;

    bytes[0] = (mm_u8)(hw1 & 0xffu);
    bytes[1] = (mm_u8)((hw1 >> 8) & 0xffu);
    bytes[2] = (mm_u8)(hw2 & 0xffu);
    bytes[3] = (mm_u8)((hw2 >> 8) & 0xffu);

    mem.buffer = bytes;
    mem.length = sizeof(bytes);
    mem.base = 0;
    for (i = 0; i < 16; ++i) cpu.r[i] = 0;
    cpu.r[15] = 1u;
    cpu.xpsr = 0;

    fetch = mm_fetch_t32(&cpu, &mem);
    if (fetch.fault) return 1;
    *out_dec = mm_decode_t32(&fetch);
    return 0;
}

static int test_b_w(void)
{
    struct mm_decoded dec;
    if (decode_insn(0xf000u, 0xb801u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_B_UNCOND_WIDE) return 1;
    return 0;
}

static int test_b_cond_w(void)
{
    struct mm_decoded dec;
    if (decode_insn(0xf040u, 0x8001u, &dec) != 0) return 1; /* bne.w +6 */
    if (dec.kind != MM_OP_B_COND_WIDE) return 1;
    if (dec.cond != MM_COND_NE) return 1;
    return 0;
}

static int test_movw(void)
{
    struct mm_decoded dec;
    if (decode_insn(0xf240u, 0x0000u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_MOVW) return 1;
    return 0;
}

static int test_movt(void)
{
    struct mm_decoded dec;
    if (decode_insn(0xf2c0u, 0x0000u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_MOVT) return 1;
    return 0;
}

static int test_mvn_imm_w(void)
{
    struct mm_decoded dec;
    if (decode_insn(0xf06fu, 0x0000u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_MVN_IMM) return 1;
    if (dec.rd != 0u) return 1;
    return 0;
}

static int test_mvn_reg_w(void)
{
    struct mm_decoded dec;
    if (decode_insn(0xea6fu, 0x0102u, &dec) != 0) return 1; /* MVN{S}.W r1, r2 */
    if (dec.kind != MM_OP_MVN_REG) return 1;
    if (dec.rd != 1u || dec.rm != 2u) return 1;
    return 0;
}

static int test_ubfx(void)
{
    struct mm_decoded dec;
    if (decode_insn(0xf3c0u, 0x0000u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_UBFX) return 1;
    return 0;
}

static int test_sbfx(void)
{
    struct mm_decoded dec;
    if (decode_insn(0xf340u, 0x0000u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_SBFX) return 1;
    return 0;
}

static int test_bfi(void)
{
    struct mm_decoded dec;
    if (decode_insn(0xf360u, 0x0000u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_BFI) return 1;
    return 0;
}

static int test_bfc(void)
{
    struct mm_decoded dec;
    if (decode_insn(0xf36fu, 0x200fu, &dec) != 0) return 1;
    if (dec.kind != MM_OP_BFC) return 1;
    return 0;
}

static int test_tbb(void)
{
    struct mm_decoded dec;
    if (decode_insn(0xe8d2u, 0xf007u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_TBB) return 1;
    if (dec.rn != 2u || dec.rm != 7u) return 1;
    return 0;
}

static int test_tbh(void)
{
    struct mm_decoded dec;
    if (decode_insn(0xe8d2u, 0xf017u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_TBH) return 1;
    if (dec.rn != 2u || dec.rm != 7u) return 1;
    return 0;
}

static int test_clrex(void)
{
    struct mm_decoded dec;
    if (decode_insn(0xf3bfu, 0x8f2fu, &dec) != 0) return 1;
    if (dec.kind != MM_OP_CLREX) return 1;
    return 0;
}

static int test_dsb_dmb_isb(void)
{
    struct mm_decoded dec;
    if (decode_insn(0xf3bfu, 0x8f4fu, &dec) != 0) return 1; /* dsb sy */
    if (dec.kind != MM_OP_DSB) return 1;
    if (decode_insn(0xf3bfu, 0x8f5fu, &dec) != 0) return 1; /* dmb sy */
    if (dec.kind != MM_OP_DMB) return 1;
    if (decode_insn(0xf3bfu, 0x8f6fu, &dec) != 0) return 1; /* isb sy */
    if (dec.kind != MM_OP_ISB) return 1;
    return 0;
}

static int test_msr_psr_fields_decode(void)
{
    struct mm_decoded dec;
    /* GCC CMSE stubs may emit MSR to PSR fields; ensure we don't mis-decode it as B<cond>.W. */
    if (decode_insn(0xf38eu, 0x8c00u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_MSR) return 1;
    if (dec.len != 4u) return 1;
    if (dec.rm != 14u) return 1;
    if (((dec.imm >> 8) & 0xfu) != 0xcu) return 1; /* mask */
    if ((dec.imm & 0xffu) != 0x00u) return 1;      /* sysm */
    return 0;
}

static int test_sxtb_w_decode(void)
{
    struct mm_decoded dec;
    /* SXTB.W r1, r2, ROR #8: hw1=0xfa4f, hw2=0xf192 */
    if (decode_insn(0xfa4fu, 0xf192u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_SXTB) return 1;
    if (dec.rd != 1u || dec.rm != 2u) return 1;
    if (dec.imm != 8u) return 1;
    return 0;
}

static int test_sxth_w_decode(void)
{
    struct mm_decoded dec;
    /* SXTH.W r1, r2, ROR #8: hw1=0xfa0f, hw2=0xf192 */
    if (decode_insn(0xfa0fu, 0xf192u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_SXTH) return 1;
    if (dec.rd != 1u || dec.rm != 2u) return 1;
    if (dec.imm != 8u) return 1;
    return 0;
}

static int test_uxth_w_decode(void)
{
    struct mm_decoded dec;
    /* UXTH.W r1, r2, ROR #8: hw1=0xfa1f, hw2=0xf192 */
    if (decode_insn(0xfa1fu, 0xf192u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_UXTH) return 1;
    if (dec.rd != 1u || dec.rm != 2u) return 1;
    if (dec.imm != 8u) return 1;
    return 0;
}

static int test_ldrsb_w_imm12(void)
{
    struct mm_decoded dec;
    /* LDRSB.W r1, [r2, #0x10] : hw1=0xf992, hw2=0x1010 */
    if (decode_insn(0xf992u, 0x1010u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_LDRSB_IMM) return 1;
    if (dec.rd != 1u || dec.rn != 2u) return 1;
    if (dec.imm != 0x10u) return 1;
    return 0;
}

static int test_ldrsb_w_imm8_pre(void)
{
    struct mm_decoded dec;
    /* LDRSB.W r3, [r4, #0x20] using T2: hw1=0xf914, hw2=0x3e20 (P=1,U=1,W=0,bit11=1) */
    if (decode_insn(0xf914u, 0x3e20u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_LDRSB_IMM) return 1;
    if (dec.rd != 3u || dec.rn != 4u) return 1;
    if (dec.imm != 0x20u) return 1;
    return 0;
}

static int test_ldrsh_w_imm12(void)
{
    struct mm_decoded dec;
    /* LDRSH.W r1, [r2, #0x10] : hw1=0xf9b2, hw2=0x1010 */
    if (decode_insn(0xf9b2u, 0x1010u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_LDRSH_IMM) return 1;
    if (dec.rd != 1u || dec.rn != 2u) return 1;
    if (dec.imm != 0x10u) return 1;
    return 0;
}

static int test_ldrsh_w_imm8_pre(void)
{
    struct mm_decoded dec;
    /* LDRSH.W r3, [r4, #0x20] using T2: hw1=0xf934, hw2=0x3e20 (P=1,U=1,W=0,bit11=1) */
    if (decode_insn(0xf934u, 0x3e20u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_LDRSH_IMM) return 1;
    if (dec.rd != 3u || dec.rn != 4u) return 1;
    if (dec.imm != 0x20u) return 1;
    return 0;
}

static int test_clz_w_decode(void)
{
    struct mm_decoded dec;
    /* CLZ.W r1, r2: hw1=0xfab2, hw2=0xf182 (rm low nibble matches, rd=1) */
    if (decode_insn(0xfab2u, 0xf182u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_CLZ) return 1;
    if (dec.rd != 1u || dec.rm != 2u) return 1;
    return 0;
}

static int test_rbit_w_decode(void)
{
    struct mm_decoded dec;
    /* RBIT.W r1, r2: hw1=0xfa92, hw2=0xf1a2 (rd=1, rm consistent) */
    if (decode_insn(0xfa92u, 0xf1a2u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_RBIT) return 1;
    if (dec.rd != 1u || dec.rm != 2u) return 1;
    return 0;
}

static int test_tt_decode(void)
{
    struct mm_decoded dec;
    /* TT r0, r1: hw1=0xe841, hw2=0xf000 */
    if (decode_insn(0xe841u, 0xf000u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_TT) return 1;
    if (dec.rd != 0u || dec.rn != 1u) return 1;
    return 0;
}

static int test_ttt_decode(void)
{
    struct mm_decoded dec;
    /* TTT r2, r3: hw1=0xe843, hw2=0xf240 */
    if (decode_insn(0xe843u, 0xf240u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_TTT) return 1;
    if (dec.rd != 2u || dec.rn != 3u) return 1;
    return 0;
}

static int test_tta_decode(void)
{
    struct mm_decoded dec;
    /* TTA r4, r5: hw1=0xe845, hw2=0xf480 */
    if (decode_insn(0xe845u, 0xf480u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_TTA) return 1;
    if (dec.rd != 4u || dec.rn != 5u) return 1;
    return 0;
}

static int test_ttat_decode(void)
{
    struct mm_decoded dec;
    /* TTAT r6, r7: hw1=0xe847, hw2=0xf6c0 */
    if (decode_insn(0xe847u, 0xf6c0u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_TTAT) return 1;
    if (dec.rd != 6u || dec.rn != 7u) return 1;
    return 0;
}

static int test_udiv_decode(void)
{
    struct mm_decoded dec;
    /* UDIV r4, r5, r6: hw1=0xfbb5, hw2=0xf4f6 */
    if (decode_insn(0xfbb5u, 0xf4f6u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_UDIV) return 1;
    if (dec.rd != 4u || dec.rn != 5u || dec.rm != 6u) return 1;
    return 0;
}

static int test_sdiv_decode(void)
{
    struct mm_decoded dec;
    /* SDIV r7, r0, r1: hw1=0xfb90, hw2=0xf7f1 */
    if (decode_insn(0xfb90u, 0xf7f1u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_SDIV) return 1;
    if (dec.rd != 7u || dec.rn != 0u || dec.rm != 1u) return 1;
    return 0;
}

static int test_umull_decode(void)
{
    struct mm_decoded dec;
    /* UMULL r4, r5, r0, r1: hw1=0xfba0, hw2=0x4501 */
    if (decode_insn(0xfba0u, 0x4501u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_UMULL) return 1;
    if (dec.rd != 4u || dec.ra != 5u || dec.rn != 0u || dec.rm != 1u) return 1;
    return 0;
}

static int test_umlal_decode(void)
{
    struct mm_decoded dec;
    /* UMLAL r4, r5, r0, r1: hw1=0xfbe0, hw2=0x4501 */
    if (decode_insn(0xfbe0u, 0x4501u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_UMLAL) return 1;
    if (dec.rd != 4u || dec.ra != 5u || dec.rn != 0u || dec.rm != 1u) return 1;
    return 0;
}

static int test_smull_decode(void)
{
    struct mm_decoded dec;
    /* SMULL r8, r9, r2, r3: hw1=0xfb82, hw2=0x8903 */
    if (decode_insn(0xfb82u, 0x8903u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_SMULL) return 1;
    if (dec.rd != 8u || dec.ra != 9u || dec.rn != 2u || dec.rm != 3u) return 1;
    return 0;
}

static int test_smlal_decode(void)
{
    struct mm_decoded dec;
    /* SMLAL r8, r9, r2, r3: hw1=0xfbc2, hw2=0x8903 */
    if (decode_insn(0xfbc2u, 0x8903u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_SMLAL) return 1;
    if (dec.rd != 8u || dec.ra != 9u || dec.rn != 2u || dec.rm != 3u) return 1;
    return 0;
}

static int test_mla_decode(void)
{
    struct mm_decoded dec;
    /* MLA r4, r5, r6, r7: hw1=0xfb05, hw2=0x7406 */
    if (decode_insn(0xfb05u, 0x7406u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_MLA) return 1;
    if (dec.rd != 4u || dec.ra != 7u || dec.rn != 5u || dec.rm != 6u) return 1;
    return 0;
}

static int test_mls_decode(void)
{
    struct mm_decoded dec;
    /* MLS r4, r5, r6, r7: hw1=0xfb05, hw2=0x7416 */
    if (decode_insn(0xfb05u, 0x7416u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_MLS) return 1;
    if (dec.rd != 4u || dec.ra != 7u || dec.rn != 5u || dec.rm != 6u) return 1;
    return 0;
}

static int test_mul_w_decode(void)
{
    struct mm_decoded dec;
    /* MUL.W r4, r5, r6: hw1=0xfb05, hw2=0xf406 (Ra=0xF) */
    if (decode_insn(0xfb05u, 0xf406u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_MUL_W) return 1;
    if (dec.rd != 4u || dec.rn != 5u || dec.rm != 6u) return 1;
    return 0;
}

static int test_rev_w_decode(void)
{
    struct mm_decoded dec;
    /* REV.W r1, r2: hw1=0xfa92, hw2=0xf182 */
    if (decode_insn(0xfa92u, 0xf182u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_REV) return 1;
    if (dec.rd != 1u || dec.rm != 2u) return 1;
    return 0;
}

static int test_rev16_w_decode(void)
{
    struct mm_decoded dec;
    /* REV16.W r1, r2: hw1=0xfa92, hw2=0xf192 */
    if (decode_insn(0xfa92u, 0xf192u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_REV16) return 1;
    if (dec.rd != 1u || dec.rm != 2u) return 1;
    return 0;
}

static int test_revsh_w_decode(void)
{
    struct mm_decoded dec;
    /* REVSH.W r1, r2: hw1=0xfa92, hw2=0xf1b2 */
    if (decode_insn(0xfa92u, 0xf1b2u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_REVSH) return 1;
    if (dec.rd != 1u || dec.rm != 2u) return 1;
    return 0;
}

static int test_ldrex(void)
{
    struct mm_decoded dec;
    if (decode_insn(0xe851u, 0x0f00u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_LDREX) return 1;
    if (dec.rn != 1u) return 1;
    if (dec.rd != 0u) return 1;
    return 0;
}

static int test_strex(void)
{
    struct mm_decoded dec;
    if (decode_insn(0xe841u, 0x0200u, &dec) != 0) return 1;
    if (dec.kind != MM_OP_STREX) return 1;
    if (dec.rn != 1u) return 1;
    if (dec.rd != 2u) return 1;
    if (dec.rm != 0u) return 1;
    return 0;
}

static int test_sg(void)
{
    struct mm_decoded dec;
    if (decode_insn(0xe97fu, 0xe97fu, &dec) != 0) return 1;
    if (dec.kind != MM_OP_SG) return 1;
    return 0;
}

static int test_ldr_literal_w_pc(void)
{
    struct mm_decoded dec;
    if (decode_insn(0xf85fu, 0xf000u, &dec) != 0) return 1; /* ldr.w pc, [pc] */
    if (dec.kind != MM_OP_LDR_LITERAL) return 1;
    if (dec.rd != 15u) return 1;
    if (dec.imm != 0u) return 1;
    return 0;
}

int main(void)
{
    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        { "b_w", test_b_w },
        { "b_cond_w", test_b_cond_w },
        { "movw", test_movw },
        { "movt", test_movt },
        { "mvn_imm_w", test_mvn_imm_w },
        { "mvn_reg_w", test_mvn_reg_w },
        { "ubfx", test_ubfx },
        { "sbfx", test_sbfx },
        { "bfi", test_bfi },
        { "bfc", test_bfc },
        { "tbb", test_tbb },
        { "tbh", test_tbh },
        { "ldrex", test_ldrex },
        { "strex", test_strex },
        { "clrex", test_clrex },
        { "dsb_dmb_isb", test_dsb_dmb_isb },
        { "msr_psr_fields_decode", test_msr_psr_fields_decode },
        { "sxtb_w_decode", test_sxtb_w_decode },
        { "sxth_w_decode", test_sxth_w_decode },
        { "uxth_w_decode", test_uxth_w_decode },
        { "ldrsb_w_imm12", test_ldrsb_w_imm12 },
        { "ldrsb_w_imm8_pre", test_ldrsb_w_imm8_pre },
        { "ldrsh_w_imm12", test_ldrsh_w_imm12 },
        { "ldrsh_w_imm8_pre", test_ldrsh_w_imm8_pre },
        { "clz_w_decode", test_clz_w_decode },
        { "rbit_w_decode", test_rbit_w_decode },
        { "tt_decode", test_tt_decode },
        { "ttt_decode", test_ttt_decode },
        { "tta_decode", test_tta_decode },
        { "ttat_decode", test_ttat_decode },
        { "udiv_decode", test_udiv_decode },
        { "sdiv_decode", test_sdiv_decode },
        { "umull_decode", test_umull_decode },
        { "umlal_decode", test_umlal_decode },
        { "smull_decode", test_smull_decode },
        { "smlal_decode", test_smlal_decode },
        { "mla_decode", test_mla_decode },
        { "mls_decode", test_mls_decode },
        { "mul_w_decode", test_mul_w_decode },
        { "rev_w_decode", test_rev_w_decode },
        { "rev16_w_decode", test_rev16_w_decode },
        { "revsh_w_decode", test_revsh_w_decode },
        { "sg", test_sg },
        { "ldr_literal_w_pc", test_ldr_literal_w_pc },
    };
    const int test_count = (int)(sizeof(tests) / sizeof(tests[0]));
    int failures = 0;
    int i;

    for (i = 0; i < test_count; ++i) {
        if (tests[i].fn() != 0) {
            ++failures;
            printf("FAIL: %s\n", tests[i].name);
        } else {
            printf("PASS: %s\n", tests[i].name);
        }
    }

    if (failures != 0) {
        printf("decode_phase3_test: %d failure(s)\n", failures);
        return 1;
    }

    return 0;
}
