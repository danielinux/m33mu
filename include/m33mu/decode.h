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

/* VFP load/store flags packed in decoded imm. */
#define MM_VFP_LS_DOUBLE (1u << 28)

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
    MM_OP_TEQ_REG,
    MM_OP_TEQ_IMM,
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
    MM_OP_LDRH_PRE_IMM,
    MM_OP_LDRH_POST_IMM,
    MM_OP_STRH_IMM,
    MM_OP_STRH_PRE_IMM,
    MM_OP_STRH_POST_IMM,
    MM_OP_STRH_REG,
    MM_OP_LDRH_REG,
    MM_OP_LDRSB_REG,
    MM_OP_LDRSB_IMM,
    MM_OP_LDRSB_POST_IMM,
    MM_OP_LDRSH_REG,
    MM_OP_LDRSH_IMM,
    MM_OP_LDRSHT,
    MM_OP_STRHT,
    MM_OP_LDRHT,
    MM_OP_LDRSBT,
    MM_OP_LDRT,
    MM_OP_STRT,
    MM_OP_LDRBT,
    MM_OP_STRBT,
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
    MM_OP_NOP_W,
    MM_OP_YIELD_W,
    MM_OP_WFE_W,
    MM_OP_WFI_W,
    MM_OP_SEV_W,
    MM_OP_SEVL_W,
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
    MM_OP_LDREXB,
    MM_OP_LDREXH,
    MM_OP_STREXB,
    MM_OP_STREXH,
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
    MM_OP_SMMUL,
    MM_OP_SMMLA,
    MM_OP_SMMLS,
    MM_OP_SMMLSR,
    MM_OP_SMLAWB,
    MM_OP_SMLAWT,
    MM_OP_SMULWB,
    MM_OP_SMULWT,

    /* DSP instructions (ARMv7-M DSP extension) - Priority 1 */
    MM_OP_SMLAD,   /* Signed dual multiply-accumulate */
    MM_OP_SMLADX,  /* Signed dual multiply-accumulate (exchange) */
    MM_OP_SMLALD,  /* Signed dual multiply-accumulate long */
    MM_OP_SMLALDX, /* Signed dual multiply-accumulate long (exchange) */
    MM_OP_SMLSD,   /* Signed dual multiply-subtract */
    MM_OP_SMLSDX,  /* Signed dual multiply-subtract (exchange) */
    MM_OP_QADD,    /* Saturating add */
    MM_OP_QSUB,    /* Saturating subtract */
    MM_OP_QDADD,   /* Saturating double and add */
    MM_OP_QDSUB,   /* Saturating double and subtract */
    MM_OP_PKHBT,   /* Pack halfword (bottom-top) */
    MM_OP_PKHTB,   /* Pack halfword (top-bottom) */
    MM_OP_SSAT,    /* Signed saturate */
    MM_OP_USAT,    /* Unsigned saturate */
    MM_OP_SMULBB,  /* Signed halfword multiply (all 4 variants: BB/BT/TB/TT) */

    /* DSP parallel 8-bit add/sub (set GE[3:0]) */
    MM_OP_SADD8,
    MM_OP_SSUB8,
    MM_OP_UADD8,
    MM_OP_USUB8,
    /* DSP parallel 8-bit saturating add/sub */
    MM_OP_QADD8,
    MM_OP_QSUB8,
    MM_OP_UQADD8,
    MM_OP_UQSUB8,
    /* DSP parallel 8-bit halving add/sub */
    MM_OP_SHADD8,
    MM_OP_SHSUB8,
    MM_OP_UHADD8,
    MM_OP_UHSUB8,

    /* DSP parallel 16-bit add/sub (set GE[3:0] in halfword pairs) */
    MM_OP_SADD16,
    MM_OP_SSUB16,
    MM_OP_UADD16,
    MM_OP_USUB16,
    MM_OP_QADD16,
    MM_OP_QSUB16,
    MM_OP_UQADD16,
    MM_OP_UQSUB16,
    MM_OP_SHADD16,
    MM_OP_SHSUB16,
    MM_OP_UHADD16,
    MM_OP_UHSUB16,

    /* DSP cross add-sub (16-bit halves with swap) */
    MM_OP_SASX,
    MM_OP_SSAX,
    MM_OP_UASX,
    MM_OP_USAX,
    MM_OP_QASX,
    MM_OP_QSAX,
    MM_OP_UQASX,
    MM_OP_UQSAX,
    MM_OP_SHASX,
    MM_OP_SHSAX,
    MM_OP_UHASX,
    MM_OP_UHSAX,

    /* DSP halfword saturate to N+1-bit signed/unsigned (parallel) */
    MM_OP_SSAT16,
    MM_OP_USAT16,

    /* DSP byte select via GE flags */
    MM_OP_SEL,

    /* DSP sum of absolute differences */
    MM_OP_USAD8,
    MM_OP_USADA8,

    /* DSP dual multiply (no accumulator) */
    MM_OP_SMUAD,
    MM_OP_SMUADX,
    MM_OP_SMUSD,
    MM_OP_SMUSDX,

    /* DSP long signed multiply-subtract */
    MM_OP_SMLSLD,
    MM_OP_SMLSLDX,

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
    MM_OP_ISB,
    MM_OP_MCR_MRC,
    MM_OP_MCRR_MRRC,
    MM_OP_CDP,
    MM_OP_STC,
    MM_OP_STC2,
    MM_OP_LDC,
    MM_OP_LDC2,
    MM_OP_VADD,
    MM_OP_VSUB,
    MM_OP_VMUL,
    MM_OP_VDIV,
    MM_OP_VNEG,
    MM_OP_VABS,
    MM_OP_VMLA,
    MM_OP_VMLS,
    MM_OP_VCMP,
    MM_OP_VCMPE,
    MM_OP_VCVT_S32_F32,
    MM_OP_VCVT_U32_F32,
    MM_OP_VCVT_F32_S32,
    MM_OP_VCVT_F32_U32,
    MM_OP_VCVTR_S32_F32,
    MM_OP_VCVTR_U32_F32,
    MM_OP_VMOV_SR,
    MM_OP_VMOV_RS,
    MM_OP_VMOV_SRR,
    MM_OP_VMOV_RSS,
    MM_OP_VMOV_DRR,
    MM_OP_VMOV_RDD,
    MM_OP_VMOV_IMM,
    MM_OP_VMOV_F32,
    MM_OP_VSQRT,
    MM_OP_VMRS,
    MM_OP_VMSR,
    MM_OP_VLDR,
    MM_OP_VSTR,
    MM_OP_VLDM,
    MM_OP_VSTM,

    /* ARMv8-M FPv5 additions: directed-rounding integer conversions */
    MM_OP_VCVTA_S32_F32,
    MM_OP_VCVTA_U32_F32,
    MM_OP_VCVTN_S32_F32,
    MM_OP_VCVTN_U32_F32,
    MM_OP_VCVTP_S32_F32,
    MM_OP_VCVTP_U32_F32,
    MM_OP_VCVTM_S32_F32,
    MM_OP_VCVTM_U32_F32,

    /* ARMv8-M FPv5 additions: round-to-integral */
    MM_OP_VRINTA_F32,
    MM_OP_VRINTN_F32,
    MM_OP_VRINTP_F32,
    MM_OP_VRINTM_F32,
    MM_OP_VRINTZ_F32,
    MM_OP_VRINTR_F32,
    MM_OP_VRINTX_F32,

    /* ARMv8-M FPv5 additions: NaN-aware min/max and conditional select */
    MM_OP_VMAXNM_F32,
    MM_OP_VMINNM_F32,
    MM_OP_VSEL_F32,

    /* ARMv8-M FPv5 additions: fused multiply-accumulate */
    MM_OP_VFMA_F32,
    MM_OP_VFMS_F32,
    MM_OP_VFNMA_F32,
    MM_OP_VFNMS_F32,

    /* ARMv8-M FPv5 additions: negated multiply variants */
    MM_OP_VNMUL_F32,
    MM_OP_VNMLA_F32,
    MM_OP_VNMLS_F32,

    /* ARMv8-M FPv5 additions: half-precision conversion (B=bottom, T=top half) */
    MM_OP_VCVTB_F32_F16,
    MM_OP_VCVTT_F32_F16,
    MM_OP_VCVTB_F16_F32,
    MM_OP_VCVTT_F16_F32,

    /* ARMv8-M fixed-point conversion (F32 <-> S16/U16/S32/U32 with #fbits).
     * d.imm packs: bit 0 = to_float, bit 1 = unsigned, bit 2 = 32-bit,
     *               bits[7:3] = frac_bits (0..32).
     */
    MM_OP_VCVT_FIXED,

    /* ARMv8-M Security Extension FP context: VLSTM / VLLDM <Rn>. */
    MM_OP_VLSTM,
    MM_OP_VLLDM,

    MM_OP_MAX
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
