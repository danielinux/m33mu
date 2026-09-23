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

#include "m33mu/decode.h"
#include "m33mu/fetch.h"
#include <stdio.h>

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
#define MM_INLINE static inline
#elif defined(__GNUC__)
#define MM_INLINE static __inline
#else
#define MM_INLINE static
#endif

static mm_u32 thumb_expand_imm12(mm_u32 imm12)
{
    /* ThumbExpandImm per Arm ARM A7.4.2 / ThumbExpandImmWithC (carry ignored). */
    mm_u32 imm8 = imm12 & 0xffu;
    mm_u32 top = (imm12 >> 10) & 0x3u;   /* imm12[11:10] */
    mm_u32 pat = (imm12 >> 8) & 0x3u;    /* imm12[9:8] */
    mm_u32 imm32;

    if (top == 0u) {
        switch (pat) {
        case 0u: imm32 = imm8; break;
        case 1u: imm32 = (imm8 << 16) | imm8; break;
        case 2u: imm32 = (imm8 << 24) | (imm8 << 8); break;
        default: imm32 = (imm8 << 24) | (imm8 << 16) | (imm8 << 8) | imm8; break;
        }
    } else {
        mm_u32 unrot = (1u << 7) | (imm12 & 0x7fu);     /* 1:imm12[6:0] */
        mm_u32 rot = (imm12 >> 7) & 0x1fu;              /* imm12[11:7] */
        rot &= 0x1fu;
        if (rot == 0u) {
            imm32 = unrot;
        } else {
            imm32 = (unrot >> rot) | (unrot << (32u - rot));
        }
    }
    return imm32;
}

static struct mm_decoded mm_decoded_default(const struct mm_fetch_result *fetch)
{
    struct mm_decoded d;
    d.kind = MM_OP_UNDEFINED;
    d.cond = MM_COND_AL;
    d.rd = 0;
    d.rn = 0;
    d.rm = 0;
    d.ra = 0;
    d.imm = 0;
    d.len = fetch != 0 ? fetch->len : 0;
    d.raw = fetch != 0 ? fetch->insn : 0;
    d.undefined = MM_TRUE;
    return d;
}

MM_INLINE mm_u8 vfp_sd(mm_u32 insn)
{
    return (mm_u8)((((insn >> 12) & 0x0fu) << 1) | ((insn >> 22) & 0x1u));
}

MM_INLINE mm_u8 vfp_dd(mm_u32 insn)
{
    return (mm_u8)(((insn >> 12) & 0x0fu) | (((insn >> 22) & 0x1u) << 4));
}

MM_INLINE mm_u8 vfp_sn(mm_u32 insn)
{
    return (mm_u8)((((insn >> 16) & 0x0fu) << 1) | ((insn >> 7) & 0x1u));
}

MM_INLINE mm_u8 vfp_sm(mm_u32 insn)
{
    return (mm_u8)((((insn >> 0) & 0x0fu) << 1) | ((insn >> 5) & 0x1u));
}

MM_INLINE mm_bool decode_16_control(mm_u16 hw1, struct mm_decoded *d)
{
    /* UDF */
    if ((hw1 & 0xff00u) == 0xde00u) {
        d->kind = MM_OP_UDF;
        d->imm = (mm_u32)(hw1 & 0x00ffu);
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* BKPT */
    if ((hw1 & 0xff00u) == 0xbe00u) {
        d->kind = MM_OP_BKPT;
        d->imm = (mm_u32)(hw1 & 0x00ffu);
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* SVC */
    if ((hw1 & 0xff00u) == 0xdf00u) {
        d->kind = MM_OP_SVC;
        d->imm = (mm_u32)(hw1 & 0x00ffu);
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* NOP */
    if (hw1 == 0xbf00u) {
        d->kind = MM_OP_NOP;
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* Hints: any 0xbfX0 encoding acts as a hint/NOP and must not fall through
     * into the IT decoder with a zero mask. Recognized hints get dedicated kinds;
     * the remaining architecturally undefined hint slots execute as NOPs. */
    if ((hw1 & 0xff0fu) == 0xbf00u) {
        mm_u8 op = (mm_u8)(hw1 & 0x00f0u);
        if (op == 0x10u) {
            d->kind = MM_OP_YIELD;
        } else if (op == 0x20u) {
            d->kind = MM_OP_WFE;
        } else if (op == 0x30u) {
            d->kind = MM_OP_WFI;
        } else if (op == 0x40u) {
            d->kind = MM_OP_SEV;
        } else if (op == 0x50u) {
            d->kind = MM_OP_NOP;
        } else {
            d->kind = MM_OP_NOP;
        }
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* CPSIE/CPSID (only I-bit handled); pattern 1011 0110 I 10 0 0 im[2:0] */
    if ((hw1 & 0xffe0u) == 0xb660u) {
        d->kind = MM_OP_CPS;
        d->imm = (mm_u32)(hw1 & 0x001fu); /* bit4=imod (0=IE,1=ID), bits2:0=mask bits */
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* IT (ITSTATE capture only) */
    if ((hw1 & 0xff00u) == 0xbf00u) {
        d->kind = MM_OP_IT;
        d->imm = (mm_u32)(hw1 & 0x00ffu);
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    return MM_FALSE;
}

MM_INLINE mm_bool decode_16_branch_cond(mm_u16 hw1, struct mm_decoded *d)
{
    /* Conditional branch (b<c>) */
    if ((hw1 & 0xf000u) == 0xd000u) {
        mm_u8 cond = (mm_u8)((hw1 >> 8) & 0x0fu);
        if (cond != MM_COND_NV) {
            mm_u32 imm11 = (mm_u32)(hw1 & 0x00ffu);
            mm_u32 imm = (imm11 << 1);
            if (imm & 0x0000100u) { /* sign extend 9 bits (8+sign) */
                imm |= 0xfffffe00u;
            }
            d->kind = MM_OP_B_COND;
            d->cond = (enum mm_cond)cond;
            d->imm = imm;
            d->undefined = MM_FALSE;
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

MM_INLINE mm_bool decode_16_branch_uncond(mm_u16 hw1, struct mm_decoded *d)
{
    /* Unconditional B (short) */
    if ((hw1 & 0xf800u) == 0xe000u) {
        mm_u32 imm11 = (mm_u32)(hw1 & 0x07ffu);
        mm_u32 imm = imm11 << 1;
        if (imm & 0x00000800u) {
            imm |= 0xfffff000u;
        }
        d->kind = MM_OP_B_UNCOND;
        d->imm = imm;
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }
    return MM_FALSE;
}

MM_INLINE mm_bool decode_16_branch_misc(mm_u16 hw1, struct mm_decoded *d)
{
    /* CBZ/CBNZ */
    if ((hw1 & 0xf500u) == 0xb100u) {
        mm_u8 i = (mm_u8)((hw1 >> 9) & 0x1u);
        mm_u8 imm5 = (mm_u8)((hw1 >> 3) & 0x1fu);
        mm_u8 rn = (mm_u8)(hw1 & 0x7u);
        mm_u32 imm = ((mm_u32)(i << 6) | (mm_u32)(imm5 << 1));
        if ((hw1 & 0x0800u) != 0u) {
            d->kind = MM_OP_CBNZ;
        } else {
            d->kind = MM_OP_CBZ;
        }
        d->rn = rn;
        d->imm = imm;
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* BXNS/BLXNS (secure state change branches) */
    if ((hw1 & 0xff87u) == 0x4704u) {
        d->kind = MM_OP_BXNS;
        d->rm = (mm_u8)((hw1 >> 3) & 0x0fu);
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }
    if ((hw1 & 0xff87u) == 0x4784u) {
        d->kind = MM_OP_BLXNS;
        d->rm = (mm_u8)((hw1 >> 3) & 0x0fu);
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* BX */
    if ((hw1 & 0xff80u) == 0x4700u) {
        d->kind = MM_OP_BX;
        d->rm = (mm_u8)((hw1 >> 3) & 0x0fu);
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }
    /* BLX (register) */
    if ((hw1 & 0xff80u) == 0x4780u) {
        d->kind = MM_OP_BLX;
        d->rm = (mm_u8)((hw1 >> 3) & 0x0fu);
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    return MM_FALSE;
}

MM_INLINE mm_bool decode_16_ext_rev(mm_u16 hw1, struct mm_decoded *d)
{
    /* UXTB (16-bit) */
    if ((hw1 & 0xffc0u) == 0xb2c0u) {
        d->kind = MM_OP_UXTB;
        d->rd = (mm_u8)(hw1 & 0x7u);
        d->rm = (mm_u8)((hw1 >> 3) & 0x7u);
        d->imm = 0u;
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* SXTB (Thumb16) */
    if ((hw1 & 0xffc0u) == 0xb240u) {
        d->kind = MM_OP_SXTB;
        d->rm = (mm_u8)((hw1 >> 3) & 0x7u);
        d->rd = (mm_u8)(hw1 & 0x7u);
        d->imm = 0;
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* SXTH (Thumb16) */
    if ((hw1 & 0xffc0u) == 0xb200u) {
        d->kind = MM_OP_SXTH;
        d->rm = (mm_u8)((hw1 >> 3) & 0x7u);
        d->rd = (mm_u8)(hw1 & 0x7u);
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* UXTH (Thumb16) */
    if ((hw1 & 0xffc0u) == 0xb280u) {
        d->kind = MM_OP_UXTH;
        d->rm = (mm_u8)((hw1 >> 3) & 0x7u);
        d->rd = (mm_u8)(hw1 & 0x7u);
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* REV/REV16/REVSH */
    if ((hw1 & 0xffc0u) == 0xba00u) {
        d->kind = MM_OP_REV;
        d->rd = (mm_u8)(hw1 & 0x7u);
        d->rm = (mm_u8)((hw1 >> 3) & 0x7u);
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }
    if ((hw1 & 0xffc0u) == 0xba40u) {
        d->kind = MM_OP_REV16;
        d->rd = (mm_u8)(hw1 & 0x7u);
        d->rm = (mm_u8)((hw1 >> 3) & 0x7u);
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }
    if ((hw1 & 0xffc0u) == 0xbac0u) {
        d->kind = MM_OP_REVSH;
        d->rd = (mm_u8)(hw1 & 0x7u);
        d->rm = (mm_u8)((hw1 >> 3) & 0x7u);
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    return MM_FALSE;
}

MM_INLINE mm_bool decode_16_data_proc(mm_u16 hw1, struct mm_decoded *d)
{
    /* ADD/CMP/MOV (high registers) */
    if ((hw1 & 0xfc00u) == 0x4400u) {
        mm_u8 op = (mm_u8)((hw1 >> 8) & 0x3u);
        mm_u8 rm = (mm_u8)((hw1 >> 3) & 0x0fu);
        mm_u8 rd = (mm_u8)((hw1 & 0x7u) | ((hw1 >> 4) & 0x8u));
        d->rm = rm;
        d->rd = rd;
        d->rn = rd;
        if (op == 0u) {
            d->kind = MM_OP_ADD_REG;
        } else if (op == 1u) {
            d->kind = MM_OP_CMP_REG;
        } else if (op == 2u) {
            d->kind = MM_OP_MOV_REG;
        }
        if (d->kind != MM_OP_UNDEFINED) {
            d->undefined = MM_FALSE;
            return MM_TRUE;
        }
    }

    /* MOV immediate (MOVS) */
    if ((hw1 & 0xf800u) == 0x2000u) {
        d->kind = MM_OP_MOV_IMM;
        d->rd = (mm_u8)((hw1 >> 8) & 0x7u);
        d->imm = (mm_u32)(hw1 & 0x00ffu);
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* CMP immediate */
    if ((hw1 & 0xf800u) == 0x2800u) {
        d->kind = MM_OP_CMP_IMM;
        d->rn = (mm_u8)((hw1 >> 8) & 0x7u);
        d->imm = (mm_u32)(hw1 & 0x00ffu);
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* ADD/SUB immediate (8-bit) */
    if ((hw1 & 0xf800u) == 0x3000u) {
        d->kind = MM_OP_ADD_IMM;
        d->rd = (mm_u8)((hw1 >> 8) & 0x7u);
        d->rn = d->rd;
        d->imm = (mm_u32)(hw1 & 0x00ffu);
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }
    if ((hw1 & 0xf800u) == 0x3800u) {
        d->kind = MM_OP_SUB_IMM;
        d->rd = (mm_u8)((hw1 >> 8) & 0x7u);
        d->rn = d->rd;
        d->imm = (mm_u32)(hw1 & 0x00ffu);
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* ADD/SUB (register or 3-bit immediate)
     * Encoding: 000 11 I op imm3/Rm Rn Rd
     * bit10 (I) distinguishes register(0) vs immediate(1).
     * For register form, bits [8:6] hold Rm; for immediate form they are imm3.
     */
    if ((hw1 & 0xf800u) == 0x1800u) {
        mm_u8 is_imm = (mm_u8)((hw1 >> 10) & 0x1u);
        mm_u8 op = (mm_u8)((hw1 >> 9) & 0x1u);
        mm_u8 bits8_6 = (mm_u8)((hw1 >> 6) & 0x7u);
        mm_u8 rn = (mm_u8)((hw1 >> 3) & 0x7u);
        mm_u8 rd = (mm_u8)(hw1 & 0x7u);

        d->rn = rn;
        d->rd = rd;
        if (is_imm) {
            d->imm = (mm_u32)bits8_6;
            d->kind = (op == 0) ? MM_OP_ADD_IMM : MM_OP_SUB_IMM;
        } else {
            d->rm = bits8_6;
            d->kind = (op == 0) ? MM_OP_ADD_REG : MM_OP_SUB_REG;
        }
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* ALU operations on registers (AND/EOR/LSL/LSR/ASR/ADC/SBC/ROR/TST/NEG/CMP/CMN/ORR/MUL/BIC/MVN) */
    if ((hw1 & 0xfc00u) == 0x4000u) {
        mm_u8 opcode = (mm_u8)((hw1 >> 6) & 0x0fu);
        mm_u8 rm = (mm_u8)((hw1 >> 3) & 0x7u);
        mm_u8 rd = (mm_u8)(hw1 & 0x7u);
        d->rm = rm;
        d->rd = rd;
        switch (opcode) {
        case 0x0: d->kind = MM_OP_AND_REG; d->rn = rd; break;
        case 0x1: d->kind = MM_OP_EOR_REG; d->rn = rd; break;
        case 0x2: d->kind = MM_OP_LSL_REG; d->rn = rd; break; /* LSL Rd,Rd,Rm */
        case 0x3: d->kind = MM_OP_LSR_REG; d->rn = rd; break;
        case 0x4: d->kind = MM_OP_ASR_REG; d->rn = rd; break;
        case 0x5: d->kind = MM_OP_ADCS_REG; d->rn = rd; break;
        case 0x6: d->kind = MM_OP_SBCS_REG; d->rn = rd; break;
        case 0x7: d->kind = MM_OP_ROR_REG; d->rn = rd; break;
        case 0x8: d->kind = MM_OP_TST_REG; d->rn = rd; break;
        case 0x9: d->kind = MM_OP_NEG; break;
        case 0xa: d->kind = MM_OP_CMP_REG; d->rn = rd; break;
        case 0xb: d->kind = MM_OP_CMN_REG; d->rn = rd; break;
        case 0xc: d->kind = MM_OP_ORR_REG; d->rn = rd; break;
        case 0xd: d->kind = MM_OP_MUL; d->rn = rd; break;
        case 0xe: d->kind = MM_OP_BIC_REG; d->rn = rd; break;
        case 0xf: d->kind = MM_OP_MVN_REG; d->rn = rd; break;
        default:
            d->kind = MM_OP_UNDEFINED;
            break;
        }
        if (d->kind != MM_OP_UNDEFINED) {
            d->undefined = MM_FALSE;
            return MM_TRUE;
        }
    }

    /* LSL/LSR/ASR immediate (shift by imm5) */
    if ((hw1 & 0xe000u) == 0x0000u) {
        mm_u8 op = (mm_u8)((hw1 >> 11) & 0x3u);
        mm_u8 imm5 = (mm_u8)((hw1 >> 6) & 0x1fu);
        mm_u8 rm = (mm_u8)((hw1 >> 3) & 0x7u);
        mm_u8 rd = (mm_u8)(hw1 & 0x7u);
        if (op <= 2u) {
            d->rm = rm;
            d->rd = rd;
            d->imm = (mm_u32)imm5;
            if (op == 0) d->kind = MM_OP_LSL_IMM;
            else if (op == 1) d->kind = MM_OP_LSR_IMM;
            else d->kind = MM_OP_ASR_IMM;
            d->undefined = MM_FALSE;
            return MM_TRUE;
        }
    }

    return MM_FALSE;
}

MM_INLINE mm_bool decode_16_loadstore(mm_u16 hw1, struct mm_decoded *d)
{
    /* LDR literal (PC-relative) */
    if ((hw1 & 0xf800u) == 0x4800u) {
        mm_u8 rd = (mm_u8)((hw1 >> 8) & 0x7u);
        mm_u32 imm = (mm_u32)(hw1 & 0x00ffu) << 2;
        d->kind = MM_OP_LDR_LITERAL;
        d->rd = rd;
        d->imm = imm;
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* STR/LDR (immediate, word) */
    if ((hw1 & 0xf800u) == 0x6000u || (hw1 & 0xf800u) == 0x6800u) {
        mm_u8 imm5 = (mm_u8)((hw1 >> 6) & 0x1fu);
        mm_u8 rn = (mm_u8)((hw1 >> 3) & 0x7u);
        mm_u8 rt = (mm_u8)(hw1 & 0x7u);
        mm_u32 imm = (mm_u32)(imm5 << 2);
        d->rn = rn;
        d->rd = rt;
        d->imm = imm;
        if ((hw1 & 0x0800u) != 0) {
            d->kind = MM_OP_LDR_IMM;
        } else {
            d->kind = MM_OP_STR_IMM;
        }
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* STR/LDR (SP-relative, word) */
    if ((hw1 & 0xf800u) == 0x9000u || (hw1 & 0xf800u) == 0x9800u) {
        mm_u8 rt = (mm_u8)((hw1 >> 8) & 0x7u);
        mm_u8 imm8 = (mm_u8)(hw1 & 0xffu);
        mm_u32 imm = (mm_u32)imm8 << 2;
        d->rn = 13u;
        d->rd = rt;
        d->imm = imm;
        if ((hw1 & 0x0800u) != 0) {
            d->kind = MM_OP_LDR_IMM;
        } else {
            d->kind = MM_OP_STR_IMM;
        }
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* STRB/LDRB (immediate) */
    if ((hw1 & 0xf000u) == 0x7000u) {
        mm_u8 imm5 = (mm_u8)((hw1 >> 6) & 0x1fu);
        mm_u8 rn = (mm_u8)((hw1 >> 3) & 0x7u);
        mm_u8 rt = (mm_u8)(hw1 & 0x7u);
        d->rn = rn;
        d->rd = rt;
        d->imm = imm5;
        d->kind = ((hw1 & 0x0800u) != 0u) ? MM_OP_LDRB_IMM : MM_OP_STRB_IMM;
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* STRH/LDRH (immediate) */
    if ((hw1 & 0xf000u) == 0x8000u) {
        mm_u8 imm5 = (mm_u8)((hw1 >> 6) & 0x1fu);
        mm_u8 rn = (mm_u8)((hw1 >> 3) & 0x7u);
        mm_u8 rt = (mm_u8)(hw1 & 0x7u);
        d->rn = rn;
        d->rd = rt;
        d->imm = (mm_u32)(imm5 << 1); /* halfword scaling */
        if ((hw1 & 0x0800u) != 0u) {
            d->kind = MM_OP_LDRH_IMM;
        } else {
            d->kind = MM_OP_STRH_IMM;
        }
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* STR/LDR (register offset, word) */
    if ((hw1 & 0xfe00u) == 0x5000u || (hw1 & 0xfe00u) == 0x5800u) {
        mm_u8 rm = (mm_u8)((hw1 >> 6) & 0x7u);
        mm_u8 rn = (mm_u8)((hw1 >> 3) & 0x7u);
        mm_u8 rt = (mm_u8)(hw1 & 0x7u);
        d->rn = rn;
        d->rm = rm;
        d->rd = rt;
        if ((hw1 & 0x0800u) != 0u) {
            d->kind = MM_OP_LDR_REG;
        } else {
            d->kind = MM_OP_STR_REG;
        }
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* STRH/LDRH (register offset) */
    if ((hw1 & 0xfe00u) == 0x5200u || (hw1 & 0xfe00u) == 0x5a00u) {
        mm_u8 rm = (mm_u8)((hw1 >> 6) & 0x7u);
        mm_u8 rn = (mm_u8)((hw1 >> 3) & 0x7u);
        mm_u8 rt = (mm_u8)(hw1 & 0x7u);
        d->rn = rn;
        d->rm = rm;
        d->rd = rt;
        d->kind = ((hw1 & 0xfe00u) == 0x5a00u) ? MM_OP_LDRH_REG : MM_OP_STRH_REG;
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* STRB (register offset) */
    if ((hw1 & 0xfe00u) == 0x5400u) {
        d->kind = MM_OP_STRB_REG;
        d->rm = (mm_u8)((hw1 >> 6) & 0x7u);
        d->rn = (mm_u8)((hw1 >> 3) & 0x7u);
        d->rd = (mm_u8)(hw1 & 0x7u);
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* LDRB (register offset) */
    if ((hw1 & 0xfe00u) == 0x5c00u) {
        d->kind = MM_OP_LDRB_REG;
        d->rm = (mm_u8)((hw1 >> 6) & 0x7u);
        d->rn = (mm_u8)((hw1 >> 3) & 0x7u);
        d->rd = (mm_u8)(hw1 & 0x7u);
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* LDRSB (register offset, 16-bit) */
    if ((hw1 & 0xfe00u) == 0x5600u) {
        d->kind = MM_OP_LDRSB_REG;
        d->rm = (mm_u8)((hw1 >> 6) & 0x7u);
        d->rn = (mm_u8)((hw1 >> 3) & 0x7u);
        d->rd = (mm_u8)(hw1 & 0x7u);
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* LDRSH (register offset, 16-bit) */
    if ((hw1 & 0xfe00u) == 0x5e00u) {
        d->kind = MM_OP_LDRSH_REG;
        d->rm = (mm_u8)((hw1 >> 6) & 0x7u);
        d->rn = (mm_u8)((hw1 >> 3) & 0x7u);
        d->rd = (mm_u8)(hw1 & 0x7u);
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* LDRSH (immediate) Thumb-2, T1/T2 handled in decode_32(). */

    return MM_FALSE;
}

MM_INLINE mm_bool decode_16_stack_misc(mm_u16 hw1, struct mm_decoded *d)
{
    /* ADR (ADD (PC-relative) to Rd) */
    if ((hw1 & 0xf800u) == 0xa000u) {
        mm_u8 rd = (mm_u8)((hw1 >> 8) & 0x7u);
        mm_u32 imm = (mm_u32)(hw1 & 0x00ffu) << 2;
        d->kind = MM_OP_ADR;
        d->rd = rd;
        d->imm = imm;
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* ADD (SP plus immediate) */
    if ((hw1 & 0xf800u) == 0xa800u) {
        mm_u8 rd = (mm_u8)((hw1 >> 8) & 0x7u);
        mm_u32 imm = (mm_u32)(hw1 & 0x00ffu) << 2;
        d->kind = MM_OP_ADD_SP_IMM;
        d->rd = rd;
        d->rn = 13u;
        d->imm = imm;
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* ADD/SUB SP, #imm7*4 */
    if ((hw1 & 0xff80u) == 0xb000u) {
        mm_u32 imm = (mm_u32)(hw1 & 0x007fu) << 2;
        d->kind = MM_OP_ADD_SP_IMM;
        d->rd = 13u;
        d->rn = 13u;
        d->imm = imm;
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }
    if ((hw1 & 0xff80u) == 0xb080u) {
        mm_u32 imm = (mm_u32)(hw1 & 0x007fu) << 2;
        d->kind = MM_OP_SUB_SP_IMM;
        d->rd = 13u;
        d->rn = 13u;
        d->imm = imm;
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    /* PUSH/POP */
    if ((hw1 & 0xf600u) == 0xb400u) {
        mm_u16 reglist = (mm_u16)(hw1 & 0x00ffu);
        if ((hw1 & 0x0100u) != 0u) {
            reglist |= 0x0100u; /* LR or PC depending on push/pop */
        }
        d->imm = reglist;
        if ((hw1 & 0x0800u) != 0) {
            d->kind = MM_OP_POP;
        } else {
            d->kind = MM_OP_PUSH;
        }
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    return MM_FALSE;
}

MM_INLINE mm_bool decode_16_stm_ldm(mm_u16 hw1, struct mm_decoded *d)
{
    /* STMIA/LDMIA (16-bit) */
    if ((hw1 & 0xf000u) == 0xc000u) {
        mm_u8 l = (mm_u8)((hw1 >> 11) & 0x1u);
        mm_u8 rn = (mm_u8)((hw1 >> 8) & 0x7u);
        mm_u16 reglist = (mm_u16)(hw1 & 0x00ffu);
        if (reglist == 0u) {
            return MM_FALSE; /* UNPRED: treat as undefined */
        }
        d->kind = l ? MM_OP_LDM : MM_OP_STM;
        d->rn = rn;
        d->imm = (1u << 24) | (1u << 16) | (mm_u32)reglist; /* IA with writeback */
        d->undefined = MM_FALSE;
        return MM_TRUE;
    }

    return MM_FALSE;
}

static struct mm_decoded decode_16(mm_u16 hw1)
{
    struct mm_decoded d;
    mm_u16 top;
    d = mm_decoded_default(0);
    d.len = 2;
    d.raw = hw1;

    if (decode_16_control(hw1, &d)) {
        return d;
    }

    top = (mm_u16)(hw1 & 0xf000u);
    switch (top) {
    case 0x0000u:
    case 0x1000u:
        if (decode_16_data_proc(hw1, &d)) return d;
        break;
    case 0x2000u:
    case 0x3000u:
        if (decode_16_data_proc(hw1, &d)) return d;
        break;
    case 0x4000u:
    case 0x5000u:
        if (decode_16_branch_misc(hw1, &d)) return d;
        if (decode_16_data_proc(hw1, &d)) return d;
        if (decode_16_ext_rev(hw1, &d)) return d;
        if (decode_16_loadstore(hw1, &d)) return d;
        break;
    case 0x6000u:
    case 0x7000u:
    case 0x8000u:
    case 0x9000u:
        if (decode_16_loadstore(hw1, &d)) return d;
        break;
    case 0xa000u:
    case 0xb000u:
        if (decode_16_stack_misc(hw1, &d)) return d;
        if (decode_16_branch_misc(hw1, &d)) return d;
        if (decode_16_ext_rev(hw1, &d)) return d;
        break;
    case 0xc000u:
        if (decode_16_stm_ldm(hw1, &d)) return d;
        break;
    case 0xd000u:
        if (decode_16_branch_cond(hw1, &d)) return d;
        break;
    case 0xe000u:
        if (decode_16_branch_uncond(hw1, &d)) return d;
        break;
    default:
        break;
    }

    return d;
}

static struct mm_decoded decode_32(mm_u32 insn)
{
    struct mm_decoded d;
    d = mm_decoded_default(0);
    d.len = 4;
    d.raw = insn;

    /* LDR (literal), Thumb-2 T3:
     * 1111 1000 U101 1111 Rt imm12
     * Used by CMSE import veneers: e.g. "ldr.w pc, [pc]" then a literal word.
     */
    if ((insn & 0xff7f0000u) == 0xf85f0000u) {
        mm_u32 u = (insn >> 23) & 0x1u;
        mm_u8 rt = (mm_u8)((insn >> 12) & 0x0fu);
        mm_u32 imm12 = insn & 0xfffu;
        d.kind = MM_OP_LDR_LITERAL;
        d.rd = rt;
        d.imm = u ? imm12 : (mm_u32)(0u - imm12);
        d.undefined = MM_FALSE;
        return d;
    }

    /* SG (Secure Gateway) */
    if (insn == 0xe97fe97fu) {
        d.kind = MM_OP_SG;
        d.undefined = MM_FALSE;
        return d;
    }

    /* CLREX */
    if (insn == 0xf3bf8f2fu) {
        d.kind = MM_OP_CLREX;
        d.undefined = MM_FALSE;
        return d;
    }

    /* Wide hints (NOP.W, YIELD.W, WFE.W, WFI.W, SEV.W, SEVL.W) - 32-bit
     * Pattern: 1111 0011 1010 1111 1000 0000 hint:4 0000
     * Encoding: 0xf3af8000 with bits[7:4] = hint
     */
    if ((insn & 0xffffff0fu) == 0xf3af8000u) {
        mm_u8 const hint = (mm_u8)((insn >> 4) & 0xfu);
        if (hint <= 0x5u) {
            switch (hint) {
            case 0x0u: d.kind = MM_OP_NOP_W; break;
            case 0x1u: d.kind = MM_OP_YIELD_W; break;
            case 0x2u: d.kind = MM_OP_WFE_W; break;
            case 0x3u: d.kind = MM_OP_WFI_W; break;
            case 0x4u: d.kind = MM_OP_SEV_W; break;
            case 0x5u: d.kind = MM_OP_SEVL_W; break;
            }
            d.undefined = MM_FALSE;
            return d;
        }
        /* hint > 5 is UNDEFINED for this pattern */
        return d;
    }

    /* REV/REV16/REVSH (Thumb-2 wide encodings share hw1 with RBIT/CLZ). */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xfa90u) {
            mm_u8 rm = (mm_u8)(hw1 & 0x0fu);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 rm2 = (mm_u8)(hw2 & 0x0fu);
            if (rm == rm2 && rd != 15u && rm != 15u) {
                mm_u16 op = (mm_u16)(hw2 & 0xf0f0u);
                if (op == 0xf080u) {
                    d.kind = MM_OP_REV;
                } else if (op == 0xf090u) {
                    d.kind = MM_OP_REV16;
                } else if (op == 0xf0b0u) {
                    d.kind = MM_OP_REVSH;
                }
                if (d.kind != MM_OP_UNDEFINED) {
                    d.rd = rd;
                    d.rm = rm;
                    d.undefined = MM_FALSE;
                    return d;
                }
            }
        }
    }

    /* RBIT (Thumb-2): 1111 1010 1001 Rm | 1111 0000 1010 Rd */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xfa90u && (hw2 & 0xf0f0u) == 0xf0a0u) {
            mm_u8 rm = (mm_u8)(hw1 & 0x0fu);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 rm2 = (mm_u8)(hw2 & 0x0fu);
            if (rm == rm2 && rd != 15u && rm != 15u) {
                d.kind = MM_OP_RBIT;
                d.rd = rd;
                d.rm = rm;
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* TT / TTT / TTA / TTAT (CMSE attribute check) */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xe840u && (hw2 & 0xf03fu) == 0xf000u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 rt = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 variant = (mm_u8)((hw2 >> 6) & 0x3u);
            if (rt == 15u || rn == 15u) {
                return d;
            }
            switch (variant) {
            case 0u: d.kind = MM_OP_TT; break;
            case 1u: d.kind = MM_OP_TTT; break;
            case 2u: d.kind = MM_OP_TTA; break;
            case 3u: d.kind = MM_OP_TTAT; break;
            default: break;
            }
            if (d.kind != MM_OP_UNDEFINED) {
                d.rn = rn;
                d.rd = rt;
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* UDIV / SDIV (Thumb-2): class differs in hw1 upper nibble; hw2 fixed mask */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw2 & 0xf0f0u) == 0xf0f0u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 rm = (mm_u8)(hw2 & 0x0fu);
            if (rn != 15u && rd != 15u && rm != 15u) {
                if ((hw1 & 0xfff0u) == 0xfbb0u) {
                    d.kind = MM_OP_UDIV;
                    d.rd = rd;
                    d.rn = rn;
                    d.rm = rm;
                    d.undefined = MM_FALSE;
                    return d;
                } else if ((hw1 & 0xfff0u) == 0xfb90u) {
                    d.kind = MM_OP_SDIV;
                    d.rd = rd;
                    d.rn = rn;
                    d.rm = rm;
                    d.undefined = MM_FALSE;
                    return d;
                }
            }
        }
    }

    /* MUL.W (Thumb-2 three-operand multiply; bit20 may request flags) */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xfb00u &&
            ((hw2 >> 12) & 0x0fu) == 0x0fu &&
            (hw2 & 0x00f0u) == 0x0000u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 rm = (mm_u8)(hw2 & 0x0fu);
            if (rn != 15u && rd != 15u && rm != 15u) {
                d.kind = MM_OP_MUL_W;
                d.rn = rn;
                d.rd = rd;
                d.rm = rm;
                d.imm = (mm_u32)((hw1 >> 4) & 0x1u); /* setflags from bit20 */
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* UMULL / UMLAL / SMULL / SMLAL (Thumb-2 long multiply) */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xfbe0u && (hw2 & 0x00f0u) == 0x0060u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 rdlo = (mm_u8)((hw2 >> 12) & 0x0fu);
            mm_u8 rdhi = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 rm = (mm_u8)(hw2 & 0x0fu);
            if (rn != 15u && rm != 15u && rdlo != 15u && rdhi != 15u) {
                d.kind = MM_OP_UMAAL;
                d.rn = rn;
                d.rd = rdlo;
                d.rm = rm;
                d.ra = rdhi;
                d.undefined = MM_FALSE;
                return d;
            }
        }
        if ((hw2 & 0x00f0u) == 0x0000u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 rdlo = (mm_u8)((hw2 >> 12) & 0x0fu);
            mm_u8 rdhi = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 rm = (mm_u8)(hw2 & 0x0fu);
            if (rn != 15u && rm != 15u && rdlo != 15u && rdhi != 15u) {
                if ((hw1 & 0xfff0u) == 0xfba0u) {
                    d.kind = MM_OP_UMULL;
                } else if ((hw1 & 0xfff0u) == 0xfbe0u) {
                    d.kind = MM_OP_UMLAL;
                } else if ((hw1 & 0xfff0u) == 0xfb80u) {
                    d.kind = MM_OP_SMULL;
                } else if ((hw1 & 0xfff0u) == 0xfbc0u) {
                    d.kind = MM_OP_SMLAL;
                }
                if (d.kind != MM_OP_UNDEFINED) {
                    d.rn = rn;
                    d.rd = rdlo; /* use rd for RdLo */
                    d.rm = rm;
                    d.ra = rdhi; /* reuse ra for RdHi */
                    d.undefined = MM_FALSE;
                    return d;
                }
            }
        }
    }

    /* SMLAxy (Thumb-2 signed halfword multiply accumulate) */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xfb10u && (hw2 & 0x00c0u) == 0x0000u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 ra = (mm_u8)((hw2 >> 12) & 0x0fu);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 rm = (mm_u8)(hw2 & 0x0fu);
            mm_u8 xy = (mm_u8)((hw2 >> 4) & 0x3u);
            if (ra != 15u && rn != 15u && rd != 15u && rm != 15u) {
                d.kind = MM_OP_SMLA;
                d.rn = rn;
                d.rm = rm;
                d.rd = rd;
                d.ra = ra;
                d.imm = xy; /* bit1 selects top halfword of Rn, bit0 selects top halfword of Rm */
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* MLA / MLS (Thumb-2 multiply accumulate/subtract) */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xfb00u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 ra = (mm_u8)((hw2 >> 12) & 0x0fu);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 rm = (mm_u8)(hw2 & 0x0fu);
            mm_u8 is_mls = (mm_u8)((hw2 >> 4) & 0x1u);
            if (ra != 15u && rn != 15u && rd != 15u && rm != 15u) {
                d.kind = (is_mls != 0u) ? MM_OP_MLS : MM_OP_MLA;
                d.rn = rn;
                d.rm = rm;
                d.rd = rd;
                d.ra = ra;
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* SMLAD / SMLADX (Thumb-2 signed dual multiply-accumulate)
     * SMLAD:  1111 1011 0010 Rn | Ra Rd 0000 Rm
     * SMLADX: 1111 1011 0010 Rn | Ra Rd 0001 Rm
     */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xfb20u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 ra = (mm_u8)((hw2 >> 12) & 0x0fu);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 rm = (mm_u8)(hw2 & 0x0fu);
            mm_u8 m_bit = (mm_u8)((hw2 >> 4) & 0x1u);
            if ((hw2 & 0x00f0u) == ((mm_u32)m_bit << 4) && ra != 15u && rn != 15u && rd != 15u && rm != 15u) {
                d.kind = m_bit ? MM_OP_SMLADX : MM_OP_SMLAD;
                d.rn = rn;
                d.rm = rm;
                d.rd = rd;
                d.ra = ra;
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* SMLALD / SMLALDX (Thumb-2 signed dual multiply-accumulate long)
     * SMLALD:  1111 1011 1100 Rn | RdLo RdHi 1100 Rm
     * SMLALDX: 1111 1011 1100 Rn | RdLo RdHi 1101 Rm
     */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xfbc0u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 rdlo = (mm_u8)((hw2 >> 12) & 0x0fu);
            mm_u8 rdhi = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 rm = (mm_u8)(hw2 & 0x0fu);
            mm_u8 m_bit = (mm_u8)((hw2 >> 4) & 0x1u);
            if ((hw2 & 0x00f0u) == (0x00c0u | ((mm_u32)m_bit << 4)) && rn != 15u && rdlo != 15u && rdhi != 15u && rm != 15u) {
                d.kind = m_bit ? MM_OP_SMLALDX : MM_OP_SMLALD;
                d.rn = rn;
                d.rm = rm;
                d.rd = rdlo;
                d.ra = rdhi;
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* PKHBT / PKHTB (Thumb-2 pack halfwords)
     * PKHBT: 1110 1010 1100 Rn | imm3 Rd tb imm2 type Rm
     * PKHTB: Same encoding, tb bit = 1
     * type field encodes shift: 00=LSL#0, 10=LSL#imm (for PKHBT), 01/10=ASR (for PKHTB)
     */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xeac0u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 rm = (mm_u8)(hw2 & 0x0fu);
            mm_u8 tb = (mm_u8)((hw2 >> 5) & 0x1u);
            mm_u8 imm3 = (mm_u8)((hw2 >> 12) & 0x7u);
            mm_u8 imm2 = (mm_u8)((hw2 >> 6) & 0x3u);
            mm_u8 shift = (mm_u8)((imm3 << 2) | imm2);
            if (rn != 15u && rd != 15u && rm != 15u) {
                d.kind = tb ? MM_OP_PKHTB : MM_OP_PKHBT;
                d.rn = rn;
                d.rm = rm;
                d.rd = rd;
                d.imm = shift;
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* DSP parallel add/sub family (ARMv7-M DSP, optional in ARMv8-M).
     * Encoding: 1111 1010 1 OPC[2:0] Rn | 1111 Rd VAR[3:0] Rm
     *   OPC[2:0]: 000=ADD8, 001=ADD16, 010=ASX, 100=SUB8, 101=SUB16, 110=SAX
     *   VAR[3:0]: 0000=signed, 0001=Q sat signed, 0010=SH halving signed,
     *             0100=unsigned, 0101=UQ sat unsigned, 0110=UH halving unsigned,
     *             1000=SEL (opc=010 only)
     */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xff80u) == 0xfa80u && (hw2 & 0xf000u) == 0xf000u) {
            mm_u8 opc = (mm_u8)((hw1 >> 4) & 0x7u);
            mm_u8 rn  = (mm_u8)(hw1 & 0x0fu);
            mm_u8 rd  = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 var = (mm_u8)((hw2 >> 4) & 0xfu);
            mm_u8 rm  = (mm_u8)(hw2 & 0x0fu);
            enum mm_op_kind kind = MM_OP_UNDEFINED;
            switch ((mm_u32)opc << 4 | var) {
            case 0x00: kind = MM_OP_SADD8;  break;
            case 0x01: kind = MM_OP_QADD8;  break;
            case 0x02: kind = MM_OP_SHADD8; break;
            case 0x04: kind = MM_OP_UADD8;  break;
            case 0x05: kind = MM_OP_UQADD8; break;
            case 0x06: kind = MM_OP_UHADD8; break;
            case 0x10: kind = MM_OP_SADD16; break;
            case 0x11: kind = MM_OP_QADD16; break;
            case 0x12: kind = MM_OP_SHADD16; break;
            case 0x14: kind = MM_OP_UADD16; break;
            case 0x15: kind = MM_OP_UQADD16; break;
            case 0x16: kind = MM_OP_UHADD16; break;
            case 0x20: kind = MM_OP_SASX;   break;
            case 0x21: kind = MM_OP_QASX;   break;
            case 0x22: kind = MM_OP_SHASX;  break;
            case 0x24: kind = MM_OP_UASX;   break;
            case 0x25: kind = MM_OP_UQASX;  break;
            case 0x26: kind = MM_OP_UHASX;  break;
            case 0x28: kind = MM_OP_SEL;    break;
            case 0x40: kind = MM_OP_SSUB8;  break;
            case 0x41: kind = MM_OP_QSUB8;  break;
            case 0x42: kind = MM_OP_SHSUB8; break;
            case 0x44: kind = MM_OP_USUB8;  break;
            case 0x45: kind = MM_OP_UQSUB8; break;
            case 0x46: kind = MM_OP_UHSUB8; break;
            case 0x50: kind = MM_OP_SSUB16; break;
            case 0x51: kind = MM_OP_QSUB16; break;
            case 0x52: kind = MM_OP_SHSUB16; break;
            case 0x54: kind = MM_OP_USUB16; break;
            case 0x55: kind = MM_OP_UQSUB16; break;
            case 0x56: kind = MM_OP_UHSUB16; break;
            case 0x60: kind = MM_OP_SSAX;   break;
            case 0x61: kind = MM_OP_QSAX;   break;
            case 0x62: kind = MM_OP_SHSAX;  break;
            case 0x64: kind = MM_OP_USAX;   break;
            case 0x65: kind = MM_OP_UQSAX;  break;
            case 0x66: kind = MM_OP_UHSAX;  break;
            default: break;
            }
            if (kind != MM_OP_UNDEFINED && rn != 15u && rd != 15u && rm != 15u) {
                d.kind = kind;
                d.rn = rn;
                d.rd = rd;
                d.rm = rm;
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* SSAT16 / USAT16 (Thumb-2 parallel halfword saturate)
     * SSAT16: 1111 0011 0010 Rn | 0000 Rd 0000 sat_imm[3:0]
     * USAT16: 1111 0011 1010 Rn | 0000 Rd 0000 sat_imm[3:0]
     */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xff60u) == 0xf320u && (hw2 & 0xf0f0u) == 0x0000u) {
            mm_u8 unsigned_op = (mm_u8)((hw1 >> 7) & 0x1u);
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 sat = (mm_u8)(hw2 & 0x0fu);
            if (rn != 15u && rd != 15u) {
                d.kind = unsigned_op ? MM_OP_USAT16 : MM_OP_SSAT16;
                d.rn = rn;
                d.rd = rd;
                /* SSAT16: sat to (sat_imm+1) bits signed (1..16);
                 * USAT16: sat to sat_imm bits unsigned (0..15). */
                d.imm = unsigned_op ? (mm_u32)sat : (mm_u32)(sat + 1u);
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* USAD8 / USADA8 (Thumb-2 sum of absolute differences)
     * USAD8:  1111 1011 0111 Rn | 1111 Rd 0000 Rm   (Ra=15)
     * USADA8: 1111 1011 0111 Rn |  Ra  Rd 0000 Rm
     */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xfb70u && (hw2 & 0x00f0u) == 0x0000u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 ra = (mm_u8)((hw2 >> 12) & 0x0fu);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 rm = (mm_u8)(hw2 & 0x0fu);
            if (rn != 15u && rd != 15u && rm != 15u) {
                d.kind = (ra == 15u) ? MM_OP_USAD8 : MM_OP_USADA8;
                d.rn = rn;
                d.rd = rd;
                d.rm = rm;
                d.ra = ra;
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* SMUAD / SMUADX (Thumb-2 signed dual multiply, no accumulator).
     * Encoding: SMLAD with Ra=15.
     * SMUAD:  1111 1011 0010 Rn | 1111 Rd 0000 Rm
     * SMUADX: 1111 1011 0010 Rn | 1111 Rd 0001 Rm
     */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xfb20u && (hw2 & 0xf0e0u) == 0xf000u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 rm = (mm_u8)(hw2 & 0x0fu);
            mm_u8 m_bit = (mm_u8)((hw2 >> 4) & 0x1u);
            if (rn != 15u && rd != 15u && rm != 15u) {
                d.kind = m_bit ? MM_OP_SMUADX : MM_OP_SMUAD;
                d.rn = rn;
                d.rd = rd;
                d.rm = rm;
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* SMUSD / SMUSDX (Thumb-2 signed dual multiply-subtract, no accumulator).
     * SMUSD:  1111 1011 0100 Rn | 1111 Rd 0000 Rm
     * SMUSDX: 1111 1011 0100 Rn | 1111 Rd 0001 Rm
     */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xfb40u && (hw2 & 0xf0e0u) == 0xf000u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 rm = (mm_u8)(hw2 & 0x0fu);
            mm_u8 m_bit = (mm_u8)((hw2 >> 4) & 0x1u);
            if (rn != 15u && rd != 15u && rm != 15u) {
                d.kind = m_bit ? MM_OP_SMUSDX : MM_OP_SMUSD;
                d.rn = rn;
                d.rd = rd;
                d.rm = rm;
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* SMLSLD / SMLSLDX (Thumb-2 signed long multiply-subtract dual)
     * SMLSLD:  1111 1011 1101 Rn | RdLo RdHi 1100 Rm
     * SMLSLDX: 1111 1011 1101 Rn | RdLo RdHi 1101 Rm
     */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xfbd0u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 rdlo = (mm_u8)((hw2 >> 12) & 0x0fu);
            mm_u8 rdhi = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 rm = (mm_u8)(hw2 & 0x0fu);
            mm_u8 m_bit = (mm_u8)((hw2 >> 4) & 0x1u);
            if ((hw2 & 0x00f0u) == (0x00c0u | ((mm_u32)m_bit << 4)) &&
                rn != 15u && rdlo != 15u && rdhi != 15u && rm != 15u) {
                d.kind = m_bit ? MM_OP_SMLSLDX : MM_OP_SMLSLD;
                d.rn = rn;
                d.rd = rdlo;
                d.ra = rdhi;
                d.rm = rm;
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* QADD / QSUB (Thumb-2 saturating add/subtract)
     * QADD: 1111 1010 1000 Rn | 1111 Rd 1000 Rm
     * QSUB: 1111 1010 1000 Rn | 1111 Rd 1010 Rm
     */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xfa80u && (hw2 & 0xf000u) == 0xf000u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 rm = (mm_u8)(hw2 & 0x0fu);
            mm_u8 op = (mm_u8)((hw2 >> 4) & 0x0fu);
            if (rn != 15u && rd != 15u && rm != 15u && (op == 0x8u || op == 0xau)) {
                d.kind = (op == 0x8u) ? MM_OP_QADD : MM_OP_QSUB;
                d.rn = rn;
                d.rm = rm;
                d.rd = rd;
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* QDADD / QDSUB (Thumb-2 saturating double and add/subtract)
     * QDADD: 1111 1010 1000 Rn | 1111 Rd 1001 Rm
     * QDSUB: 1111 1010 1000 Rn | 1111 Rd 1011 Rm
     */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xfa80u && (hw2 & 0xf000u) == 0xf000u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 rm = (mm_u8)(hw2 & 0x0fu);
            mm_u8 op = (mm_u8)((hw2 >> 4) & 0x0fu);
            if (rn != 15u && rd != 15u && rm != 15u && (op == 0x9u || op == 0xbu)) {
                d.kind = (op == 0x9u) ? MM_OP_QDADD : MM_OP_QDSUB;
                d.rn = rn;
                d.rm = rm;
                d.rd = rd;
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* SMLSD / SMLSDX (Thumb-2 signed dual multiply-subtract)
     * SMLSD:  1111 1011 0100 Rn | Ra Rd 0000 Rm
     * SMLSDX: 1111 1011 0100 Rn | Ra Rd 0001 Rm
     */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xfb40u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 ra = (mm_u8)((hw2 >> 12) & 0x0fu);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 rm = (mm_u8)(hw2 & 0x0fu);
            mm_u8 m_bit = (mm_u8)((hw2 >> 4) & 0x1u);
            if ((hw2 & 0x00f0u) == ((mm_u32)m_bit << 4) && ra != 15u && rn != 15u && rd != 15u && rm != 15u) {
                d.kind = m_bit ? MM_OP_SMLSDX : MM_OP_SMLSD;
                d.rn = rn;
                d.rm = rm;
                d.rd = rd;
                d.ra = ra;
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* SMULBB/BT/TB/TT (Thumb-2 signed halfword multiply)
     * SMULxy: 1111 1011 0001 Rn | 1111 Rd 00xy Rm
     * x selects Rn halfword: 0=bottom, 1=top
     * y selects Rm halfword: 0=bottom, 1=top
     */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xfb10u && (hw2 & 0xf0c0u) == 0xf000u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 rm = (mm_u8)(hw2 & 0x0fu);
            mm_u8 xy = (mm_u8)((hw2 >> 4) & 0x3u);
            if (rn != 15u && rd != 15u && rm != 15u) {
                d.kind = MM_OP_SMULBB;
                d.rn = rn;
                d.rm = rm;
                d.rd = rd;
                d.imm = xy;
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* SSAT / USAT (Thumb-2 saturate to N bits)
     * SSAT: 1111 0011 00sh Rn | 0 imm3 Rd imm2 sh sat_imm
     * USAT: 1111 0011 10sh Rn | 0 imm3 Rd imm2 sh sat_imm
     */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xff00u) == 0xf300u && (hw2 & 0x8000u) == 0x0000u) {
            mm_u8 opcode = (mm_u8)((hw1 >> 6) & 0x3u);  /* bits [7:6]: 00=SSAT, 10=USAT */
            mm_u8 sh_hi = (mm_u8)((hw1 >> 5) & 0x1u);
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 imm3 = (mm_u8)((hw2 >> 12) & 0x7u);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 imm2 = (mm_u8)((hw2 >> 6) & 0x3u);
            mm_u8 sh_lo = (mm_u8)((hw2 >> 5) & 0x1u);
            mm_u8 sat_imm = (mm_u8)(hw2 & 0x1fu);
            mm_u8 shift_type = (mm_u8)((sh_hi << 1) | sh_lo);
            mm_u8 shift_imm = (mm_u8)((imm3 << 2) | imm2);
            if ((opcode == 0x0u || opcode == 0x2u) && rn != 15u && rd != 15u && shift_type <= 0x2u) {
                d.kind = (opcode == 0x0u) ? MM_OP_SSAT : MM_OP_USAT;
                d.rn = rn;
                d.rd = rd;
                d.imm = (shift_type << 16) | (shift_imm << 8) | sat_imm;
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* SMMUL / SMMLA / SMMLS (Thumb-2 signed most significant word multiply) */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xfb50u || (hw1 & 0xfff0u) == 0xfb60u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 ra = (mm_u8)((hw2 >> 12) & 0x0fu);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 rm = (mm_u8)(hw2 & 0x0fu);
            mm_u8 round = (mm_u8)((hw2 >> 4) & 0x1u);
            if ((hw2 & 0x00f0u) == ((mm_u32)round << 4)) {
                if (rn != 15u && rd != 15u && rm != 15u) {
                    if (ra == 15u) {
                        d.kind = MM_OP_SMMUL;
                        d.rn = rn;
                        d.rm = rm;
                        d.rd = rd;
                        d.imm = round;
                        d.undefined = MM_FALSE;
                        return d;
                    } else {
                        mm_bool is_smmls = ((hw1 & 0xfff0u) == 0xfb60u) ? MM_TRUE : MM_FALSE;
                        d.kind = is_smmls ? MM_OP_SMMLS : MM_OP_SMMLA;
                        d.rn = rn;
                        d.rm = rm;
                        d.rd = rd;
                        d.ra = ra;
                        d.imm = round;
                        d.undefined = MM_FALSE;
                        return d;
                    }
                }
            }
        }
    }

    /* SMLAWB, SMLAWT, SMULWB, SMULWT - DSP signed multiply word by halfword
     * Pattern: 1111 1011 0011 Rn:4 Ra:4 Rd:4 000 M:1 Rm:4
     * M=0: SMLAWB/SMULWB (bottom halfword), M=1: SMLAWT/SMULWT (top halfword)
     * Ra=1111 distinguishes SMULW* (no accumulate) from SMLAW* (with accumulate)
     * UNPREDICTABLE: Rd/Rn/Rm/Ra = SP or PC, but Capstone accepts SP, so we do too
     */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xfb30u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 ra = (mm_u8)((hw2 >> 12) & 0x0fu);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 m_bit = (mm_u8)((hw2 >> 4) & 0x1u);
            mm_u8 rm = (mm_u8)(hw2 & 0x0fu);
            if ((hw2 & 0x00f0u) == ((mm_u32)m_bit << 4)) {
                /* PC is always UNDEFINED, but accept SP for Capstone compatibility */
                if (rn != 15u && rd != 15u && rm != 15u) {
                    if (ra == 15u) {
                        d.kind = m_bit ? MM_OP_SMULWT : MM_OP_SMULWB;
                        d.rn = rn;
                        d.rm = rm;
                        d.rd = rd;
                        d.undefined = MM_FALSE;
                        return d;
                    } else if (ra != 15u) {
                        d.kind = m_bit ? MM_OP_SMLAWT : MM_OP_SMLAWB;
                        d.rn = rn;
                        d.rm = rm;
                        d.rd = rd;
                        d.ra = ra;
                        d.undefined = MM_FALSE;
                        return d;
                    }
                }
            }
        }
    }

    /* CLZ (Thumb-2): 1111 1010 1011 Rm | 1111 0000 1000 Rd */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xfab0u && (hw2 & 0xf0f0u) == 0xf080u) {
            mm_u8 rm = (mm_u8)(hw1 & 0x0fu);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 rm2 = (mm_u8)(hw2 & 0x0fu);
            if (rm == rm2 && rd != 15u && rm != 15u) {
                d.kind = MM_OP_CLZ;
                d.rd = rd;
                d.rm = rm;
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* LDRSH (immediate) Thumb-2, T1: 1111 1001 1011 Rn | Rt imm12 */
    if ((insn & 0xfff00000u) == 0xf9b00000u) {
        mm_u8 rn = (mm_u8)((insn >> 16) & 0x0fu);
        mm_u8 rt = (mm_u8)((insn >> 12) & 0x0fu);
        mm_u32 imm12 = insn & 0x0fffu;
        if (rt == 15u) {
            return d;
        }
        d.kind = MM_OP_LDRSH_IMM;
        d.rn = rn;
        d.rd = rt;
        d.imm = imm12;
        d.undefined = MM_FALSE;
        return d;
    }
    /* LDRSH (immediate) Thumb-2, T2: 1111 1001 0011 Rn | 1 P U W imm8 
     * Also handles LDRSHT when P=0, W=1, bits[11:8]=0xe (unprivileged) */
    if ((insn & 0xfff00000u) == 0xf9300000u && (insn & 0x00000800u) == 0x00000800u) {
        mm_u8 p = (mm_u8)((insn >> 10) & 1u);
        mm_u8 u = (mm_u8)((insn >> 9) & 1u);
        mm_u8 w = (mm_u8)((insn >> 8) & 1u);
        
        /* Check for unprivileged variant (P=0, W=1, bits[11:8]=0xe) */
        if (p == 0u && w == 1u && ((insn >> 8) & 0xfu) == 0xeu) {
            mm_u8 rn = (mm_u8)((insn >> 16) & 0x0fu);
            mm_u8 rt = (mm_u8)((insn >> 12) & 0x0fu);
            mm_u32 imm8 = insn & 0xffu;
            d.kind = MM_OP_LDRSHT;
            d.rn = rn;
            d.rd = rt;
            d.imm = imm8;
            d.undefined = MM_FALSE;
            return d;
        }
        
        if (p == 1u && w == 0u) {
            mm_u8 rn = (mm_u8)((insn >> 16) & 0x0fu);
            mm_u8 rt = (mm_u8)((insn >> 12) & 0x0fu);
            mm_u32 imm8 = insn & 0xffu;
            mm_u32 off = u ? imm8 : (0u - imm8);
            if (rt == 15u) {
                return d;
            }
            d.kind = MM_OP_LDRSH_IMM;
            d.rn = rn;
            d.rd = rt;
            d.imm = off;
            d.undefined = MM_FALSE;
            return d;
        }
    }

    /* LDAEX (word) */
    if ((insn & 0xfff00ff0u) == 0xe8d00fe0u) {
        d.kind = MM_OP_LDREX;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.undefined = MM_FALSE;
        return d;
    }

    /* STLEX (word) */
    if ((insn & 0xfff00ff0u) == 0xe8c00fe0u) {
        d.kind = MM_OP_STREX;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rm = (mm_u8)((insn >> 12) & 0x0fu); /* Rt value */
        d.rd = (mm_u8)(insn & 0x0fu);        /* Rd status */
        d.undefined = MM_FALSE;
        return d;
    }

    /* LDREX (word) */
    if ((insn & 0xfff00f00u) == 0xe8500f00u) {
        d.kind = MM_OP_LDREX;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.imm = (insn & 0xffu) << 2;
        d.undefined = MM_FALSE;
        return d;
    }

    /* LDAEXB/LDREXB (byte) */
    if ((insn & 0xfff00ff0u) == 0xe8d00fc0u) {
        d.kind = MM_OP_LDREXB;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.undefined = MM_FALSE;
        return d;
    }

    /* LDAEXH/LDREXH (halfword) */
    if ((insn & 0xfff00ff0u) == 0xe8d00f50u) {
        d.kind = MM_OP_LDREXH;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.undefined = MM_FALSE;
        return d;
    }

    /* STREX (word) */
    if ((insn & 0xfff00000u) == 0xe8400000u) {
        d.kind = MM_OP_STREX;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rm = (mm_u8)((insn >> 12) & 0x0fu); /* Rt value */
        d.rd = (mm_u8)((insn >> 8) & 0x0fu);  /* Rd status */
        d.imm = (insn & 0xffu) << 2;
        d.undefined = MM_FALSE;
        return d;
    }

    /* STREXB (byte) */
    if ((insn & 0xfff00ff0u) == 0xe8c00f40u) {
        d.kind = MM_OP_STREXB;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rm = (mm_u8)((insn >> 12) & 0x0fu); /* Rt value */
        d.rd = (mm_u8)(insn & 0x0fu);        /* Rd status */
        d.undefined = MM_FALSE;
        return d;
    }

    /* STLEXH/STREXH (halfword) */
    if ((insn & 0xfff00ff0u) == 0xe8c00f50u) {
        d.kind = MM_OP_STREXH;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rm = (mm_u8)((insn >> 12) & 0x0fu); /* Rt value */
        d.rd = (mm_u8)(insn & 0x0fu);        /* Rd status */
        d.undefined = MM_FALSE;
        return d;
    }

    /* LDRD/STRD (immediate)
     *
     * Thumb‑2 encoding T1/T2 layout (Armv8‑M):
     *   hw1: 1110 100x 0 PUW Rn Rt<15:12>
     *   hw2: Rt2<15:12> imm4<11:8> imm8<7:0>
     *
     * In the assembled 32‑bit word, Rt is in bits 12..15 and Rt2 is in bits 8..11.
     * The previous implementation mistakenly swapped these, so STRD/LDRD wrote
     * the second register where the first should go (and vice‑versa), corrupting
     * stacked frames for the RTOS test.  Swap the fields to match the architecture.
     */
    if ((insn & 0xfe400000u) == 0xe8400000u &&
        (insn & 0xfff0ffe0u) != 0xe8d0f000u) {
        mm_bool load = ((insn >> 20) & 1u) != 0u;
        mm_bool u = ((insn >> 23) & 1u) != 0u;
        mm_bool w = ((insn >> 21) & 1u) != 0u;
        mm_bool p = ((insn >> 24) & 1u) != 0u;
        mm_u32 imm = (insn & 0xffu) << 2;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        if (d.rn == 15u) {
            return d; /* UNPRED/undefined (or SG handled earlier) */
        }
        d.kind = load ? MM_OP_LDRD : MM_OP_STRD;
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);  /* Rt  */
        d.rm = (mm_u8)((insn >> 8)  & 0x0fu);  /* Rt2 */
        d.imm = imm | (u ? 0x80000000u : 0u) | (w ? 0x40000000u : 0u) | (p ? 0x20000000u : 0u);
        d.undefined = MM_FALSE;
        return d;
    }

    /* 32‑bit STM/LDM (Thumb‑2) encodings:
     * First halfword: 1110 100 opc0 W L Rn
     * Second halfword: P M 0 register_list[12:0]
     */
    if ((insn & 0xfe400000u) == 0xe8000000u) {
        mm_u8 opc = (mm_u8)((insn >> 23) & 0x3u); /* 01=IA, 10=DB */
        mm_u8 w = (mm_u8)((insn >> 21) & 0x1u);
        mm_u8 l = (mm_u8)((insn >> 20) & 0x1u);
        mm_u8 rn = (mm_u8)((insn >> 16) & 0x0fu);
        mm_u32 mask = insn & 0xffffu;
        /* Bits: [15]=P(PC), [14]=M(LR), [12:0]=R0-R12 (bit13 reserved/zero) */
        d.kind = l ? MM_OP_LDM : MM_OP_STM;
        d.rn = rn;
        d.imm = (mm_u32)((opc << 24) | (w << 16) | mask);
        d.undefined = MM_FALSE;
        return d;
    }

    /* MRS (system register to general-purpose register) */
    if ((insn & 0xfffff000u) == 0xf3ef8000u) {
        d.kind = MM_OP_MRS;
        d.rd = (mm_u8)((insn >> 8) & 0x0fu);
        if (d.rd == 15u) {
            return d; /* UNPRED: treat as undefined */
        }
        d.imm = insn & 0xffu; /* sysm value */
        d.undefined = MM_FALSE;
        return d;
    }

    /* STLB (store-release byte) Thumb-2: 1110 1000 1100 Rn | Rt 1111 1000 1111 */
    if ((insn & 0xfff00fffu) == 0xe8c00f8fu) {
        d.kind = MM_OP_STRB_IMM;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.imm = 0u;
        d.undefined = MM_FALSE;
        return d;
    }

    /* Barrier instructions: decode as distinct ops (semantics currently NOP). */
    if ((insn & 0xfffffff0u) == 0xf3bf8f40u) { /* DSB (option in low nibble) */
        d.kind = MM_OP_DSB;
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xfffffff0u) == 0xf3bf8f50u) { /* DMB (option in low nibble) */
        d.kind = MM_OP_DMB;
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xfffffff0u) == 0xf3bf8f60u) { /* ISB (option in low nibble) */
        d.kind = MM_OP_ISB;
        d.undefined = MM_FALSE;
        return d;
    }

    /* MVN (register) Thumb-2, T2 (shift/RRX): 1110 1010 0 011 S 1111 | 0 imm3 Rd imm2 type Rm */
    if ((insn & 0xffef8000u) == 0xea6f0000u) {
        mm_u8 rd = (mm_u8)((insn >> 8) & 0x0fu);
        mm_u8 rm = (mm_u8)(insn & 0x0fu);
        mm_u8 imm3 = (mm_u8)((insn >> 12) & 0x7u);
        mm_u8 imm2 = (mm_u8)((insn >> 6) & 0x3u);
        mm_u8 type = (mm_u8)((insn >> 4) & 0x3u);
        if (rd == 15u || rm == 15u) {
            return d;
        }
        d.kind = MM_OP_MVN_REG;
        d.rd = rd;
        d.rm = rm;
        d.imm = (mm_u32)((type << 5) | ((imm3 << 2) | imm2));
        d.undefined = MM_FALSE;
        return d;
    }

    /* MVN (immediate) Thumb-2, T1 (modified immediate):
     * 11110 i 0 0011 S 1111 0 imm3 Rd imm8
     * Keep imm12 (unexpanded) so execution can compute carry-out.
     */
    if ((insn & 0xfbef8000u) == 0xf06f0000u) {
        mm_u32 i = (insn >> 26) & 1u;
        mm_u8 rd = (mm_u8)((insn >> 8) & 0x0fu);
        mm_u32 imm3 = (insn >> 12) & 0x7u;
        mm_u32 imm8 = insn & 0xffu;
        mm_u32 imm12 = (i << 11) | (imm3 << 8) | imm8;
        if (rd == 15u) {
            return d;
        }
        d.kind = MM_OP_MVN_IMM;
        d.rd = rd;
        d.imm = imm12;
        d.undefined = MM_FALSE;
        return d;
    }

    /* MSR (general-purpose to system register / PSR fields) Thumb-2, T1:
     * 1111 0011 1000 Rm | 1000 mask sysm
     * Match MSR only (op=0). Keep (mask, sysm) so execution can either
     * select a special register (mask=8) or update APSR fields. */
    if ((insn & 0xfff0f000u) == 0xf3808000u) {
        mm_u8 mask = (mm_u8)((insn >> 8) & 0xfu);
        mm_u8 sysm = (mm_u8)(insn & 0xffu);
        d.kind = MM_OP_MSR;
        d.rm = (mm_u8)((insn >> 16) & 0x0fu); /* source register is in hw1[3:0] */
        d.imm = ((mm_u32)mask << 8) | (mm_u32)sysm; /* pack mask+sysm */
        d.undefined = MM_FALSE;
        return d;
    }

    /* BLX (immediate) Thumb-2 (T2).
     * Not supported on M-profile (no ARM state), treat as undefined. */
    if ((insn & 0xf800d000u) == 0xf000c000u) {
        return d;
    }

    /* BL (T1) */
    if ((insn & 0xf800d000u) == 0xf000d000u) {
        mm_u32 s = (insn >> 26) & 0x1u;
        mm_u32 j1 = (insn >> 13) & 0x1u;
        mm_u32 j2 = (insn >> 11) & 0x1u;
        mm_u32 imm10 = (insn >> 16) & 0x3ffu;
        mm_u32 imm11 = insn & 0x7ffu;
        mm_u32 i1 = (j1 ^ 1u) ^ s;
        mm_u32 i2 = (j2 ^ 1u) ^ s;
        mm_u32 imm = (s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1);
        if (s != 0) {
            imm |= 0xfe000000u;
        }
        d.kind = MM_OP_BL;
        d.imm = imm;
        d.undefined = MM_FALSE;
        return d;
    }

    /* UDF.W (Thumb-2): 1111 0111 1111 0 imm4 | 1010 imm12 */
    if ((insn & 0xfff0f000u) == 0xf7f0a000u) {
        d.kind = MM_OP_UDF;
        d.imm = ((insn >> 4) & 0xf000u) | (insn & 0x0fffu);
        d.undefined = MM_FALSE;
        return d;
    }

    /* B<cond>.W (conditional branch) */
    if ((insn & 0xf800d000u) == 0xf0008000u) {
        mm_u32 s = (insn >> 26) & 0x1u;
        mm_u32 cond = (insn >> 22) & 0x0fu;
        mm_u32 imm6 = (insn >> 16) & 0x003fu;
        mm_u32 j1 = (insn >> 13) & 0x1u;
        mm_u32 j2 = (insn >> 11) & 0x1u;
        mm_u32 imm11 = insn & 0x07ffu;
        mm_u32 imm;
        if (cond >= 0xEu) {
            return d;
        }
        /* T3: offset = SignExtend(S:J1:J2:imm6:imm11:0). J1/J2 are not inverted. */
        /* T3: offset = SignExtend(S:J2:J1:imm6:imm11:0). J1/J2 are not inverted. */
        imm = (s << 20) | (j2 << 19) | (j1 << 18) | (imm6 << 12) | (imm11 << 1);
        if (s != 0u) {
            imm |= 0xffe00000u;
        }
        d.kind = MM_OP_B_COND_WIDE;
        d.cond = (enum mm_cond)cond;
        d.imm = imm;
        d.undefined = MM_FALSE;
        return d;
    }

    /* B.W unconditional (T4) */
    if ((insn & 0xf800d000u) == 0xf0009000u) {
        mm_u32 s = (insn >> 26) & 0x1u;
        mm_u32 imm10 = (insn >> 16) & 0x03ffu;
        mm_u32 j1 = (insn >> 13) & 0x1u;
        mm_u32 j2 = (insn >> 11) & 0x1u;
        mm_u32 imm11 = insn & 0x07ffu;
        mm_u32 i1 = (j1 ^ 1u) ^ s;
        mm_u32 i2 = (j2 ^ 1u) ^ s;
        mm_u32 imm = (s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1);
        if (s != 0u) {
            imm |= 0xfe000000u;
        }
        d.kind = MM_OP_B_UNCOND_WIDE;
        d.imm = imm;
        d.undefined = MM_FALSE;
        return d;
    }

    /* LSL/LSR/ASR (register) Thumb-2, T2: 1111 1010 000{0,1,2} Rm | 1111 Rd 0000 Rs
     * S bit is bit[20] (ignored in mask to allow flag-setting variants).
     */
    if ((insn & 0xffe0f0f0u) == 0xfa00f000u) {
        d.kind = MM_OP_LSL_REG;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu); /* Rm (value) */
        d.rd = (mm_u8)((insn >> 8) & 0x0fu);
        d.rm = (mm_u8)(insn & 0x0fu);         /* Rs (shift) */
        d.undefined = MM_FALSE;
        return d;
    }

    /* REV (Thumb-2): 1111 1010 1001 0000 1111 Rd 1000 Rm */
    if ((insn & 0xfff0f0f0u) == 0xfa90f080u) {
        d.kind = MM_OP_REV;
        d.rd = (mm_u8)((insn >> 8) & 0x0fu);
        d.rm = (mm_u8)(insn & 0x0fu);
        d.undefined = MM_FALSE;
        return d;
    }

    /* REV16 (Thumb-2): 1111 1010 1001 0000 1111 Rd 1001 Rm */
    if ((insn & 0xfff0f0f0u) == 0xfa90f090u) {
        d.kind = MM_OP_REV16;
        d.rd = (mm_u8)((insn >> 8) & 0x0fu);
        d.rm = (mm_u8)(insn & 0x0fu);
        d.undefined = MM_FALSE;
        return d;
    }

    /* RBIT (Thumb-2): 1111 1010 1001 0000 1111 Rd 1010 Rm */
    if ((insn & 0xfff0f0f0u) == 0xfa90f0a0u) {
        d.kind = MM_OP_RBIT;
        d.rd = (mm_u8)((insn >> 8) & 0x0fu);
        d.rm = (mm_u8)(insn & 0x0fu);
        d.undefined = MM_FALSE;
        return d;
    }

    /* REVSH (Thumb-2): 1111 1010 1001 0000 1111 Rd 1011 Rm */
    if ((insn & 0xfff0f0f0u) == 0xfa90f0b0u) {
        d.kind = MM_OP_REVSH;
        d.rd = (mm_u8)((insn >> 8) & 0x0fu);
        d.rm = (mm_u8)(insn & 0x0fu);
        d.undefined = MM_FALSE;
        return d;
    }

    /* CLZ (Thumb-2): 1111 1010 1011 0000 1111 Rd 1000 Rm */
    if ((insn & 0xfff0f0f0u) == 0xfab0f080u) {
        d.kind = MM_OP_CLZ;
        d.rd = (mm_u8)((insn >> 8) & 0x0fu);
        d.rm = (mm_u8)(insn & 0x0fu);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffe0f0f0u) == 0xfa20f000u) {
        d.kind = MM_OP_LSR_REG;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu); /* Rm (value) */
        d.rd = (mm_u8)((insn >> 8) & 0x0fu);
        d.rm = (mm_u8)(insn & 0x0fu);         /* Rs (shift) */
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffe0f0f0u) == 0xfa40f000u) {
        d.kind = MM_OP_ASR_REG;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu); /* Rm (value) */
        d.rd = (mm_u8)((insn >> 8) & 0x0fu);
        d.rm = (mm_u8)(insn & 0x0fu);         /* Rs (shift) */
        d.undefined = MM_FALSE;
        return d;
    }

    /* ROR (register) Thumb-2, T2: 1111 1010 0110 Rm | 1111 Rd 0000 Rs */
    if ((insn & 0xffe0f0f0u) == 0xfa60f000u) {
        d.kind = MM_OP_ROR_REG;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu); /* Rm (value) */
        d.rd = (mm_u8)((insn >> 8) & 0x0fu);
        d.rm = (mm_u8)(insn & 0x0fu);         /* Rs (shift) */
        d.undefined = MM_FALSE;
        return d;
    }

    /* ADD (immediate) Thumb-2:
     *  - opcode 0100, S=1 -> ADD (imm) T3 uses ThumbExpandImm
     *  - opcode 0100, S=0 -> ADDW (imm) T4 uses plain imm12
     */
    /* RSB (immediate) Thumb-2, T2: 11110 i 01110 S Rn | 0 imm3 Rd imm8 */
    if ((insn & 0xfbe08000u) == 0xf1c00000u) {
        mm_u32 imm12 = (((insn >> 26) & 1u) << 11) | (((insn >> 12) & 0x7u) << 8) | (insn & 0xffu);
        d.kind = MM_OP_RSB_IMM;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 8) & 0x0fu);
        d.imm = thumb_expand_imm12(imm12);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xfbf08000u) == 0xf2000000u) { /* ADDW (imm) T4: op=0100, S=0 */
        mm_u32 imm12 = (((insn >> 26) & 1u) << 11) | (((insn >> 12) & 0x7u) << 8) | (insn & 0xffu);
        d.kind = MM_OP_ADD_IMM;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 8) & 0x0fu);
        d.imm = imm12; /* no expansion */
        d.undefined = MM_FALSE;
        return d;
    }
    /* ORR (immediate) Thumb-2: opcode 0010, S bit selects flag-setting vs not. Always ThumbExpandImm.
     * MOV (immediate) is an alias with Rn=1111.
     */
    if ((insn & 0xfb600000u) == 0xf0400000u) { /* ORR (imm) T1, S bit ignored (i bit is part of imm12) */
        mm_u32 imm12 = (((insn >> 26) & 1u) << 11) | (((insn >> 12) & 0x7u) << 8) | (insn & 0xffu);
        mm_u8 rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 8) & 0x0fu);
        d.imm = thumb_expand_imm12(imm12);
        if (rn == 0x0fu) {
            d.kind = MM_OP_MOV_IMM;
        } else {
            d.kind = MM_OP_ORR_REG;
            d.rn = rn;
        }
        d.undefined = MM_FALSE;
        return d;
    }

    /* Data-processing (register) Thumb-2: minimal ADD/ORR (register) support */
    if ((insn & 0xfe000000u) == 0xea000000u) {
        mm_u8 opcode = (mm_u8)((insn >> 21) & 0x0fu);
        mm_u8 sbit = (mm_u8)((insn >> 20) & 0x1u);
        mm_u32 imm3 = (insn >> 12) & 0x7u;        /* bits 14:12 of hw2 */
        mm_u32 imm2 = (insn >> 6) & 0x3u;         /* bits 6:5 of hw2 */
        mm_u32 type = (insn >> 4) & 0x3u;         /* bits 5:4 of hw2 */
        mm_u32 imm5 = (imm3 << 2) | imm2;         /* shift amount */
        mm_u8 rn = (mm_u8)((insn >> 16) & 0x0fu);
        mm_u8 rd = (mm_u8)((insn >> 8) & 0x0fu);
        mm_u8 rm = (mm_u8)(insn & 0x0fu);
        if (opcode == 0x8u) { /* ADD (register) / CMN alias */
            if (rd == 0x0fu && sbit != 0u) {
                d.kind = MM_OP_CMN_REG;
                d.rn = rn;
                d.rm = rm;
                d.imm = (type << 5) | imm5;
                d.undefined = MM_FALSE;
                return d;
            }
            d.kind = MM_OP_ADD_REG;
            d.rn = rn;
            d.rd = rd;
            d.rm = rm;
            d.imm = (type << 5) | imm5;
            d.undefined = MM_FALSE;
            return d;
        } else if (opcode == 0xDu) { /* SUB/CMP (register) */
            if (rd == 0x0fu && sbit != 0u) {
                d.kind = MM_OP_CMP_REG;
                d.rn = rn;
            } else {
                d.kind = MM_OP_SUB_REG;
                d.rn = rn;
                d.rd = rd;
            }
            d.rm = rm;
            d.imm = (type << 5) | imm5;
            d.undefined = MM_FALSE;
            return d;
        } else if (opcode == 0x4u) { /* EOR (register) */
            if (rd == 0x0fu && sbit != 0u) {
                d.kind = MM_OP_TEQ_REG;
                d.rn = rn;
                d.rm = rm;
            } else {
                d.kind = MM_OP_EOR_REG;
                d.rn = rn;
                d.rd = rd;
                d.rm = rm;
            }
            d.imm = (type << 5) | imm5;
            d.undefined = MM_FALSE;
            return d;
        } else if (opcode == 0xAu) { /* ADC (register) */
            d.kind = MM_OP_ADCS_REG;
            d.rn = rn;
            d.rd = rd;
            d.rm = rm;
            d.imm = (type << 5) | imm5;
            d.undefined = MM_FALSE;
            return d;
        } else if (opcode == 0xBu) { /* SBC (register) */
            d.kind = MM_OP_SBCS_REG;
            d.rn = rn;
            d.rd = rd;
            d.rm = rm;
            d.imm = (type << 5) | imm5;
            d.undefined = MM_FALSE;
            return d;
        } else if (opcode == 0xEu) { /* RSB (register) */
            d.kind = MM_OP_RSB_REG;
            d.rn = rn;
            d.rd = rd;
            d.rm = rm;
            d.imm = (type << 5) | imm5;
            d.undefined = MM_FALSE;
            return d;
        } else if (opcode == 0x1u) { /* BIC (register) */
            d.kind = MM_OP_BIC_REG;
            d.rn = rn;
            d.rd = rd;
            d.rm = rm;
            d.imm = (type << 5) | imm5;
            d.undefined = MM_FALSE;
            return d;
        } else if (opcode == 0x0u) { /* AND (register) / TST alias */
            if (rd == 0x0fu && sbit != 0u) {
                d.kind = MM_OP_TST_REG;
                d.rn = rn;
                d.rm = rm;
                d.imm = (type << 5) | imm5;
                d.undefined = MM_FALSE;
                return d;
            }
            d.kind = MM_OP_AND_REG;
            d.rn = rn;
            d.rd = rd;
            d.rm = rm;
            d.imm = (type << 5) | imm5;
            d.undefined = MM_FALSE;
            return d;
        } else if (opcode == 0x2u) { /* ORR (register) */
            if (rn == 0x0fu && type <= 3u) {
                d.rd = rd;
                d.rm = rm;
                d.imm = imm5;
                if (type == 0u) {
                    if (imm5 == 0u) {
                        d.kind = MM_OP_MOV_REG;
                    } else {
                        d.kind = MM_OP_LSL_IMM;
                    }
                } else if (type == 1u) {
                    d.kind = MM_OP_LSR_IMM;
                } else if (type == 2u) {
                    d.kind = MM_OP_ASR_IMM;
                } else {
                    d.kind = MM_OP_ROR_IMM;
                }
                d.undefined = MM_FALSE;
                return d;
            }
            d.kind = MM_OP_ORR_REG;
            d.rn = rn;
            d.rd = rd;
            d.rm = rm;
            /* Pack type in bits 6:5, imm5 in bits 4:0 for executor. */
            d.imm = (type << 5) | imm5;
            d.undefined = MM_FALSE;
            return d;
        } else if (opcode == 0x3u) { /* ORN (register) */
            d.kind = MM_OP_ORN_REG;
            d.rn = rn;
            d.rd = rd;
            d.rm = rm;
            d.imm = (type << 5) | imm5;
            d.undefined = MM_FALSE;
            return d;
        }
    }

    /* SXTB/SXTH/UXTB/UXTH and add variants (Thumb-2): FAxF family with optional Rn add. */
    {
        mm_u32 mask = 0xfff0f000u;
        mm_u32 pat_uxtb = 0xfa50f000u;
        mm_u32 pat_sxtb = 0xfa40f000u;
        mm_u32 pat_uxth = 0xfa10f000u;
        mm_u32 pat_sxth = 0xfa00f000u;
        if ((insn & mask) == pat_uxtb || (insn & mask) == pat_sxtb ||
            (insn & mask) == pat_uxth || (insn & mask) == pat_sxth) {
            mm_u8 rn = (mm_u8)((insn >> 16) & 0x0fu);
            mm_u8 rd = (mm_u8)((insn >> 8) & 0x0fu);
            mm_u8 rm = (mm_u8)(insn & 0x0fu);
            mm_u8 rot2 = (mm_u8)((insn >> 4) & 0x3u);
            mm_bool add = (rn != 15u) ? MM_TRUE : MM_FALSE;
            if (rd == 15u || rm == 15u) {
                return d; /* UNPRED */
            }
            if ((insn & mask) == pat_sxtb) {
                d.kind = MM_OP_SXTB;
            } else if ((insn & mask) == pat_uxtb) {
                d.kind = MM_OP_UXTB;
            } else if ((insn & mask) == pat_sxth) {
                d.kind = MM_OP_SXTH;
            } else {
                d.kind = MM_OP_UXTH;
            }
            d.rd = rd;
            d.rm = rm;
            d.rn = rn;
            d.imm = (mm_u32)(rot2 << 3) | (add ? 0x80000000u : 0u);
            d.undefined = MM_FALSE;
            return d;
        }
    }

    /* LDRSB (immediate) Thumb-2, T1: 1111 1001 1001 Rn | Rt imm12 */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xf990u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 rt = (mm_u8)((hw2 >> 12) & 0x0fu);
            mm_u32 imm12 = hw2 & 0x0fffu;
            if (rt == 15u) return d;
            d.kind = MM_OP_LDRSB_IMM;
            d.rn = rn;
            d.rd = rt;
            d.imm = imm12;
            d.undefined = MM_FALSE;
            return d;
        }
    }
    /* LDRSB (immediate) Thumb-2, T2: 1111 1001 0001 Rn | 1 P U W imm8 
     * Also handles LDRSBT when P=0, W=1, bits[11:8]=0xe (unprivileged) */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xf910u && (hw2 & 0x0800u) == 0x0800u) {
            mm_u8 p = (mm_u8)((hw2 >> 10) & 1u);
            mm_u8 u = (mm_u8)((hw2 >> 9) & 1u);
            mm_u8 w = (mm_u8)((hw2 >> 8) & 1u);
            
            /* Check for unprivileged variant (P=0, W=1, bits[11:8]=0xe) */
            if (p == 0u && w == 1u && ((hw2 >> 8) & 0xfu) == 0xeu) {
                mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
                mm_u8 rt = (mm_u8)((hw2 >> 12) & 0x0fu);
                mm_u32 imm8 = hw2 & 0x00ffu;
                d.kind = MM_OP_LDRSBT;
                d.rn = rn;
                d.rd = rt;
                d.imm = imm8;
                d.undefined = MM_FALSE;
                return d;
            }
            
            if (p == 1u && w == 0u) { /* pre-indexed, no writeback => simple offset */
                mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
                mm_u8 rt = (mm_u8)((hw2 >> 12) & 0x0fu);
                mm_u32 imm8 = hw2 & 0x00ffu;
                mm_u32 off = u ? imm8 : (0u - imm8);
                if (rt == 15u) return d;
                d.kind = MM_OP_LDRSB_IMM;
                d.rn = rn;
                d.rd = rt;
                d.imm = off;
                d.undefined = MM_FALSE;
                return d;
            }
            if (p == 0u && w == 1u) { /* post-indexed */
                mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
                mm_u8 rt = (mm_u8)((hw2 >> 12) & 0x0fu);
                mm_u32 imm8 = hw2 & 0x00ffu;
                mm_u32 off = u ? imm8 : (0u - imm8);
                if (rt == 15u) return d;
                d.kind = MM_OP_LDRSB_POST_IMM;
                d.rn = rn;
                d.rd = rt;
                d.imm = off;
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* MOVW (T3) */
    if ((insn & 0xfbf08000u) == 0xf2400000u) {
        mm_u32 imm4 = (insn >> 16) & 0x0fu;
        mm_u32 i = (insn >> 26) & 0x1u;
        mm_u32 imm3 = (insn >> 12) & 0x7u;
        mm_u32 imm8 = insn & 0xffu;
        d.rd = (mm_u8)((insn >> 8) & 0x0fu);
        d.imm = (i << 11) | (imm4 << 12) | (imm3 << 8) | imm8;
        d.kind = MM_OP_MOVW;
        d.undefined = MM_FALSE;
        return d;
    }

    /* MOVT (T1) */
    if ((insn & 0xfbf08000u) == 0xf2c00000u) {
        mm_u32 imm4 = (insn >> 16) & 0x0fu;
        mm_u32 i = (insn >> 26) & 0x1u;
        mm_u32 imm3 = (insn >> 12) & 0x7u;
        mm_u32 imm8 = insn & 0xffu;
        d.rd = (mm_u8)((insn >> 8) & 0x0fu);
        d.imm = (i << 11) | (imm4 << 12) | (imm3 << 8) | imm8;
        d.kind = MM_OP_MOVT;
        d.undefined = MM_FALSE;
        return d;
    }

    /* Data-processing (modified immediate) Thumb-2:
     * minimum support for ORR/EOR/BIC/TST.
     */
    if ((insn & 0xfa000000u) == 0xf0000000u) {
        mm_u8 opcode = (mm_u8)((insn >> 21) & 0xfu);
        mm_u8 sbit = (mm_u8)((insn >> 20) & 0x1u);
        mm_u8 rn = (mm_u8)((insn >> 16) & 0x0fu);
        mm_u8 rd = (mm_u8)((insn >> 8) & 0x0fu);
        mm_u32 imm12 = ((insn >> 26) & 0x1u) << 11;
        imm12 |= ((insn >> 12) & 0x7u) << 8;
        imm12 |= (insn & 0xffu);
        d.imm = thumb_expand_imm12(imm12);
        d.rn = rn;
        d.rd = rd;
        switch (opcode) {
        case 0x0u:
            d.kind = (rd == 15u && sbit != 0u) ? MM_OP_TST_IMM : MM_OP_AND_REG; /* TST alias when Rd==PC and S=1 */
            break;
        case 0x1u: d.kind = MM_OP_BIC_REG; break;   /* BIC (imm) */
        case 0x2u: d.kind = MM_OP_ORR_REG; break;   /* ORR (imm) */
        case 0x3u: d.kind = MM_OP_ORN_IMM; break;   /* ORN (imm) */
        case 0x4u:
            d.kind = (rd == 15u && sbit != 0u) ? MM_OP_TEQ_IMM : MM_OP_EOR_REG; /* TEQ alias when Rd==PC and S=1 */
            break;   /* EOR (imm) */
        case 0xAu: d.kind = MM_OP_ADC_IMM; break;   /* ADC (imm) */
        case 0x8u:
            if (rd == 15u && sbit != 0u) {
                d.kind = MM_OP_CMN_IMM;
            } else {
                d.kind = MM_OP_ADD_IMM;
            }
            break;   /* ADD (imm) / CMN alias */
        case 0xBu:
            d.kind = (sbit != 0u) ? MM_OP_SBC_IMM : MM_OP_SBC_IMM_NF;
            break;
        case 0xDu:
            /* CMP alias when Rd == PC and S=1, otherwise SUB (imm). */
            if (rd == 15u && sbit != 0u) {
                d.kind = MM_OP_CMP_IMM;
            } else {
                d.kind = (sbit != 0u) ? MM_OP_SUB_IMM : MM_OP_SUB_IMM_NF;
            }
            break;
        default: d.kind = MM_OP_UNDEFINED; break;
        }
        if (d.kind != MM_OP_UNDEFINED) {
            d.undefined = MM_FALSE;
            return d;
        }
    }

    /* SUBW (immediate) Thumb-2: opcode 0010, S=0 uses plain imm12 */
    if (((insn & 0xff700000u) == 0xf2200000u || (insn & 0xff700000u) == 0xf6200000u) &&
        (((insn >> 20) & 1u) == 0u)) {
        mm_u32 imm12 = (((insn >> 26) & 1u) << 11) | (((insn >> 12) & 0x7u) << 8) | (insn & 0xffu);
        d.kind = MM_OP_SUB_IMM_NF;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 8) & 0x0fu);
        d.imm = imm12;
        d.undefined = MM_FALSE;
        return d;
    }

    /* STRH/LDRH (immediate) Thumb-2, T2: imm12 offset, no writeback. */
    if ((insn & 0xfff00000u) == 0xf8a00000u) {
        d.kind = MM_OP_STRH_IMM;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.imm = insn & 0x0fffu;
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xfff00000u) == 0xf8b00000u) {
        d.kind = MM_OP_LDRH_IMM;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.imm = insn & 0x0fffu;
        d.undefined = MM_FALSE;
        return d;
    }

    /* STR/LDR (immediate, word) Thumb-2 */
    if ((insn & 0xfff00000u) == 0xf8c00000u) {
        d.kind = MM_OP_STR_IMM;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.imm = insn & 0x0fffu;
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xfff00000u) == 0xf8d00000u) {
        d.kind = MM_OP_LDR_IMM;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.imm = insn & 0x0fffu;
        d.undefined = MM_FALSE;
        return d;
    }

    /* STR/LDR (register offset, word) Thumb-2 */
    if ((insn & 0xffc00f00u) == 0xf8400000u) {
        mm_bool load = ((insn >> 20) & 1u) != 0u;
        mm_bool wbit = ((insn >> 21) & 1u) != 0u;
        if (!wbit) { /* non writeback forms */
            d.kind = load ? MM_OP_LDR_REG : MM_OP_STR_REG;
            d.rn = (mm_u8)((insn >> 16) & 0x0fu);
            d.rd = (mm_u8)((insn >> 12) & 0x0fu);
            d.rm = (mm_u8)(insn & 0x0fu);
            d.imm = (insn >> 4) & 0x3u; /* imm2 shift */
            d.undefined = MM_FALSE;
            return d;
        }
    }

    /* STR/LDR post-indexed (Thumb-2) (encoding T4: imm8) */
    if ((insn & 0xfff00f00u) == 0xf8400b00u || (insn & 0xfff00f00u) == 0xf8400900u) {
        mm_u8 u = (mm_u8)((insn >> 9) & 1u);
        mm_u8 imm8 = (mm_u8)(insn & 0xffu);
        mm_i32 offset = u ? (mm_i32)imm8 : -(mm_i32)imm8;
        d.kind = MM_OP_STR_POST_IMM;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.imm = (mm_u32)offset;
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xfff00f00u) == 0xf8500b00u || (insn & 0xfff00f00u) == 0xf8500900u) {
        mm_u8 u = (mm_u8)((insn >> 9) & 1u);
        mm_u8 imm8 = (mm_u8)(insn & 0xffu);
        mm_i32 offset = u ? (mm_i32)imm8 : -(mm_i32)imm8;
        d.kind = MM_OP_LDR_POST_IMM;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.imm = (mm_u32)offset;
        d.undefined = MM_FALSE;
        return d;
    }

    /* STR/LDR offset (Thumb-2) (encoding T3: imm8, P=1, W=0)
     * Also handles STRT/LDRT when bits[11:8]=0xe (unprivileged) */
    if ((insn & 0xfff00f00u) == 0xf8400c00u) {
        mm_u8 u = (mm_u8)((insn >> 9) & 1u);
        mm_u8 imm8 = (mm_u8)(insn & 0xffu);
        mm_i32 offset = u ? (mm_i32)imm8 : -(mm_i32)imm8;
        d.kind = MM_OP_STR_IMM;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.imm = (mm_u32)offset;
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xfff00f00u) == 0xf8500c00u) {
        mm_u8 u = (mm_u8)((insn >> 9) & 1u);
        mm_u8 imm8 = (mm_u8)(insn & 0xffu);
        mm_i32 offset = u ? (mm_i32)imm8 : -(mm_i32)imm8;
        d.kind = MM_OP_LDR_IMM;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.imm = (mm_u32)offset;
        d.undefined = MM_FALSE;
        return d;
    }
    /* STRT/LDRT - unprivileged immediate offset (bits[11:8]=0xe) */
    if ((insn & 0xfff00f00u) == 0xf8400e00u) {
        mm_u8 imm8 = (mm_u8)(insn & 0xffu);
        d.kind = MM_OP_STRT;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.imm = (mm_u32)imm8;
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xfff00f00u) == 0xf8500e00u) {
        mm_u8 imm8 = (mm_u8)(insn & 0xffu);
        d.kind = MM_OP_LDRT;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.imm = (mm_u32)imm8;
        d.undefined = MM_FALSE;
        return d;
    }

    /* STR/LDR pre-indexed writeback (Thumb-2) (encoding T3: imm8, opcode bits 11:8 = 0b11x1) */
    if ((insn & 0xfff00f00u) == 0xf8400f00u || (insn & 0xfff00f00u) == 0xf8400d00u) {
        mm_u8 u = (mm_u8)((insn >> 9) & 1u);
        mm_u8 imm8 = (mm_u8)(insn & 0xffu);
        mm_i32 offset = u ? (mm_i32)imm8 : -(mm_i32)imm8;
        d.kind = MM_OP_STR_PRE_IMM;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.imm = (mm_u32)offset;
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xfff00f00u) == 0xf8500f00u || (insn & 0xfff00f00u) == 0xf8500d00u) {
        mm_u8 u = (mm_u8)((insn >> 9) & 1u);
        mm_u8 imm8 = (mm_u8)(insn & 0xffu);
        mm_i32 offset = u ? (mm_i32)imm8 : -(mm_i32)imm8;
        d.kind = MM_OP_LDR_PRE_IMM;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.imm = (mm_u32)offset;
        d.undefined = MM_FALSE;
        return d;
    }

    /* STRB/LDRB (register) Thumb-2: 1111 1000 000{0,1} Rn | Rt 0000 00ii Rm */
    if ((insn & 0xfff00fc0u) == 0xf8000000u || (insn & 0xfff00fc0u) == 0xf8100000u) {
        mm_bool load = ((insn >> 20) & 1u) != 0u;
        mm_u8 imm2 = (mm_u8)((insn >> 4) & 0x3u);
        mm_u8 rn = (mm_u8)((insn >> 16) & 0x0fu);
        if (rn == 15u) {
            /* Rn=PC is UNPRED for register form; allow immediate forms to decode. */
        } else {
        d.kind = load ? MM_OP_LDRB_REG : MM_OP_STRB_REG;
        d.rn = rn;
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.rm = (mm_u8)(insn & 0x0fu);
        d.imm = imm2; /* LSL #imm2 */
        d.undefined = MM_FALSE;
        return d;
        }
    }

    /* LDRSH (register) Thumb-2: 1111 1001 0011 Rn | Rt 0000 00ii Rm */
    if ((insn & 0xfff00fc0u) == 0xf9300000u) {
        mm_u8 imm2 = (mm_u8)((insn >> 4) & 0x3u);
        d.kind = MM_OP_LDRSH_REG;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.rm = (mm_u8)(insn & 0x0fu);
        d.imm = imm2; /* LSL #imm2 */
        d.undefined = MM_FALSE;
        return d;
    }

    /* LDRSB (register) Thumb-2: 1111 1001 0001 Rn | Rt 0000 00ii Rm */
    if ((insn & 0xfff00fc0u) == 0xf9100000u) {
        mm_u8 imm2 = (mm_u8)((insn >> 4) & 0x3u);
        d.kind = MM_OP_LDRSB_REG;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.rm = (mm_u8)(insn & 0x0fu);
        d.imm = imm2; /* LSL #imm2 */
        d.undefined = MM_FALSE;
        return d;
    }

    /* STRB/LDRB (immediate) Thumb-2, imm12 (T2). */
    if ((insn & 0xfff00000u) == 0xf8800000u || (insn & 0xfff00000u) == 0xf8900000u) {
        mm_u8 u = (mm_u8)((insn >> 23) & 1u);
        mm_u8 l = (mm_u8)((insn >> 20) & 1u);
        mm_u8 rn = (mm_u8)((insn >> 16) & 0x0fu);
        mm_u8 rt = (mm_u8)((insn >> 12) & 0x0fu);
        mm_u32 imm12 = insn & 0x0fffu;
        mm_u32 offset = u ? imm12 : (mm_u32)(0u - imm12);
        d.kind = (l != 0u) ? MM_OP_LDRB_IMM : MM_OP_STRB_IMM;
        d.rn = rn;
        d.rd = rt;
        d.imm = offset;
        d.undefined = MM_FALSE;
        return d;
    }

    /* STRB/LDRB (immediate) Thumb-2 (T3/T4). 
     * Also handles STRBT/LDRBT when P=0, W=1, bits[11:8]=0xe (unprivileged) */
    if ((insn & 0xff000000u) == 0xf8000000u) {
        mm_u8 op1 = (mm_u8)((insn >> 20) & 0x7u);
        mm_u8 l = (mm_u8)((insn >> 20) & 1u);
        mm_u8 p = (mm_u8)((insn >> 10) & 1u);
        mm_u8 u = (mm_u8)((insn >> 9) & 1u);
        mm_u8 w = (mm_u8)((insn >> 8) & 1u);
        mm_u8 imm8 = (mm_u8)(insn & 0xffu);
        mm_u8 rn = (mm_u8)((insn >> 16) & 0x0fu);
        mm_u8 rt = (mm_u8)((insn >> 12) & 0x0fu);
        mm_i32 offset = (mm_i32)imm8;
        if (op1 > 1u) {
            /* Not a byte load/store immediate form. */
        } else {
        /* Check for unprivileged variant (P=0, W=1, bits[11:8]=0xe) */
        if (p == 0u && w == 1u && ((insn >> 8) & 0xfu) == 0xeu) {
            d.kind = (l != 0u) ? MM_OP_LDRBT : MM_OP_STRBT;
            d.rn = rn;
            d.rd = rt;
            d.imm = (mm_u32)imm8;
            d.undefined = MM_FALSE;
            return d;
        }
        if (u == 0u) {
            offset = -(mm_i32)imm8;
        }
        if (rn == 15u) {
            /* PC base does not permit writeback; treat as simple offset form. */
            d.kind = (l != 0u) ? MM_OP_LDRB_IMM : MM_OP_STRB_IMM;
            d.rn = rn;
            d.rd = rt;
            d.imm = (mm_u32)offset;
            d.undefined = MM_FALSE;
            return d;
        }
        if (p == 0u && w == 1u) {
            /* Post-index writeback form: LDRB/STRB Rt, [Rn], #+/-imm8 */
            d.kind = (l != 0u) ? MM_OP_LDRB_POST_IMM : MM_OP_STRB_POST_IMM;
            d.rn = rn;
            d.rd = rt;
            d.imm = (mm_u32)offset;
            d.undefined = MM_FALSE;
            return d;
        }
        if (p == 1u && w == 1u) {
            /* Pre-index writeback form: LDRB/STRB Rt, [Rn, #+/-imm8]! */
            d.kind = (l != 0u) ? MM_OP_LDRB_PRE_IMM : MM_OP_STRB_PRE_IMM;
            d.rn = rn;
            d.rd = rt;
            d.imm = (mm_u32)offset;
            d.undefined = MM_FALSE;
            return d;
        }
        /* Treat remaining forms as simple offset without writeback. */
        d.kind = (l != 0u) ? MM_OP_LDRB_IMM : MM_OP_STRB_IMM;
        d.rn = rn;
        d.rd = rt;
        d.imm = (mm_u32)offset;
        d.undefined = MM_FALSE;
        return d;
        }
    }

    /* LDRH (immediate) Thumb-2, T3: 1111 1000 0011 Rn | Rt 1 P U W imm8 */
    if ((insn & 0xfff00000u) == 0xf8300000u && (insn & 0x00000800u) != 0u) {
        mm_u8 p = (mm_u8)((insn >> 10) & 1u);
        mm_u8 u = (mm_u8)((insn >> 9) & 1u);
        mm_u8 w = (mm_u8)((insn >> 8) & 1u);
        mm_u8 rn = (mm_u8)((insn >> 16) & 0x0fu);
        mm_u8 rt = (mm_u8)((insn >> 12) & 0x0fu);
        mm_u32 imm8 = insn & 0xffu;
        if (p == 0u && w == 1u && rt != 15u) {
            /* Post-index writeback form: LDRH Rt, [Rn], #+/-imm8 */
            d.kind = MM_OP_LDRH_POST_IMM;
            d.rn = rn;
            d.rd = rt;
            d.imm = u ? imm8 : (mm_u32)(0u - imm8);
            d.undefined = MM_FALSE;
            return d;
        }
        if (p == 1u && w == 1u && rt != 15u) {
            /* Pre-index writeback form: LDRH Rt, [Rn, #+/-imm8]! */
            d.kind = MM_OP_LDRH_PRE_IMM;
            d.rn = rn;
            d.rd = rt;
            d.imm = u ? imm8 : (mm_u32)(0u - imm8);
            d.undefined = MM_FALSE;
            return d;
        }
        /* Pre-indexed no-writeback form (P=1, W=0). */
        if (p == 1u && w == 0u && rt != 15u) {
            d.kind = MM_OP_LDRH_IMM;
            d.rn = rn;
            d.rd = rt;
            d.imm = u ? imm8 : (mm_u32)(0u - imm8); /* signed offset */
            d.undefined = MM_FALSE;
            return d;
        }
    }

    /* STRH (immediate) Thumb-2, T3: 1111 1000 0010 Rn | Rt 1 P U W imm8 */
    if ((insn & 0xfff00000u) == 0xf8200000u && (insn & 0x00000800u) != 0u) {
        mm_u8 p = (mm_u8)((insn >> 10) & 1u);
        mm_u8 u = (mm_u8)((insn >> 9) & 1u);
        mm_u8 w = (mm_u8)((insn >> 8) & 1u);
        mm_u8 rn = (mm_u8)((insn >> 16) & 0x0fu);
        mm_u8 rt = (mm_u8)((insn >> 12) & 0x0fu);
        mm_u32 imm8 = insn & 0xffu;
        if (p == 0u && w == 1u && rt != 13u && rt != 15u) {
            /* Post-index writeback form: STRH Rt, [Rn], #+/-imm8 */
            d.kind = MM_OP_STRH_POST_IMM;
            d.rn = rn;
            d.rd = rt;
            d.imm = u ? imm8 : (mm_u32)(0u - imm8);
            d.undefined = MM_FALSE;
            return d;
        }
        if (p == 1u && w == 1u && rt != 13u && rt != 15u) {
            /* Pre-index writeback form: STRH Rt, [Rn, #+/-imm8]! */
            d.kind = MM_OP_STRH_PRE_IMM;
            d.rn = rn;
            d.rd = rt;
            d.imm = u ? imm8 : (mm_u32)(0u - imm8);
            d.undefined = MM_FALSE;
            return d;
        }
        /* Pre-indexed no-writeback form (P=1, W=0). */
        if (p == 1u && w == 0u && rt != 13u && rt != 15u) {
            d.kind = MM_OP_STRH_IMM;
            d.rn = rn;
            d.rd = rt;
            d.imm = u ? imm8 : (mm_u32)(0u - imm8);
            d.undefined = MM_FALSE;
            return d;
        }
    }

    /* STRH (register) Thumb-2: 1111 1000 0010 Rn | Rt 0000 00ii Rm */
    if ((insn & 0xfff00fc0u) == 0xf8200000u) {
        mm_u8 imm2 = (mm_u8)((insn >> 4) & 0x3u);
        d.kind = MM_OP_STRH_REG;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.rm = (mm_u8)(insn & 0x0fu);
        d.imm = imm2; /* LSL #imm2 */
        d.undefined = MM_FALSE;
        return d;
    }
    /* LDRH (register) Thumb-2: 1111 1000 0011 Rn | Rt 0000 00ii Rm */
    if ((insn & 0xfff00fc0u) == 0xf8300000u) {
        mm_u8 imm2 = (mm_u8)((insn >> 4) & 0x3u);
        d.kind = MM_OP_LDRH_REG;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = (mm_u8)((insn >> 12) & 0x0fu);
        d.rm = (mm_u8)(insn & 0x0fu);
        d.imm = imm2; /* LSL #imm2 */
        d.undefined = MM_FALSE;
        return d;
    }

    /* VFP double load/store (VLDR/VSTR). */
    if ((insn & 0xff000f00u) == 0xed000b00u) {
        if ((insn & (1u << 21)) == 0u) {
        mm_bool load = ((insn >> 20) & 1u) != 0u;
        mm_bool u = ((insn >> 23) & 1u) != 0u;
        mm_bool p = ((insn >> 24) & 1u) != 0u;
        mm_bool w = ((insn >> 21) & 1u) != 0u;
        mm_u32 imm = (insn & 0xffu) << 2;
        d.kind = load ? MM_OP_VLDR : MM_OP_VSTR;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = vfp_dd(insn);
        d.imm = imm | (u ? 0x80000000u : 0u) | (w ? 0x40000000u : 0u) |
            (p ? 0x20000000u : 0u) | MM_VFP_LS_DOUBLE;
        d.undefined = MM_FALSE;
        return d;
        }
    }

    /* VFP single load/store (VLDR/VSTR). */
    if ((insn & 0xff000f00u) == 0xed000a00u) {
        if ((insn & (1u << 21)) == 0u) {
        mm_bool load = ((insn >> 20) & 1u) != 0u;
        mm_bool u = ((insn >> 23) & 1u) != 0u;
        mm_bool p = ((insn >> 24) & 1u) != 0u;
        mm_bool w = ((insn >> 21) & 1u) != 0u;
        mm_u32 imm = (insn & 0xffu) << 2;
        d.kind = load ? MM_OP_VLDR : MM_OP_VSTR;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rd = vfp_sd(insn);
        d.imm = imm | (u ? 0x80000000u : 0u) | (w ? 0x40000000u : 0u) | (p ? 0x20000000u : 0u);
        d.undefined = MM_FALSE;
        return d;
        }
    }

    /* VMOV (between two core regs and two s/d registers). */
    if ((insn & 0x0f000000u) == 0x0c000000u) {
        mm_u8 coproc = (mm_u8)((insn >> 8) & 0x0fu);
        mm_u8 op1 = (mm_u8)((insn >> 4) & 0x0fu);
        mm_u8 opcode = (mm_u8)((insn >> 20) & 0x1u); /* 0=core->s/d, 1=s/d->core */
        mm_u8 op23_21 = (mm_u8)((insn >> 21) & 0x7u);
        if ((coproc == 10u || coproc == 11u) && ((op1 & 0x1u) != 0u) && op23_21 == 0x2u) {
            mm_u8 s_low = vfp_sm(insn);
            mm_u8 s_high = (mm_u8)(s_low + 1u);
            mm_u8 rt = (mm_u8)((insn >> 12) & 0x0fu);
            mm_u8 rt2 = (mm_u8)((insn >> 16) & 0x0fu);
            if (opcode != 0u) {
                if (coproc == 11u) {
                    if ((s_low & 1u) != 0u) {
                        return d;
                    }
                    d.kind = MM_OP_VMOV_RDD;
                    d.rd = rt;
                    d.rn = rt2;
                    d.rm = (mm_u8)(s_low >> 1);
                } else {
                    d.kind = MM_OP_VMOV_RSS;
                    d.rd = rt;
                    d.rn = rt2;
                    d.rm = s_low;
                    d.ra = s_high;
                }
            } else {
                if (coproc == 11u) {
                    if ((s_low & 1u) != 0u) {
                        return d;
                    }
                    d.kind = MM_OP_VMOV_DRR;
                    d.rd = (mm_u8)(s_low >> 1);
                    d.rm = rt;
                    d.ra = rt2;
                } else {
                    d.kind = MM_OP_VMOV_SRR;
                    d.rd = s_low;
                    d.rn = s_high;
                    d.rm = rt;
                    d.ra = rt2;
                }
            }
            d.undefined = MM_FALSE;
            return d;
        }
    }

    /* VLSTM / VLLDM <Rn> (T1): 1110 1100 001L Rn 0000 1010 0000 0000. */
    if ((insn & 0xffe0ffffu) == 0xec200a00u) {
        d.kind = ((insn >> 20) & 1u) ? MM_OP_VLLDM : MM_OP_VLSTM;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.undefined = MM_FALSE;
        return d;
    }

    /* VFP load/store multiple (VLDM/VSTM). */
    if ((insn & 0xfe000f00u) == 0xec000a00u || (insn & 0xfe000f00u) == 0xec000b00u) {
        mm_bool load = ((insn >> 20) & 1u) != 0u;
        mm_bool u = ((insn >> 23) & 1u) != 0u;
        mm_bool p = ((insn >> 24) & 1u) != 0u;
        mm_bool w = ((insn >> 21) & 1u) != 0u;
        mm_u32 count = insn & 0xffu;
        d.kind = load ? MM_OP_VLDM : MM_OP_VSTM;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        if ((insn & 0x00000f00u) == 0x00000b00u) {
            d.rd = vfp_dd(insn);
            d.imm = count | (u ? 0x80000000u : 0u) | (w ? 0x40000000u : 0u) |
                (p ? 0x20000000u : 0u) | MM_VFP_LS_DOUBLE;
        } else {
            d.rd = vfp_sd(insn);
            d.imm = count | (u ? 0x80000000u : 0u) | (w ? 0x40000000u : 0u) | (p ? 0x20000000u : 0u);
        }
        d.undefined = MM_FALSE;
        return d;
    }

    /* VMOV (between core and s registers). */
    if ((insn & 0xffe00f7fu) == 0xee000a10u) {
        mm_bool to_core = ((insn >> 20) & 1u) != 0u;
        mm_u8 sreg = vfp_sn(insn);
        mm_u8 core = (mm_u8)((insn >> 12) & 0x0fu);
        if (to_core) {
            d.kind = MM_OP_VMOV_RS;
            d.rd = core;
            d.rn = sreg;
        } else {
            d.kind = MM_OP_VMOV_SR;
            d.rd = sreg;
            d.rn = core;
        }
        d.undefined = MM_FALSE;
        return d;
    }

    /* VMOV (immediate, single-precision). */
    if ((insn & 0xffb00ff0u) == 0xeeb00a00u) {
        mm_u8 imm8 = (mm_u8)((((insn >> 16) & 0x0fu) << 4) | (insn & 0x0fu));
        d.kind = MM_OP_VMOV_IMM;
        d.rd = vfp_sd(insn);
        d.imm = imm8;
        d.undefined = MM_FALSE;
        return d;
    }
    /* VMOV (register) T1: 1110 1110 1D11 0000 Vd 1010 01M0 Vm */
    if ((insn & 0xffbf0fd0u) == 0xeeb00a40u) {
        d.kind = MM_OP_VMOV_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }

    /* VFP data processing (single-precision). */
    if ((insn & 0xffb00f50u) == 0xee300a00u) {
        d.kind = MM_OP_VADD;
        d.rd = vfp_sd(insn);
        d.rn = vfp_sn(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffb00f50u) == 0xee300a40u) {
        d.kind = MM_OP_VSUB;
        d.rd = vfp_sd(insn);
        d.rn = vfp_sn(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffb00f50u) == 0xee200a00u) {
        d.kind = MM_OP_VMUL;
        d.rd = vfp_sd(insn);
        d.rn = vfp_sn(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffb00f50u) == 0xee800a00u) {
        d.kind = MM_OP_VDIV;
        d.rd = vfp_sd(insn);
        d.rn = vfp_sn(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffb00f50u) == 0xee000a00u) {
        d.kind = MM_OP_VMLA;
        d.rd = vfp_sd(insn);
        d.rn = vfp_sn(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffb00f50u) == 0xee000a40u) {
        d.kind = MM_OP_VMLS;
        d.rd = vfp_sd(insn);
        d.rn = vfp_sn(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xeeb10a40u) {
        d.kind = MM_OP_VNEG;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xeeb00ac0u) {
        d.kind = MM_OP_VABS;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xeeb40a40u) {
        d.kind = MM_OP_VCMP;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xeeb50a40u) {
        d.kind = MM_OP_VCMP;
        d.rd = vfp_sd(insn);
        d.rm = 0u;
        d.imm = 1u; /* immediate zero */
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xeeb40ac0u) {
        d.kind = MM_OP_VCMPE;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xeeb50ac0u) {
        d.kind = MM_OP_VCMPE;
        d.rd = vfp_sd(insn);
        d.rm = 0u;
        d.imm = 1u; /* immediate zero */
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xeebd0ac0u) {
        d.kind = MM_OP_VCVT_S32_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xeebd0a40u) {
        d.kind = MM_OP_VCVTR_S32_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xeebc0ac0u) {
        d.kind = MM_OP_VCVT_U32_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xeebc0a40u) {
        d.kind = MM_OP_VCVTR_U32_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xeeb80ac0u) {
        d.kind = MM_OP_VCVT_F32_S32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xeeb80a40u) {
        d.kind = MM_OP_VCVT_F32_U32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xeeb10ac0u) {
        d.kind = MM_OP_VSQRT;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }

    /* ARMv8-M VCVT{A,N,P,M}.{S32,U32}.F32 — directed rounding (FPv5).
     * Encoding: 1111 1110 1D11 11 RM | Vd 101 sz op 1 M 0 Vm
     *   sz=0 (F32), op=1 (signed) or op=0 (unsigned), bit 6 fixed=1,
     *   bit 4 fixed=0, bit 5=M (Vm low bit), RM in hw1[17:16]:
     *     00 = A (nearest, ties away), 01 = N (nearest, ties even),
     *     10 = P (+inf), 11 = M (-inf).
     * Mask: hw1=0xffbf (D=hw1[6] free), hw2=0x0fd0 (Vd/M/Vm free).
     * Verified against arm-none-eabi-as for cortex-m33 / fpv5-sp-d16.
     */
    if ((insn & 0xffbf0fd0u) == 0xfebc0ac0u) {
        d.kind = MM_OP_VCVTA_S32_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xfebc0a40u) {
        d.kind = MM_OP_VCVTA_U32_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xfebd0ac0u) {
        d.kind = MM_OP_VCVTN_S32_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xfebd0a40u) {
        d.kind = MM_OP_VCVTN_U32_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xfebe0ac0u) {
        d.kind = MM_OP_VCVTP_S32_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xfebe0a40u) {
        d.kind = MM_OP_VCVTP_U32_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xfebf0ac0u) {
        d.kind = MM_OP_VCVTM_S32_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xfebf0a40u) {
        d.kind = MM_OP_VCVTM_U32_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }

    /* ARMv8-M VRINT{A,N,P,M}.F32 — round-to-integral with directed rounding.
     * Encoding: 1111 1110 1D11 10 RM | Vd 101 sz 0 1 M 0 Vm
     * Bit 7 = 0 (op2=0 for VRINT), bit 6 = 1 (fixed), bit 5 = M.
     */
    if ((insn & 0xffbf0fd0u) == 0xfeb80a40u) {
        d.kind = MM_OP_VRINTA_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xfeb90a40u) {
        d.kind = MM_OP_VRINTN_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xfeba0a40u) {
        d.kind = MM_OP_VRINTP_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xfebb0a40u) {
        d.kind = MM_OP_VRINTM_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }

    /* VMAXNM/VMINNM.F32 (T2, FPv5, unconditional).
     * Encoding: 1111 1110 1D00 Vn | Vd 101 sz N op M 0 Vm
     *   bit 6 = op (0=VMAXNM, 1=VMINNM), bit 4 = 0 fixed.
     * Mask: hw1 0xffb0 (D, Vn free), hw2 0x0f50 (Vd, N, M, Vm free).
     */
    if ((insn & 0xffb00f50u) == 0xfe800a00u) {
        d.kind = MM_OP_VMAXNM_F32;
        d.rd = vfp_sd(insn);
        d.rn = vfp_sn(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffb00f50u) == 0xfe800a40u) {
        d.kind = MM_OP_VMINNM_F32;
        d.rd = vfp_sd(insn);
        d.rn = vfp_sn(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }

    /* VSEL.F32 (T1, FPv5, unconditional with embedded cond).
     * Encoding: 1111 1110 0D cc Vn | Vd 101 sz N 0 M 0 Vm
     *   cc[1:0] in hw1[21:20] selects condition:
     *     00=EQ, 01=VS, 10=GE, 11=GT
     * Mask: hw1 0xff80 (D, cc, Vn free), hw2 0x0f50 (Vd, N, M, Vm free).
     * d.imm carries the 2-bit cond selector.
     */
    if ((insn & 0xff800f50u) == 0xfe000a00u) {
        d.kind = MM_OP_VSEL_F32;
        d.rd = vfp_sd(insn);
        d.rn = vfp_sn(insn);
        d.rm = vfp_sm(insn);
        d.imm = (insn >> 20) & 0x3u;
        d.undefined = MM_FALSE;
        return d;
    }

    /* VFMA/VFMS/VFNMA/VFNMS.F32 (T2, FPv5, unconditional).
     * VFMA:  1110 1110 1D10 Vn | Vd 101 sz N 0 M 0 Vm  (op=0)
     * VFMS:  1110 1110 1D10 Vn | Vd 101 sz N 1 M 0 Vm  (op=1, bit 6=1)
     * VFNMA: 1110 1110 1D01 Vn | Vd 101 sz N 1 M 0 Vm  (op=1)
     * VFNMS: 1110 1110 1D01 Vn | Vd 101 sz N 0 M 0 Vm  (op=0)
     * Distinguished by hw1[20] and hw2[6].
     */
    if ((insn & 0xffb00f50u) == 0xeea00a00u) {
        d.kind = MM_OP_VFMA_F32;
        d.rd = vfp_sd(insn);
        d.rn = vfp_sn(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffb00f50u) == 0xeea00a40u) {
        d.kind = MM_OP_VFMS_F32;
        d.rd = vfp_sd(insn);
        d.rn = vfp_sn(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffb00f50u) == 0xee900a40u) {
        d.kind = MM_OP_VFNMA_F32;
        d.rd = vfp_sd(insn);
        d.rn = vfp_sn(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffb00f50u) == 0xee900a00u) {
        d.kind = MM_OP_VFNMS_F32;
        d.rd = vfp_sd(insn);
        d.rn = vfp_sn(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }

    /* VNMUL/VNMLA/VNMLS.F32 (legacy VFPv2, conditional in source but encoded
     * as unconditional bits in Thumb).
     * VNMUL: 1110 1110 0D10 Vn | Vd 101 sz N 1 M 0 Vm
     * VNMLA: 1110 1110 0D01 Vn | Vd 101 sz N 1 M 0 Vm
     * VNMLS: 1110 1110 0D01 Vn | Vd 101 sz N 0 M 0 Vm
     */
    if ((insn & 0xffb00f50u) == 0xee200a40u) {
        d.kind = MM_OP_VNMUL_F32;
        d.rd = vfp_sd(insn);
        d.rn = vfp_sn(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffb00f50u) == 0xee100a40u) {
        d.kind = MM_OP_VNMLA_F32;
        d.rd = vfp_sd(insn);
        d.rn = vfp_sn(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffb00f50u) == 0xee100a00u) {
        d.kind = MM_OP_VNMLS_F32;
        d.rd = vfp_sd(insn);
        d.rn = vfp_sn(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }

    /* VCVTB / VCVTT half<->single conversion (FPv5).
     * Encoding: 1110 1110 1D11 001 op | Vd 101 sz T 1 M 0 Vm
     *   op = 0 -> F32 <- F16 (top/bottom half decided by T);
     *   op = 1 -> F16 <- F32 (write top/bottom half).
     *   sz = 0 (F32 is single-precision).
     */
    if ((insn & 0xffbf0fd0u) == 0xeeb20a40u) {
        d.kind = MM_OP_VCVTB_F32_F16;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xeeb20ac0u) {
        d.kind = MM_OP_VCVTT_F32_F16;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xeeb30a40u) {
        d.kind = MM_OP_VCVTB_F16_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xeeb30ac0u) {
        d.kind = MM_OP_VCVTT_F16_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }

    /* Fixed-point VCVT (F32 <-> S16/U16/S32/U32 with #fbits).
     * Encoding: 1110 1110 1D11 1 op2 U | Vd 101 sz sx 1 i 0 imm4
     *   op2 = 1 means "to-fixed" (bit 18 = 1), op2 = 0 means "to-float"
     *   U   = unsigned (bit 16)
     *   sx  = 32-bit fixed (bit 7), 0 -> 16-bit
     *   imm5 = (imm4 << 1) | i, frac_bits = size - imm5
     * Single mask + pattern check: hw1 fixed parts and hw2 fixed parts.
     */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xffbau) == 0xeebau && (hw2 & 0x0f50u) == 0x0a40u) {
            mm_bool to_float = ((hw1 >> 2) & 1u) == 0u;
            mm_bool unsigned_op = (hw1 & 1u) != 0u;
            mm_bool sx32 = ((hw2 >> 7) & 1u) != 0u;
            mm_u32 imm4 = hw2 & 0x0fu;
            mm_u32 i = (hw2 >> 5) & 1u;
            mm_u32 imm5 = (imm4 << 1) | i;
            mm_u32 size = sx32 ? 32u : 16u;
            mm_u32 frac_bits = size - imm5;
            d.kind = MM_OP_VCVT_FIXED;
            d.rd = vfp_sd(insn);
            d.rm = vfp_sd(insn);  /* same register; ARM uses Vd as both src and dst here */
            d.imm = (to_float ? 1u : 0u)
                  | (unsigned_op ? 2u : 0u)
                  | (sx32 ? 4u : 0u)
                  | ((frac_bits & 0x1fu) << 3);
            d.undefined = MM_FALSE;
            return d;
        }
    }

    /* Legacy VRINT{Z,R,X}.F32 — FPSCR or implicit rounding.
     * VRINTZ:  hw1=0xEEB6, hw2 bit7=1 (op=1, truncate)
     * VRINTR:  hw1=0xEEB6, hw2 bit7=0 (op=0, FPSCR rmode)
     * VRINTX:  hw1=0xEEB7, hw2 bit7=0 (op=0, FPSCR rmode + inexact)
     */
    if ((insn & 0xffbf0fd0u) == 0xeeb60ac0u) {
        d.kind = MM_OP_VRINTZ_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xeeb60a40u) {
        d.kind = MM_OP_VRINTR_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }
    if ((insn & 0xffbf0fd0u) == 0xeeb70a40u) {
        d.kind = MM_OP_VRINTX_F32;
        d.rd = vfp_sd(insn);
        d.rm = vfp_sm(insn);
        d.undefined = MM_FALSE;
        return d;
    }

    /* VMRS/VMSR (FPSCR transfer). */
    {
        mm_u8 op1 = (mm_u8)((insn >> 21) & 0x7u);
        mm_u8 opcode = (mm_u8)((insn >> 20) & 0x1u); /* 0=VMSR, 1=VMRS */
        mm_u8 crn = (mm_u8)((insn >> 16) & 0x0fu);
        mm_u8 coproc = (mm_u8)((insn >> 8) & 0x0fu);
        if ((insn & 0x0f000010u) == 0x0e000010u &&
            (coproc == 10u || coproc == 11u) && op1 == 7u && crn == 1u) {
            d.kind = (opcode != 0u) ? MM_OP_VMRS : MM_OP_VMSR;
            d.rd = (mm_u8)((insn >> 12) & 0x0fu);
            d.undefined = MM_FALSE;
            return d;
        }
    }

    /* STC / STC2 (Store Coprocessor), Thumb-2
     * STC (T1):  1110 110P UDWL Rn | CRd coproc imm8
     * STC2 (T2): 1111 110P UDWL Rn | CRd coproc imm8
     * 
     * Constraints:
     * - L bit (bit 20/hw1[4]) must be 0 for STC (1 for LDC)
     * - P=0, U=0, D=1, W=0 → MCRR/MCRR2 (not STC)
     * - P=0, U=0, D=0, W=0 → UNDEFINED
     * - coproc = 101x: STC→FP redirect, STC2→UNDEFINED
     * - Rn = 15 (PC) → UNPREDICTABLE
     */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        mm_bool is_stc2 = (hw1 & 0xfe00u) == 0xfc00u;
        mm_bool is_stc = (hw1 & 0xfe00u) == 0xec00u;
        mm_u8 lbit = (mm_u8)((hw1 >> 4) & 0x1u);  /* L bit: 0=STC, 1=LDC */
        
        if ((is_stc || is_stc2) && (lbit == 0)) {  /* Only match STC when L=0 */
            mm_u8 p = (mm_u8)((hw1 >> 8) & 0x1u);
            mm_u8 u = (mm_u8)((hw1 >> 7) & 0x1u);
            mm_u8 dbit = (mm_u8)((hw1 >> 6) & 0x1u);  /* D bit (L suffix) */
            mm_u8 w = (mm_u8)((hw1 >> 5) & 0x1u);
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 crd = (mm_u8)((hw2 >> 12) & 0x0fu);
            mm_u8 coproc = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 imm8 = (mm_u8)(hw2 & 0xffu);
            
            /* Check for special case encodings */
            if (p == 0 && u == 0 && dbit == 1 && w == 0) {
                /* This is MCRR/MCRR2, not STC - let it fall through */
            } else if (p == 0 && u == 0 && dbit == 0 && w == 0) {
                /* UNDEFINED encoding */
                d.undefined = MM_TRUE;
                return d;
            } else if ((coproc & 0xeu) == 0xau) {  /* 101x = p10/p11 */
                /* STC/STC2 with p10/p11: redirects to FP or UNDEFINED */
                d.undefined = MM_TRUE;
                return d;
            } else if (rn == 15) {
                /* Rn = PC is UNPREDICTABLE */
                d.undefined = MM_TRUE;
                return d;
            } else {
                /* Valid STC/STC2 encoding */
                d.kind = is_stc2 ? MM_OP_STC2 : MM_OP_STC;
                d.rn = rn;
                d.rd = crd;
                d.ra = coproc;
                d.imm = (mm_u32)imm8 | ((mm_u32)p << 8) | ((mm_u32)u << 9) | 
                        ((mm_u32)dbit << 10) | ((mm_u32)w << 11);
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* LDC / LDC2 (Load Coprocessor), Thumb-2  
     * LDC (T1):  1110 110P UDWL Rn | CRd coproc imm8
     * LDC2 (T2): 1111 110P UDWL Rn | CRd coproc imm8
     * (L bit is set, distinguishing from STC)
     * Same constraints as STC
     */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        mm_bool is_ldc2 = (hw1 & 0xfe00u) == 0xfc00u;
        mm_bool is_ldc = (hw1 & 0xfe00u) == 0xec00u;
        
        if ((is_ldc || is_ldc2) && ((hw1 >> 4) & 0x1u)) {  /* L bit must be 1 for LDC */
            mm_u8 p = (mm_u8)((hw1 >> 8) & 0x1u);
            mm_u8 u = (mm_u8)((hw1 >> 7) & 0x1u);
            mm_u8 dbit = (mm_u8)((hw1 >> 6) & 0x1u);
            mm_u8 w = (mm_u8)((hw1 >> 5) & 0x1u);
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 crd = (mm_u8)((hw2 >> 12) & 0x0fu);
            mm_u8 coproc = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 imm8 = (mm_u8)(hw2 & 0xffu);
            
            /* Check for special case encodings */
            if (p == 0 && u == 0 && dbit == 1 && w == 0) {
                /* This is MRRC/MRRC2, not LDC - let it fall through */
            } else if (p == 0 && u == 0 && dbit == 0 && w == 0) {
                /* UNDEFINED */
                d.undefined = MM_TRUE;
                return d;
            } else if ((coproc & 0xeu) == 0xau) {  /* 101x = p10/p11 */
                /* LDC/LDC2 with p10/p11: redirects to FP or UNDEFINED */
                d.undefined = MM_TRUE;
                return d;
            } else if (rn == 15) {
                /* Rn = PC: allowed for literal loads, but mark as such */
                /* For now, treat as UNPREDICTABLE */
                d.undefined = MM_TRUE;
                return d;
            } else {
                /* Valid LDC/LDC2 */
                d.kind = is_ldc2 ? MM_OP_LDC2 : MM_OP_LDC;
                d.rn = rn;
                d.rd = crd;
                d.ra = coproc;
                d.imm = (mm_u32)imm8 | ((mm_u32)p << 8) | ((mm_u32)u << 9) | 
                        ((mm_u32)dbit << 10) | ((mm_u32)w << 11);
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* Coprocessor register transfer (MCR/MRC), Thumb-2 */
    if ((insn & 0x0f000010u) == 0x0e000010u) {
        mm_u8 p = (mm_u8)((insn >> 28) & 0x1u);
        mm_u8 op1 = (mm_u8)((insn >> 21) & 0x7u);
        mm_u8 opcode = (mm_u8)((insn >> 20) & 0x1u); /* 0=MCR, 1=MRC */
        mm_u8 crn = (mm_u8)((insn >> 16) & 0x0fu);
        mm_u8 rd = (mm_u8)((insn >> 12) & 0x0fu);
        mm_u8 coproc = (mm_u8)((insn >> 8) & 0x0fu);
        mm_u8 op2 = (mm_u8)((insn >> 5) & 0x7u);
        mm_u8 crm = (mm_u8)(insn & 0x0fu);
        d.kind = MM_OP_MCR_MRC;
        d.rd = rd;
        d.rn = crn;
        d.rm = crm;
        d.ra = coproc;
        d.imm = (mm_u32)op1 | ((mm_u32)op2 << 3) | ((mm_u32)opcode << 6) | ((mm_u32)p << 7);
        d.undefined = MM_FALSE;
        return d;
    }

    /* Coprocessor data processing (CDP), Thumb-2 */
    if ((insn & 0x0f000010u) == 0x0e000000u) {
        mm_u8 p = (mm_u8)((insn >> 28) & 0x1u);
        mm_u8 op1 = (mm_u8)((insn >> 20) & 0x0fu);
        mm_u8 crn = (mm_u8)((insn >> 16) & 0x0fu);
        mm_u8 crd = (mm_u8)((insn >> 12) & 0x0fu);
        mm_u8 coproc = (mm_u8)((insn >> 8) & 0x0fu);
        mm_u8 op2 = (mm_u8)((insn >> 5) & 0x7u);
        mm_u8 crm = (mm_u8)(insn & 0x0fu);
        d.kind = MM_OP_CDP;
        d.rd = crd;
        d.rn = crn;
        d.rm = crm;
        d.ra = coproc;
        d.imm = (mm_u32)op1 | ((mm_u32)op2 << 4) | ((mm_u32)p << 7);
        d.undefined = MM_FALSE;
        return d;
    }

    /* Coprocessor 64-bit transfer (MCRR/MRRC), Thumb-2 */
    if ((insn & 0x0f000000u) == 0x0c000000u) {
        mm_u8 p = (mm_u8)((insn >> 28) & 0x1u);
        mm_u8 opcode = (mm_u8)((insn >> 20) & 0x1u); /* 0=MCRR, 1=MRRC */
        mm_u8 rt2 = (mm_u8)((insn >> 16) & 0x0fu);
        mm_u8 rt = (mm_u8)((insn >> 12) & 0x0fu);
        mm_u8 coproc = (mm_u8)((insn >> 8) & 0x0fu);
        mm_u8 op1 = (mm_u8)((insn >> 4) & 0x0fu);
        mm_u8 crm = (mm_u8)(insn & 0x0fu);
        d.kind = MM_OP_MCRR_MRRC;
        d.rd = rt;
        d.rn = rt2;
        d.rm = crm;
        d.ra = coproc;
        d.imm = (mm_u32)op1 | ((mm_u32)opcode << 8) | ((mm_u32)p << 9);
        d.undefined = MM_FALSE;
        return d;
    }

    /* UBFX (Thumb-2): 1111 0011 1100 Rn | 0 imm3 Rd 0 imm2 widthm1 */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xf3c0u || (hw1 & 0xfff0u) == 0xf7c0u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 imm3 = (mm_u8)((hw2 >> 12) & 0x7u);
            mm_u8 imm2 = (mm_u8)((hw2 >> 6) & 0x3u);
            mm_u8 lsb = (mm_u8)((imm3 << 2) | imm2);
            mm_u8 widthm1 = (mm_u8)(hw2 & 0x1fu);
            if (rn != 15u && rd != 15u) {
                d.kind = MM_OP_UBFX;
                d.rd = rd;
                d.rn = rn;
                d.imm = (mm_u32)lsb | ((mm_u32)widthm1 << 8);
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* SBFX (Thumb-2): 1111 0011 0100 Rn | 0 imm3 Rd 0 imm2 widthm1 */
    {
        mm_u16 hw1 = (mm_u16)(insn >> 16);
        mm_u16 hw2 = (mm_u16)(insn & 0xffffu);
        if ((hw1 & 0xfff0u) == 0xf340u) {
            mm_u8 rn = (mm_u8)(hw1 & 0x0fu);
            mm_u8 rd = (mm_u8)((hw2 >> 8) & 0x0fu);
            mm_u8 imm3 = (mm_u8)((hw2 >> 12) & 0x7u);
            mm_u8 imm2 = (mm_u8)((hw2 >> 6) & 0x3u);
            mm_u8 lsb = (mm_u8)((imm3 << 2) | imm2);
            mm_u8 widthm1 = (mm_u8)(hw2 & 0x1fu);
            if (rn != 15u && rd != 15u) {
                d.kind = MM_OP_SBFX;
                d.rd = rd;
                d.rn = rn;
                d.imm = (mm_u32)lsb | ((mm_u32)widthm1 << 8);
                d.undefined = MM_FALSE;
                return d;
            }
        }
    }

    /* BFI/BFC (Thumb-2), encoding family with bit5 in low halfword required 0.
     * BFC is selected by Rn==0xF (source is "all zeros").
     */
    if ((insn & 0xfff08020u) == 0xf3600000u) {
        mm_u8 rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.kind = (rn == 0x0fu) ? MM_OP_BFC : MM_OP_BFI;
        d.rd = (mm_u8)((insn >> 8) & 0x0fu);
        d.rn = rn;
        d.undefined = MM_FALSE;
        return d;
    }

    /* TBB/TBH (Thumb-2 table branch), T1:
     * hw1: 1110 1000 1101 Rn
     * hw2: 1111 0000 0000 h 000 Rm
     * Use h to select TBH (h=1) vs TBB (h=0).
     */
    if ((insn & 0xfff0ffe0u) == 0xe8d0f000u) {
        mm_u8 h = (mm_u8)((insn >> 4) & 1u);
        d.kind = h ? MM_OP_TBH : MM_OP_TBB;
        d.rn = (mm_u8)((insn >> 16) & 0x0fu);
        d.rm = (mm_u8)(insn & 0x0fu);
        d.undefined = MM_FALSE;
        return d;
    }

    return d;
}

struct mm_decoded mm_decode_t32(const struct mm_fetch_result *fetch)
{
    if (fetch == 0 || fetch->fault || fetch->len == 0u) {
        return mm_decoded_default(fetch);
    }

    if (fetch->len == 2u) {
        return decode_16((mm_u16)(fetch->insn & 0xffffu));
    }

    return decode_32(fetch->insn);
}
