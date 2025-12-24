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

static int decode_from_bytes(const mm_u8 *bytes, size_t len_bytes, struct mm_decoded *out_dec, mm_u32 start_pc)
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
    cpu.r[15] = start_pc | 1u;
    cpu.xpsr = 0;

    fetch = mm_fetch_t32(&cpu, &mem);
    if (fetch.fault) {
        return 1;
    }
    *out_dec = mm_decode_t32(&fetch);
    return 0;
}

static int test_nop(void)
{
    mm_u8 bytes[] = { 0x00, 0xbf };
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_NOP) return 1;
    return 0;
}

static int test_b_cond(void)
{
    mm_u8 bytes[] = { 0x00, 0xd1 }; /* BNE +0 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_B_COND) return 1;
    if (dec.cond != MM_COND_NE) return 1;
    if (dec.imm != 0) return 1;
    return 0;
}

static int test_b_uncond(void)
{
    mm_u8 bytes[] = { 0xfe, 0xe7 }; /* B #-4 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_B_UNCOND) return 1;
    if (dec.imm != 0xfffffffcu) return 1;
    return 0;
}

static int test_cbz(void)
{
    mm_u8 bytes[] = { 0x00, 0xb1 }; /* CBZ r0, +0 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_CBZ) return 1;
    if (dec.rn != 0) return 1;
    if (dec.imm != 0) return 1;
    return 0;
}

static int test_cbnz(void)
{
    mm_u8 bytes[] = { 0x00, 0xb9 }; /* CBNZ r0, +0 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_CBNZ) return 1;
    return 0;
}

static int test_bx(void)
{
    mm_u8 bytes[] = { 0x18, 0x47 }; /* BX r3 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_BX) return 1;
    if (dec.rm != 3) return 1;
    return 0;
}

static int test_mov_imm(void)
{
    mm_u8 bytes[] = { 0x34, 0x20 }; /* MOVS r0, #0x34 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_MOV_IMM) return 1;
    if (dec.rd != 0) return 1;
    if (dec.imm != 0x34) return 1;
    return 0;
}

static int test_cmp_imm(void)
{
    mm_u8 bytes[] = { 0x05, 0x28 }; /* CMP r0, #5 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_CMP_IMM) return 1;
    if (dec.rn != 0) return 1;
    if (dec.imm != 5) return 1;
    return 0;
}

static int test_add_imm3(void)
{
    mm_u8 bytes[] = { 0x48, 0x1c }; /* ADDS (imm3=1, rn=1, rd=0) */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_ADD_IMM) return 1;
    if (dec.imm != 1u) return 1;
    return 0;
}

static int test_sub_imm3(void)
{
    mm_u8 bytes[] = { 0x48, 0x1e }; /* SUBS (imm3=1, rn=1, rd=0) */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_SUB_IMM) return 1;
    if (dec.imm != 1u) return 1;
    return 0;
}

static int test_and_reg(void)
{
    mm_u8 bytes[] = { 0x01, 0x40 }; /* AND r1, r0 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_AND_REG) return 1;
    if (dec.rd != 1) return 1;
    if (dec.rm != 0) return 1;
    return 0;
}

static int test_adr(void)
{
    mm_u8 bytes[] = { 0xff, 0xa2 }; /* ADR r2, #0x3fc */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_ADR) return 1;
    if (dec.rd != 2) return 1;
    if (dec.imm != 0x3fcu) return 1;
    return 0;
}

static int test_mvn_reg(void)
{
    mm_u8 bytes[] = { 0xc8, 0x43 }; /* MVN r0, r1 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_MVN_REG) return 1;
    if (dec.rd != 0 || dec.rm != 1) return 1;
    return 0;
}

static int test_ror_reg(void)
{
    mm_u8 bytes[] = { 0xc8, 0x41 }; /* ROR r0, r1 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_ROR_REG) return 1;
    if (dec.rd != 0 || dec.rm != 1) return 1;
    if (dec.rn != 0) return 1; /* Rdn */
    return 0;
}

static int test_sbcs_reg(void)
{
    mm_u8 bytes[] = { 0x88, 0x41 }; /* SBC r0, r1 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_SBCS_REG) return 1;
    if (dec.rd != 0 || dec.rm != 1) return 1;
    if (dec.rn != 0) return 1; /* Rdn */
    return 0;
}

static int test_adcs_reg(void)
{
    mm_u8 bytes[] = { 0x48, 0x41 }; /* ADC r0, r1 (0x4148) */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_ADCS_REG) return 1;
    if (dec.rd != 0 || dec.rm != 1) return 1;
    if (dec.rn != 0) return 1; /* Rdn */
    return 0;
}

static int test_lsl_imm(void)
{
    mm_u8 bytes[] = { 0x85, 0x00 }; /* LSLS r0, r1, #2 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_LSL_IMM) return 1;
    if (dec.rd != 5) return 1; /* rd bits in low position (binary 101) */
    if (dec.rm != 0) return 1; /* actually source bits in rm */
    return 0;
}

static int test_ldr_literal(void)
{
    mm_u8 bytes[] = { 0x00, 0x48 }; /* LDR r0, [PC, #0] */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_LDR_LITERAL) return 1;
    if (dec.rd != 0) return 1;
    if (dec.imm != 0) return 1;
    return 0;
}

static int test_ldr_imm(void)
{
    mm_u8 bytes[] = { 0x01, 0x68 }; /* LDR r1, [r0, #0] */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_LDR_IMM) return 1;
    if (dec.rn != 0 || dec.rd != 1) return 1;
    if (dec.imm != 0) return 1;
    return 0;
}

static int test_str_imm(void)
{
    mm_u8 bytes[] = { 0x01, 0x60 }; /* STR r1, [r0, #0] */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_STR_IMM) return 1;
    return 0;
}

static int test_push(void)
{
    mm_u8 bytes[] = { 0x0f, 0xb4 }; /* PUSH {r0-r3} */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_PUSH) return 1;
    if (dec.imm != 0x0f) return 1;
    return 0;
}

static int test_pop(void)
{
    mm_u8 bytes[] = { 0x0f, 0xbc }; /* POP {r0-r3} */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_POP) return 1;
    if (dec.imm != 0x0f) return 1;
    return 0;
}

static int test_sxtb(void)
{
    mm_u8 bytes[] = { 0x48, 0xb2 }; /* SXTB r0, r1 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_SXTB) return 1;
    if (dec.rd != 0 || dec.rm != 1) return 1;
    return 0;
}

static int test_sxth(void)
{
    mm_u8 bytes[] = { 0x08, 0xb2 }; /* SXTH r0, r1 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_SXTH) return 1;
    if (dec.rd != 0 || dec.rm != 1) return 1;
    return 0;
}

static int test_uxth(void)
{
    mm_u8 bytes[] = { 0x88, 0xb2 }; /* UXTH r0, r1 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_UXTH) return 1;
    if (dec.rd != 0 || dec.rm != 1) return 1;
    return 0;
}

static int test_ldrsb_reg(void)
{
    mm_u8 bytes[] = { 0x50, 0x56 }; /* LDRSB r0, [r2, r1] */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_LDRSB_REG) return 1;
    if (dec.rd != 0 || dec.rn != 2 || dec.rm != 1) return 1;
    return 0;
}

static int test_ldrsh_reg(void)
{
    mm_u8 bytes[] = { 0x58, 0x5e }; /* LDRSH r0, [r3, r1] */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_LDRSH_REG) return 1;
    if (dec.rd != 0 || dec.rn != 3 || dec.rm != 1) return 1;
    return 0;
}

static int test_bl(void)
{
    mm_u8 bytes[] = { 0x00, 0xf0, 0x00, 0xf8 }; /* BL with zero offset */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_BL) return 1;
    if (dec.imm != 0) return 1;
    return 0;
}

static int test_add_sp_imm7(void)
{
    mm_u8 bytes[] = { 0x08, 0xb0 }; /* ADD sp, #32 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_ADD_SP_IMM) return 1;
    if (dec.rd != 13u) return 1;
    if (dec.imm != 32u) return 1;
    return 0;
}

static int test_sub_sp_imm7(void)
{
    mm_u8 bytes[] = { 0x88, 0xb0 }; /* SUB sp, #32 */
    struct mm_decoded dec;
    if (decode_from_bytes(bytes, sizeof(bytes), &dec, 0) != 0) return 1;
    if (dec.kind != MM_OP_SUB_SP_IMM) return 1;
    if (dec.rd != 13u) return 1;
    if (dec.imm != 32u) return 1;
    return 0;
}

int main(void)
{
    struct {
        const char *name;
        int (*fn)(void);
	    } tests[] = {
	        { "nop", test_nop },
	        { "b_cond", test_b_cond },
	        { "b_uncond", test_b_uncond },
	        { "cbz", test_cbz },
	        { "cbnz", test_cbnz },
	        { "bx", test_bx },
	        { "adr", test_adr },
	        { "mov_imm", test_mov_imm },
	        { "cmp_imm", test_cmp_imm },
	        { "add_imm3", test_add_imm3 },
	        { "sub_imm3", test_sub_imm3 },
	        { "and_reg", test_and_reg },
	        { "mvn_reg", test_mvn_reg },
	        { "ror_reg", test_ror_reg },
	        { "sbcs_reg", test_sbcs_reg },
	        { "adcs_reg", test_adcs_reg },
	        { "lsl_imm", test_lsl_imm },
	        { "ldr_literal", test_ldr_literal },
	        { "ldr_imm", test_ldr_imm },
        { "str_imm", test_str_imm },
        { "push", test_push },
        { "pop", test_pop },
        { "sxtb", test_sxtb },
        { "sxth", test_sxth },
        { "uxth", test_uxth },
        { "ldrsb_reg", test_ldrsb_reg },
        { "ldrsh_reg", test_ldrsh_reg },
        { "bl", test_bl },
        { "add_sp_imm7", test_add_sp_imm7 },
        { "sub_sp_imm7", test_sub_sp_imm7 },
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
        printf("decode_phase1_test: %d failure(s)\n", failures);
        return 1;
    }

    return 0;
}
