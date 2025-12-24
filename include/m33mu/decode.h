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

#ifndef M33MU_DECODE_H
#define M33MU_DECODE_H

#include "m33mu/types.h"

/* Condition codes for conditional execution and branches. */
enum mm_cond {
    MM_COND_EQ = 0,
    MM_COND_NE = 1,
    MM_COND_CS = 2,
    MM_COND_CC = 3,
    MM_COND_MI = 4,
    MM_COND_PL = 5,
    MM_COND_VS = 6,
    MM_COND_VC = 7,
    MM_COND_HI = 8,
    MM_COND_LS = 9,
    MM_COND_GE = 10,
    MM_COND_LT = 11,
    MM_COND_GT = 12,
    MM_COND_LE = 13,
    MM_COND_AL = 14,
    MM_COND_NV = 15
};

enum mm_op_kind {
    MM_OP_UNDEFINED = 0,
    MM_OP_NOP,
    MM_OP_IT,
    MM_OP_B_COND,
    MM_OP_B_UNCOND,
    MM_OP_BL,
    MM_OP_BX,
    MM_OP_BLX,
    MM_OP_CBZ,
    MM_OP_CBNZ,
    MM_OP_ADR,
    MM_OP_MOV_IMM,
    MM_OP_MOV_REG,
    MM_OP_MVN_REG,
    MM_OP_ADD_IMM,
    MM_OP_ADD_REG,
    MM_OP_SUB_IMM,
    MM_OP_SUB_REG,
    MM_OP_CMP_IMM,
    MM_OP_CMP_REG,
    MM_OP_AND_REG,
    MM_OP_EOR_REG,
    MM_OP_ORR_REG,
    MM_OP_BIC_REG,
    MM_OP_TST_REG,
    MM_OP_UXTB,
    MM_OP_SXTB,
    MM_OP_SXTH,
    MM_OP_UXTH,
    MM_OP_LSL_IMM,
    MM_OP_LSL_REG,
    MM_OP_LSR_IMM,
    MM_OP_LSR_REG,
    MM_OP_ASR_IMM,
    MM_OP_ASR_REG,
    MM_OP_ROR_REG,
    MM_OP_ROR_IMM,
    MM_OP_NEG,
    MM_OP_ADD_SP_IMM,
    MM_OP_SUB_SP_IMM,
    MM_OP_LDR_IMM,
    MM_OP_STR_IMM,
    MM_OP_LDR_REG,
    MM_OP_STR_REG,
    MM_OP_LDR_POST_IMM,
    MM_OP_LDR_PRE_IMM,
    MM_OP_LDRB_POST_IMM,
    MM_OP_LDRB_PRE_IMM,
    MM_OP_STRB_POST_IMM,
    MM_OP_STRB_PRE_IMM,
    MM_OP_STR_POST_IMM,
    MM_OP_STR_PRE_IMM,
    MM_OP_LDRD,
    MM_OP_STRD,
    MM_OP_LDR_LITERAL,
    MM_OP_LDM,
    MM_OP_STM,
    MM_OP_LDRB_IMM,
    MM_OP_STRB_IMM,
    MM_OP_STRB_REG,
    MM_OP_LDRB_REG,
    MM_OP_LDRH_IMM,
    MM_OP_LDRH_POST_IMM,
    MM_OP_STRH_IMM,
    MM_OP_STRH_POST_IMM,
    MM_OP_STRH_REG,
    MM_OP_LDRH_REG,
    MM_OP_LDRSB_REG,
    MM_OP_LDRSB_IMM,
    MM_OP_LDRSH_REG,
    MM_OP_LDRSH_IMM,
    MM_OP_CLZ,
    MM_OP_RBIT,
    MM_OP_PUSH,
    MM_OP_POP,
    MM_OP_ADCS_REG,
    MM_OP_ADC_IMM,
    MM_OP_SBCS_REG,
    MM_OP_SUB_IMM_NF,
    MM_OP_REV,
    MM_OP_REV16,
    MM_OP_REVSH,
    MM_OP_MUL,
    MM_OP_WFI,
    MM_OP_WFE,
    MM_OP_SEV,
    MM_OP_YIELD,
    MM_OP_SVC,
    MM_OP_BKPT,
    MM_OP_UDF,
    MM_OP_B_UNCOND_WIDE,
    MM_OP_B_COND_WIDE,
    MM_OP_MOVW,
    MM_OP_MOVT,
    MM_OP_UBFX,
    MM_OP_SBFX,
    MM_OP_BFI,
    MM_OP_BFC,
    MM_OP_TBB,
    MM_OP_TBH,
    MM_OP_LDREX,
    MM_OP_STREX,
    MM_OP_CLREX,
    MM_OP_TST_IMM,
    MM_OP_MRS,
    MM_OP_MSR,
    MM_OP_MVN_IMM,
    MM_OP_CMN_IMM,
    MM_OP_CMN_REG,
    MM_OP_CPS,
    MM_OP_SG,
    MM_OP_BXNS,
    MM_OP_BLXNS,
    MM_OP_TT,
    MM_OP_TTT,
    MM_OP_TTA,
    MM_OP_TTAT,
    MM_OP_UDIV,
    MM_OP_SDIV,
    MM_OP_UMULL,
    MM_OP_UMLAL,
    MM_OP_UMAAL,
    MM_OP_SMULL,
    MM_OP_SMLAL,
    MM_OP_SMLA,
    MM_OP_MLA,
    MM_OP_MLS,
    MM_OP_MUL_W,
    MM_OP_ORN_REG,
    MM_OP_ORN_IMM,
    MM_OP_RSB_IMM,
    MM_OP_RSB_REG,
    MM_OP_ROR_REG_NF,
    MM_OP_SBC_IMM,
    MM_OP_SBC_IMM_NF,

    /* Barriers: decoded distinctly but currently executed as no-ops. */
    MM_OP_DSB,
    MM_OP_DMB,
    MM_OP_ISB
};

struct mm_decoded {
    enum mm_op_kind kind;
    enum mm_cond cond;
    mm_u8 rd;
    mm_u8 rn;
    mm_u8 rm;
    mm_u8 ra;  /* secondary dest/source when needed (e.g., long multiply high) */
    mm_u32 imm;
    mm_u8 len;   /* 2 or 4 */
    mm_u32 raw;  /* original instruction bits */
    mm_bool undefined;
};

struct mm_fetch_result;

/* Decode a fetched T32 instruction into a structured representation. */
struct mm_decoded mm_decode_t32(const struct mm_fetch_result *fetch);

#endif /* M33MU_DECODE_H */
