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
#include <stdlib.h>
#include <string.h>
#include "imxrt700/imxrt700_secure.h"
#include "imxrt700/imxrt700_mmio.h"
#include "imxrt700/cpu_config.h"
#include "m33mu/memmap.h"
#include "m33mu/mmio.h"
#include "m33mu/host_rng.h"

#ifdef M33MU_HAS_WOLFSSL
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/wolfcrypt/hmac.h>
#include <wolfssl/wolfcrypt/integer.h>
#endif

static mm_u32 rd32le(const mm_u8 *p)
{
    return (mm_u32)p[0] | ((mm_u32)p[1] << 8) | ((mm_u32)p[2] << 16) | ((mm_u32)p[3] << 24);
}

/* ------------------------------------------------------------------------ */
/* GLIKEY                                                                   */
/* ------------------------------------------------------------------------ */

#define GLIKEY_CTRL_0 0x0u
#define GLIKEY_CTRL_1 0x4u
#define GLIKEY_INTR_CTRL 0x8u
#define GLIKEY_STATUS 0xCu

#define CTRL0_WRITE_INDEX_MASK 0xFFu
#define CTRL0_WR_EN_0_SHIFT 16u
#define CTRL0_SFT_RST (1u << 18)
#define CTRL1_READ_INDEX_MASK 0xFFu
#define CTRL1_WR_EN_1_SHIFT 16u
#define CTRL1_SFR_LOCK_SHIFT 18u
#define SFR_UNLOCKED 0xAu

#define FSM_WR_DIS 0x0Bu
#define FSM_INIT 0x16u
#define FSM_STEP1 0x2Cu
#define FSM_STEP2 0x58u
#define FSM_STEP3 0xB0u
#define FSM_STEP4 0x160u
#define FSM_LOCKED 0xC01u
#define FSM_WR_EN 0x1802u

#define GLIKEY_COUNT 6u

struct glikey_state {
    struct imxrt700_dev *dev;
    mm_u32 fsm;
    mm_u32 write_index;
    mm_bool error;
    mm_u8 index_locked[32]; /* 256 write indexes */
};

static struct glikey_state glikeys[GLIKEY_COUNT];

static mm_bool glikey_index_locked(const struct glikey_state *g, mm_u32 idx)
{
    return (g->index_locked[(idx >> 3) & 31u] & (1u << (idx & 7u))) != 0u ? MM_TRUE : MM_FALSE;
}

static void glikey_reset_one(struct glikey_state *g)
{
    g->fsm = FSM_INIT;
    g->write_index = 0u;
    g->error = MM_FALSE;
}

/* The FSM advances on the (WR_EN_0, WR_EN_1) pair the SDK codewords
 * produce: (1,0) STEP1, (1,1) STEP2, (2,1) STEP3, (2,0) STEP4, (0,0) WR_EN.
 * (2,0) from WR_EN ends the operation, (3,x) from WR_EN locks the index. */
static void glikey_step(struct glikey_state *g, mm_u32 wr0, mm_u32 wr1, mm_u32 prev0, mm_u32 prev1)
{
    if (wr0 == prev0 && wr1 == prev1) {
        return;
    }
    if (g->fsm == FSM_WR_DIS) {
        return;
    }
    if (wr0 == 1u && wr1 == 0u) {
        mm_u32 idx = mm_imxrt700_reg(g->dev, GLIKEY_CTRL_0) & CTRL0_WRITE_INDEX_MASK;
        if (glikey_index_locked(g, idx)) {
            g->fsm = FSM_WR_DIS;
            g->error = MM_TRUE;
            return;
        }
        g->write_index = idx;
        g->fsm = FSM_STEP1;
        return;
    }
    switch (g->fsm) {
    case FSM_STEP1:
        if (wr0 == 1u && wr1 == 1u) { g->fsm = FSM_STEP2; return; }
        break;
    case FSM_STEP2:
        if (wr0 == 2u && wr1 == 1u) { g->fsm = FSM_STEP3; return; }
        break;
    case FSM_STEP3:
        if (wr0 == 2u && wr1 == 0u) { g->fsm = FSM_STEP4; return; }
        break;
    case FSM_STEP4:
        if (wr0 == 0u && wr1 == 0u) { g->fsm = FSM_WR_EN; return; }
        break;
    case FSM_WR_EN:
        if (wr0 == 3u) {
            g->index_locked[(g->write_index >> 3) & 31u] |= (mm_u8)(1u << (g->write_index & 7u));
            g->fsm = FSM_LOCKED;
            return;
        }
        if (wr0 == 2u) { g->fsm = FSM_INIT; return; }
        break;
    case FSM_LOCKED:
    case FSM_INIT:
        if (wr0 == 2u) { g->fsm = FSM_INIT; return; }
        break;
    default:
        break;
    }
    g->fsm = FSM_WR_DIS;
    g->error = MM_TRUE;
}

static mm_bool glikey_read(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 *value_out)
{
    struct glikey_state *g = (struct glikey_state *)dev->opaque;
    if (off == GLIKEY_STATUS && size == 4u) {
        mm_u32 ridx = mm_imxrt700_reg(dev, GLIKEY_CTRL_1) & CTRL1_READ_INDEX_MASK;
        mm_u32 v = (g->fsm << 19) | (g->error ? (1u << 2) : 0u);
        if (glikey_index_locked(g, ridx)) {
            v |= 1u << 1;
        }
        v |= mm_imxrt700_reg(dev, GLIKEY_STATUS) & 1u;
        *value_out = v;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool glikey_write(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 value)
{
    struct glikey_state *g = (struct glikey_state *)dev->opaque;
    mm_u32 c0 = mm_imxrt700_reg(dev, GLIKEY_CTRL_0);
    mm_u32 c1 = mm_imxrt700_reg(dev, GLIKEY_CTRL_1);
    mm_u32 prev0 = (c0 >> CTRL0_WR_EN_0_SHIFT) & 3u;
    mm_u32 prev1 = (c1 >> CTRL1_WR_EN_1_SHIFT) & 3u;
    mm_bool sfr_locked = ((c1 >> CTRL1_SFR_LOCK_SHIFT) & 0xFu) != SFR_UNLOCKED ? MM_TRUE : MM_FALSE;

    if (size != 4u) {
        return MM_FALSE;
    }
    switch (off) {
    case GLIKEY_CTRL_0:
        if (sfr_locked) {
            return MM_TRUE;
        }
        if (value & CTRL0_SFT_RST) {
            glikey_reset_one(g);
            mm_imxrt700_reg_put(dev, GLIKEY_CTRL_0, 2u << CTRL0_WR_EN_0_SHIFT);
            mm_imxrt700_reg_put(dev, GLIKEY_CTRL_1, c1 & ~(3u << CTRL1_WR_EN_1_SHIFT));
            return MM_TRUE;
        }
        value &= CTRL0_WRITE_INDEX_MASK | (3u << CTRL0_WR_EN_0_SHIFT);
        mm_imxrt700_reg_put(dev, GLIKEY_CTRL_0, value);
        glikey_step(g, (value >> CTRL0_WR_EN_0_SHIFT) & 3u, prev1, prev0, prev1);
        return MM_TRUE;
    case GLIKEY_CTRL_1:
        if (sfr_locked) {
            return MM_TRUE;
        }
        value &= CTRL1_READ_INDEX_MASK | (3u << CTRL1_WR_EN_1_SHIFT) | (0xFu << CTRL1_SFR_LOCK_SHIFT);
        mm_imxrt700_reg_put(dev, GLIKEY_CTRL_1, value);
        glikey_step(g, prev0, (value >> CTRL1_WR_EN_1_SHIFT) & 3u, prev0, prev1);
        return MM_TRUE;
    case GLIKEY_INTR_CTRL: {
        mm_u32 st = mm_imxrt700_reg(dev, GLIKEY_STATUS);
        if (value & 2u) st &= ~1u;
        if (value & 4u) st |= 1u;
        mm_imxrt700_reg_put(dev, GLIKEY_STATUS, st);
        mm_imxrt700_reg_put(dev, GLIKEY_INTR_CTRL, value & 1u);
        return MM_TRUE;
    }
    case GLIKEY_STATUS:
        return MM_TRUE;
    default:
        return MM_FALSE;
    }
}

mm_bool mm_imxrt700_glikey_write_enabled(mm_u32 glikey, mm_u32 index)
{
    if (glikey >= GLIKEY_COUNT) {
        return MM_FALSE;
    }
    return (glikeys[glikey].fsm == FSM_WR_EN && glikeys[glikey].write_index == index) ? MM_TRUE : MM_FALSE;
}

/* ------------------------------------------------------------------------ */
/* AHB secure controllers                                                   */
/* ------------------------------------------------------------------------ */

#define AHBSC_MISC_CTRL_DP 0xFF8u
#define AHBSC_MISC_CTRL 0xFFCu
#define AHBSC_RULES_END 0x580u /* memory and peripheral rule registers */
#define MISC_FIELD(v, shift) (((v) >> (shift)) & 3u)
#define MISC_WRITE_LOCK 0u
#define MISC_ENABLE_SECURE_CHECKING 2u
#define MISC_IDAU_ALL_NS 14u
#define FIELD_ENABLED 1u

/* GLIKEY0 index 1 unlocks the AHBSC0 MISC_CTRL registers (SDK BOARD_InitAHBSC). */
#define GLIKEY0_IDX_AHBSC0_MISC 1u

static struct imxrt700_dev *ahbsc0;
static mm_u32 active_core;

static mm_bool ahbsc_write(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 value)
{
    mm_u32 misc = mm_imxrt700_reg(dev, AHBSC_MISC_CTRL);
    mm_bool locked = MISC_FIELD(misc, MISC_WRITE_LOCK) == FIELD_ENABLED ? MM_TRUE : MM_FALSE;
    (void)size;
    (void)value;
    if (off == AHBSC_MISC_CTRL || off == AHBSC_MISC_CTRL_DP) {
        if (locked) {
            return MM_TRUE;
        }
        if (dev == ahbsc0 && !mm_imxrt700_glikey_write_enabled(0u, GLIKEY0_IDX_AHBSC0_MISC)) {
            return MM_TRUE;
        }
        return MM_FALSE;
    }
    if (off < AHBSC_RULES_END && locked) {
        return MM_TRUE;
    }
    return MM_FALSE;
}

/* SRAM partitions P0..P29 (KB) and their AHBSC0 SRAM_n_RULE offsets.  Each
 * partition has 4 rule registers x 8 rules = 32 equal sub-regions. */
static const mm_u16 sram_part_kb[30] = {
    32, 32, 32, 32, 64, 64, 128, 128, 256, 256, 512, 512, 1024, 1024, 512, 512, 256, 256,
    32, 32, 32, 32, 64, 64, 128, 128, 512, 512, 256, 256
};
static const mm_u16 sram_rule_off[30] = {
    0x110, 0x120, 0x130, 0x140, 0x160, 0x170, 0x190, 0x1A0, 0x1C0, 0x1D0,
    0x1F0, 0x200, 0x220, 0x230, 0x250, 0x260, 0x280, 0x290, 0x2B0, 0x2C0,
    0x2D0, 0x2E0, 0x2F0, 0x300, 0x310, 0x320, 0x340, 0x350, 0x360, 0x370
};

static mm_u32 sram_rule(mm_u32 part, mm_u32 sub)
{
    mm_u32 reg = mm_imxrt700_reg(ahbsc0, sram_rule_off[part] + (sub / 8u) * 4u);
    return (reg >> ((sub % 8u) * 4u)) & 3u;
}

mm_bool mm_imxrt700_mpcbb_block_secure(int bank, mm_u32 block_index)
{
    mm_u32 misc;
    mm_u32 start = block_index * IMXRT700_MPCBB_BLOCK_SIZE;
    mm_u32 end = start + IMXRT700_MPCBB_BLOCK_SIZE;
    mm_u32 base = 0;
    mm_u32 p;

    if (bank != 0 || ahbsc0 == 0 || active_core != 0u) {
        return MM_FALSE; /* only CPU0 (compute domain) rules are modelled */
    }
    misc = mm_imxrt700_reg(ahbsc0, AHBSC_MISC_CTRL);
    if (MISC_FIELD(misc, MISC_ENABLE_SECURE_CHECKING) != FIELD_ENABLED ||
        MISC_FIELD(misc, MISC_IDAU_ALL_NS) == FIELD_ENABLED) {
        return MM_FALSE;
    }
    for (p = 0; p < 30u; ++p) {
        mm_u32 size = (mm_u32)sram_part_kb[p] * 1024u;
        mm_u32 sub_size = size / 32u;
        if (start < base + size && end > base) {
            mm_u32 lo = (start > base ? start : base) - base;
            mm_u32 hi = (end < base + size ? end : base + size) - base;
            mm_u32 sub;
            for (sub = lo / sub_size; sub * sub_size < hi && sub < 32u; ++sub) {
                if (sram_rule(p, sub) >= 2u) {
                    return MM_TRUE;
                }
            }
        }
        base += size;
    }
    return MM_FALSE;
}

void mm_imxrt700_secure_set_active_core(mm_u32 core)
{
    active_core = core;
}

/* ------------------------------------------------------------------------ */
/* TRNG                                                                     */
/* ------------------------------------------------------------------------ */

#define TRNG_MCTL 0x00u
#define TRNG_ENT0 0x40u
#define TRNG_ENT_COUNT 16u
#define MCTL_RST_DEF (1u << 6)
#define MCTL_ENT_VAL (1u << 10)
#define MCTL_ERR (1u << 12)
#define MCTL_TSTOP_OK (1u << 13)
#define MCTL_PRGM (1u << 16)

static mm_u32 trng_ent[TRNG_ENT_COUNT];

static void trng_refill(void)
{
    mm_u32 i;
    for (i = 0; i < TRNG_ENT_COUNT; ++i) {
        trng_ent[i] = mm_host_rng_u32();
    }
}

static mm_bool trng_read(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 *value_out)
{
    if (size != 4u) {
        return MM_FALSE;
    }
    if (off == TRNG_MCTL) {
        mm_u32 v = mm_imxrt700_reg(dev, TRNG_MCTL) & ~(MCTL_ENT_VAL | MCTL_TSTOP_OK | MCTL_ERR | MCTL_RST_DEF);
        v |= (v & MCTL_PRGM) ? MCTL_TSTOP_OK : MCTL_ENT_VAL;
        *value_out = v;
        return MM_TRUE;
    }
    if (off >= TRNG_ENT0 && off < TRNG_ENT0 + 4u * TRNG_ENT_COUNT) {
        mm_u32 idx = (off - TRNG_ENT0) / 4u;
        if (mm_imxrt700_reg(dev, TRNG_MCTL) & MCTL_PRGM) {
            *value_out = 0u;
            return MM_TRUE;
        }
        *value_out = trng_ent[idx];
        if (idx == TRNG_ENT_COUNT - 1u && !mmio_peek_mode()) {
            trng_refill(); /* reading the last word starts a new generation */
        }
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool trng_write(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 value)
{
    if (off == TRNG_MCTL && size == 4u) {
        mm_imxrt700_reg_put(dev, TRNG_MCTL, value & ~(MCTL_ERR | MCTL_RST_DEF | MCTL_ENT_VAL | MCTL_TSTOP_OK));
        return MM_TRUE;
    }
    if (off >= TRNG_ENT0 && off < TRNG_ENT0 + 4u * TRNG_ENT_COUNT) {
        return MM_TRUE;
    }
    return MM_FALSE;
}

/* ------------------------------------------------------------------------ */
/* OCOTP                                                                    */
/* ------------------------------------------------------------------------ */

#define OCOTP_FUSE_WORDS 512u
#define OCOTP_SHADOW_END 0x800u
#define OCOTP_CTRL 0x800u
#define OCOTP_CTRL_SET 0x804u
#define OCOTP_WRITE_DATA 0x808u
#define OCOTP_READ_CTRL 0x80Cu
#define OCOTP_READ_DATA 0x810u
#define OCOTP_CTRL_ADDR_MASK 0x1FFu
#define OCOTP_CTRL_RELOAD_SHADOWS (1u << 11)
#define OCOTP_WR_UNLOCK_KEY 0x3E77u

/* Fuses keep their value across emulated resets within one run. */
static mm_u32 fuses[OCOTP_FUSE_WORDS];
static mm_bool fuses_init;

static void fuses_seed(void)
{
    if (fuses_init) {
        return;
    }
    memset(fuses, 0, sizeof(fuses));
    /* Unique ID words, stable across runs. */
    fuses[0x10] = 0x52543730u; /* "RT70" */
    fuses[0x11] = 0x00000798u;
    fuses_init = MM_TRUE;
}

static mm_bool ocotp_read(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 *value_out)
{
    (void)dev;
    if (size != 4u) {
        return MM_FALSE;
    }
    if (off < OCOTP_SHADOW_END) {
        *value_out = fuses[off / 4u];
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool ocotp_write(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 value)
{
    mm_u32 ctrl;
    if (size != 4u) {
        return MM_FALSE;
    }
    if (off < OCOTP_SHADOW_END) {
        return MM_TRUE; /* shadows are loaded from the fuses */
    }
    switch (off) {
    case OCOTP_READ_CTRL:
        if (value & 1u) {
            ctrl = mm_imxrt700_reg(dev, OCOTP_CTRL);
            mm_imxrt700_reg_put(dev, OCOTP_READ_DATA, fuses[ctrl & OCOTP_CTRL_ADDR_MASK]);
        }
        mm_imxrt700_reg_put(dev, OCOTP_READ_CTRL, value & ~1u);
        return MM_TRUE;
    case OCOTP_WRITE_DATA:
        ctrl = mm_imxrt700_reg(dev, OCOTP_CTRL);
        if ((ctrl >> 16) == OCOTP_WR_UNLOCK_KEY) {
            fuses[ctrl & OCOTP_CTRL_ADDR_MASK] |= value; /* fuses only blow 0 -> 1 */
        }
        mm_imxrt700_reg_put(dev, OCOTP_CTRL, ctrl & 0xFFFFu);
        return MM_TRUE;
    case OCOTP_CTRL:
    case OCOTP_CTRL_SET:
        if (!mm_imxrt700_regfile_write(dev, off, size, value)) {
            return MM_FALSE;
        }
        /* RELOAD_SHADOWS completes immediately. */
        mm_imxrt700_reg_put(dev, OCOTP_CTRL, mm_imxrt700_reg(dev, OCOTP_CTRL) & ~OCOTP_CTRL_RELOAD_SHADOWS);
        return MM_TRUE;
    default:
        return MM_FALSE;
    }
}

mm_bool mm_imxrt700_otp_read(mm_u32 index, mm_u32 *value_out)
{
    if (index >= OCOTP_FUSE_WORDS || value_out == 0) {
        return MM_FALSE;
    }
    *value_out = fuses[index];
    return MM_TRUE;
}

mm_bool mm_imxrt700_otp_program(mm_u32 index, mm_u32 value)
{
    if (index >= OCOTP_FUSE_WORDS) {
        return MM_FALSE;
    }
    fuses[index] |= value;
    return MM_TRUE;
}

/* ------------------------------------------------------------------------ */
/* ELS / PKC / PUF synthetic command interface                              */
/* ------------------------------------------------------------------------ */

#define SEC_CMD 0x000u
#define SEC_STATUS 0x004u
#define SEC_ARG0 0x008u
#define SEC_RESULT0 0x020u
#define SEC_KEYIN0 0x080u
#define SEC_DATA0 0x100u

#define ST_BUSY (1u << 0)
#define ST_DONE (1u << 1)
#define ST_ERROR (1u << 2)
#define ST_SECURE (1u << 3)

#define ELS_SLOTS 8u
#define SEC_MAX_BUF 0x10000u

enum sec_block { BLK_ELS = 0, BLK_PKC = 1, BLK_PUF = 2 };

struct sec_regs {
    enum sec_block blk;
    mm_u32 cmd;
    mm_u32 status;
    mm_u32 arg[4];
    mm_u32 result[4];
    mm_u32 keyin[4];
    mm_u32 data[4];
};

struct sec_slot {
    mm_bool valid;
    mm_u8 key[32];
};

static struct sec_regs sec[3];
static struct sec_slot slots[ELS_SLOTS];
static mm_u8 device_key[32];
static mm_u8 puf_secret[32];
static mm_bool puf_enrolled;

static mm_bool mem_put(mm_u32 addr, const mm_u8 *buf, mm_u32 len)
{
    struct mm_memmap *map = mm_imxrt700_memmap();
    mm_u32 i;
    if (map == 0 || len > SEC_MAX_BUF) {
        return MM_FALSE;
    }
    for (i = 0; i < len; ++i) {
        if (!mm_memmap_write(map, MM_SECURE, addr + i, 1u, buf[i])) {
            return MM_FALSE;
        }
    }
    return MM_TRUE;
}

static void rng_fill(mm_u8 *out, mm_u32 len)
{
    mm_u32 i;
    for (i = 0; i < len; i += 4u) {
        mm_u32 w = mm_host_rng_u32();
        mm_u32 n = (len - i < 4u) ? len - i : 4u;
        memcpy(out + i, &w, n);
    }
}

static void regs_put32(struct sec_regs *r, const mm_u8 v[32])
{
    mm_u32 i;
    for (i = 0; i < 4u; ++i) {
        r->result[i] = rd32le(v + 4u * i);
        r->data[i] = rd32le(v + 16u + 4u * i);
    }
}

#ifdef M33MU_HAS_WOLFSSL
static void wr32le(mm_u8 *p, mm_u32 v)
{
    p[0] = (mm_u8)v;
    p[1] = (mm_u8)(v >> 8);
    p[2] = (mm_u8)(v >> 16);
    p[3] = (mm_u8)(v >> 24);
}

static mm_bool mem_get(mm_u32 addr, mm_u8 *buf, mm_u32 len)
{
    struct mm_memmap *map = mm_imxrt700_memmap();
    mm_u32 i;
    if (map == 0 || len > SEC_MAX_BUF) {
        return MM_FALSE;
    }
    for (i = 0; i < len; ++i) {
        if (!mm_memmap_read8(map, MM_SECURE, addr + i, &buf[i])) {
            return MM_FALSE;
        }
    }
    return MM_TRUE;
}

static void keyin_bytes(const struct sec_regs *r, mm_u8 out[16])
{
    mm_u32 i;
    for (i = 0; i < 4u; ++i) {
        wr32le(out + 4u * i, r->keyin[i]);
    }
}

static mm_bool slot_ok(mm_u32 slot)
{
    return slot < ELS_SLOTS ? MM_TRUE : MM_FALSE;
}

static mm_bool hmac256(const mm_u8 *key, mm_u32 key_len, const mm_u8 *in, mm_u32 in_len, mm_u8 out[32])
{
    Hmac h;
    int rc;
    if (wc_HmacInit(&h, NULL, INVALID_DEVID) != 0) {
        return MM_FALSE;
    }
    rc = wc_HmacSetKey(&h, WC_SHA256, key, key_len);
    if (rc == 0) rc = wc_HmacUpdate(&h, in, in_len);
    if (rc == 0) rc = wc_HmacFinal(&h, out);
    wc_HmacFree(&h);
    return rc == 0 ? MM_TRUE : MM_FALSE;
}

static mm_bool sha256(const mm_u8 *in, mm_u32 len, mm_u8 out[32])
{
    wc_Sha256 s;
    int rc = wc_InitSha256(&s);
    if (rc == 0) rc = wc_Sha256Update(&s, in, len);
    if (rc == 0) rc = wc_Sha256Final(&s, out);
    wc_Sha256Free(&s);
    return rc == 0 ? MM_TRUE : MM_FALSE;
}

static mm_bool aes_cbc(int enc, const mm_u8 key[32], const mm_u8 iv[16], mm_u8 *buf, mm_u32 len)
{
    Aes aes;
    int rc;
    if (wc_AesInit(&aes, NULL, INVALID_DEVID) != 0) {
        return MM_FALSE;
    }
    rc = wc_AesSetKey(&aes, key, 32, iv, enc ? AES_ENCRYPTION : AES_DECRYPTION);
    if (rc == 0) {
        rc = enc ? wc_AesCbcEncrypt(&aes, buf, buf, len) : wc_AesCbcDecrypt(&aes, buf, buf, len);
    }
    wc_AesFree(&aes);
    return rc == 0 ? MM_TRUE : MM_FALSE;
}

static mm_bool modop(mm_u32 op, const mm_u8 *a, const mm_u8 *b, const mm_u8 *m, mm_u32 len, mm_u8 *out)
{
    mp_int A, B, M, Z;
    int rc = -1;
    int n;
    if (mp_init_multi(&A, &B, &M, &Z, NULL, NULL) != 0) {
        return MM_FALSE;
    }
    if (mp_read_unsigned_bin(&A, a, (int)len) == 0 &&
        mp_read_unsigned_bin(&B, b, (int)len) == 0 &&
        mp_read_unsigned_bin(&M, m, (int)len) == 0) {
        rc = (op == 1u) ? mp_exptmod(&A, &B, &M, &Z) : mp_mulmod(&A, &B, &M, &Z);
    }
    if (rc == 0) {
        n = mp_unsigned_bin_size(&Z);
        if (n < 0 || (mm_u32)n > len) {
            rc = -1;
        } else {
            memset(out, 0, len);
            rc = mp_to_unsigned_bin(&Z, out + (len - (mm_u32)n));
        }
    }
    mp_clear(&A);
    mp_clear(&B);
    mp_clear(&M);
    mp_clear(&Z);
    return rc == 0 ? MM_TRUE : MM_FALSE;
}

static mm_bool els_mac(const struct sec_regs *r, mm_u8 mac[32])
{
    static mm_u8 buf[SEC_MAX_BUF];
    mm_u32 slot = r->arg[0];
    if (!slot_ok(slot) || !slots[slot].valid || !mem_get(r->arg[1], buf, r->arg[2])) {
        return MM_FALSE;
    }
    return hmac256(slots[slot].key, 32u, buf, r->arg[2], mac);
}

static mm_bool els_exec(struct sec_regs *r)
{
    static mm_u8 buf[SEC_MAX_BUF];
    mm_u8 v[32];
    mm_u8 ctx[16];
    mm_u32 slot = r->arg[0];

    switch (r->cmd) {
    case 1u: /* GENERATE */
        if (!slot_ok(slot)) return MM_FALSE;
        rng_fill(slots[slot].key, 32u);
        slots[slot].valid = MM_TRUE;
        return MM_TRUE;
    case 2u: /* DERIVE */
        if (!slot_ok(slot)) return MM_FALSE;
        keyin_bytes(r, ctx);
        if (!hmac256(device_key, 32u, ctx, 16u, slots[slot].key)) return MM_FALSE;
        slots[slot].valid = MM_TRUE;
        return MM_TRUE;
    case 3u: /* IMPORT */
        if (!slot_ok(slot)) return MM_FALSE;
        keyin_bytes(r, slots[slot].key);
        wr32le(slots[slot].key + 16u, r->data[0]);
        wr32le(slots[slot].key + 20u, r->data[1]);
        wr32le(slots[slot].key + 24u, r->data[2]);
        wr32le(slots[slot].key + 28u, r->data[3]);
        slots[slot].valid = MM_TRUE;
        return MM_TRUE;
    case 4u: /* SIGN */
        if (!els_mac(r, v)) return MM_FALSE;
        regs_put32(r, v);
        return MM_TRUE;
    case 5u: { /* VERIFY */
        mm_u8 expect[32];
        if (!els_mac(r, v) || !mem_get(r->arg[3], expect, 32u)) return MM_FALSE;
        r->result[0] = (memcmp(v, expect, 32u) == 0) ? 1u : 0u;
        return r->result[0] ? MM_TRUE : MM_FALSE;
    }
    case 8u: /* SHA256 */
        if (!mem_get(r->arg[1], buf, r->arg[2]) || !sha256(buf, r->arg[2], v)) return MM_FALSE;
        regs_put32(r, v);
        if (r->arg[3] != 0u && !mem_put(r->arg[3], v, 32u)) return MM_FALSE;
        return MM_TRUE;
    case 9u:  /* AES_ENC */
    case 10u: /* AES_DEC */
        if (!slot_ok(slot) || !slots[slot].valid || (r->arg[2] & 15u) != 0u) return MM_FALSE;
        keyin_bytes(r, ctx);
        if (!mem_get(r->arg[1], buf, r->arg[2])) return MM_FALSE;
        if (!aes_cbc(r->cmd == 9u, slots[slot].key, ctx, buf, r->arg[2])) return MM_FALSE;
        return mem_put(r->arg[3], buf, r->arg[2]);
    default:
        return MM_FALSE;
    }
}

static mm_bool pkc_exec(struct sec_regs *r)
{
    static mm_u8 a[512], b[512], m[512], z[512];
    mm_u32 len = r->arg[3];
    if (len == 0u || len > sizeof(a) || (r->cmd != 1u && r->cmd != 2u)) {
        return MM_FALSE;
    }
    if (!mem_get(r->arg[0], a, len) || !mem_get(r->arg[1], b, len) || !mem_get(r->arg[2], m, len)) {
        return MM_FALSE;
    }
    if (!modop(r->cmd, a, b, m, len, z)) {
        return MM_FALSE;
    }
    return mem_put(r->data[0], z, len);
}

static mm_bool puf_exec(struct sec_regs *r)
{
    mm_u8 ctx[16];
    mm_u8 fp[32];
    mm_u32 slot = r->arg[0];
    if (r->cmd == 1u) {
        puf_enrolled = MM_TRUE;
        r->result[0] = 1u;
        return MM_TRUE;
    }
    if (r->cmd != 2u || !puf_enrolled || !slot_ok(slot)) {
        return MM_FALSE;
    }
    keyin_bytes(r, ctx);
    if (!hmac256(puf_secret, 32u, ctx, 16u, slots[slot].key) || !sha256(slots[slot].key, 32u, fp)) {
        return MM_FALSE;
    }
    slots[slot].valid = MM_TRUE;
    r->result[0] = 0xCAFE0000u | slot;
    r->result[1] = rd32le(fp);
    return MM_TRUE;
}
#endif /* M33MU_HAS_WOLFSSL */

static void sec_exec(struct sec_regs *r)
{
    enum sec_block blk = r->blk;
    mm_bool ok = MM_FALSE;
    if (blk == BLK_ELS && r->cmd == 7u) { /* RNG needs no crypto library */
        mm_u8 v[32];
        rng_fill(v, sizeof(v));
        regs_put32(r, v);
        ok = MM_TRUE;
        if (r->arg[1] != 0u) {
            static mm_u8 buf[SEC_MAX_BUF];
            ok = (r->arg[2] <= SEC_MAX_BUF) ? MM_TRUE : MM_FALSE;
            if (ok) {
                rng_fill(buf, r->arg[2]);
                ok = mem_put(r->arg[1], buf, r->arg[2]);
            }
        }
    } else {
#ifdef M33MU_HAS_WOLFSSL
        switch (blk) {
        case BLK_ELS: ok = els_exec(r); break;
        case BLK_PKC: ok = pkc_exec(r); break;
        case BLK_PUF: ok = puf_exec(r); break;
        default: break;
        }
#else
        ok = MM_FALSE; /* crypto engines need wolfSSL */
#endif
    }
    r->status &= ~(ST_BUSY | ST_DONE | ST_ERROR);
    r->status |= ok ? ST_DONE : ST_ERROR;
}

static mm_u32 *sec_word(struct sec_regs *r, mm_u32 off)
{
    if (off == SEC_CMD) return &r->cmd;
    if (off == SEC_STATUS) return &r->status;
    if (off >= SEC_ARG0 && off < SEC_ARG0 + 16u) return &r->arg[(off - SEC_ARG0) / 4u];
    if (off >= SEC_RESULT0 && off < SEC_RESULT0 + 16u) return &r->result[(off - SEC_RESULT0) / 4u];
    if (off >= SEC_KEYIN0 && off < SEC_KEYIN0 + 16u) return &r->keyin[(off - SEC_KEYIN0) / 4u];
    if (off >= SEC_DATA0 && off < SEC_DATA0 + 16u) return &r->data[(off - SEC_DATA0) / 4u];
    return 0;
}

static mm_bool sec_read(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 *value_out)
{
    struct sec_regs *r = (struct sec_regs *)dev->opaque;
    mm_u32 *w;
    if (mmio_active_sec() != MM_SECURE) {
        *value_out = 0u; /* secure-only block: RAZ from the non-secure world */
        return MM_TRUE;
    }
    w = sec_word(r, off & ~3u);
    if (w == 0 || size != 4u) {
        return MM_FALSE;
    }
    *value_out = *w;
    return MM_TRUE;
}

static mm_bool sec_write(struct imxrt700_dev *dev, mm_u32 off, mm_u32 size, mm_u32 value)
{
    struct sec_regs *r = (struct sec_regs *)dev->opaque;
    mm_u32 *w;
    if (mmio_active_sec() != MM_SECURE) {
        return MM_TRUE; /* WI */
    }
    w = sec_word(r, off);
    if (w == 0 || size != 4u) {
        return MM_FALSE;
    }
    if (off == SEC_STATUS) {
        r->status &= ~(value & (ST_DONE | ST_ERROR));
        return MM_TRUE;
    }
    *w = value;
    if (off == SEC_CMD) {
        r->status |= ST_BUSY;
        sec_exec(r);
    }
    return MM_TRUE;
}

static void sec_identity_init(void)
{
    static const char default_uds[] = "m33mu-imxrt700-device-secret";
    const char *hex = getenv("M33MU_IMXRT700_UDS_HEX");
    mm_u32 i;
    memset(device_key, 0, sizeof(device_key));
    if (hex != 0 && strlen(hex) == 64u) {
        for (i = 0; i < 32u; ++i) {
            char byte[3];
            byte[0] = hex[2u * i];
            byte[1] = hex[2u * i + 1u];
            byte[2] = 0;
            device_key[i] = (mm_u8)strtoul(byte, 0, 16);
        }
    } else {
        memcpy(device_key, default_uds, sizeof(default_uds) - 1u);
    }
    for (i = 0; i < 32u; ++i) {
        puf_secret[i] = (mm_u8)(device_key[i] ^ 0x5Cu ^ (mm_u8)(i * 37u));
    }
}

/* ------------------------------------------------------------------------ */

void mm_imxrt700_secure_attach(void)
{
    static const char *const ahbsc_names[] = { "AHBSC0", "AHBSC3", "AHBSC4" };
    mm_u32 i;
    char name[8];

    for (i = 0; i < GLIKEY_COUNT; ++i) {
        snprintf(name, sizeof(name), "GLIKEY%lu", (unsigned long)i);
        glikeys[i].dev = mm_imxrt700_dev(name);
        (void)mm_imxrt700_dev_hook(name, glikey_read, glikey_write, &glikeys[i]);
    }
    ahbsc0 = mm_imxrt700_dev("AHBSC0");
    for (i = 0; i < 3u; ++i) {
        (void)mm_imxrt700_dev_hook(ahbsc_names[i], 0, ahbsc_write, 0);
    }
    (void)mm_imxrt700_dev_hook("TRNG", trng_read, trng_write, 0);
    (void)mm_imxrt700_dev_hook("OCOTP", ocotp_read, ocotp_write, 0);
    (void)mm_imxrt700_dev_hook("ELS", sec_read, sec_write, &sec[BLK_ELS]);
    (void)mm_imxrt700_dev_hook("PKC", sec_read, sec_write, &sec[BLK_PKC]);
    (void)mm_imxrt700_dev_hook("PUF_CTRL", sec_read, sec_write, &sec[BLK_PUF]);
}

void mm_imxrt700_secure_reset(void)
{
    mm_u32 i;
    for (i = 0; i < GLIKEY_COUNT; ++i) {
        glikey_reset_one(&glikeys[i]);
        memset(glikeys[i].index_locked, 0, sizeof(glikeys[i].index_locked));
    }
    active_core = 0u;
    trng_refill();
    fuses_seed();
    memset(sec, 0, sizeof(sec));
    for (i = 0; i < 3u; ++i) {
        sec[i].blk = (enum sec_block)i;
        sec[i].status = ST_SECURE | ST_DONE;
    }
    memset(slots, 0, sizeof(slots));
    puf_enrolled = MM_FALSE;
    sec_identity_init();
}

void mm_imxrt700_secure_flash_bind(mm_u8 *flash, mm_u32 flash_size)
{
    (void)flash;
    (void)flash_size;
}
