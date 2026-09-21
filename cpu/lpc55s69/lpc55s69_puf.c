/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * LPC55S69 PUF (QuiddiKey) functional model.
 *
 * Register layout per CMSIS PUF_Type (LPC55S69, verified against the SVD):
 *   0x00  CTRL        command (write): ZEROIZE=1 ENROLL=2 START=4
 *                                   GENERATEKEY=8 SETKEY=0x10 GETKEY=0x40
 *   0x04  KEYINDEX    KEYIDX[3:0]
 *   0x08  KEYSIZE     KEYSIZE[5:0] (in 8-byte units)
 *   0x20  STAT        BUSY=1 SUCCESS=2 ERROR=4 KEYINREQ=0x10
 *                                   KEYOUTAVAIL=0x20 CODEINREQ=0x40
 *                                   CODEOUTAVAIL=0x80
 *   0x28  ALLOW       ENROLL=1 START=2 SETKEY=4 GETKEY=8
 *   0x40  KEYINPUT    key data in (write-only)
 *   0x44  CODEINPUT   AC/KC in (write-only)
 *   0x48  CODEOUTPUT  AC/KC out
 *   0x60  KEYOUTINDEX index of the key currently on KEYOUTPUT
 *   0x64  KEYOUTPUT   key data out
 *   0xDC  IFSTAT      APB error, write-1-to-clear
 *   0xFC  VERSION
 *   0x100 INTEN
 *   0x104 INTSTAT     write-1-to-clear
 *   0x108 PWRCTRL     RAMON=1 (RW), RAMSTAT=2 (RO)
 *   0x10C CFG         BLOCKENROLL_SETKEY=1 BLOCKKEYOUTPUT=2
 *   0x200 KEYLOCK     reset 0xAA
 *   0x204 KEYENABLE   reset 0x55
 *   0x208 KEYRESET    self-clearing
 *   0x20C IDXBLK_L    reset 0x8000AAAA
 *   0x210 IDXBLK_H_DP reset 0xAAAA
 *   0x214 KEYMASK[4]
 *   0x254 IDXBLK_H    reset 0x8000AAAA
 *   0x258 IDXBLK_L_DP reset 0xAAAA
 *   0x25C SHIFT_STATUS 4 bits per key slot (2*keyWords-1)
 *
 * Behavioural model:
 *  - A persistent 1192-byte "physical" SRAM pattern (the emulated
 *    silicon) is derived from a seed: M33MU_LPC55S69_PUF_SEED_HEX (64
 *    hex chars) or a fixed default. The fingerprint fp = SHA256(phys),
 *    the activation code AC = SHA256 counter stream over fp.
 *  - ENROLL streams the AC out on CODEOUTPUT (298 words).
 *  - START consumes the AC on CODEINPUT; a match activates the PUF,
 *    a mismatch puts it in the error state.
 *  - GENERATEKEY/SETKEY produce a key code KC = 20-byte header + key.
 *    Header: byte0 type, byte1 index, byte2 0, byte3 keyWords, bytes
 *    4..19 = SHA256(fp || index || key)[0..15] (the binding tag).
 *  - GETKEY consumes the KC, verifies the tag against the current
 *    fingerprint, and streams the key back on KEYOUTPUT (register keys,
 *    index 1..15) or updates SHIFT_STATUS (HW key, index 0). A bad tag
 *    puts the PUF in the error state.
 *  - ZEROIZE is permanent: error state, ALLOW = 0, survives reset.
 *
 * The NXP driver (fsl_puf.c) only uses 32-bit accesses; sub-word
 * accesses are rejected, matching the syscon model.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../mm_host_crypto.h"
#include "lpc55s69/lpc55s69_mmio.h"
#include "lpc55s69/lpc55s69_puf.h"
#include "m33mu/host_rng.h"
#include "m33mu/mmio.h"
#include "m33mu/types.h"

#define PUF_BASE_NS  0x4003B000u
#define PUF_BASE_S   0x5003B000u
#define PUF_SIZE     0x260u

/* SYSCON AHBCLKCTRL2 bit 23 = PUF clock/reset (kCLOCK_Puf) */
#define PUF_AHBCLK_OFFSET 0x208u
#define PUF_AHBCLK_BIT    23u

#define PUF_OFF_CTRL        0x00u
#define PUF_OFF_KEYINDEX    0x04u
#define PUF_OFF_KEYSIZE     0x08u
#define PUF_OFF_STAT        0x20u
#define PUF_OFF_ALLOW       0x28u
#define PUF_OFF_KEYINPUT    0x40u
#define PUF_OFF_CODEINPUT   0x44u
#define PUF_OFF_CODEOUTPUT  0x48u
#define PUF_OFF_KEYOUTINDEX 0x60u
#define PUF_OFF_KEYOUTPUT   0x64u
#define PUF_OFF_IFSTAT      0xDCu
#define PUF_OFF_VERSION     0xFCu
#define PUF_OFF_INTEN       0x100u
#define PUF_OFF_INTSTAT     0x104u
#define PUF_OFF_PWRCTRL     0x108u
#define PUF_OFF_CFG         0x10Cu
#define PUF_OFF_KEYLOCK     0x200u
#define PUF_OFF_KEYENABLE   0x204u
#define PUF_OFF_KEYRESET    0x208u
#define PUF_OFF_IDXBLK_L    0x20Cu
#define PUF_OFF_IDXBLK_H_DP 0x210u
#define PUF_OFF_KEYMASK0    0x214u
#define PUF_OFF_IDXBLK_H    0x254u
#define PUF_OFF_IDXBLK_L_DP 0x258u
#define PUF_OFF_SHIFT_STATUS 0x25Cu

#define PUF_CTRL_ZEROIZE     0x1u
#define PUF_CTRL_ENROLL      0x2u
#define PUF_CTRL_START       0x4u
#define PUF_CTRL_GENERATEKEY 0x8u
#define PUF_CTRL_SETKEY      0x10u
#define PUF_CTRL_GETKEY      0x40u

#define PUF_STAT_BUSY         0x1u
#define PUF_STAT_SUCCESS      0x2u
#define PUF_STAT_ERROR        0x4u
#define PUF_STAT_KEYINREQ     0x10u
#define PUF_STAT_KEYOUTAVAIL  0x20u
#define PUF_STAT_CODEINREQ    0x40u
#define PUF_STAT_CODEOUTAVAIL 0x80u

#define PUF_ALLOW_ENROLL 0x1u
#define PUF_ALLOW_START  0x2u
#define PUF_ALLOW_SETKEY 0x4u
#define PUF_ALLOW_GETKEY 0x8u

#define PUF_AC_BYTES    1192u
#define PUF_AC_WORDS    (PUF_AC_BYTES / 4u)
#define PUF_KC_HEADER_BYTES 20u
#define PUF_KC_HEADER_WORDS (PUF_KC_HEADER_BYTES / 4u)
#define PUF_MAX_KEY_BYTES   512u
#define PUF_TAG_BYTES       16u

enum lpc55s69_puf_op {
    PUF_OP_NONE = 0,
    PUF_OP_ENROLL,
    PUF_OP_START,
    PUF_OP_GENKEY,
    PUF_OP_SETKEY,
    PUF_OP_GETKEY
};

struct lpc55s69_puf_state {
    /* RW registers */
    mm_u32 keyindex, keysize, ifstat, inten, cfg, pwrctl;
    mm_u32 keylock, keyenable, idxblk_l, idxblk_h_dp;
    mm_u32 keymask[4], idxblk_h, idxblk_l_dp;
    /* persistent physical pattern + lifecycle */
    mm_u8 phys[PUF_AC_BYTES];
    mm_bool phys_init;
    mm_bool zeroized;
    /* activation + operation state (volatile) */
    mm_bool activated, error;
    mm_u8 fp[32];
    enum lpc55s69_puf_op op;
    mm_u8 out[PUF_AC_BYTES];
    mm_u32 out_pos, out_words;
    mm_u8 in[PUF_AC_BYTES];
    mm_u32 in_pos, in_words;
    mm_u32 keyout_idx;
    mm_u32 shift_status;
    mm_bool out_is_key;
};

static struct lpc55s69_puf_state puf;

/* SHA256 counter stream: out[0..len) = H(fp || label || ctr) blocks */
static void puf_stream(const mm_u8 *fp, const char *label, mm_u8 *out,
                       size_t len)
{
    struct mm_host_sha256_ctx ctx;
    mm_u8 ctr[4];
    mm_u8 block[32];
    size_t done = 0;
    mm_u32 count = 0;

    while (done < len) {
        size_t take = len - done;
        if (take > 32u) {
            take = 32u;
        }
        if (!mm_host_sha256_stream_init(&ctx)) {
            memset(out + done, 0, take);
            done += take;
            continue;
        }
        mm_host_sha256_stream_update(&ctx, fp, 32u);
        mm_host_sha256_stream_update(&ctx, (const mm_u8 *)label,
                                     strlen(label));
        ctr[0] = (mm_u8)(count & 0xffu);
        ctr[1] = (mm_u8)((count >> 8) & 0xffu);
        ctr[2] = (mm_u8)((count >> 16) & 0xffu);
        ctr[3] = (mm_u8)((count >> 24) & 0xffu);
        mm_host_sha256_stream_update(&ctx, ctr, 4u);
        if (!mm_host_sha256_stream_final(&ctx, block)) {
            memset(block, 0, 32u);
        }
        memcpy(out + done, block, take);
        done += take;
        count++;
    }
}

static void puf_build_fp(struct lpc55s69_puf_state *s)
{
    (void)mm_host_sha256(s->phys, PUF_AC_BYTES, s->fp);
}

static void puf_tag(const mm_u8 *fp, mm_u32 idx, const mm_u8 *key,
                    size_t key_len, mm_u8 tag[PUF_TAG_BYTES])
{
    struct mm_host_sha256_ctx ctx;
    mm_u8 digest[32];
    mm_u8 idx_byte = (mm_u8)idx;

    if (!mm_host_sha256_stream_init(&ctx)) {
        memset(tag, 0, PUF_TAG_BYTES);
        return;
    }
    mm_host_sha256_stream_update(&ctx, fp, 32u);
    mm_host_sha256_stream_update(&ctx, &idx_byte, 1u);
    mm_host_sha256_stream_update(&ctx, key, key_len);
    if (!mm_host_sha256_stream_final(&ctx, digest)) {
        memset(digest, 0, 32u);
    }
    memcpy(tag, digest, PUF_TAG_BYTES);
}

static void puf_seed_phys(struct lpc55s69_puf_state *s)
{
    const char *seed_hex;
    mm_u8 seed[32];
    int i;

    seed_hex = getenv("M33MU_LPC55S69_PUF_SEED_HEX");
    if (seed_hex != 0 && strlen(seed_hex) == 64u) {
        /* 64 hex chars -> 32 raw bytes */
        for (i = 0; i < 32; i++) {
            unsigned int byte = 0;
            if (sscanf(seed_hex + 2u * i, "%2x", &byte) != 1) {
                byte = 0;
            }
            seed[i] = (mm_u8)byte;
        }
    } else {
        const char *def = "m33mu-lpc55s69-default-puf";
        (void)mm_host_sha256((const mm_u8 *)def, strlen(def), seed);
    }
    puf_stream(seed, "phys", s->phys, PUF_AC_BYTES);
    s->phys_init = MM_TRUE;
}

static void puf_op_reset(struct lpc55s69_puf_state *s)
{
    s->op        = PUF_OP_NONE;
    s->out_pos   = 0;
    s->out_words = 0;
    s->in_pos    = 0;
    s->in_words  = 0;
    s->out_is_key = MM_FALSE;
}

static void puf_regs_reset(struct lpc55s69_puf_state *s)
{
    s->keyindex = 0;
    s->keysize  = 0;
    s->ifstat   = 0;
    s->inten    = 0;
    s->cfg      = 0;
    s->pwrctl   = 0xF8u;
    s->keylock  = 0xAAu;
    s->keyenable = 0x55u;
    s->idxblk_l = 0x8000AAAAu;
    s->idxblk_h_dp = 0xAAAAu;
    memset(s->keymask, 0, sizeof(s->keymask));
    s->idxblk_h   = 0x8000AAAAu;
    s->idxblk_l_dp = 0xAAAAu;
}

static void puf_power_off(struct lpc55s69_puf_state *s)
{
    s->activated = MM_FALSE;
    s->error     = MM_FALSE;
    puf_op_reset(s);
}

void mm_lpc55s69_puf_reset(void)
{
    /* phys + zeroized are chip-level: survive a machine reset */
    puf_regs_reset(&puf);
    puf.activated    = MM_FALSE;
    puf.error        = MM_FALSE;
    puf.shift_status = 0;
    puf_op_reset(&puf);
    if (!puf.phys_init) {
        puf_seed_phys(&puf);
    }
    puf_build_fp(&puf);
}

static mm_u32 puf_allow(const struct lpc55s69_puf_state *s)
{
    if (s->zeroized) {
        return 0;
    }
    return PUF_ALLOW_ENROLL | PUF_ALLOW_START |
           PUF_ALLOW_SETKEY | PUF_ALLOW_GETKEY;
}

static mm_u32 puf_stat(const struct lpc55s69_puf_state *s)
{
    mm_u32 stat;
    if ((s->pwrctl & 0x1u) == 0u) {
        return 0;
    }
    if (s->zeroized || s->error) {
        return PUF_STAT_ERROR;
    }
    stat = 0;
    if (s->out_pos < s->out_words) {
        stat |= PUF_STAT_BUSY;
        if (s->out_is_key) {
            stat |= PUF_STAT_KEYOUTAVAIL;
        } else {
            stat |= PUF_STAT_CODEOUTAVAIL;
        }
    }
    if (s->in_pos < s->in_words) {
        stat |= PUF_STAT_BUSY;
        if (s->op == PUF_OP_SETKEY) {
            stat |= PUF_STAT_KEYINREQ;
        } else {
            stat |= PUF_STAT_CODEINREQ;
        }
    }
    if (stat == 0u) {
        return PUF_STAT_SUCCESS;
    }
    return stat;
}

static void puf_do_zeroize(struct lpc55s69_puf_state *s)
{
    s->zeroized     = MM_TRUE;
    s->error        = MM_TRUE;
    s->activated    = MM_FALSE;
    s->shift_status = 0;
    puf_op_reset(s);
}

static void puf_start_enroll(struct lpc55s69_puf_state *s)
{
    puf_build_fp(s);
    puf_stream(s->fp, "ac", s->out, PUF_AC_BYTES);
    s->op        = PUF_OP_ENROLL;
    s->out_pos   = 0;
    s->out_words = PUF_AC_WORDS;
    s->in_pos    = 0;
    s->in_words  = 0;
    s->out_is_key = MM_FALSE;
}

static void puf_start_start(struct lpc55s69_puf_state *s)
{
    puf_stream(s->fp, "ac", s->out, PUF_AC_BYTES);
    s->op        = PUF_OP_START;
    s->out_pos   = 0;
    s->out_words = 0;
    s->in_pos    = 0;
    s->in_words  = PUF_AC_WORDS;
    s->out_is_key = MM_FALSE;
}

static void puf_build_kc(struct lpc55s69_puf_state *s, mm_u32 idx,
                         const mm_u8 *key, mm_u32 key_bytes)
{
    mm_u32 key_words = key_bytes / 4u;
    mm_u32 kc_words  = PUF_KC_HEADER_WORDS + key_words;
    mm_u8 tag[PUF_TAG_BYTES];

    puf_tag(s->fp, idx, key, key_bytes, tag);
    memset(s->out, 0, PUF_KC_HEADER_BYTES);
    s->out[0] = (mm_u8)(idx == 0u ? 0u : 1u); /* 0=user, 1=intrinsic */
    s->out[1] = (mm_u8)idx;
    s->out[2] = 0;
    s->out[3] = (mm_u8)key_words;
    memcpy(s->out + 4u, tag, PUF_TAG_BYTES);
    memcpy(s->out + PUF_KC_HEADER_BYTES, key, key_bytes);
    s->out_pos   = 0;
    s->out_words = kc_words;
    s->out_is_key = MM_FALSE;
}

static void puf_start_genkey(struct lpc55s69_puf_state *s)
{
    mm_u32 idx = s->keyindex & 0xFu;
    mm_u32 key_bytes = (s->keysize & 0x3Fu) * 8u;

    if (key_bytes == 0u || key_bytes > PUF_MAX_KEY_BYTES) {
        s->error = MM_TRUE;
        return;
    }
    mm_host_rng_bytes(s->out + PUF_KC_HEADER_BYTES, key_bytes);
    puf_build_kc(s, idx, s->out + PUF_KC_HEADER_BYTES, key_bytes);
    s->op = PUF_OP_GENKEY;
    s->in_pos = 0;
    s->in_words = 0;
}

static void puf_start_setkey(struct lpc55s69_puf_state *s)
{
    mm_u32 key_bytes = (s->keysize & 0x3Fu) * 8u;

    if (key_bytes == 0u || key_bytes > PUF_MAX_KEY_BYTES) {
        s->error = MM_TRUE;
        return;
    }
    s->op        = PUF_OP_SETKEY;
    s->out_pos   = 0;
    s->out_words = 0;
    s->in_pos    = 0;
    s->in_words  = key_bytes / 4u;
    s->out_is_key = MM_FALSE;
}

static void puf_start_getkey(struct lpc55s69_puf_state *s)
{
    s->op        = PUF_OP_GETKEY;
    s->out_pos   = 0;
    s->out_words = 0;
    s->in_pos    = 0;
    s->in_words  = 0; /* determined by the KC header word */
    s->out_is_key = MM_FALSE;
}

static void puf_codeinput(struct lpc55s69_puf_state *s, mm_u32 word)
{
    /* in_words is 0 until the GETKEY header word fixes it */
    if (s->in_words != 0u && s->in_pos >= s->in_words) {
        return;
    }
    s->in[4u * s->in_pos + 0u] = (mm_u8)(word & 0xffu);
    s->in[4u * s->in_pos + 1u] = (mm_u8)((word >> 8) & 0xffu);
    s->in[4u * s->in_pos + 2u] = (mm_u8)((word >> 16) & 0xffu);
    s->in[4u * s->in_pos + 3u] = (mm_u8)((word >> 24) & 0xffu);
    s->in_pos++;

    if (s->op == PUF_OP_START) {
        if (s->in_pos == s->in_words) {
            if (memcmp(s->in, s->out, PUF_AC_BYTES) == 0) {
                s->activated = MM_TRUE;
            } else {
                s->error = MM_TRUE;
            }
            puf_op_reset(s);
        }
        return;
    }

    if (s->op == PUF_OP_GETKEY) {
        if (s->in_pos == 1u) {
            /* header word 0: byte3 = key words */
            mm_u32 key_words = (mm_u32)s->in[3] & 0xFu;
            s->in_words = PUF_KC_HEADER_WORDS + key_words;
        }
        if (s->in_pos == s->in_words) {
            mm_u32 idx = (mm_u32)s->in[1] & 0xFu;
            mm_u32 key_words = (mm_u32)s->in[3] & 0xFu;
            mm_u32 key_bytes = key_words * 4u;
            const mm_u8 *key = s->in + PUF_KC_HEADER_BYTES;
            mm_u8 tag[PUF_TAG_BYTES];
            mm_bool ok;

            puf_tag(s->fp, idx, key, key_bytes, tag);
            ok = (memcmp(tag, s->in + 4u, PUF_TAG_BYTES) == 0);
            if (ok) {
                if (idx == 0u) {
                    /* HW key: no KEYOUTPUT, update shift status */
                    s->shift_status =
                        (s->shift_status & ~0xFu) | (2u * key_words - 1u);
                } else {
                    memcpy(s->out, key, key_bytes);
                    s->out_pos   = 0;
                    s->out_words = key_words;
                    s->keyout_idx = idx;
                    s->out_is_key = MM_TRUE;
                }
            } else {
                s->error = MM_TRUE;
            }
            /* input consumed; the key (if any) stays pending on
             * KEYOUTPUT until the driver drains it */
            s->op = PUF_OP_NONE;
        }
    }
}

static void puf_keyinput(struct lpc55s69_puf_state *s, mm_u32 word)
{
    if (s->op != PUF_OP_SETKEY || s->in_pos >= s->in_words) {
        return;
    }
    s->in[4u * s->in_pos + 0u] = (mm_u8)(word & 0xffu);
    s->in[4u * s->in_pos + 1u] = (mm_u8)((word >> 8) & 0xffu);
    s->in[4u * s->in_pos + 2u] = (mm_u8)((word >> 16) & 0xffu);
    s->in[4u * s->in_pos + 3u] = (mm_u8)((word >> 24) & 0xffu);
    s->in_pos++;

    if (s->in_pos == s->in_words) {
        mm_u32 idx = s->keyindex & 0xFu;
        mm_u32 key_bytes = (mm_u32)s->in_words * 4u;
        puf_build_kc(s, idx, s->in, key_bytes);
    }
}

static mm_u32 puf_codeoutput(struct lpc55s69_puf_state *s)
{
    mm_u32 word;
    if (s->out_pos >= s->out_words) {
        return 0;
    }
    word = ((mm_u32)s->out[4u * s->out_pos + 0u]) |
           (((mm_u32)s->out[4u * s->out_pos + 1u]) << 8) |
           (((mm_u32)s->out[4u * s->out_pos + 2u]) << 16) |
           (((mm_u32)s->out[4u * s->out_pos + 3u]) << 24);
    s->out_pos++;
    if (s->out_pos == s->out_words) {
        puf_op_reset(s);
    }
    return word;
}

static mm_u32 puf_keyoutput(struct lpc55s69_puf_state *s)
{
    if (!s->out_is_key) {
        return 0;
    }
    return puf_codeoutput(s);
}

static void puf_ctrl_write(struct lpc55s69_puf_state *s, mm_u32 value)
{
    if (s->op != PUF_OP_NONE) {
        return;
    }
    if (s->zeroized || s->error) {
        return;
    }
    if (value & PUF_CTRL_ZEROIZE) {
        puf_do_zeroize(s);
        return;
    }
    if (value & PUF_CTRL_ENROLL) {
        if ((puf_allow(s) & PUF_ALLOW_ENROLL) == 0u) {
            return;
        }
        puf_start_enroll(s);
        return;
    }
    if (value & PUF_CTRL_START) {
        if ((puf_allow(s) & PUF_ALLOW_START) == 0u) {
            return;
        }
        puf_start_start(s);
        return;
    }
    if (value & PUF_CTRL_GENERATEKEY) {
        if ((puf_allow(s) & PUF_ALLOW_SETKEY) == 0u) {
            return;
        }
        puf_start_genkey(s);
        return;
    }
    if (value & PUF_CTRL_SETKEY) {
        if ((puf_allow(s) & PUF_ALLOW_SETKEY) == 0u) {
            return;
        }
        puf_start_setkey(s);
        return;
    }
    if (value & PUF_CTRL_GETKEY) {
        if ((puf_allow(s) & PUF_ALLOW_GETKEY) == 0u) {
            return;
        }
        puf_start_getkey(s);
        return;
    }
}

static mm_bool puf_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                        mm_u32 *value_out)
{
    struct lpc55s69_puf_state *s = (struct lpc55s69_puf_state *)opaque;
    mm_u32 value = 0;

    if (s == 0 || value_out == 0 || size_bytes != 4u) {
        return MM_FALSE;
    }
    if ((offset + size_bytes) > PUF_SIZE) {
        return MM_FALSE;
    }
    if (!mm_lpc55s69_syscon_periph_active(PUF_AHBCLK_OFFSET, PUF_AHBCLK_BIT)) {
        return MM_FALSE;
    }

    switch (offset) {
    case PUF_OFF_CTRL:
        value = 0;
        break;
    case PUF_OFF_KEYINDEX:
        value = s->keyindex & 0xFu;
        break;
    case PUF_OFF_KEYSIZE:
        value = s->keysize & 0x3Fu;
        break;
    case PUF_OFF_STAT:
        value = puf_stat(s);
        break;
    case PUF_OFF_ALLOW:
        value = puf_allow(s);
        break;
    case PUF_OFF_CODEOUTPUT:
        value = puf_codeoutput(s);
        break;
    case PUF_OFF_KEYOUTINDEX:
        value = s->keyout_idx & 0xFu;
        break;
    case PUF_OFF_KEYOUTPUT:
        value = puf_keyoutput(s);
        break;
    case PUF_OFF_IFSTAT:
        value = s->ifstat;
        break;
    case PUF_OFF_VERSION:
        value = 0;
        break;
    case PUF_OFF_INTEN:
        value = s->inten;
        break;
    case PUF_OFF_INTSTAT:
        value = puf_stat(s) & ~PUF_STAT_BUSY;
        break;
    case PUF_OFF_PWRCTRL:
        value = (s->pwrctl & ~0x2u) |
                (((s->pwrctl & 0x1u) != 0u) ? 0x2u : 0u);
        break;
    case PUF_OFF_CFG:
        value = s->cfg;
        break;
    case PUF_OFF_KEYLOCK:
        value = s->keylock;
        break;
    case PUF_OFF_KEYENABLE:
        value = s->keyenable;
        break;
    case PUF_OFF_KEYRESET:
        value = 0;
        break;
    case PUF_OFF_IDXBLK_L:
        value = s->idxblk_l;
        break;
    case PUF_OFF_IDXBLK_H_DP:
        value = s->idxblk_h_dp;
        break;
    case PUF_OFF_KEYMASK0:
        value = s->keymask[0];
        break;
    case PUF_OFF_KEYMASK0 + 4u:
        value = s->keymask[1];
        break;
    case PUF_OFF_KEYMASK0 + 8u:
        value = s->keymask[2];
        break;
    case PUF_OFF_KEYMASK0 + 12u:
        value = s->keymask[3];
        break;
    case PUF_OFF_IDXBLK_H:
        value = s->idxblk_h;
        break;
    case PUF_OFF_IDXBLK_L_DP:
        value = s->idxblk_l_dp;
        break;
    case PUF_OFF_SHIFT_STATUS:
        value = s->shift_status;
        break;
    default:
        value = 0;
        break;
    }
    *value_out = value;
    return MM_TRUE;
}

static mm_bool puf_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 value)
{
    struct lpc55s69_puf_state *s = (struct lpc55s69_puf_state *)opaque;
    if (s == 0 || size_bytes != 4u) {
        return MM_FALSE;
    }
    if ((offset + size_bytes) > PUF_SIZE) {
        return MM_FALSE;
    }
    if (!mm_lpc55s69_syscon_periph_active(PUF_AHBCLK_OFFSET, PUF_AHBCLK_BIT)) {
        return MM_FALSE;
    }

    switch (offset) {
    case PUF_OFF_CTRL:
        puf_ctrl_write(s, value);
        break;
    case PUF_OFF_KEYINDEX:
        s->keyindex = value & 0xFu;
        break;
    case PUF_OFF_KEYSIZE:
        s->keysize = value & 0x3Fu;
        break;
    case PUF_OFF_KEYINPUT:
        puf_keyinput(s, value);
        break;
    case PUF_OFF_CODEINPUT:
        puf_codeinput(s, value);
        break;
    case PUF_OFF_IFSTAT:
        s->ifstat &= ~value;
        break;
    case PUF_OFF_INTEN:
        s->inten = value;
        break;
    case PUF_OFF_INTSTAT:
        break;
    case PUF_OFF_PWRCTRL: {
        mm_u32 new_ram_on = value & 0x1u;
        mm_u32 old_ram_on = s->pwrctl & 0x1u;
        s->pwrctl = (s->pwrctl & ~0x1u) | new_ram_on;
        if (old_ram_on != 0u && new_ram_on == 0u) {
            puf_power_off(s);
        }
        break;
    }
    case PUF_OFF_CFG:
        s->cfg = value;
        break;
    case PUF_OFF_KEYLOCK:
        s->keylock = value;
        break;
    case PUF_OFF_KEYENABLE:
        s->keyenable = value;
        break;
    case PUF_OFF_KEYRESET:
        break;
    case PUF_OFF_IDXBLK_L:
        s->idxblk_l = value;
        break;
    case PUF_OFF_IDXBLK_H_DP:
        s->idxblk_h_dp = value;
        break;
    case PUF_OFF_KEYMASK0:
        s->keymask[0] = value;
        break;
    case PUF_OFF_KEYMASK0 + 4u:
        s->keymask[1] = value;
        break;
    case PUF_OFF_KEYMASK0 + 8u:
        s->keymask[2] = value;
        break;
    case PUF_OFF_KEYMASK0 + 12u:
        s->keymask[3] = value;
        break;
    case PUF_OFF_IDXBLK_H:
        s->idxblk_h = value;
        break;
    case PUF_OFF_IDXBLK_L_DP:
        s->idxblk_l_dp = value;
        break;
    default:
        break;
    }
    return MM_TRUE;
}

mm_bool mm_lpc55s69_puf_register(struct mmio_bus *bus)
{
    struct mmio_region reg;
    if (bus == 0) {
        return MM_FALSE;
    }
    reg.size   = PUF_SIZE;
    reg.opaque = &puf;
    reg.read   = puf_read;
    reg.write  = puf_write;
    reg.base   = PUF_BASE_NS;
    if (!mmio_bus_register_region(bus, &reg)) {
        return MM_FALSE;
    }
    reg.base = PUF_BASE_S;
    return mmio_bus_register_region(bus, &reg);
}
