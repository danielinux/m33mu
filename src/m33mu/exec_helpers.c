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

#include "m33mu/exec_helpers.h"

void mm_add_with_carry(mm_u32 a, mm_u32 b, mm_bool carry_in,
                       mm_u32 *result_out, mm_bool *carry_out, mm_bool *overflow_out)
{
    mm_u64 ua = (mm_u64)a;
    mm_u64 ub = (mm_u64)b;
    mm_u64 ures = ua + ub + (carry_in ? 1u : 0u);
    mm_u32 res = (mm_u32)ures;
    mm_bool carry = (ures >> 32) != 0;
    mm_bool overflow = MM_FALSE;

    if ((((~(a ^ b)) & (a ^ res)) & 0x80000000u) != 0u) {
        overflow = MM_TRUE;
    }

    if (result_out != 0) *result_out = res;
    if (carry_out != 0) *carry_out = carry;
    if (overflow_out != 0) *overflow_out = overflow;
}

static struct mm_shift_result shift_zero(mm_u32 value, mm_bool carry_in)
{
    struct mm_shift_result r;
    r.value = value;
    r.carry_out = carry_in;
    return r;
}

struct mm_shift_result mm_lsl(mm_u32 value, mm_u8 amount, mm_bool carry_in)
{
    struct mm_shift_result r;
    if (amount == 0u) {
        return shift_zero(value, carry_in);
    }
    if (amount < 32u) {
        r.value = value << amount;
        r.carry_out = ((value >> (32u - amount)) & 1u) != 0u;
    } else if (amount == 32u) {
        r.value = 0;
        r.carry_out = (value & 1u) != 0u;
    } else {
        r.value = 0;
        r.carry_out = MM_FALSE;
    }
    return r;
}

struct mm_shift_result mm_lsr(mm_u32 value, mm_u8 amount, mm_bool carry_in)
{
    struct mm_shift_result r;
    if (amount == 0u) {
        return shift_zero(value, carry_in);
    }
    if (amount < 32u) {
        r.value = value >> amount;
        r.carry_out = ((value >> (amount - 1u)) & 1u) != 0u;
    } else if (amount == 32u) {
        r.value = 0;
        r.carry_out = (value >> 31) != 0u;
    } else {
        r.value = 0;
        r.carry_out = MM_FALSE;
    }
    return r;
}

struct mm_shift_result mm_asr(mm_u32 value, mm_u8 amount, mm_bool carry_in)
{
    struct mm_shift_result r;
    if (amount == 0u) {
        return shift_zero(value, carry_in);
    }
    if (amount < 32u) {
        mm_u32 sign = value & 0x80000000u;
        r.value = (value >> amount) | (sign ? (~0u << (32u - amount)) : 0u);
        r.carry_out = ((value >> (amount - 1u)) & 1u) != 0u;
    } else {
        mm_u32 sign_mask = (value & 0x80000000u) ? 0xffffffffu : 0u;
        r.value = sign_mask;
        r.carry_out = (value & 0x80000000u) != 0u;
    }
    return r;
}

struct mm_shift_result mm_ror(mm_u32 value, mm_u8 amount, mm_bool carry_in)
{
    struct mm_shift_result r;
    mm_u8 rot;
    if (amount == 0u) {
        return shift_zero(value, carry_in);
    }
    rot = (mm_u8)(amount & 31u);
    if (rot == 0u) {
        /* Effective rotate is a multiple of 32: result unchanged, carry = bit31. */
        r.value = value;
        r.carry_out = (value & 0x80000000u) != 0u;
        return r;
    }
    r.value = (value >> rot) | (value << (32u - rot));
    r.carry_out = (r.value >> 31) != 0u;
    return r;
}

mm_u32 mm_pc_operand(const struct mm_fetch_result *fetch)
{
    mm_u32 pc = 0;
    if (fetch != 0) {
        pc = fetch->pc_fetch + 4u;
        pc &= ~3u;
    }
    return pc;
}

mm_u32 mm_adr_value(const struct mm_fetch_result *fetch, mm_u32 imm32)
{
    return mm_pc_operand(fetch) + imm32;
}

mm_u32 mm_xpsr_write_nzcvq(mm_u32 xpsr, mm_u32 reg_value)
{
    mm_u32 mask = 0xF8000000u; /* N Z C V Q */
    return (xpsr & ~mask) | (reg_value & mask);
}

void mm_itstate_init(struct mm_itstate *it)
{
    if (it != 0) {
        it->raw = 0;
    }
}

void mm_itstate_set(struct mm_itstate *it, mm_u8 raw)
{
    if (it != 0) {
        it->raw = raw;
    }
}

mm_u8 mm_itstate_raw(const struct mm_itstate *it)
{
    if (it == 0) {
        return 0;
    }
    return it->raw;
}

mm_u32 mm_bswap32(mm_u32 value)
{
    mm_u32 b0 = (value >> 0) & 0xffu;
    mm_u32 b1 = (value >> 8) & 0xffu;
    mm_u32 b2 = (value >> 16) & 0xffu;
    mm_u32 b3 = (value >> 24) & 0xffu;
    return (b0 << 24) | (b1 << 16) | (b2 << 8) | (b3 << 0);
}

mm_u32 mm_rev16(mm_u32 value)
{
    /* Swap bytes within each 16-bit halfword:
     * [31:24][23:16][15:8][7:0] => [23:16][31:24][7:0][15:8]
     */
    return ((value & 0x00ff00ffu) << 8) | ((value & 0xff00ff00u) >> 8);
}

mm_u32 mm_revsh(mm_u32 value)
{
    mm_u32 half = value & 0xffffu;
    mm_u32 swapped = ((half & 0xffu) << 8) | ((half >> 8) & 0xffu);
    if ((swapped & 0x8000u) != 0u) {
        swapped |= 0xffff0000u;
    }
    return swapped;
}

mm_u32 mm_sxtb(mm_u32 value, mm_u8 rotate)
{
    mm_u32 rotated = value;
    if (rotate != 0u) {
        mm_u8 r = (mm_u8)(rotate & 31u);
        if (r != 0u) {
            rotated = (value >> r) | (value << (32u - r));
        }
    }
    return (mm_u32)((mm_i32)((mm_i8)(rotated & 0xffu)));
}

mm_u32 mm_sxth(mm_u32 value)
{
    return (mm_u32)((mm_i32)((mm_i16)(value & 0xffffu)));
}

mm_u32 mm_uxth(mm_u32 value)
{
    return value & 0xffffu;
}

mm_u32 mm_clz(mm_u32 value)
{
    mm_u32 count = 0u;
    mm_u32 mask = 0x80000000u;
    if (value == 0u) {
        return 32u;
    }
    while ((value & mask) == 0u && mask != 0u) {
        ++count;
        mask >>= 1;
    }
    return count;
}

mm_u32 mm_rbit(mm_u32 value)
{
    mm_u32 out = 0u;
    mm_u32 i;
    for (i = 0; i < 32u; ++i) {
        out <<= 1;
        out |= (value >> i) & 1u;
    }
    return out;
}

void mm_umul64(mm_u32 a, mm_u32 b, mm_u32 *lo_out, mm_u32 *hi_out)
{
    mm_u64 prod = (mm_u64)a * (mm_u64)b;
    if (lo_out != 0) *lo_out = (mm_u32)prod;
    if (hi_out != 0) *hi_out = (mm_u32)(prod >> 32);
}

void mm_smul64(mm_u32 a, mm_u32 b, mm_u32 *lo_out, mm_u32 *hi_out)
{
    mm_i64 prod = (mm_i64)((mm_i32)a) * (mm_i64)((mm_i32)b);
    if (lo_out != 0) *lo_out = (mm_u32)prod;
    if (hi_out != 0) *hi_out = (mm_u32)((mm_u64)prod >> 32);
}

mm_u32 mm_ubfx(mm_u32 value, mm_u8 lsb, mm_u8 width)
{
    mm_u32 mask;

    if (width == 0u || width > 32u) {
        return 0;
    }
    if (lsb >= 32u) {
        return 0;
    }
    if ((mm_u32)lsb + (mm_u32)width > 32u) {
        return 0;
    }

    if (width == 32u) {
        mask = 0xffffffffu;
    } else {
        mask = (1u << width) - 1u;
    }
    return (value >> lsb) & mask;
}

mm_u32 mm_sbfx(mm_u32 value, mm_u8 lsb, mm_u8 width)
{
    mm_u32 extracted;
    mm_u32 sign_bit;

    extracted = mm_ubfx(value, lsb, width);
    if (width == 0u || width > 32u) {
        return 0;
    }
    if (width == 32u) {
        return extracted;
    }
    sign_bit = 1u << (width - 1u);
    if ((extracted & sign_bit) != 0u) {
        extracted |= ~((sign_bit << 1) - 1u);
    }
    return extracted;
}

mm_u32 mm_bfi(mm_u32 dst, mm_u32 src, mm_u8 lsb, mm_u8 width)
{
    mm_u32 mask;

    if (width == 0u || width > 32u) {
        return dst;
    }
    if (lsb >= 32u) {
        return dst;
    }
    if ((mm_u32)lsb + (mm_u32)width > 32u) {
        return dst;
    }

    if (width == 32u) {
        mask = 0xffffffffu;
    } else {
        mask = ((1u << width) - 1u) << lsb;
    }

    return (dst & ~mask) | ((src << lsb) & mask);
}

mm_u32 mm_bfc(mm_u32 dst, mm_u8 lsb, mm_u8 width)
{
    return mm_bfi(dst, 0u, lsb, width);
}

mm_u32 mm_mvn_reg(mm_u32 rm_value, mm_u32 *xpsr_inout, mm_bool setflags)
{
    mm_u32 res;
    res = ~rm_value;
    if (setflags && xpsr_inout != 0) {
        *xpsr_inout &= ~(0xC0000000u);
        if (res == 0u) {
            *xpsr_inout |= (1u << 30);
        }
        if ((res & 0x80000000u) != 0u) {
            *xpsr_inout |= (1u << 31);
        }
    }
    return res;
}

void mm_thumb_expand_imm12_c(mm_u32 imm12, mm_bool carry_in, mm_u32 *imm32_out, mm_bool *carry_out)
{
    mm_u32 imm8;
    mm_u32 top;
    mm_u32 pat;
    mm_u32 imm32;

    imm8 = imm12 & 0xffu;
    top = (imm12 >> 10) & 0x3u; /* imm12[11:10] */
    pat = (imm12 >> 8) & 0x3u;  /* imm12[9:8] */

    if (top == 0u) {
        switch (pat) {
        case 0u: imm32 = imm8; break;
        case 1u: imm32 = (imm8 << 16) | imm8; break;
        case 2u: imm32 = (imm8 << 24) | (imm8 << 8); break;
        default: imm32 = (imm8 << 24) | (imm8 << 16) | (imm8 << 8) | imm8; break;
        }
        if (carry_out != 0) {
            *carry_out = carry_in;
        }
    } else {
        mm_u32 unrot;
        mm_u32 rot;
        unrot = (1u << 7) | (imm12 & 0x7fu);
        rot = (imm12 >> 7) & 0x1fu; /* imm12[11:7] */
        if (rot == 0u) {
            imm32 = unrot;
        } else {
            imm32 = (unrot >> rot) | (unrot << (32u - rot));
        }
        if (carry_out != 0) {
            *carry_out = (imm32 & 0x80000000u) != 0u;
        }
    }

    if (imm32_out != 0) {
        *imm32_out = imm32;
    }
}

mm_u32 mm_shift_c_imm(mm_u32 value, mm_u8 type, mm_u8 imm5, mm_bool carry_in, mm_bool *carry_out)
{
    struct mm_shift_result r;

    if (carry_out != 0) {
        *carry_out = carry_in;
    }

    switch (type & 0x3u) {
    case 0u: /* LSL */
        if (imm5 == 0u) {
            if (carry_out != 0) {
                *carry_out = carry_in;
            }
            return value;
        }
        r = mm_lsl(value, imm5, carry_in);
        if (carry_out != 0) {
            *carry_out = r.carry_out;
        }
        return r.value;
    case 1u: /* LSR */
        if (imm5 == 0u) {
            imm5 = 32u;
        }
        r = mm_lsr(value, imm5, carry_in);
        if (carry_out != 0) {
            *carry_out = r.carry_out;
        }
        return r.value;
    case 2u: /* ASR */
        if (imm5 == 0u) {
            imm5 = 32u;
        }
        r = mm_asr(value, imm5, carry_in);
        if (carry_out != 0) {
            *carry_out = r.carry_out;
        }
        return r.value;
    case 3u: /* ROR / RRX */
    default:
        if (imm5 == 0u) {
            mm_u32 shifted = ((carry_in ? 1u : 0u) << 31) | (value >> 1);
            mm_bool c = (value & 1u) != 0u;
            if (carry_out != 0) {
                *carry_out = c;
            }
            return shifted;
        }
        r = mm_ror(value, imm5, carry_in);
        if (carry_out != 0) {
            *carry_out = r.carry_out;
        }
        return r.value;
    }
}

mm_u32 mm_ror_reg_shift_c(mm_u32 value, mm_u32 shift_n, mm_bool carry_in, mm_bool *carry_out)
{
    mm_u32 amount;
    mm_u32 rot;
    mm_u32 res;

    /* Register-specified shift amount uses the low byte. */
    amount = shift_n & 0xffu;

    if (amount == 0u) {
        if (carry_out != 0) {
            *carry_out = carry_in;
        }
        return value;
    }

    rot = amount & 31u;
    if (rot == 0u) {
        res = value;
        if (carry_out != 0) {
            *carry_out = (res & 0x80000000u) != 0u;
        }
        return res;
    }

    res = (value >> rot) | (value << (32u - rot));
    if (carry_out != 0) {
        *carry_out = (res & 0x80000000u) != 0u;
    }
    return res;
}

mm_u32 mm_sbcs_reg(mm_u32 rn_value, mm_u32 rm_value, mm_u32 *xpsr_inout, mm_bool setflags)
{
    mm_u32 res = 0;
    mm_bool carry_in = MM_FALSE;
    mm_bool carry_out = MM_FALSE;
    mm_bool overflow_out = MM_FALSE;

    if (xpsr_inout != 0) {
        carry_in = ((*xpsr_inout) & (1u << 29)) != 0u;
    }

    mm_add_with_carry(rn_value, ~rm_value, carry_in, &res, &carry_out, &overflow_out);

    if (setflags && xpsr_inout != 0) {
        *xpsr_inout &= ~(0xF0000000u);
        if (res == 0u) *xpsr_inout |= (1u << 30);
        if ((res & 0x80000000u) != 0u) *xpsr_inout |= (1u << 31);
        if (carry_out) *xpsr_inout |= (1u << 29);
        if (overflow_out) *xpsr_inout |= (1u << 28);
    }

    return res;
}

mm_u32 mm_adcs_reg(mm_u32 rn_value, mm_u32 rm_value, mm_u32 *xpsr_inout, mm_bool setflags)
{
    mm_u32 res = 0;
    mm_bool carry_in = MM_FALSE;
    mm_bool carry_out = MM_FALSE;
    mm_bool overflow_out = MM_FALSE;

    if (xpsr_inout != 0) {
        carry_in = ((*xpsr_inout) & (1u << 29)) != 0u;
    }

    mm_add_with_carry(rn_value, rm_value, carry_in, &res, &carry_out, &overflow_out);

    if (setflags && xpsr_inout != 0) {
        *xpsr_inout &= ~(0xF0000000u);
        if (res == 0u) *xpsr_inout |= (1u << 30);
        if ((res & 0x80000000u) != 0u) *xpsr_inout |= (1u << 31);
        if (carry_out) *xpsr_inout |= (1u << 29);
        if (overflow_out) *xpsr_inout |= (1u << 28);
    }

    return res;
}
