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
#include "m33mu/exec_helpers.h"

static int test_add_with_carry_basic(void)
{
    mm_u32 res = 0;
    mm_bool c = MM_FALSE;
    mm_bool v = MM_FALSE;
    mm_add_with_carry(1u, 2u, MM_FALSE, &res, &c, &v);
    if (res != 3u || c || v) {
        printf("add_with_carry case1 res=%lu c=%d v=%d\n", (unsigned long)res, c, v);
        return 1;
    }
    mm_add_with_carry(0xffffffffu, 1u, MM_FALSE, &res, &c, &v);
    if (res != 0u || !c || v) {
        printf("add_with_carry case2 res=%lu c=%d v=%d\n", (unsigned long)res, c, v);
        return 1;
    }
    return 0;
}

static int test_shifts(void)
{
    struct mm_shift_result r;
    r = mm_lsl(1u, 1u, MM_FALSE);
    if (r.value != 2u || r.carry_out) {
        printf("lsl fail value=0x%lx c=%d\n", (unsigned long)r.value, r.carry_out);
        return 1;
    }
    r = mm_lsr(4u, 2u, MM_TRUE);
    if (r.value != 1u || r.carry_out) {
        printf("lsr fail value=0x%lx c=%d\n", (unsigned long)r.value, r.carry_out);
        return 1;
    }
    r = mm_asr(0x80000000u, 31u, MM_FALSE);
    if (r.value != 0xffffffffu || r.carry_out) {
        printf("asr fail value=0x%lx c=%d\n", (unsigned long)r.value, r.carry_out);
        return 1;
    }
    r = mm_ror(0x80000001u, 1u, MM_FALSE);
    if (r.value != 0xc0000000u || !r.carry_out) {
        printf("ror fail value=0x%lx c=%d\n", (unsigned long)r.value, r.carry_out);
        return 1;
    }
    return 0;
}

static int test_pc_operand(void)
{
    struct mm_fetch_result f;
    f.pc_fetch = 0x1000u;
    if (mm_pc_operand(&f) != 0x1004u) return 1;
    f.pc_fetch = 0x1002u;
    if (mm_pc_operand(&f) != 0x1004u) return 1;
    return 0;
}

static int test_adr_value(void)
{
    struct mm_fetch_result f;
    f.pc_fetch = 0x1002u;
    if (mm_adr_value(&f, 0u) != 0x1004u) return 1;
    if (mm_adr_value(&f, 0x3fcu) != 0x1400u) return 1;
    return 0;
}

static int test_xpsr_write_nzcvq(void)
{
    mm_u32 xpsr;
    mm_u32 got;

    xpsr = 0x01000000u | (0x155u); /* T-bit plus some low bits; flags clear */
    got = mm_xpsr_write_nzcvq(xpsr, 0xF8000000u);
    if ((got & 0xF8000000u) != 0xF8000000u) return 1;
    if ((got & 0x07ffffffu) != (xpsr & 0x07ffffffu)) return 1;

    got = mm_xpsr_write_nzcvq(got, 0u);
    if ((got & 0xF8000000u) != 0u) return 1;
    if ((got & 0x07ffffffu) != (xpsr & 0x07ffffffu)) return 1;

    return 0;
}

static int test_itstate(void)
{
    struct mm_itstate it;
    mm_itstate_init(&it);
    if (mm_itstate_raw(&it) != 0u) return 1;
    mm_itstate_set(&it, 0xaa);
    if (mm_itstate_raw(&it) != 0xaau) return 1;
    return 0;
}

static int test_bswap32(void)
{
    mm_u32 v = 0x11223344u;
    mm_u32 got = mm_bswap32(v);
    if (got != 0x44332211u) return 1;
    return 0;
}

static int test_rev16(void)
{
    mm_u32 v = 0x11223344u;
    mm_u32 got = mm_rev16(v);
    if (got != 0x22114433u) return 1;
    return 0;
}

static int test_sxtb(void)
{
    if (mm_sxtb(0x00000080u, 0u) != 0xffffff80u) return 1;
    if (mm_sxtb(0x0000007fu, 0u) != 0x0000007fu) return 1;
    /* Rotate by 8 should sign-extend previous byte. */
    if (mm_sxtb(0x55008011u, 8u) != 0xffffff80u) return 1;
    return 0;
}

static int test_sxth(void)
{
    if (mm_sxth(0x00008000u) != 0xffff8000u) return 1;
    if (mm_sxth(0x00007fffu) != 0x00007fffu) return 1;
    if (mm_sxth(0x12348000u) != 0xffff8000u) return 1;
    return 0;
}

static int test_uxth(void)
{
    if (mm_uxth(0x12348000u) != 0x00008000u) return 1;
    if (mm_uxth(0xffff7fffu) != 0x00007fffu) return 1;
    return 0;
}

static int test_clz_helper(void)
{
    if (mm_clz(0x80000000u) != 0u) return 1;
    if (mm_clz(0x40000000u) != 1u) return 1;
    if (mm_clz(0x00000001u) != 31u) return 1;
    if (mm_clz(0x00000000u) != 32u) return 1;
    return 0;
}

static int test_rbit_helper(void)
{
    if (mm_rbit(0x80000000u) != 0x00000001u) return 1;
    if (mm_rbit(0x00000001u) != 0x80000000u) return 1;
    if (mm_rbit(0x01234567u) != 0xe6a2c480u) return 1;
    return 0;
}

static int test_revsh(void)
{
    mm_u32 v;
    mm_u32 got;

    v = 0x00001234u;
    got = mm_revsh(v);
    if (got != 0x00003412u) return 1;

    v = 0x000080ffu;
    got = mm_revsh(v);
    if (got != 0xffffff80u) return 1;

    return 0;
}

static int test_ubfx(void)
{
    mm_u32 v;
    mm_u32 got;

    v = 0xf0u;
    got = mm_ubfx(v, 4u, 4u);
    if (got != 0x0fu) return 1;

    v = 0x80000000u;
    got = mm_ubfx(v, 31u, 1u);
    if (got != 1u) return 1;

    v = 0xffffffffu;
    got = mm_ubfx(v, 31u, 1u);
    if (got != 1u) return 1;

    return 0;
}

static int test_sbfx(void)
{
    mm_u32 v;
    mm_u32 got;

    v = 0x000000f0u;
    got = mm_sbfx(v, 4u, 4u); /* extract 0xF => should sign-extend to -1 */
    if (got != 0xffffffffu) return 1;

    v = 0x00000070u;
    got = mm_sbfx(v, 4u, 4u); /* extract 0x7 => +7 */
    if (got != 0x00000007u) return 1;

    v = 0x80000000u;
    got = mm_sbfx(v, 31u, 1u); /* sign bit of 1-bit field */
    if (got != 0xffffffffu) return 1;

    return 0;
}

static int test_bfi(void)
{
    mm_u32 dst;
    mm_u32 src;
    mm_u32 got;

    dst = 0xffffffffu;
    src = 0x00000000u;
    got = mm_bfi(dst, src, 8u, 8u);
    if (got != 0xffff00ffu) return 1;

    dst = 0x00000000u;
    src = 0x12345678u;
    got = mm_bfi(dst, src, 4u, 8u);
    if (got != 0x00000780u) return 1;

    dst = 0xaaaaaaaau;
    src = 0x55555555u;
    got = mm_bfi(dst, src, 0u, 32u);
    if (got != 0x55555555u) return 1;

    return 0;
}

static int test_bfc(void)
{
    mm_u32 dst;
    mm_u32 got;

    dst = 0xffffffffu;
    got = mm_bfc(dst, 8u, 8u);
    if (got != 0xffff00ffu) return 1;

    dst = 0x0000ffffu;
    got = mm_bfc(dst, 0u, 16u);
    if (got != 0x00000000u) return 1;

    return 0;
}

static int test_mvn_reg(void)
{
    mm_u32 xpsr;
    mm_u32 got;

    /* setflags=1: update NZ, keep C/V. */
    xpsr = 0x30000000u; /* C=1, V=1, N=0, Z=0 */
    got = mm_mvn_reg(0x00000000u, &xpsr, MM_TRUE);
    if (got != 0xffffffffu) return 1;
    if (xpsr != 0xb0000000u) return 1; /* N=1, Z=0, C/V preserved */

    xpsr = 0x10000000u; /* V=1 */
    got = mm_mvn_reg(0xffffffffu, &xpsr, MM_TRUE);
    if (got != 0x00000000u) return 1;
    if (xpsr != 0x50000000u) return 1; /* Z=1, V preserved */

    /* setflags=0: xPSR unchanged. */
    xpsr = 0xf0000000u;
    got = mm_mvn_reg(0x00000000u, &xpsr, MM_FALSE);
    if (got != 0xffffffffu) return 1;
    if (xpsr != 0xf0000000u) return 1;

    return 0;
}

static int test_thumb_expand_imm12_c(void)
{
    mm_u32 imm32 = 0;
    mm_bool c = MM_FALSE;

    /* top==0 path: carry_out = carry_in */
    mm_thumb_expand_imm12_c(0x000u, MM_TRUE, &imm32, &c);
    if (imm32 != 0u) return 1;
    if (!c) return 1;

    /* rotate path: carry_out = bit31 of result */
    mm_thumb_expand_imm12_c(0x801u, MM_FALSE, &imm32, &c);
    if (c != ((imm32 & 0x80000000u) != 0u)) return 1;
    return 0;
}

static int test_shift_c_imm(void)
{
    mm_bool c;
    mm_u32 got;

    /* LSL #0 keeps carry_in and leaves value unchanged */
    got = mm_shift_c_imm(0x80000000u, 0u, 0u, MM_TRUE, &c);
    if (got != 0x80000000u) return 1;
    if (!c) return 1;

    /* LSR #0 means LSR #32 */
    got = mm_shift_c_imm(0x80000000u, 1u, 0u, MM_FALSE, &c);
    if (got != 0x00000000u) return 1;
    if (!c) return 1;

    /* RRX (type=ROR, imm5=0) */
    got = mm_shift_c_imm(0x00000001u, 3u, 0u, MM_TRUE, &c);
    if (got != 0x80000000u) return 1;
    if (!c) return 1;

    return 0;
}

static int test_ror_reg_shift_c(void)
{
    mm_bool c;
    mm_u32 got;

    /* shift_n == 0: result unchanged, carry unchanged */
    c = MM_FALSE;
    got = mm_ror_reg_shift_c(0x80000001u, 0u, MM_TRUE, &c);
    if (got != 0x80000001u) return 1;
    if (!c) return 1;

    /* shift_n == 1: rotate right by 1, carry = bit31(result) */
    c = MM_FALSE;
    got = mm_ror_reg_shift_c(0x80000001u, 1u, MM_FALSE, &c);
    if (got != 0xc0000000u) return 1;
    if (!c) return 1;

    /* shift_n multiple of 32 (e.g. 32): result unchanged, carry = bit31(result) */
    c = MM_FALSE;
    got = mm_ror_reg_shift_c(0x7fffffffu, 32u, MM_TRUE, &c);
    if (got != 0x7fffffffu) return 1;
    if (c) return 1;

    return 0;
}

static int test_mul64_helpers(void)
{
    mm_u32 lo = 0;
    mm_u32 hi = 0;

    mm_umul64(0x12345678u, 0x01020304u, &lo, &hi);
    if (lo != 0x0ac4c1e0u) return 1;
    if (hi != 0x001258f6u) return 1;

    lo = hi = 0;
    mm_smul64(0xffffffffu, 0x00000002u, &lo, &hi); /* (-1) * 2 */
    if (lo != 0xfffffffeu) return 1;
    if (hi != 0xffffffffu) return 1;

    lo = hi = 0;
    mm_smul64(0x80000000u, 0x00000002u, &lo, &hi); /* (-2147483648)*2 */
    if (lo != 0x00000000u) return 1;
    if (hi != 0xffffffffu) return 1;

    return 0;
}

static int test_sbcs_reg(void)
{
    mm_u32 xpsr;
    mm_u32 res;

    /* 0 - 1 with C=1 => -1, carry(clear borrow)=0 */
    xpsr = (1u << 29); /* C=1 */
    res = mm_sbcs_reg(0u, 1u, &xpsr, MM_TRUE);
    if (res != 0xffffffffu) return 1;
    if ((xpsr & (1u << 29)) != 0u) return 1; /* C must be 0 (borrow occurred) */
    if ((xpsr & (1u << 31)) == 0u) return 1; /* N=1 */

    /* 1 - 1 with C=1 => 0, carry=1, Z=1 */
    xpsr = (1u << 29);
    res = mm_sbcs_reg(1u, 1u, &xpsr, MM_TRUE);
    if (res != 0u) return 1;
    if ((xpsr & (1u << 29)) == 0u) return 1;
    if ((xpsr & (1u << 30)) == 0u) return 1;

    /* setflags=0 leaves xpsr unchanged */
    xpsr = 0xa0000000u; /* N=1, C=1 */
    res = mm_sbcs_reg(5u, 3u, &xpsr, MM_FALSE);
    if (res != 2u) return 1;
    if (xpsr != 0xa0000000u) return 1;

    return 0;
}

static int test_adcs_reg(void)
{
    mm_u32 xpsr;
    mm_u32 res;

    /* 0xffffffff + 0 with C=1 => 0, carry=1, Z=1 */
    xpsr = (1u << 29);
    res = mm_adcs_reg(0xffffffffu, 0u, &xpsr, MM_TRUE);
    if (res != 0u) return 1;
    if ((xpsr & (1u << 29)) == 0u) return 1;
    if ((xpsr & (1u << 30)) == 0u) return 1;

    /* 1 + 1 with C=0 => 2, no carry, no overflow */
    xpsr = 0u;
    res = mm_adcs_reg(1u, 1u, &xpsr, MM_TRUE);
    if (res != 2u) return 1;
    if ((xpsr & (1u << 29)) != 0u) return 1;
    if ((xpsr & (1u << 28)) != 0u) return 1;

    /* Overflow case: 0x7fffffff + 0 + C=1 => 0x80000000, V=1 */
    xpsr = (1u << 29);
    res = mm_adcs_reg(0x7fffffffu, 0u, &xpsr, MM_TRUE);
    if (res != 0x80000000u) return 1;
    if ((xpsr & (1u << 28)) == 0u) return 1;

    /* setflags=0 leaves xpsr unchanged */
    xpsr = 0x50000000u;
    res = mm_adcs_reg(5u, 6u, &xpsr, MM_FALSE);
    if (res != 11u) return 1;
    if (xpsr != 0x50000000u) return 1;

    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "add_with_carry", test_add_with_carry_basic },
        { "shifts", test_shifts },
        { "pc_operand", test_pc_operand },
        { "adr_value", test_adr_value },
        { "xpsr_write_nzcvq", test_xpsr_write_nzcvq },
        { "itstate", test_itstate },
        { "bswap32", test_bswap32 },
        { "rev16", test_rev16 },
        { "sxtb", test_sxtb },
        { "sxth", test_sxth },
        { "uxth", test_uxth },
        { "clz_helper", test_clz_helper },
        { "rbit_helper", test_rbit_helper },
        { "revsh", test_revsh },
        { "ubfx", test_ubfx },
        { "sbfx", test_sbfx },
        { "bfi", test_bfi },
        { "bfc", test_bfc },
        { "mvn_reg", test_mvn_reg },
        { "thumb_expand_imm12_c", test_thumb_expand_imm12_c },
        { "shift_c_imm", test_shift_c_imm },
        { "ror_reg_shift_c", test_ror_reg_shift_c },
        { "mul64_helpers", test_mul64_helpers },
        { "sbcs_reg", test_sbcs_reg },
        { "adcs_reg", test_adcs_reg },
    };
    const int count = (int)(sizeof(tests) / sizeof(tests[0]));
    int failures = 0;
    int i;
    for (i = 0; i < count; ++i) {
        if (tests[i].fn() != 0) {
            ++failures;
            printf("FAIL: %s\n", tests[i].name);
        } else {
            printf("PASS: %s\n", tests[i].name);
        }
    }
    if (failures != 0) {
        printf("exec_helper_test: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
