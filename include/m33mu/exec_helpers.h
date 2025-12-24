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

#ifndef M33MU_EXEC_HELPERS_H
#define M33MU_EXEC_HELPERS_H

#include "m33mu/types.h"
#include "m33mu/decode.h"
#include "m33mu/fetch.h"

struct mm_shift_result {
    mm_u32 value;
    mm_bool carry_out;
};

/* Add with carry; returns result and updates carry/overflow flags. */
void mm_add_with_carry(mm_u32 a, mm_u32 b, mm_bool carry_in,
                       mm_u32 *result_out, mm_bool *carry_out, mm_bool *overflow_out);

struct mm_shift_result mm_lsl(mm_u32 value, mm_u8 amount, mm_bool carry_in);
struct mm_shift_result mm_lsr(mm_u32 value, mm_u8 amount, mm_bool carry_in);
struct mm_shift_result mm_asr(mm_u32 value, mm_u8 amount, mm_bool carry_in);
struct mm_shift_result mm_ror(mm_u32 value, mm_u8 amount, mm_bool carry_in);

/* PC operand value for PC-relative instructions: align(fetch_pc+4) per Arm. */
mm_u32 mm_pc_operand(const struct mm_fetch_result *fetch);

/* ADR (Thumb16 T1) address calculation: Align(PC,4) + imm32. */
mm_u32 mm_adr_value(const struct mm_fetch_result *fetch, mm_u32 imm32);

/* MSR to APSR fields: update NZCVQ from reg_value, leaving other xPSR bits intact. */
mm_u32 mm_xpsr_write_nzcvq(mm_u32 xpsr, mm_u32 reg_value);

/* ITSTATE stub tracking raw state. */
struct mm_itstate {
    mm_u8 raw;
};

void mm_itstate_init(struct mm_itstate *it);
void mm_itstate_set(struct mm_itstate *it, mm_u8 raw);
mm_u8 mm_itstate_raw(const struct mm_itstate *it);

/* Byte swap helper used by REV. */
mm_u32 mm_bswap32(mm_u32 value);

/* Swap bytes within each halfword (REV16). */
mm_u32 mm_rev16(mm_u32 value);

/* Reverse bytes in low halfword then sign-extend (REVSH). */
mm_u32 mm_revsh(mm_u32 value);

/* Sign-extend byte after optional rotate (used by SXTB family). */
mm_u32 mm_sxtb(mm_u32 value, mm_u8 rotate);

/* Sign-extend halfword (used by SXTH family). */
mm_u32 mm_sxth(mm_u32 value);

/* Zero-extend halfword (used by UXTH family). */
mm_u32 mm_uxth(mm_u32 value);

/* Count leading zeros (CLZ). */
mm_u32 mm_clz(mm_u32 value);

/* Bit-reverse 32-bit word (RBIT). */
mm_u32 mm_rbit(mm_u32 value);

/* Bitfield extract used by UBFX. */
mm_u32 mm_ubfx(mm_u32 value, mm_u8 lsb, mm_u8 width);

/* Bitfield extract used by SBFX (sign-extend). */
mm_u32 mm_sbfx(mm_u32 value, mm_u8 lsb, mm_u8 width);

/* Bitfield insert used by BFI. */
mm_u32 mm_bfi(mm_u32 dst, mm_u32 src, mm_u8 lsb, mm_u8 width);

/* Bitfield clear used by BFC. */
mm_u32 mm_bfc(mm_u32 dst, mm_u8 lsb, mm_u8 width);

/* MVN (register) helper: computes ~rm_value and optionally updates xPSR NZ. */
mm_u32 mm_mvn_reg(mm_u32 rm_value, mm_u32 *xpsr_inout, mm_bool setflags);

/* Thumb-2 modified immediate expansion with carry-out (ThumbExpandImm_C). */
void mm_thumb_expand_imm12_c(mm_u32 imm12, mm_bool carry_in, mm_u32 *imm32_out, mm_bool *carry_out);

/* Shift with carry for immediate shift encodings (type=0..3). */
mm_u32 mm_shift_c_imm(mm_u32 value, mm_u8 type, mm_u8 imm5, mm_bool carry_in, mm_bool *carry_out);

/* Rotate right with carry for a register-specified shift amount (ROR reg). */
mm_u32 mm_ror_reg_shift_c(mm_u32 value, mm_u32 shift_n, mm_bool carry_in, mm_bool *carry_out);

/* SBC (register) helper: computes Rn - Rm - (1-C) with optional NZCV update. */
mm_u32 mm_sbcs_reg(mm_u32 rn_value, mm_u32 rm_value, mm_u32 *xpsr_inout, mm_bool setflags);

/* ADC (register) helper: computes Rn + Rm + C with optional NZCV update. */
mm_u32 mm_adcs_reg(mm_u32 rn_value, mm_u32 rm_value, mm_u32 *xpsr_inout, mm_bool setflags);

/* 32x32 -> 64 multiplies. */
void mm_umul64(mm_u32 a, mm_u32 b, mm_u32 *lo_out, mm_u32 *hi_out);
void mm_smul64(mm_u32 a, mm_u32 b, mm_u32 *lo_out, mm_u32 *hi_out);

#endif /* M33MU_EXEC_HELPERS_H */
