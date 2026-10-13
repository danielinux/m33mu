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

#include "m33mu/capstone.h"
#include "m33mu/decode.h"
#include "m33mu/fetch.h"
#include <capstone/capstone.h>
#include <stdio.h>

#if defined(CS_API_MAJOR) && (CS_API_MAJOR >= 5)
#define M33MU_CAPSTONE_HAS_POST_INDEX 1
#else
#define M33MU_CAPSTONE_HAS_POST_INDEX 0
#endif

struct capstone_ctx {
    csh handle;
    mm_bool ready;
    mm_bool enabled;
};

static struct capstone_ctx g_capstone;

mm_bool capstone_available(void)
{
    return MM_TRUE;
}

mm_bool capstone_init(void)
{
    if (g_capstone.ready) {
        return MM_TRUE;
    }
    if (cs_open(CS_ARCH_ARM, CS_MODE_THUMB, &g_capstone.handle) != CS_ERR_OK) {
        return MM_FALSE;
    }
    if (cs_option(g_capstone.handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK) {
        cs_close(&g_capstone.handle);
        return MM_FALSE;
    }
    g_capstone.ready = MM_TRUE;
    g_capstone.enabled = MM_TRUE;
    return MM_TRUE;
}

void capstone_shutdown(void)
{
    if (g_capstone.ready) {
        cs_close(&g_capstone.handle);
        g_capstone.ready = MM_FALSE;
        g_capstone.enabled = MM_FALSE;
    }
}

mm_bool capstone_set_enabled(mm_bool enabled)
{
    if (!g_capstone.ready) {
        return MM_FALSE;
    }
    g_capstone.enabled = enabled;
    return MM_TRUE;
}

mm_bool capstone_is_enabled(void)
{
    return (g_capstone.ready && g_capstone.enabled);
}

static int arm_reg_from_mm(mm_u8 reg)
{
    if (reg <= 12u) {
        return (int)(ARM_REG_R0 + reg);
    }
    if (reg == 13u) {
        return ARM_REG_SP;
    }
    if (reg == 14u) {
        return ARM_REG_LR;
    }
    if (reg == 15u) {
        return ARM_REG_PC;
    }
    return ARM_REG_INVALID;
}

static int arm_vfp_reg_from_mm(mm_u8 reg)
{
    if (reg < 32u) {
        return (int)(ARM_REG_S0 + reg);
    }
    return ARM_REG_INVALID;
}

static int arm_vfp_d_reg_from_mm(mm_u8 reg)
{
    if (reg < 16u) {
        return (int)(ARM_REG_D0 + reg);
    }
    return ARM_REG_INVALID;
}

static mm_u32 thumb_expand_imm12(mm_u32 imm12)
{
    mm_u32 imm8 = imm12 & 0xffu;
    mm_u32 top = (imm12 >> 10) & 0x3u;
    mm_u32 pat = (imm12 >> 8) & 0x3u;
    mm_u32 imm32;

    if (top == 0u) {
        switch (pat) {
        case 0u: imm32 = imm8; break;
        case 1u: imm32 = (imm8 << 16) | imm8; break;
        case 2u: imm32 = (imm8 << 24) | (imm8 << 8); break;
        default: imm32 = (imm8 << 24) | (imm8 << 16) | (imm8 << 8) | imm8; break;
        }
    } else {
        mm_u32 unrot = (1u << 7) | (imm12 & 0x7fu);
        mm_u32 rot = (imm12 >> 7) & 0x1fu;
        if (rot == 0u) {
            imm32 = unrot;
        } else {
            imm32 = (unrot >> rot) | (unrot << (32u - rot));
        }
    }
    return imm32;
}

static long signed_imm32(mm_u32 value)
{
    return (long)((mm_i32)value);
}

void capstone_log(const struct mm_fetch_result *fetch)
{
    uint8_t code_buf[4];
    const uint8_t *code;
    size_t size;
    uint64_t address;
    cs_insn *insn = 0;
    size_t count;

    if (!g_capstone.ready || !g_capstone.enabled || fetch == 0 || fetch->len == 0u) {
        return;
    }

    if (fetch->len == 2u) {
        mm_u16 hw1 = (mm_u16)(fetch->insn & 0xffffu);
        code_buf[0] = (uint8_t)(hw1 & 0xffu);
        code_buf[1] = (uint8_t)((hw1 >> 8) & 0xffu);
    } else {
        mm_u16 hw1 = (mm_u16)((fetch->insn >> 16) & 0xffffu);
        mm_u16 hw2 = (mm_u16)(fetch->insn & 0xffffu);
        code_buf[0] = (uint8_t)(hw1 & 0xffu);
        code_buf[1] = (uint8_t)((hw1 >> 8) & 0xffu);
        code_buf[2] = (uint8_t)(hw2 & 0xffu);
        code_buf[3] = (uint8_t)((hw2 >> 8) & 0xffu);
    }
    code = code_buf;
    size = (size_t)fetch->len;
    address = (uint64_t)fetch->pc_fetch;

    count = cs_disasm(g_capstone.handle, code, size, address, 1, &insn);
    if (count == 0 || insn == 0) {
        printf("[CAPSTONE] PC=0x%08lx len=%u raw=0x%08lx decode failed\n",
               (unsigned long)(fetch->pc_fetch | 1u),
               (unsigned)fetch->len,
               (unsigned long)fetch->insn);
        return;
    }
    printf("[CAPSTONE] PC=0x%08lx %s %s\n",
           (unsigned long)(fetch->pc_fetch | 1u),
           insn[0].mnemonic,
           insn[0].op_str);
    cs_free(insn, count);
}

int capstone_decode_one(const struct mm_fetch_result *fetch, int *id_out,
                        char *mnemonic_out, size_t mnemonic_cap,
                        char *op_str_out, size_t op_str_cap)
{
    uint8_t code_buf[4];
    const uint8_t *code;
    size_t size;
    uint64_t address;
    cs_insn *insn = 0;
    size_t count;

    if (!g_capstone.ready || fetch == 0 || fetch->len == 0u) {
        return 0;
    }

    if (fetch->len == 2u) {
        mm_u16 hw1 = (mm_u16)(fetch->insn & 0xffffu);
        code_buf[0] = (uint8_t)(hw1 & 0xffu);
        code_buf[1] = (uint8_t)((hw1 >> 8) & 0xffu);
    } else {
        mm_u16 hw1 = (mm_u16)((fetch->insn >> 16) & 0xffffu);
        mm_u16 hw2 = (mm_u16)(fetch->insn & 0xffffu);
        code_buf[0] = (uint8_t)(hw1 & 0xffu);
        code_buf[1] = (uint8_t)((hw1 >> 8) & 0xffu);
        code_buf[2] = (uint8_t)(hw2 & 0xffu);
        code_buf[3] = (uint8_t)((hw2 >> 8) & 0xffu);
    }
    code = code_buf;
    size = (size_t)fetch->len;
    address = (uint64_t)fetch->pc_fetch;

    count = cs_disasm(g_capstone.handle, code, size, address, 1, &insn);
    if (count == 0 || insn == 0) {
        return 0;
    }
    if (id_out != 0) {
        *id_out = insn[0].id;
    }
    if (mnemonic_out != 0 && mnemonic_cap > 0u) {
        (void)snprintf(mnemonic_out, mnemonic_cap, "%s", insn[0].mnemonic);
    }
    if (op_str_out != 0 && op_str_cap > 0u) {
        (void)snprintf(op_str_out, op_str_cap, "%s", insn[0].op_str);
    }
    cs_free(insn, count);
    return 1;
}

static void log_reg_mismatch(int idx, int cap_reg, mm_u8 mm_reg)
{
    const char *cap_name = cs_reg_name(g_capstone.handle, cap_reg);
    const char *mm_name = cs_reg_name(g_capstone.handle, arm_reg_from_mm(mm_reg));
    printf("[CAPSTONE] operand %d reg mismatch expected=%s actual=%s\n",
           idx, mm_name ? mm_name : "?", cap_name ? cap_name : "?");
}

static void log_vfp_reg_mismatch(int idx, int cap_reg, mm_u8 mm_reg)
{
    const char *cap_name = cs_reg_name(g_capstone.handle, cap_reg);
    const char *mm_name = cs_reg_name(g_capstone.handle, arm_vfp_reg_from_mm(mm_reg));
    printf("[CAPSTONE] operand %d vfp reg mismatch expected=%s actual=%s\n",
           idx, mm_name ? mm_name : "?", cap_name ? cap_name : "?");
}

static void log_vfp_d_reg_mismatch(int idx, int cap_reg, mm_u8 mm_reg)
{
    const char *cap_name = cs_reg_name(g_capstone.handle, cap_reg);
    const char *mm_name = cs_reg_name(g_capstone.handle, arm_vfp_d_reg_from_mm(mm_reg));
    printf("[CAPSTONE] operand %d vfp reg mismatch expected=%s actual=%s\n",
           idx, mm_name ? mm_name : "?", cap_name ? cap_name : "?");
}

static void log_imm_mismatch(int idx, long expected, long actual)
{
    printf("[CAPSTONE] operand %d imm mismatch expected=%ld actual=%ld\n", idx, expected, actual);
}

static void log_mem_mismatch(const char *label)
{
    printf("[CAPSTONE] operand mem mismatch: %s\n", label);
}

static mm_bool check_reg_operand(const cs_arm_op *op, int idx, mm_u8 mm_reg)
{
    int expected = arm_reg_from_mm(mm_reg);
    if (op->type != ARM_OP_REG) {
        log_mem_mismatch("type is not REG");
        return MM_FALSE;
    }
    if (op->reg != expected) {
        log_reg_mismatch(idx, op->reg, mm_reg);
        return MM_FALSE;
    }
    return MM_TRUE;
}

static mm_bool check_vfp_reg_operand(const cs_arm_op *op, int idx, mm_u8 mm_reg)
{
    int expected = arm_vfp_reg_from_mm(mm_reg);
    if (op->type != ARM_OP_REG) {
        log_mem_mismatch("type is not REG");
        return MM_FALSE;
    }
    if (op->reg != expected) {
        log_vfp_reg_mismatch(idx, op->reg, mm_reg);
        return MM_FALSE;
    }
    return MM_TRUE;
}

static mm_bool check_vfp_d_reg_operand(const cs_arm_op *op, int idx, mm_u8 mm_reg)
{
    int expected = arm_vfp_d_reg_from_mm(mm_reg);
    if (op->type != ARM_OP_REG) {
        log_mem_mismatch("type is not REG");
        return MM_FALSE;
    }
    if (op->reg != expected) {
        log_vfp_d_reg_mismatch(idx, op->reg, mm_reg);
        return MM_FALSE;
    }
    return MM_TRUE;
}

static mm_bool reg_operand_matches(const cs_arm_op *op, mm_u8 mm_reg)
{
    int expected = arm_reg_from_mm(mm_reg);
    if (op->type != ARM_OP_REG) {
        return MM_FALSE;
    }
    return op->reg == expected;
}

static mm_bool check_imm_operand(const cs_arm_op *op, int idx, long expected)
{
    if (op->type != ARM_OP_IMM) {
        log_mem_mismatch("type is not IMM");
        return MM_FALSE;
    }
    if ((long)op->imm == expected) {
        return MM_TRUE;
    }
    if ((mm_u32)op->imm == (mm_u32)expected) {
        return MM_TRUE;
    }
    log_imm_mismatch(idx, expected, (long)op->imm);
    return MM_FALSE;
}

static mm_bool match_reg_reg_commutative(const cs_arm *arm, mm_u8 rn, mm_u8 rm)
{
    if (arm->op_count < 3u) {
        return MM_FALSE;
    }
    if (arm->operands[1].type != ARM_OP_REG || arm->operands[2].type != ARM_OP_REG) {
        return MM_FALSE;
    }
    if (reg_operand_matches(&arm->operands[1], rn) && reg_operand_matches(&arm->operands[2], rm)) {
        return MM_TRUE;
    }
    if (reg_operand_matches(&arm->operands[1], rm) && reg_operand_matches(&arm->operands[2], rn)) {
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool check_mem_operand(const cs_arm_op *op, mm_u8 base, mm_u8 index, long disp)
{
    int expected_base = arm_reg_from_mm(base);
    int expected_index = (index <= 15u) ? arm_reg_from_mm(index) : ARM_REG_INVALID;
    if (op->type != ARM_OP_MEM) {
        log_mem_mismatch("type is not MEM");
        return MM_FALSE;
    }
    if ((int)op->mem.base != expected_base) {
        log_mem_mismatch("base mismatch");
        return MM_FALSE;
    }
    if ((int)op->mem.index != expected_index) {
        log_mem_mismatch("index mismatch");
        return MM_FALSE;
    }
    if (op->mem.disp != (int)disp) {
        log_mem_mismatch("disp mismatch");
        return MM_FALSE;
    }
    return MM_TRUE;
}

static mm_bool mem_operand_matches(const cs_arm_op *op, int base, int index, long disp)
{
    if (op == 0 || op->type != ARM_OP_MEM) {
        return MM_FALSE;
    }
    if ((int)op->mem.base != base) {
        return MM_FALSE;
    }
    if ((int)op->mem.index != index) {
        return MM_FALSE;
    }
    if (op->mem.disp != (int)disp) {
        return MM_FALSE;
    }
    return MM_TRUE;
}

static arm_cc mm_cond_to_arm_cc(mm_u8 cond)
{
    if (cond <= MM_COND_LE) {
        return (arm_cc)(cond + 1u);
    }
    if (cond == MM_COND_AL) {
        return ARM_CC_AL;
    }
    return ARM_CC_INVALID;
}

static arm_cc arm_cc_invert(arm_cc cc)
{
    switch (cc) {
        case ARM_CC_EQ: return ARM_CC_NE;
        case ARM_CC_NE: return ARM_CC_EQ;
        case ARM_CC_HS: return ARM_CC_LO;
        case ARM_CC_LO: return ARM_CC_HS;
        case ARM_CC_MI: return ARM_CC_PL;
        case ARM_CC_PL: return ARM_CC_MI;
        case ARM_CC_VS: return ARM_CC_VC;
        case ARM_CC_VC: return ARM_CC_VS;
        case ARM_CC_HI: return ARM_CC_LS;
        case ARM_CC_LS: return ARM_CC_HI;
        case ARM_CC_GE: return ARM_CC_LT;
        case ARM_CC_LT: return ARM_CC_GE;
        case ARM_CC_GT: return ARM_CC_LE;
        case ARM_CC_LE: return ARM_CC_GT;
        default: return cc;
    }
}

static void it_mask_to_pattern(mm_u8 cond, mm_u8 mask, mm_u8 *pattern_out, mm_u8 *remaining_out)
{
    mm_u8 pattern = 0;
    mm_u8 remaining = 0;
    mm_u8 i;
    if (mask != 0u) {
        for (i = 0; i < 4u; ++i) {
            if ((mask & (1u << i)) != 0u) {
                remaining = (mm_u8)(4u - i);
                break;
            }
        }
        if (remaining != 0u) {
            pattern = 1u;
            for (i = 1u; i < remaining; ++i) {
                mm_u8 bit = (mm_u8)((mask >> (4u - i)) & 1u);
                mm_u8 pat_bit = (bit != 0u) ? 0u : 1u;
                pattern |= (mm_u8)(pat_bit << i);
            }
            if ((cond & 1u) != 0u && remaining > 1u) {
                mm_u8 flip = (mm_u8)(((1u << remaining) - 1u) & 0x0eu);
                pattern ^= flip;
            }
        }
    }
    if (pattern_out) {
        *pattern_out = pattern;
    }
    if (remaining_out) {
        *remaining_out = remaining;
    }
}

static mm_bool cross_check_kind(const cs_insn *insn, const struct mm_decoded *dec)
{
    if (insn == 0 || dec == 0) {
        return MM_FALSE;
    }
    switch (insn->id) {
        case ARM_INS_NOP: 
            return (dec->kind == MM_OP_NOP || dec->kind == MM_OP_NOP_W);
        case ARM_INS_YIELD: 
            return (dec->kind == MM_OP_YIELD || dec->kind == MM_OP_YIELD_W);
        case ARM_INS_WFE: 
            return (dec->kind == MM_OP_WFE || dec->kind == MM_OP_WFE_W);
        case ARM_INS_WFI: 
            return (dec->kind == MM_OP_WFI || dec->kind == MM_OP_WFI_W);
        case ARM_INS_SEV: 
            return (dec->kind == MM_OP_SEV || dec->kind == MM_OP_SEV_W);
        case ARM_INS_SEVL: 
            return dec->kind == MM_OP_SEVL_W;
        case ARM_INS_IT: return dec->kind == MM_OP_IT;
        case ARM_INS_B: return (dec->kind == MM_OP_B_COND || dec->kind == MM_OP_B_UNCOND || dec->kind == MM_OP_B_COND_WIDE || dec->kind == MM_OP_B_UNCOND_WIDE);
        case ARM_INS_BL: return dec->kind == MM_OP_BL;
        /* Capstone 4.x predates ARMv8-M TrustZone and disassembles the
         * BXNS / BLXNS encodings (`0x47[01][0..F][4]`) as plain BX / BLX.
         * Accept the TrustZone variants under the BX / BLX cases so the
         * cross-check works under capstone 4 (CI) and capstone 5 (local). */
        case ARM_INS_BX: return (dec->kind == MM_OP_BX || dec->kind == MM_OP_BXNS);
        case ARM_INS_BLX: return (dec->kind == MM_OP_BLX || dec->kind == MM_OP_BLXNS);
        case ARM_INS_CBZ: return dec->kind == MM_OP_CBZ;
        case ARM_INS_CBNZ: return dec->kind == MM_OP_CBNZ;
        case ARM_INS_BKPT: return dec->kind == MM_OP_BKPT;
        case ARM_INS_UDF: return dec->kind == MM_OP_UDF;
        case ARM_INS_SVC: return dec->kind == MM_OP_SVC;
        case ARM_INS_MOV:
            if (dec->kind == MM_OP_MOVW || dec->kind == MM_OP_MOVT) {
                return MM_TRUE;
            }
            /* Capstone 4.x reports `MOV rd, rm` for the Thumb LSLS rd, rm, #0
             * encoding (e.g. raw `0x0000` = movs r0, r0). Per ARMv7-M ARM
             * A8.6.97/A8.6.99, LSL rd, rm, #0 IS MOV rd, rm — they share the
             * same encoding family. Accept LSL_IMM with imm==0 as a MOV
             * alias to keep the cross-check working under both capstone 4
             * (id=ARM_INS_MOV) and capstone 5 (id=ARM_INS_MOVS). */
            if (dec->kind == MM_OP_LSL_IMM && dec->imm == 0u) {
                return MM_TRUE;
            }
            return (dec->kind == MM_OP_MOV_IMM || dec->kind == MM_OP_MOV_REG);
        case ARM_INS_MOVW: return dec->kind == MM_OP_MOVW;
        case ARM_INS_MOVT: return dec->kind == MM_OP_MOVT;
        case ARM_INS_MVN:
            if (dec->kind == MM_OP_MVN_REG || dec->kind == MM_OP_MVN_IMM) {
                return MM_TRUE;
            }
            return (dec->kind == MM_OP_ORN_REG && dec->rn == 15u);
        case ARM_INS_ADD:
        case ARM_INS_ADDW:
            return (dec->kind == MM_OP_ADD_IMM || dec->kind == MM_OP_ADD_REG || dec->kind == MM_OP_ADD_SP_IMM);
        case ARM_INS_SUB:
        case ARM_INS_SUBW:
            return (dec->kind == MM_OP_SUB_IMM || dec->kind == MM_OP_SUB_REG || dec->kind == MM_OP_SUB_SP_IMM || dec->kind == MM_OP_SUB_IMM_NF);
        case ARM_INS_ADC: return (dec->kind == MM_OP_ADCS_REG || dec->kind == MM_OP_ADC_IMM);
        case ARM_INS_SBC: return (dec->kind == MM_OP_SBCS_REG || dec->kind == MM_OP_SBC_IMM || dec->kind == MM_OP_SBC_IMM_NF);
        case ARM_INS_RSB: return (dec->kind == MM_OP_RSB_IMM || dec->kind == MM_OP_RSB_REG || dec->kind == MM_OP_NEG);
        case ARM_INS_AND: return dec->kind == MM_OP_AND_REG;
        case ARM_INS_EOR: return dec->kind == MM_OP_EOR_REG;
        case ARM_INS_ORR: return dec->kind == MM_OP_ORR_REG;
        case ARM_INS_ORN: return (dec->kind == MM_OP_ORN_REG || dec->kind == MM_OP_ORN_IMM);
        case ARM_INS_BIC: return dec->kind == MM_OP_BIC_REG;
        case ARM_INS_TST: return (dec->kind == MM_OP_TST_REG || dec->kind == MM_OP_TST_IMM);
        case ARM_INS_CMP: return (dec->kind == MM_OP_CMP_REG || dec->kind == MM_OP_CMP_IMM);
        case ARM_INS_CMN: return (dec->kind == MM_OP_CMN_REG || dec->kind == MM_OP_CMN_IMM);
        case ARM_INS_LSL: return (dec->kind == MM_OP_LSL_IMM || dec->kind == MM_OP_LSL_REG);
        case ARM_INS_LSR: return (dec->kind == MM_OP_LSR_IMM || dec->kind == MM_OP_LSR_REG);
        case ARM_INS_ASR: return (dec->kind == MM_OP_ASR_IMM || dec->kind == MM_OP_ASR_REG);
        case ARM_INS_ROR: return (dec->kind == MM_OP_ROR_REG || dec->kind == MM_OP_ROR_REG_NF || dec->kind == MM_OP_ROR_IMM);
        case ARM_INS_LDR: return (dec->kind == MM_OP_LDR_IMM || dec->kind == MM_OP_LDR_REG || dec->kind == MM_OP_LDR_POST_IMM || dec->kind == MM_OP_LDR_PRE_IMM || dec->kind == MM_OP_LDR_LITERAL);
        case ARM_INS_STR: return (dec->kind == MM_OP_STR_IMM || dec->kind == MM_OP_STR_REG || dec->kind == MM_OP_STR_POST_IMM || dec->kind == MM_OP_STR_PRE_IMM);
        case ARM_INS_LDRB: return (dec->kind == MM_OP_LDRB_IMM || dec->kind == MM_OP_LDRB_REG || dec->kind == MM_OP_LDRB_POST_IMM || dec->kind == MM_OP_LDRB_PRE_IMM);
        case ARM_INS_STRB: return (dec->kind == MM_OP_STRB_IMM || dec->kind == MM_OP_STRB_REG || dec->kind == MM_OP_STRB_POST_IMM || dec->kind == MM_OP_STRB_PRE_IMM);
        case ARM_INS_LDRH: return (dec->kind == MM_OP_LDRH_IMM || dec->kind == MM_OP_LDRH_REG ||
                                   dec->kind == MM_OP_LDRH_POST_IMM || dec->kind == MM_OP_LDRH_PRE_IMM);
        case ARM_INS_STRH: return (dec->kind == MM_OP_STRH_IMM || dec->kind == MM_OP_STRH_REG ||
                                   dec->kind == MM_OP_STRH_POST_IMM || dec->kind == MM_OP_STRH_PRE_IMM);
        case ARM_INS_LDRD: return dec->kind == MM_OP_LDRD;
        case ARM_INS_STRD: return dec->kind == MM_OP_STRD;
        case ARM_INS_LDRSB: return (dec->kind == MM_OP_LDRSB_IMM ||
                                    dec->kind == MM_OP_LDRSB_POST_IMM ||
                                    dec->kind == MM_OP_LDRSB_REG);
        case ARM_INS_LDRSH: return (dec->kind == MM_OP_LDRSH_IMM || dec->kind == MM_OP_LDRSH_REG);
        case ARM_INS_DSB: return dec->kind == MM_OP_DSB;
        case ARM_INS_DMB: return dec->kind == MM_OP_DMB;
        case ARM_INS_ISB: return dec->kind == MM_OP_ISB;
        case ARM_INS_UBFX: return dec->kind == MM_OP_UBFX;
        case ARM_INS_SBFX: return dec->kind == MM_OP_SBFX;
        case ARM_INS_BFI: return dec->kind == MM_OP_BFI;
        case ARM_INS_BFC: return dec->kind == MM_OP_BFC;
        case ARM_INS_TBB: return dec->kind == MM_OP_TBB;
        case ARM_INS_TBH: return dec->kind == MM_OP_TBH;
        case ARM_INS_CPS: return dec->kind == MM_OP_CPS;
        case ARM_INS_UXTB: return dec->kind == MM_OP_UXTB;
        case ARM_INS_SXTB: return dec->kind == MM_OP_SXTB;
        case ARM_INS_SXTH: return dec->kind == MM_OP_SXTH;
        case ARM_INS_UXTH: return dec->kind == MM_OP_UXTH;
        case ARM_INS_CLZ: return dec->kind == MM_OP_CLZ;
        case ARM_INS_RBIT: return dec->kind == MM_OP_RBIT;
        case ARM_INS_REV: return dec->kind == MM_OP_REV;
        case ARM_INS_REV16: return dec->kind == MM_OP_REV16;
        case ARM_INS_REVSH: return dec->kind == MM_OP_REVSH;
        case ARM_INS_MRS: return dec->kind == MM_OP_MRS;
        case ARM_INS_MSR: return dec->kind == MM_OP_MSR;
#ifdef ARM_INS_TT
        case ARM_INS_TT: return dec->kind == MM_OP_TT;
#endif
#ifdef ARM_INS_TTT
        case ARM_INS_TTT: return dec->kind == MM_OP_TTT;
#endif
#ifdef ARM_INS_TTA
        case ARM_INS_TTA: return dec->kind == MM_OP_TTA;
#endif
#ifdef ARM_INS_TTAT
        case ARM_INS_TTAT: return dec->kind == MM_OP_TTAT;
#endif
        case ARM_INS_SDIV: return dec->kind == MM_OP_SDIV;
        case ARM_INS_UDIV: return dec->kind == MM_OP_UDIV;
        case ARM_INS_SMULL: return dec->kind == MM_OP_SMULL;
        case ARM_INS_UMULL: return dec->kind == MM_OP_UMULL;
        case ARM_INS_SMLAL: return dec->kind == MM_OP_SMLAL;
        case ARM_INS_UMLAL: return dec->kind == MM_OP_UMLAL;
        case ARM_INS_UMAAL: return dec->kind == MM_OP_UMAAL;
        case ARM_INS_MUL: return (dec->kind == MM_OP_MUL || dec->kind == MM_OP_MUL_W);
        case ARM_INS_MLA: return dec->kind == MM_OP_MLA;
        case ARM_INS_MLS: return dec->kind == MM_OP_MLS;
        case ARM_INS_SMLABB:
        case ARM_INS_SMLABT:
        case ARM_INS_SMLATB:
        case ARM_INS_SMLATT:
            return dec->kind == MM_OP_SMLA;
        case ARM_INS_SMMUL:
        case ARM_INS_SMMULR:
            return dec->kind == MM_OP_SMMUL;
        case ARM_INS_SMMLA:
        case ARM_INS_SMMLAR:
            return dec->kind == MM_OP_SMMLA;
        case ARM_INS_SMMLS:
        case ARM_INS_SMMLSR:
            return dec->kind == MM_OP_SMMLS;
        case ARM_INS_SMLAWB:
            return dec->kind == MM_OP_SMLAWB;
        case ARM_INS_SMLAWT:
            return dec->kind == MM_OP_SMLAWT;
        case ARM_INS_SMULWB:
            return dec->kind == MM_OP_SMULWB;
        case ARM_INS_SMULWT:
            return dec->kind == MM_OP_SMULWT;
#ifdef ARM_INS_BXNS
        case ARM_INS_BXNS: return dec->kind == MM_OP_BXNS;
#endif
#ifdef ARM_INS_BLXNS
        case ARM_INS_BLXNS: return dec->kind == MM_OP_BLXNS;
#endif
#ifdef ARM_INS_SG
        case ARM_INS_SG: return dec->kind == MM_OP_SG;
#endif
        case ARM_INS_VADD: return dec->kind == MM_OP_VADD;
        case ARM_INS_VSUB: return dec->kind == MM_OP_VSUB;
        case ARM_INS_VMUL: return dec->kind == MM_OP_VMUL;
        case ARM_INS_VDIV: return dec->kind == MM_OP_VDIV;
        case ARM_INS_VNEG: return dec->kind == MM_OP_VNEG;
        case ARM_INS_VABS: return dec->kind == MM_OP_VABS;
        case ARM_INS_VMLA: return dec->kind == MM_OP_VMLA;
        case ARM_INS_VMLS: return dec->kind == MM_OP_VMLS;
        case ARM_INS_VSQRT: return dec->kind == MM_OP_VSQRT;
        case ARM_INS_VCMP: return dec->kind == MM_OP_VCMP;
        case ARM_INS_VCMPE: return dec->kind == MM_OP_VCMPE;
        case ARM_INS_VCVT:
            return (dec->kind == MM_OP_VCVT_S32_F32 || dec->kind == MM_OP_VCVT_U32_F32 ||
                    dec->kind == MM_OP_VCVT_F32_S32 || dec->kind == MM_OP_VCVT_F32_U32 ||
                    dec->kind == MM_OP_VCVTB_F32_F16 || dec->kind == MM_OP_VCVTT_F32_F16 ||
                    dec->kind == MM_OP_VCVTB_F16_F32 || dec->kind == MM_OP_VCVTT_F16_F32 ||
                    dec->kind == MM_OP_VCVT_FIXED);
        case ARM_INS_VCVTR:
            return (dec->kind == MM_OP_VCVTR_S32_F32 || dec->kind == MM_OP_VCVTR_U32_F32);
#ifdef ARM_INS_VCVTA
        case ARM_INS_VCVTA:
            return (dec->kind == MM_OP_VCVTA_S32_F32 || dec->kind == MM_OP_VCVTA_U32_F32);
#endif
#ifdef ARM_INS_VCVTN
        case ARM_INS_VCVTN:
            return (dec->kind == MM_OP_VCVTN_S32_F32 || dec->kind == MM_OP_VCVTN_U32_F32);
#endif
#ifdef ARM_INS_VCVTP
        case ARM_INS_VCVTP:
            return (dec->kind == MM_OP_VCVTP_S32_F32 || dec->kind == MM_OP_VCVTP_U32_F32);
#endif
#ifdef ARM_INS_VCVTM
        case ARM_INS_VCVTM:
            return (dec->kind == MM_OP_VCVTM_S32_F32 || dec->kind == MM_OP_VCVTM_U32_F32);
#endif
#ifdef ARM_INS_VCVTB
        case ARM_INS_VCVTB:
            return (dec->kind == MM_OP_VCVTB_F32_F16 || dec->kind == MM_OP_VCVTB_F16_F32);
#endif
#ifdef ARM_INS_VCVTT
        case ARM_INS_VCVTT:
            return (dec->kind == MM_OP_VCVTT_F32_F16 || dec->kind == MM_OP_VCVTT_F16_F32);
#endif
#ifdef ARM_INS_VRINTA
        case ARM_INS_VRINTA: return dec->kind == MM_OP_VRINTA_F32;
#endif
#ifdef ARM_INS_VRINTN
        case ARM_INS_VRINTN: return dec->kind == MM_OP_VRINTN_F32;
#endif
#ifdef ARM_INS_VRINTP
        case ARM_INS_VRINTP: return dec->kind == MM_OP_VRINTP_F32;
#endif
#ifdef ARM_INS_VRINTM
        case ARM_INS_VRINTM: return dec->kind == MM_OP_VRINTM_F32;
#endif
#ifdef ARM_INS_VRINTZ
        case ARM_INS_VRINTZ: return dec->kind == MM_OP_VRINTZ_F32;
#endif
#ifdef ARM_INS_VRINTR
        case ARM_INS_VRINTR: return dec->kind == MM_OP_VRINTR_F32;
#endif
#ifdef ARM_INS_VRINTX
        case ARM_INS_VRINTX: return dec->kind == MM_OP_VRINTX_F32;
#endif
#ifdef ARM_INS_VMAXNM
        case ARM_INS_VMAXNM: return dec->kind == MM_OP_VMAXNM_F32;
#endif
#ifdef ARM_INS_VMINNM
        case ARM_INS_VMINNM: return dec->kind == MM_OP_VMINNM_F32;
#endif
#ifdef ARM_INS_VSELEQ
        case ARM_INS_VSELEQ:
#endif
#ifdef ARM_INS_VSELVS
        case ARM_INS_VSELVS:
#endif
#ifdef ARM_INS_VSELGE
        case ARM_INS_VSELGE:
#endif
#ifdef ARM_INS_VSELGT
        case ARM_INS_VSELGT:
#endif
#if defined(ARM_INS_VSELEQ) || defined(ARM_INS_VSELVS) || defined(ARM_INS_VSELGE) || defined(ARM_INS_VSELGT)
            return dec->kind == MM_OP_VSEL_F32;
#endif
#ifdef ARM_INS_VFMA
        case ARM_INS_VFMA: return dec->kind == MM_OP_VFMA_F32;
#endif
#ifdef ARM_INS_VFMS
        case ARM_INS_VFMS: return dec->kind == MM_OP_VFMS_F32;
#endif
#ifdef ARM_INS_VFNMA
        case ARM_INS_VFNMA: return dec->kind == MM_OP_VFNMA_F32;
#endif
#ifdef ARM_INS_VFNMS
        case ARM_INS_VFNMS: return dec->kind == MM_OP_VFNMS_F32;
#endif
        case ARM_INS_VNMUL: return dec->kind == MM_OP_VNMUL_F32;
        case ARM_INS_VNMLA: return dec->kind == MM_OP_VNMLA_F32;
        case ARM_INS_VNMLS: return dec->kind == MM_OP_VNMLS_F32;

        /* DSP parallel arithmetic (added in fidelity update) */
#ifdef ARM_INS_SADD8
        case ARM_INS_SADD8: return dec->kind == MM_OP_SADD8;
#endif
#ifdef ARM_INS_SSUB8
        case ARM_INS_SSUB8: return dec->kind == MM_OP_SSUB8;
#endif
#ifdef ARM_INS_UADD8
        case ARM_INS_UADD8: return dec->kind == MM_OP_UADD8;
#endif
#ifdef ARM_INS_USUB8
        case ARM_INS_USUB8: return dec->kind == MM_OP_USUB8;
#endif
#ifdef ARM_INS_QADD8
        case ARM_INS_QADD8: return dec->kind == MM_OP_QADD8;
#endif
#ifdef ARM_INS_QSUB8
        case ARM_INS_QSUB8: return dec->kind == MM_OP_QSUB8;
#endif
#ifdef ARM_INS_UQADD8
        case ARM_INS_UQADD8: return dec->kind == MM_OP_UQADD8;
#endif
#ifdef ARM_INS_UQSUB8
        case ARM_INS_UQSUB8: return dec->kind == MM_OP_UQSUB8;
#endif
#ifdef ARM_INS_SHADD8
        case ARM_INS_SHADD8: return dec->kind == MM_OP_SHADD8;
#endif
#ifdef ARM_INS_SHSUB8
        case ARM_INS_SHSUB8: return dec->kind == MM_OP_SHSUB8;
#endif
#ifdef ARM_INS_UHADD8
        case ARM_INS_UHADD8: return dec->kind == MM_OP_UHADD8;
#endif
#ifdef ARM_INS_UHSUB8
        case ARM_INS_UHSUB8: return dec->kind == MM_OP_UHSUB8;
#endif
#ifdef ARM_INS_SADD16
        case ARM_INS_SADD16: return dec->kind == MM_OP_SADD16;
#endif
#ifdef ARM_INS_SSUB16
        case ARM_INS_SSUB16: return dec->kind == MM_OP_SSUB16;
#endif
#ifdef ARM_INS_UADD16
        case ARM_INS_UADD16: return dec->kind == MM_OP_UADD16;
#endif
#ifdef ARM_INS_USUB16
        case ARM_INS_USUB16: return dec->kind == MM_OP_USUB16;
#endif
#ifdef ARM_INS_QADD16
        case ARM_INS_QADD16: return dec->kind == MM_OP_QADD16;
#endif
#ifdef ARM_INS_QSUB16
        case ARM_INS_QSUB16: return dec->kind == MM_OP_QSUB16;
#endif
#ifdef ARM_INS_UQADD16
        case ARM_INS_UQADD16: return dec->kind == MM_OP_UQADD16;
#endif
#ifdef ARM_INS_UQSUB16
        case ARM_INS_UQSUB16: return dec->kind == MM_OP_UQSUB16;
#endif
#ifdef ARM_INS_SHADD16
        case ARM_INS_SHADD16: return dec->kind == MM_OP_SHADD16;
#endif
#ifdef ARM_INS_SHSUB16
        case ARM_INS_SHSUB16: return dec->kind == MM_OP_SHSUB16;
#endif
#ifdef ARM_INS_UHADD16
        case ARM_INS_UHADD16: return dec->kind == MM_OP_UHADD16;
#endif
#ifdef ARM_INS_UHSUB16
        case ARM_INS_UHSUB16: return dec->kind == MM_OP_UHSUB16;
#endif
#ifdef ARM_INS_SASX
        case ARM_INS_SASX: return dec->kind == MM_OP_SASX;
#endif
#ifdef ARM_INS_SSAX
        case ARM_INS_SSAX: return dec->kind == MM_OP_SSAX;
#endif
#ifdef ARM_INS_UASX
        case ARM_INS_UASX: return dec->kind == MM_OP_UASX;
#endif
#ifdef ARM_INS_USAX
        case ARM_INS_USAX: return dec->kind == MM_OP_USAX;
#endif
#ifdef ARM_INS_QASX
        case ARM_INS_QASX: return dec->kind == MM_OP_QASX;
#endif
#ifdef ARM_INS_QSAX
        case ARM_INS_QSAX: return dec->kind == MM_OP_QSAX;
#endif
#ifdef ARM_INS_UQASX
        case ARM_INS_UQASX: return dec->kind == MM_OP_UQASX;
#endif
#ifdef ARM_INS_UQSAX
        case ARM_INS_UQSAX: return dec->kind == MM_OP_UQSAX;
#endif
#ifdef ARM_INS_SHASX
        case ARM_INS_SHASX: return dec->kind == MM_OP_SHASX;
#endif
#ifdef ARM_INS_SHSAX
        case ARM_INS_SHSAX: return dec->kind == MM_OP_SHSAX;
#endif
#ifdef ARM_INS_UHASX
        case ARM_INS_UHASX: return dec->kind == MM_OP_UHASX;
#endif
#ifdef ARM_INS_UHSAX
        case ARM_INS_UHSAX: return dec->kind == MM_OP_UHSAX;
#endif
#ifdef ARM_INS_SSAT16
        case ARM_INS_SSAT16: return dec->kind == MM_OP_SSAT16;
#endif
#ifdef ARM_INS_USAT16
        case ARM_INS_USAT16: return dec->kind == MM_OP_USAT16;
#endif
#ifdef ARM_INS_SEL
        case ARM_INS_SEL: return dec->kind == MM_OP_SEL;
#endif
#ifdef ARM_INS_USAD8
        case ARM_INS_USAD8: return dec->kind == MM_OP_USAD8;
#endif
#ifdef ARM_INS_USADA8
        case ARM_INS_USADA8: return dec->kind == MM_OP_USADA8;
#endif
#ifdef ARM_INS_SMUAD
        case ARM_INS_SMUAD: return dec->kind == MM_OP_SMUAD;
#endif
#ifdef ARM_INS_SMUADX
        case ARM_INS_SMUADX: return dec->kind == MM_OP_SMUADX;
#endif
#ifdef ARM_INS_SMUSD
        case ARM_INS_SMUSD: return dec->kind == MM_OP_SMUSD;
#endif
#ifdef ARM_INS_SMUSDX
        case ARM_INS_SMUSDX: return dec->kind == MM_OP_SMUSDX;
#endif
#ifdef ARM_INS_SMLSLD
        case ARM_INS_SMLSLD: return dec->kind == MM_OP_SMLSLD;
#endif
#ifdef ARM_INS_SMLSLDX
        case ARM_INS_SMLSLDX: return dec->kind == MM_OP_SMLSLDX;
#endif
#ifdef ARM_INS_VPUSH
        case ARM_INS_VPUSH: return dec->kind == MM_OP_VSTM;
#endif
#ifdef ARM_INS_VPOP
        case ARM_INS_VPOP: return dec->kind == MM_OP_VLDM;
#endif
        case ARM_INS_VMOV:
            return (dec->kind == MM_OP_VMOV_SR || dec->kind == MM_OP_VMOV_RS ||
                    dec->kind == MM_OP_VMOV_SRR || dec->kind == MM_OP_VMOV_RSS ||
                    dec->kind == MM_OP_VMOV_DRR || dec->kind == MM_OP_VMOV_RDD ||
                    dec->kind == MM_OP_VMOV_IMM || dec->kind == MM_OP_VMOV_F32);
        case ARM_INS_VMRS: return dec->kind == MM_OP_VMRS;
        case ARM_INS_VMSR: return dec->kind == MM_OP_VMSR;
        case ARM_INS_VLDR: return dec->kind == MM_OP_VLDR;
        case ARM_INS_VSTR: return dec->kind == MM_OP_VSTR;
        case ARM_INS_VLDMIA:
        case ARM_INS_VLDMDB:
            return dec->kind == MM_OP_VLDM;
        case ARM_INS_VSTMIA:
        case ARM_INS_VSTMDB:
            return dec->kind == MM_OP_VSTM;
        
        /* Priority 1 DSP instructions */
        case ARM_INS_SMLAD: return dec->kind == MM_OP_SMLAD;
        case ARM_INS_SMLADX: return dec->kind == MM_OP_SMLADX;
        case ARM_INS_SMLALD: return dec->kind == MM_OP_SMLALD;
        case ARM_INS_SMLALDX: return dec->kind == MM_OP_SMLALDX;
        case ARM_INS_SMLSD: return dec->kind == MM_OP_SMLSD;
        case ARM_INS_SMLSDX: return dec->kind == MM_OP_SMLSDX;
        case ARM_INS_QADD: return dec->kind == MM_OP_QADD;
        case ARM_INS_QSUB: return dec->kind == MM_OP_QSUB;
        case ARM_INS_QDADD: return dec->kind == MM_OP_QDADD;
        case ARM_INS_QDSUB: return dec->kind == MM_OP_QDSUB;
        case ARM_INS_PKHBT: return dec->kind == MM_OP_PKHBT;
        case ARM_INS_PKHTB: return dec->kind == MM_OP_PKHTB;
        case ARM_INS_SSAT: return dec->kind == MM_OP_SSAT;
        case ARM_INS_USAT: return dec->kind == MM_OP_USAT;
        case ARM_INS_SMULBB:
        case ARM_INS_SMULBT:
        case ARM_INS_SMULTB:
        case ARM_INS_SMULTT:
            return dec->kind == MM_OP_SMULBB;
        
        /* Instructions intentionally not implemented */
        case ARM_INS_SETEND:
            /* SETEND is deprecated in ARMv8 and not supported in Cortex-M */
            return MM_TRUE;
        
        /* Coprocessor instructions - map to our decoders */
        case ARM_INS_STC:
        case ARM_INS_STCL:
            return dec->kind == MM_OP_STC;
        case ARM_INS_STC2:
        case ARM_INS_STC2L:
            return dec->kind == MM_OP_STC2;
        case ARM_INS_LDC:
        case ARM_INS_LDCL:
            return dec->kind == MM_OP_LDC;
        case ARM_INS_LDC2:
        case ARM_INS_LDC2L:
            return dec->kind == MM_OP_LDC2;
        case ARM_INS_CDP:
        case ARM_INS_CDP2:
        case ARM_INS_MCR:
        case ARM_INS_MCR2:
        case ARM_INS_MRC:
        case ARM_INS_MRC2:
            return dec->kind == MM_OP_MCR_MRC || dec->kind == MM_OP_CDP;
        case ARM_INS_MCRR:
        case ARM_INS_MCRR2:
        case ARM_INS_MRRC:
        case ARM_INS_MRRC2:
            return dec->kind == MM_OP_MCRR_MRRC;
        
        default:
            /* Unmapped instruction in cross_check_kind.
             * If m33mu returned UNDEFINED, this might be a gap in coverage.
             * Print a warning to help identify missing implementations. */
            if (dec->kind == MM_OP_UNDEFINED) {
                static mm_bool warned = MM_FALSE;
                if (!warned) {
                    fprintf(stderr, "[CAPSTONE] WARNING: unmapped instruction capstone_id=%u decoded as MM_OP_UNDEFINED\n",
                            (unsigned)insn->id);
                    fprintf(stderr, "[CAPSTONE] Expected assembly: %s %s\n",
                            insn->mnemonic, insn->op_str);
                    fprintf(stderr, "[CAPSTONE] This may indicate missing instruction support in m33mu.\n");
                    fprintf(stderr, "[CAPSTONE] Further warnings will be suppressed.\n");
                    warned = MM_TRUE;
                }
            }
            return MM_TRUE;
    }
}

static mm_bool cross_check_operands(const struct mm_fetch_result *fetch, const cs_insn *insn, const struct mm_decoded *dec)
{
    const cs_arm *arm;
    const cs_arm_op *op;
    mm_u32 imm;

    if (fetch == 0 || insn == 0 || dec == 0) {
        return MM_TRUE;
    }
    if (insn->detail == 0) {
        return MM_FALSE;
    }
    arm = &insn->detail->arm;

    switch (insn->id) {
        case ARM_INS_VADD:
        case ARM_INS_VSUB:
        case ARM_INS_VMUL:
        case ARM_INS_VDIV:
        case ARM_INS_VMLA:
        case ARM_INS_VMLS:
            if (arm->op_count < 3u) {
                return MM_FALSE;
            }
            if (!check_vfp_reg_operand(&arm->operands[0], 0, dec->rd)) {
                return MM_FALSE;
            }
            if (!check_vfp_reg_operand(&arm->operands[1], 1, dec->rn)) {
                return MM_FALSE;
            }
            return check_vfp_reg_operand(&arm->operands[2], 2, dec->rm);
        case ARM_INS_VNEG:
        case ARM_INS_VABS:
        case ARM_INS_VSQRT:
            if (arm->op_count < 2u) {
                return MM_FALSE;
            }
            if (!check_vfp_reg_operand(&arm->operands[0], 0, dec->rd)) {
                return MM_FALSE;
            }
            return check_vfp_reg_operand(&arm->operands[1], 1, dec->rm);
        case ARM_INS_VCMP:
        case ARM_INS_VCMPE:
            if (arm->op_count < 2u) {
                return MM_FALSE;
            }
            if (!check_vfp_reg_operand(&arm->operands[0], 0, dec->rd)) {
                return MM_FALSE;
            }
            if (arm->operands[1].type == ARM_OP_IMM) {
                return dec->imm != 0u && arm->operands[1].imm == 0;
            }
            return check_vfp_reg_operand(&arm->operands[1], 1, dec->rm);
        case ARM_INS_VCVT:
        case ARM_INS_VCVTR:
            if (arm->op_count < 2u) {
                return MM_FALSE;
            }
            if (!check_vfp_reg_operand(&arm->operands[0], 0, dec->rd)) {
                return MM_FALSE;
            }
            return check_vfp_reg_operand(&arm->operands[1], 1, dec->rm);
        case ARM_INS_VMOV:
            if (arm->op_count < 2u) {
                return MM_FALSE;
            }
            if (dec->kind == MM_OP_VMOV_IMM) {
                if (!check_vfp_reg_operand(&arm->operands[0], 0, dec->rd)) {
                    return MM_FALSE;
                }
                return arm->operands[1].type == ARM_OP_IMM;
            }
            if (dec->kind == MM_OP_VMOV_SR) {
                if (!check_vfp_reg_operand(&arm->operands[0], 0, dec->rd)) {
                    return MM_FALSE;
                }
                return check_reg_operand(&arm->operands[1], 1, dec->rn);
            }
            if (dec->kind == MM_OP_VMOV_RS) {
                if (!check_reg_operand(&arm->operands[0], 0, dec->rd)) {
                    return MM_FALSE;
                }
                return check_vfp_reg_operand(&arm->operands[1], 1, dec->rn);
            }
            if (dec->kind == MM_OP_VMOV_SRR) {
                if (arm->op_count < 4u) {
                    return MM_FALSE;
                }
                if (!check_vfp_reg_operand(&arm->operands[0], 0, dec->rd)) {
                    return MM_FALSE;
                }
                if (!check_vfp_reg_operand(&arm->operands[1], 1, dec->rn)) {
                    return MM_FALSE;
                }
                if (!check_reg_operand(&arm->operands[2], 2, dec->rm)) {
                    return MM_FALSE;
                }
                return check_reg_operand(&arm->operands[3], 3, dec->ra);
            }
            if (dec->kind == MM_OP_VMOV_RSS) {
                if (arm->op_count < 4u) {
                    return MM_FALSE;
                }
                if (!check_reg_operand(&arm->operands[0], 0, dec->rd)) {
                    return MM_FALSE;
                }
                if (!check_reg_operand(&arm->operands[1], 1, dec->rn)) {
                    return MM_FALSE;
                }
                if (!check_vfp_reg_operand(&arm->operands[2], 2, dec->rm)) {
                    return MM_FALSE;
                }
                return check_vfp_reg_operand(&arm->operands[3], 3, dec->ra);
            }
            if (dec->kind == MM_OP_VMOV_DRR) {
                if (arm->op_count < 3u) {
                    return MM_FALSE;
                }
                if (!check_vfp_d_reg_operand(&arm->operands[0], 0, dec->rd)) {
                    return MM_FALSE;
                }
                if (!check_reg_operand(&arm->operands[1], 1, dec->rm)) {
                    return MM_FALSE;
                }
                return check_reg_operand(&arm->operands[2], 2, dec->ra);
            }
            if (dec->kind == MM_OP_VMOV_RDD) {
                if (arm->op_count < 3u) {
                    return MM_FALSE;
                }
                if (!check_reg_operand(&arm->operands[0], 0, dec->rd)) {
                    return MM_FALSE;
                }
                if (!check_reg_operand(&arm->operands[1], 1, dec->rn)) {
                    return MM_FALSE;
                }
                return check_vfp_d_reg_operand(&arm->operands[2], 2, dec->rm);
            }
            return MM_TRUE;
        case ARM_INS_VLDR:
        case ARM_INS_VSTR: {
            long disp;
            mm_u32 imm = dec->imm & 0x0FFFFFFFu;
            mm_bool u = (dec->imm & 0x80000000u) != 0u;
            mm_bool is_double = (dec->imm & MM_VFP_LS_DOUBLE) != 0u;
            if (arm->op_count < 2u) {
                return MM_FALSE;
            }
            if (is_double) {
                if (!check_vfp_d_reg_operand(&arm->operands[0], 0, dec->rd)) {
                    return MM_FALSE;
                }
            } else {
                if (!check_vfp_reg_operand(&arm->operands[0], 0, dec->rd)) {
                    return MM_FALSE;
                }
            }
            disp = u ? (long)imm : -(long)imm;
            return check_mem_operand(&arm->operands[1], dec->rn, 0xffu, disp);
        }
        case ARM_INS_VLDMIA:
        case ARM_INS_VLDMDB:
        case ARM_INS_VSTMIA:
        case ARM_INS_VSTMDB:
            return MM_TRUE;
        case ARM_INS_MOV:
        case ARM_INS_MOVW:
        case ARM_INS_MOVT:
        case ARM_INS_MVN:
            if (arm->op_count < 2u) {
                return MM_FALSE;
            }
            if (!check_reg_operand(&arm->operands[0], 0, dec->rd)) {
                return MM_FALSE;
            }
            op = &arm->operands[1];
            if (op->type == ARM_OP_IMM || dec->kind == MM_OP_MOV_IMM || dec->kind == MM_OP_MOVW ||
                dec->kind == MM_OP_MOVT || dec->kind == MM_OP_MVN_IMM) {
                imm = dec->imm;
                if (dec->kind == MM_OP_MVN_IMM) {
                    imm = thumb_expand_imm12(dec->imm);
                }
                return check_imm_operand(op, 1, (long)imm);
            }
            return check_reg_operand(op, 1, dec->rm);
        case ARM_INS_ADD:
        case ARM_INS_ADDW:
        case ARM_INS_SUB:
        case ARM_INS_SUBW:
        case ARM_INS_ADC:
        case ARM_INS_SBC:
        case ARM_INS_RSB:
            if (arm->op_count < 2u) {
                return MM_FALSE;
            }
            if (!check_reg_operand(&arm->operands[0], 0, dec->rd)) {
                return MM_FALSE;
            }
            if (dec->kind == MM_OP_NEG) {
                if (!check_reg_operand(&arm->operands[1], 1, dec->rm)) {
                    return MM_FALSE;
                }
                if (arm->op_count >= 3u && arm->operands[2].type == ARM_OP_IMM) {
                    return check_imm_operand(&arm->operands[2], 2, 0);
                }
                return MM_TRUE;
            }
            if (arm->op_count == 2u) {
                op = &arm->operands[1];
                if (op->type == ARM_OP_IMM) {
                    return check_imm_operand(op, 1, (long)dec->imm);
                }
                return check_reg_operand(op, 1, dec->rm);
            }
            op = &arm->operands[2];
            if (op->type == ARM_OP_IMM || dec->kind == MM_OP_ADD_IMM || dec->kind == MM_OP_SUB_IMM ||
                dec->kind == MM_OP_SUB_IMM_NF || dec->kind == MM_OP_ADC_IMM || dec->kind == MM_OP_SBC_IMM ||
                dec->kind == MM_OP_SBC_IMM_NF || dec->kind == MM_OP_RSB_IMM || dec->kind == MM_OP_ADD_SP_IMM ||
                dec->kind == MM_OP_SUB_SP_IMM) {
                if (!check_reg_operand(&arm->operands[1], 1, dec->rn)) {
                    return MM_FALSE;
                }
                return check_imm_operand(op, 2, (long)dec->imm);
            }
            if (insn->id == ARM_INS_ADD || insn->id == ARM_INS_ADDW || insn->id == ARM_INS_ADC) {
                return match_reg_reg_commutative(arm, dec->rn, dec->rm);
            }
            if (!check_reg_operand(&arm->operands[1], 1, dec->rn)) {
                return MM_FALSE;
            }
            return check_reg_operand(op, 2, dec->rm);
        case ARM_INS_AND:
        case ARM_INS_ORR:
        case ARM_INS_EOR:
        case ARM_INS_BIC:
            if (arm->op_count < 2u) {
                return MM_FALSE;
            }
            if (!check_reg_operand(&arm->operands[0], 0, dec->rd)) {
                return MM_FALSE;
            }
            if (arm->op_count == 2u) {
                op = &arm->operands[1];
                if (op->type == ARM_OP_IMM) {
                    return check_imm_operand(op, 1, (long)dec->imm);
                }
                return check_reg_operand(op, 1, dec->rm);
            }
            op = &arm->operands[2];
            if (op->type == ARM_OP_IMM) {
                if (!check_reg_operand(&arm->operands[1], 1, dec->rn)) {
                    return MM_FALSE;
                }
                return check_imm_operand(op, 2, (long)dec->imm);
            }
            if (insn->id == ARM_INS_AND || insn->id == ARM_INS_ORR || insn->id == ARM_INS_EOR) {
                return match_reg_reg_commutative(arm, dec->rn, dec->rm);
            }
            if (!check_reg_operand(&arm->operands[1], 1, dec->rn)) {
                return MM_FALSE;
            }
            return check_reg_operand(op, 2, dec->rm);
        case ARM_INS_TST:
        case ARM_INS_CMP:
        case ARM_INS_CMN:
            if (arm->op_count < 2u) {
                return MM_FALSE;
            }
            if (!check_reg_operand(&arm->operands[0], 0, dec->rn)) {
                return MM_FALSE;
            }
            op = &arm->operands[1];
            if (op->type == ARM_OP_IMM) {
                return check_imm_operand(op, 1, (long)dec->imm);
            }
            return check_reg_operand(op, 1, dec->rm);
        case ARM_INS_LSL:
        case ARM_INS_LSR:
        case ARM_INS_ASR:
        case ARM_INS_ROR:
            if (arm->op_count < 2u) {
                return MM_FALSE;
            }
            if (!check_reg_operand(&arm->operands[0], 0, dec->rd)) {
                return MM_FALSE;
            }
            if (arm->op_count == 2u) {
                return check_reg_operand(&arm->operands[1], 1, dec->rm);
            }
            if (dec->kind == MM_OP_LSL_IMM || dec->kind == MM_OP_LSR_IMM || dec->kind == MM_OP_ASR_IMM ||
                dec->kind == MM_OP_ROR_IMM) {
                long imm_val = (long)dec->imm;
                if ((dec->kind == MM_OP_LSR_IMM || dec->kind == MM_OP_ASR_IMM) && (dec->imm & 0x1fu) == 0u) {
                    imm_val = 32;
                }
                if (!check_reg_operand(&arm->operands[1], 1, dec->rm)) {
                    return MM_FALSE;
                }
                return check_imm_operand(&arm->operands[2], 2, imm_val);
            }
            if (!check_reg_operand(&arm->operands[1], 1, dec->rn)) {
                return MM_FALSE;
            }
            return check_reg_operand(&arm->operands[2], 2, dec->rm);
        case ARM_INS_LDR:
        case ARM_INS_STR:
        case ARM_INS_LDRB:
        case ARM_INS_STRB:
        case ARM_INS_LDRH:
        case ARM_INS_STRH:
        case ARM_INS_LDRSB:
        case ARM_INS_LDRSH:
            if (arm->op_count < 2u) {
                return MM_FALSE;
            }
            if (!check_reg_operand(&arm->operands[0], 0, dec->rd)) {
                return MM_FALSE;
            }
            op = &arm->operands[1];
            if (op->type == ARM_OP_MEM) {
                mm_bool expect_wb = MM_FALSE;
                mm_bool expect_post = MM_FALSE;
                long imm_expected = signed_imm32(dec->imm);
                if (dec->kind == MM_OP_LDR_POST_IMM || dec->kind == MM_OP_STR_POST_IMM ||
                    dec->kind == MM_OP_LDRB_POST_IMM || dec->kind == MM_OP_STRB_POST_IMM ||
                    dec->kind == MM_OP_LDRH_POST_IMM || dec->kind == MM_OP_STRH_POST_IMM ||
                    dec->kind == MM_OP_LDRSB_POST_IMM) {
                    expect_wb = MM_TRUE;
                    expect_post = MM_TRUE;
                } else if (dec->kind == MM_OP_LDR_PRE_IMM || dec->kind == MM_OP_STR_PRE_IMM ||
                           dec->kind == MM_OP_LDRB_PRE_IMM || dec->kind == MM_OP_STRB_PRE_IMM ||
                           dec->kind == MM_OP_LDRH_PRE_IMM || dec->kind == MM_OP_STRH_PRE_IMM) {
                    expect_wb = MM_TRUE;
                    expect_post = MM_FALSE;
                }
                if (expect_wb) {
                    if (!arm->writeback) {
                        log_mem_mismatch("writeback/post-index mismatch");
                        return MM_FALSE;
                    }
#if M33MU_CAPSTONE_HAS_POST_INDEX
                    if (arm->post_index != expect_post) {
                        log_mem_mismatch("writeback/post-index mismatch");
                        return MM_FALSE;
                    }
#else
                    (void)expect_post;
#endif
                }
                if (expect_wb && expect_post && arm->op_count >= 3u && arm->operands[2].type == ARM_OP_IMM) {
                    int base = arm_reg_from_mm(dec->rn);
                    int index = ARM_REG_INVALID;
                    if (mem_operand_matches(op, base, index, 0) &&
                        check_imm_operand(&arm->operands[2], 2, imm_expected)) {
                        return MM_TRUE;
                    }
                    if (mem_operand_matches(op, base, index, imm_expected) &&
                        check_imm_operand(&arm->operands[2], 2, 0)) {
                        return MM_TRUE;
                    }
                    log_mem_mismatch("post-index offset mismatch");
                    return MM_FALSE;
                }
                if (dec->kind == MM_OP_LDR_REG || dec->kind == MM_OP_STR_REG ||
                    dec->kind == MM_OP_LDRB_REG || dec->kind == MM_OP_STRB_REG ||
                    dec->kind == MM_OP_LDRH_REG || dec->kind == MM_OP_STRH_REG ||
                    dec->kind == MM_OP_LDRSB_REG || dec->kind == MM_OP_LDRSH_REG) {
                    return check_mem_operand(op, dec->rn, dec->rm, 0);
                }
                if (dec->kind == MM_OP_LDRH_POST_IMM) {
                    int base = arm_reg_from_mm(dec->rn);
                    if (mem_operand_matches(op, base, ARM_REG_INVALID, 0)) {
                        return MM_TRUE;
                    }
                    return check_mem_operand(op, dec->rn, 0xffu, signed_imm32(dec->imm));
                }
                if (dec->kind == MM_OP_STRH_POST_IMM) {
                    int base = arm_reg_from_mm(dec->rn);
                    if (mem_operand_matches(op, base, ARM_REG_INVALID, 0)) {
                        return MM_TRUE;
                    }
                    return check_mem_operand(op, dec->rn, 0xffu, signed_imm32(dec->imm));
                }
                if (dec->kind == MM_OP_LDR_LITERAL) {
                    return check_mem_operand(op, 15u, 0xffu, signed_imm32(dec->imm));
                }
                return check_mem_operand(op, dec->rn, 0xffu, imm_expected);
            }
            return MM_FALSE;
        case ARM_INS_LDRD:
        case ARM_INS_STRD:
            if (arm->op_count < 3u) {
                return MM_FALSE;
            }
            if (!check_reg_operand(&arm->operands[0], 0, dec->rd)) {
                return MM_FALSE;
            }
            if (!check_reg_operand(&arm->operands[1], 1, dec->rm)) {
                return MM_FALSE;
            }
            op = &arm->operands[2];
            if (op->type != ARM_OP_MEM) {
                return MM_FALSE;
            }
            {
                mm_u32 imm = dec->imm & 0x3ffu;
                mm_bool u = (dec->imm & 0x80000000u) != 0u;
                long disp = u ? (long)imm : -(long)imm;
                {
                    int base = arm_reg_from_mm(dec->rn);
                    if ((int)op->mem.base != base) {
                        return MM_FALSE;
                    }
                }
                if (op->mem.disp != disp && op->mem.disp != 0) {
                    return MM_FALSE;
                }
            }
            return MM_TRUE;
        case ARM_INS_B:
        case ARM_INS_BL:
            if (arm->op_count < 1u) {
                return MM_FALSE;
            }
            op = &arm->operands[0];
            if (op->type == ARM_OP_IMM) {
                long target = (long)fetch->pc_fetch + 4 + signed_imm32(dec->imm);
                return check_imm_operand(op, 0, target);
            }
            return MM_TRUE;
        case ARM_INS_BX:
        case ARM_INS_BLX:
            if (arm->op_count < 1u) {
                return MM_FALSE;
            }
            return check_reg_operand(&arm->operands[0], 0, dec->rm);
        case ARM_INS_CBZ:
        case ARM_INS_CBNZ:
            if (arm->op_count < 2u) {
                return MM_FALSE;
            }
            if (!check_reg_operand(&arm->operands[0], 0, dec->rn)) {
                return MM_FALSE;
            }
            op = &arm->operands[1];
            if (op->type == ARM_OP_IMM) {
                long target2 = (long)fetch->pc_fetch + 4 + signed_imm32(dec->imm);
                return check_imm_operand(op, 1, target2);
            }
            return MM_FALSE;
        case ARM_INS_UBFX:
        case ARM_INS_SBFX:
            if (arm->op_count < 4u) {
                return MM_FALSE;
            }
            if (!check_reg_operand(&arm->operands[0], 0, dec->rd)) {
                return MM_FALSE;
            }
            if (!check_reg_operand(&arm->operands[1], 1, dec->rn)) {
                return MM_FALSE;
            }
            if (insn->id == ARM_INS_SBFX) {
                return MM_TRUE;
            }
            if (!check_imm_operand(&arm->operands[2], 2, (long)(dec->imm & 0xffu))) {
                return MM_FALSE;
            }
            return check_imm_operand(&arm->operands[3], 3, (long)(((dec->imm >> 8) & 0x1fu) + 1u));
        case ARM_INS_SMLABB:
        case ARM_INS_SMLABT:
        case ARM_INS_SMLATB:
        case ARM_INS_SMLATT:
            {
                mm_u8 expected_xy = 0u;
                switch (insn->id) {
                    case ARM_INS_SMLABT: expected_xy = 1u; break;
                    case ARM_INS_SMLATB: expected_xy = 2u; break;
                    case ARM_INS_SMLATT: expected_xy = 3u; break;
                    default: expected_xy = 0u; break;
                }
                if ((dec->imm & 0x3u) != expected_xy) {
                    return MM_FALSE;
                }
            }
            if (arm->op_count < 4u) {
                return MM_FALSE;
            }
            if (!check_reg_operand(&arm->operands[0], 0, dec->rd)) {
                return MM_FALSE;
            }
            if (!check_reg_operand(&arm->operands[1], 1, dec->rn)) {
                return MM_FALSE;
            }
            if (!check_reg_operand(&arm->operands[2], 2, dec->rm)) {
                return MM_FALSE;
            }
            return check_reg_operand(&arm->operands[3], 3, dec->ra);
        default:
            return MM_TRUE;
    }
}

mm_bool capstone_cross_check(const struct mm_fetch_result *fetch, const struct mm_decoded *dec)
{
    uint8_t code_buf[4];
    const uint8_t *code;
    size_t size;
    uint64_t address;
    cs_insn *insn = 0;
    size_t count;
    mm_bool ok = MM_TRUE;

    if (!g_capstone.ready || !g_capstone.enabled || fetch == 0 || dec == 0 || fetch->len == 0u) {
        return MM_TRUE;
    }

    if (fetch->len == 2u) {
        mm_u16 hw1 = (mm_u16)(fetch->insn & 0xffffu);
        code_buf[0] = (uint8_t)(hw1 & 0xffu);
        code_buf[1] = (uint8_t)((hw1 >> 8) & 0xffu);
    } else {
        mm_u16 hw1 = (mm_u16)((fetch->insn >> 16) & 0xffffu);
        mm_u16 hw2 = (mm_u16)(fetch->insn & 0xffffu);
        code_buf[0] = (uint8_t)(hw1 & 0xffu);
        code_buf[1] = (uint8_t)((hw1 >> 8) & 0xffu);
        code_buf[2] = (uint8_t)(hw2 & 0xffu);
        code_buf[3] = (uint8_t)((hw2 >> 8) & 0xffu);
    }
    code = code_buf;
    size = (size_t)fetch->len;
    address = (uint64_t)fetch->pc_fetch;
    count = cs_disasm(g_capstone.handle, code, size, address, 1, &insn);
    if (count == 0 || insn == 0) {
        const char *kind_name = "unknown";
        mm_bool allow = MM_FALSE;
        switch (dec->kind) {
            case MM_OP_SG: kind_name = "SG"; allow = MM_TRUE; break;
            case MM_OP_BXNS: kind_name = "BXNS"; allow = MM_TRUE; break;
            case MM_OP_BLXNS: kind_name = "BLXNS"; allow = MM_TRUE; break;
            case MM_OP_TT: kind_name = "TT"; allow = MM_TRUE; break;
            case MM_OP_TTT: kind_name = "TTT"; allow = MM_TRUE; break;
            case MM_OP_TTA: kind_name = "TTA"; allow = MM_TRUE; break;
            case MM_OP_TTAT: kind_name = "TTAT"; allow = MM_TRUE; break;
            case MM_OP_MSR: kind_name = "MSR"; allow = MM_TRUE; break;
            case MM_OP_MRS: kind_name = "MRS"; allow = MM_TRUE; break;
            case MM_OP_CPS: kind_name = "CPS"; allow = MM_TRUE; break;
            case MM_OP_CLREX: kind_name = "CLREX"; allow = MM_TRUE; break;
            default: break;
        }
        if (allow) {
            (void)kind_name;
            /* Skip ARMv8-M instruction not supported by capstone */
            return MM_TRUE;
        }
        printf("[CAPSTONE] decode failed PC=0x%08lx len=%u raw=0x%08lx kind=%u\n",
               (unsigned long)(fetch->pc_fetch | 1u),
               (unsigned)fetch->len,
               (unsigned long)fetch->insn,
               (unsigned)dec->kind);
        return MM_FALSE;
    }

    if (!cross_check_kind(insn, dec)) {
        printf("[CAPSTONE] kind mismatch PC=0x%08lx raw=0x%08lx capstone=%s/%s mm_kind=%u\n",
               (unsigned long)(fetch->pc_fetch | 1u),
               (unsigned long)fetch->insn,
               insn[0].mnemonic,
               insn[0].op_str,
               (unsigned)dec->kind);
        ok = MM_FALSE;
    } else if (!cross_check_operands(fetch, insn, dec)) {
        printf("[CAPSTONE] operand mismatch PC=0x%08lx raw=0x%08lx capstone=%s/%s mm_kind=%u rd=%u rn=%u rm=%u imm=0x%08lx\n",
               (unsigned long)(fetch->pc_fetch | 1u),
               (unsigned long)fetch->insn,
               insn[0].mnemonic,
               insn[0].op_str,
               (unsigned)dec->kind,
               (unsigned)dec->rd,
               (unsigned)dec->rn,
               (unsigned)dec->rm,
               (unsigned long)dec->imm);
        ok = MM_FALSE;
    }

    cs_free(insn, count);
    return ok;
}

mm_bool capstone_it_check_pre(const struct mm_fetch_result *fetch, const struct mm_decoded *dec,
                              mm_u8 it_pattern, mm_u8 it_remaining, mm_u8 it_cond)
{
    uint8_t code_buf[4];
    const uint8_t *code;
    size_t size;
    uint64_t address;
    cs_insn *insn = 0;
    size_t count;
    mm_bool ok = MM_TRUE;

    if (!g_capstone.ready || !g_capstone.enabled || fetch == 0 || dec == 0 || fetch->len == 0u) {
        return MM_TRUE;
    }

    if (fetch->len == 2u) {
        mm_u16 hw1 = (mm_u16)(fetch->insn & 0xffffu);
        code_buf[0] = (uint8_t)(hw1 & 0xffu);
        code_buf[1] = (uint8_t)((hw1 >> 8) & 0xffu);
    } else {
        mm_u16 hw1 = (mm_u16)((fetch->insn >> 16) & 0xffffu);
        mm_u16 hw2 = (mm_u16)(fetch->insn & 0xffffu);
        code_buf[0] = (uint8_t)(hw1 & 0xffu);
        code_buf[1] = (uint8_t)((hw1 >> 8) & 0xffu);
        code_buf[2] = (uint8_t)(hw2 & 0xffu);
        code_buf[3] = (uint8_t)((hw2 >> 8) & 0xffu);
    }
    code = code_buf;
    size = (size_t)fetch->len;
    address = (uint64_t)fetch->pc_fetch;
    count = cs_disasm(g_capstone.handle, code, size, address, 1, &insn);
    if (count == 0 || insn == 0) {
        return MM_TRUE;
    }

    if (insn->id == ARM_INS_IT && fetch->len == 2u) {
        mm_u8 imm = (mm_u8)(fetch->insn & 0xffu);
        mm_u8 cond = (mm_u8)((imm >> 4) & 0x0fu);
        mm_u8 mask = (mm_u8)(imm & 0x0fu);
        if (dec->imm != (mm_u32)imm) {
            printf("[CAPSTONE] IT imm mismatch PC=0x%08lx raw=0x%08lx imm=0x%02x dec=0x%02lx\n",
                   (unsigned long)(fetch->pc_fetch | 1u),
                   (unsigned long)fetch->insn,
                   (unsigned)imm,
                   (unsigned long)dec->imm);
            ok = MM_FALSE;
        }
        if (dec->kind != MM_OP_IT) {
            printf("[CAPSTONE] IT kind mismatch PC=0x%08lx raw=0x%08lx capstone=%s/%s mm_kind=%u\n",
                   (unsigned long)(fetch->pc_fetch | 1u),
                   (unsigned long)fetch->insn,
                   insn[0].mnemonic,
                   insn[0].op_str,
                   (unsigned)dec->kind);
            ok = MM_FALSE;
        }
        (void)cond;
        (void)mask;
    } else if (it_remaining > 0u && dec->kind != MM_OP_IT) {
        const cs_arm *arm = &insn[0].detail->arm;
        arm_cc expected_cc = mm_cond_to_arm_cc(it_cond);
        if ((it_pattern & 0x1u) == 0u) {
            expected_cc = arm_cc_invert(expected_cc);
        }
        if (arm->cc != ARM_CC_INVALID && arm->cc != ARM_CC_AL && arm->cc != expected_cc) {
            printf("[CAPSTONE] IT cond mismatch PC=0x%08lx raw=0x%08lx capstone_cc=%d expected_cc=%d it_pat=0x%02x it_cond=0x%02x\n",
                   (unsigned long)(fetch->pc_fetch | 1u),
                   (unsigned long)fetch->insn,
                   (int)arm->cc,
                   (int)expected_cc,
                   (unsigned)it_pattern,
                   (unsigned)it_cond);
            ok = MM_FALSE;
        }
    }

    cs_free(insn, count);
    return ok;
}

mm_bool capstone_it_check_post(const struct mm_fetch_result *fetch, const struct mm_decoded *dec,
                               mm_u8 it_pattern, mm_u8 it_remaining, mm_u8 it_cond)
{
    mm_u8 expected_pattern = 0;
    mm_u8 expected_remaining = 0;
    mm_u8 cond;
    mm_u8 mask;

    if (!g_capstone.ready || !g_capstone.enabled || fetch == 0 || dec == 0) {
        return MM_TRUE;
    }
    if (dec->kind != MM_OP_IT || fetch->len != 2u) {
        return MM_TRUE;
    }

    cond = (mm_u8)((dec->imm >> 4) & 0x0fu);
    mask = (mm_u8)(dec->imm & 0x0fu);
    it_mask_to_pattern(cond, mask, &expected_pattern, &expected_remaining);

    if (it_cond != cond || it_remaining != expected_remaining || it_pattern != expected_pattern) {
        printf("[CAPSTONE] ITSTATE mismatch PC=0x%08lx raw=0x%08lx cond=%u mask=0x%01x exp_pat=0x%02x exp_rem=%u got_pat=0x%02x got_rem=%u got_cond=%u\n",
               (unsigned long)(fetch->pc_fetch | 1u),
               (unsigned long)fetch->insn,
               (unsigned)cond,
               (unsigned)mask,
               (unsigned)expected_pattern,
               (unsigned)expected_remaining,
               (unsigned)it_pattern,
               (unsigned)it_remaining,
               (unsigned)it_cond);
        return MM_FALSE;
    }

    return MM_TRUE;
}
