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
#include <string.h>
#include "stm32_gpdma.h"
#include "m33mu/memmap.h"
#include "m33mu/mmio.h"

#define TR1_SDW(v)     ((v) & 3u)
#define TR1_SINC       (1u << 3)
#define TR1_SBL        (0x3Fu << 4)
#define TR1_PAM        (3u << 11)
#define TR1_SSEC       (1u << 15)
#define TR1_DDW(v)     (((v) >> 16) & 3u)
#define TR1_DINC       (1u << 19)
#define TR1_DBL        (0x3Fu << 20)
#define TR1_DSEC       (1u << 31)
#define TR2_REQSEL(v)  ((v) & 0xFFu)
#define TR2_SWREQ      (1u << 9)
#define TR2_BREQ       (1u << 11)
#define TR2_PFREQ      (1u << 12)
#define TR2_TRIGPOL(v) (((v) >> 24) & 3u)
#define TR2_TCEM(v)    ((v) >> 30)
#define BR1_BNDT       0xFFFFu
#define BR1_BRC        (0x7FFu << 16)
#define CR_LSM         (1u << 16)
#define CR_RO_WHEN_EN  0x00FF0000u /* PRIO, LAP, LSM */
#define LLR_UT1        (1u << 31)
#define LLR_UT2        (1u << 30)
#define LLR_UB1        (1u << 29)
#define LLR_USA        (1u << 28)
#define LLR_UDA        (1u << 27)
#define LLR_UT3        (1u << 26)
#define LLR_UB2        (1u << 25)
#define LLR_ULL        (1u << 16)
#define LLR_LA         0xFFFCu
#define LLR_MASK_1D    (LLR_UT1 | LLR_UT2 | LLR_UB1 | LLR_USA | LLR_UDA | LLR_ULL | LLR_LA)
#define FLAG_MASK      0x7F00u /* CxSR flags 14:8, same positions as their CxCR enables */
#define LINK_BOUND     1024u

#define WARN_PAM       (1u << 0)
#define WARN_2D        (1u << 1)
#define WARN_TRIG      (1u << 2)
#define WARN_BURST     (1u << 3)
#define WARN_PFREQ     (1u << 4)
#define WARN_BREQ      (1u << 5)
#define WARN_DUP_REQ   (1u << 6)
#define WARN_BOUND     (1u << 7)
#define WARN_LSM       (1u << 8)

/* Channel interrupts are numbered irq_base + index unless the part supplies
 * an explicit table (see struct stm32_gpdma_state::irq_map). */
static mm_u32 gpdma_channel_irq(const struct stm32_gpdma_state *d, mm_u32 ch_idx)
{
    if (d->irq_map != 0) {
        return (mm_u32)d->irq_map[ch_idx];
    }
    return d->irq_base + ch_idx;
}

void stm32_gpdma_reset(struct stm32_gpdma_state *d)
{
    mm_u32 i;
    d->seccfgr = 0;
    d->privcfgr = 0;
    d->rcfglockr = 0;
    d->misr = 0;
    d->smisr = 0;
    d->irq_held = 0;
    for (i = 0; i < d->num_channels; ++i) {
        memset(&d->ch[i], 0, sizeof(d->ch[i]));
        d->ch[i].sr = STM32_GPDMA_CxSR_IDLEF;
    }
}

void stm32_gpdma_set_nvic(struct stm32_gpdma_state *d, struct mm_nvic *nvic)
{
    d->nvic = nvic;
}

static void gpdma_warn(struct stm32_gpdma_state *d, mm_u32 bit, const char *msg)
{
    if ((d->warned & bit) != 0u) {
        return;
    }
    d->warned |= bit;
    fprintf(stderr, "[GPDMA%u] warning: %s\n", (unsigned)d->instance + 1u, msg);
}

static mm_bool gpdma_ch_secure(const struct stm32_gpdma_state *d, mm_u32 ch_idx)
{
    return ((d->seccfgr >> ch_idx) & 1u) != 0u;
}

static enum mm_sec_state gpdma_side_sec(const struct stm32_gpdma_state *d, mm_u32 ch_idx,
                                        mm_u32 sec_bit)
{
    if (gpdma_ch_secure(d, ch_idx) && (d->ch[ch_idx].tr1 & sec_bit) != 0u) {
        return MM_SECURE;
    }
    return MM_NONSECURE;
}

static void gpdma_update_misr(struct stm32_gpdma_state *d)
{
    mm_u32 i;
    d->misr = 0u;
    d->smisr = 0u;
    for (i = 0; i < d->num_channels && i < STM32_GPDMA_CH_COUNT; ++i) {
        const struct stm32_gpdma_channel *ch = &d->ch[i];
        if ((ch->sr & ch->cr & FLAG_MASK) == 0u) {
            continue;
        }
        if (gpdma_ch_secure(d, i)) {
            d->smisr |= 1u << i;
        } else {
            d->misr |= 1u << i;
        }
    }
}

/* Level-like line: pend while CxSR & CxCR flags are non-zero. A pend made by a
 * register write from the active handler is held, and retracted if the line drops. */
static void gpdma_irq_eval(struct stm32_gpdma_state *d, mm_u32 ch_idx, mm_bool new_flag)
{
    const struct stm32_gpdma_channel *ch = &d->ch[ch_idx];
    mm_u32 irq = gpdma_channel_irq(d, ch_idx);
    mm_u32 bit = 1u << ch_idx;
    mm_bool held = (d->irq_held & bit) != 0u;

    gpdma_update_misr(d);
    if (d->nvic == 0) {
        return;
    }
    d->irq_held &= ~bit;
    if ((ch->sr & ch->cr & FLAG_MASK) == 0u) {
        if (held) {
            mm_nvic_set_pending(d->nvic, irq, MM_FALSE);
        }
        return;
    }
    if (mm_nvic_is_pending(d->nvic, irq)) {
        if (held && !new_flag) {
            d->irq_held |= bit;
        }
        return;
    }
    mm_nvic_set_pending(d->nvic, irq, MM_TRUE);
    if (!new_flag && mm_nvic_is_active(d->nvic, irq)) {
        d->irq_held |= bit;
    }
}

static void gpdma_flag(struct stm32_gpdma_state *d, mm_u32 ch_idx, mm_u32 flag)
{
    struct stm32_gpdma_channel *ch = &d->ch[ch_idx];
    ch->sr |= flag;
    gpdma_irq_eval(d, ch_idx, (ch->cr & flag) != 0u ? MM_TRUE : MM_FALSE);
}

static void gpdma_stop(struct stm32_gpdma_state *d, mm_u32 ch_idx, mm_u32 flag)
{
    d->ch[ch_idx].cr &= ~STM32_GPDMA_CxCR_EN;
    d->ch[ch_idx].sr |= STM32_GPDMA_CxSR_IDLEF;
    gpdma_flag(d, ch_idx, flag);
}

static mm_u32 gpdma_width_mask(mm_u32 bytes)
{
    return (bytes >= 4u) ? 0xFFFFFFFFu : ((1u << (bytes * 8u)) - 1u);
}

/* USEF cases of RM0517 16.8.10/16.8.12; unmodelled settings warn once. */
static mm_bool gpdma_settings_ok(struct stm32_gpdma_state *d, mm_u32 ch_idx)
{
    struct stm32_gpdma_channel *ch = &d->ch[ch_idx];
    mm_u32 sdw = TR1_SDW(ch->tr1);
    mm_u32 ddw = TR1_DDW(ch->tr1);
    mm_u32 trigpol = TR2_TRIGPOL(ch->tr2);

    if (sdw == 3u || ddw == 3u ||
        (ch->sar & ((1u << sdw) - 1u)) != 0u ||
        (ch->dar & ((1u << ddw) - 1u)) != 0u ||
        ch->remaining == 0u ||
        (ch->remaining & ((1u << sdw) - 1u)) != 0u) {
        gpdma_stop(d, ch_idx, STM32_GPDMA_CxSR_USEF);
        return MM_FALSE;
    }
    if (sdw != ddw && (ch->tr1 & TR1_PAM) != 0u) {
        gpdma_warn(d, WARN_PAM, "PAM other than 00 not modelled, right-aligning");
    }
    if ((ch->tr1 & (TR1_SBL | TR1_DBL)) != 0u) {
        gpdma_warn(d, WARN_BURST, "bursts not modelled, moving single beats");
    }
    if (trigpol == 1u || trigpol == 2u) {
        gpdma_warn(d, WARN_TRIG, "triggers (TRIGPOL) not modelled, ignoring TRIGSEL");
    }
    if ((ch->tr2 & TR2_SWREQ) == 0u && (ch->tr2 & TR2_PFREQ) != 0u) {
        gpdma_warn(d, WARN_PFREQ, "peripheral flow control (PFREQ) not modelled");
    }
    if ((ch->tr2 & TR2_SWREQ) == 0u && (ch->tr2 & TR2_BREQ) != 0u) {
        gpdma_warn(d, WARN_BREQ, "block requests (BREQ) not modelled, one beat per request");
    }
    if (ch_idx >= (mm_u32)d->first_2d_channel &&
        ((ch->br1 & BR1_BRC) != 0u || ch->tr3 != 0u || ch->br2 != 0u)) {
        gpdma_warn(d, WARN_2D, "2D addressing not modelled, running a 1D block");
    }
    if ((ch->cr & CR_LSM) != 0u) {
        gpdma_warn(d, WARN_LSM, "link step mode (LSM) not modelled, running the full list");
    }
    return MM_TRUE;
}

/* Loads the next LLI; MM_FALSE when the channel stopped (ULEF or USEF). */
static mm_bool gpdma_link(struct stm32_gpdma_state *d, mm_u32 ch_idx)
{
    static const mm_u32 update_bits[8] = {
        LLR_UT1, LLR_UT2, LLR_UB1, LLR_USA, LLR_UDA, LLR_UT3, LLR_UB2, LLR_ULL
    };
    struct stm32_gpdma_channel *ch = &d->ch[ch_idx];
    struct mm_memmap *map = mm_memmap_current();
    enum mm_sec_state sec = gpdma_ch_secure(d, ch_idx) ? MM_SECURE : MM_NONSECURE;
    mm_bool is_2d = (ch_idx >= (mm_u32)d->first_2d_channel) ? MM_TRUE : MM_FALSE;
    mm_u32 llr = ch->llr;
    mm_u32 addr = (ch->lbar & 0xFFFF0000u) | (llr & LLR_LA);
    mm_u32 words[8];
    mm_u32 n = 0;
    mm_u32 i;

    if ((llr & LLR_UB1) == 0u) {
        ch->remaining = ch->bndt_reload;
    }
    for (i = 0; i < 8u; ++i) {
        words[i] = 0u;
        if ((llr & update_bits[i]) == 0u || ((i == 5u || i == 6u) && !is_2d)) {
            continue;
        }
        if (!mm_memmap_dma_read(map, sec, addr + n * 4u, 4u, &words[i])) {
            gpdma_stop(d, ch_idx, STM32_GPDMA_CxSR_ULEF);
            return MM_FALSE;
        }
        ++n;
    }
    if ((llr & LLR_UT1) != 0u) {
        ch->tr1 = words[0];
        if (!gpdma_ch_secure(d, ch_idx)) {
            ch->tr1 &= ~(TR1_SSEC | TR1_DSEC);
        }
    }
    if ((llr & LLR_UT2) != 0u) {
        ch->tr2 = words[1];
    }
    if ((llr & LLR_UB1) != 0u) {
        ch->br1 = words[2];
        ch->remaining = words[2] & BR1_BNDT;
        ch->bndt_reload = ch->remaining;
    }
    if ((llr & LLR_USA) != 0u) {
        ch->sar = words[3];
    }
    if ((llr & LLR_UDA) != 0u) {
        ch->dar = words[4];
    }
    if (is_2d && (llr & LLR_UT3) != 0u) {
        ch->tr3 = words[5];
    }
    if (is_2d && (llr & LLR_UB2) != 0u) {
        ch->br2 = words[6];
    }
    if ((llr & LLR_ULL) != 0u) {
        ch->llr = words[7] & (is_2d ? (LLR_MASK_1D | LLR_UT3 | LLR_UB2) : LLR_MASK_1D);
    }
    ch->block_bytes = ch->remaining;
    ch->ht_done = 0u;
    return gpdma_settings_ok(d, ch_idx);
}

/* Moves one beat with PAM = 00; at the end of a block completes the channel
 * or runs the link transfer (*linked set when the channel goes on). */
static enum stm32_dma_req_result gpdma_beat(struct stm32_gpdma_state *d, mm_u32 ch_idx,
                                            mm_bool *linked)
{
    struct stm32_gpdma_channel *ch = &d->ch[ch_idx];
    struct mm_memmap *map = mm_memmap_current();
    mm_u32 sbytes = 1u << TR1_SDW(ch->tr1);
    mm_u32 dbytes = 1u << TR1_DDW(ch->tr1);
    mm_u32 tcem = TR2_TCEM(ch->tr2);
    mm_u32 val = 0u;

    *linked = MM_FALSE;
    if (!mm_memmap_dma_read(map, gpdma_side_sec(d, ch_idx, TR1_SSEC), ch->sar, sbytes, &val)) {
        gpdma_stop(d, ch_idx, STM32_GPDMA_CxSR_DTEF);
        return STM32_DMA_REQ_NONE;
    }
    val &= gpdma_width_mask(sbytes) & gpdma_width_mask(dbytes);
    if (!mm_memmap_dma_write(map, gpdma_side_sec(d, ch_idx, TR1_DSEC), ch->dar, dbytes, val)) {
        gpdma_stop(d, ch_idx, STM32_GPDMA_CxSR_DTEF);
        return STM32_DMA_REQ_NONE;
    }
    if ((ch->tr1 & TR1_SINC) != 0u) {
        ch->sar += sbytes;
    }
    if ((ch->tr1 & TR1_DINC) != 0u) {
        ch->dar += dbytes;
    }
    ch->remaining = (ch->remaining > sbytes) ? ch->remaining - sbytes : 0u;
    if (ch->ht_done == 0u && (tcem != 3u || ch->llr == 0u) &&
        ch->block_bytes - ch->remaining >= (ch->block_bytes + 1u) / 2u) {
        ch->ht_done = 1u;
        gpdma_flag(d, ch_idx, STM32_GPDMA_CxSR_HTF);
    }
    if (ch->remaining != 0u) {
        return STM32_DMA_REQ_SERVED;
    }
    if (ch->llr == 0u) {
        gpdma_stop(d, ch_idx, STM32_GPDMA_CxSR_TCF);
        return STM32_DMA_REQ_LAST;
    }
    if (tcem <= 1u) {
        gpdma_flag(d, ch_idx, STM32_GPDMA_CxSR_TCF);
    }
    if (!gpdma_link(d, ch_idx)) {
        return STM32_DMA_REQ_SERVED;
    }
    if (tcem == 2u) {
        gpdma_flag(d, ch_idx, STM32_GPDMA_CxSR_TCF);
    }
    *linked = MM_TRUE;
    return STM32_DMA_REQ_SERVED;
}

static void gpdma_notify_ready(struct stm32_gpdma_state *d, mm_u32 ch_idx)
{
    struct stm32_dma_router *r = d->router;
    mm_u32 line = TR2_REQSEL(d->ch[ch_idx].tr2);
    if (r == 0 || (r->depth > 0u && line == r->serving_line) || r->ready[line] == 0) {
        return;
    }
    r->ready[line](r->ready_ctx[line]);
}

static mm_bool gpdma_ch_running(const struct stm32_gpdma_channel *ch)
{
    return (ch->cr & (STM32_GPDMA_CxCR_EN | STM32_GPDMA_CxCR_SUSP)) == STM32_GPDMA_CxCR_EN;
}

/* SWREQ = 1: copy until the channel completes, stops, or LINK_BOUND links. */
static void gpdma_run_sw(struct stm32_gpdma_state *d, mm_u32 ch_idx)
{
    struct stm32_gpdma_channel *ch = &d->ch[ch_idx];
    mm_u32 links = 0;
    mm_bool linked = MM_FALSE;

    while (gpdma_ch_running(ch) && (ch->tr2 & TR2_SWREQ) != 0u) {
        (void)gpdma_beat(d, ch_idx, &linked);
        if (!linked) {
            continue;
        }
        if ((ch->tr2 & TR2_SWREQ) == 0u) {
            gpdma_notify_ready(d, ch_idx);
            return;
        }
        if (++links >= LINK_BOUND) {
            gpdma_warn(d, WARN_BOUND, "memory-to-memory linked list not completing, paused");
            return;
        }
    }
}

static void gpdma_start(struct stm32_gpdma_state *d, mm_u32 ch_idx)
{
    struct stm32_gpdma_channel *ch = &d->ch[ch_idx];
    if (!gpdma_ch_running(ch)) {
        return;
    }
    if ((ch->tr2 & TR2_SWREQ) != 0u) {
        gpdma_run_sw(d, ch_idx);
    } else {
        gpdma_notify_ready(d, ch_idx);
    }
}

/* EN 0 -> 1. A null LLI0 (BNDT = 0, LLR.UB1 = 1) loads LLI1 before any data. */
static void gpdma_enable(struct stm32_gpdma_state *d, mm_u32 ch_idx)
{
    struct stm32_gpdma_channel *ch = &d->ch[ch_idx];

    ch->sr &= ~STM32_GPDMA_CxSR_IDLEF;
    ch->ht_done = 0u;
    ch->block_bytes = ch->remaining;
    if (ch->remaining == 0u && (ch->llr & LLR_UB1) != 0u) {
        mm_u32 tcem = TR2_TCEM(ch->tr2);
        if (gpdma_link(d, ch_idx) && tcem == 2u) {
            gpdma_flag(d, ch_idx, STM32_GPDMA_CxSR_TCF);
        }
        return;
    }
    (void)gpdma_settings_ok(d, ch_idx);
}

static mm_u32 gpdma_ch_read_reg(const struct stm32_gpdma_channel *ch, mm_u32 ch_offset)
{
    switch (ch_offset) {
    case STM32_GPDMA_CxLBAR: return ch->lbar;
    case STM32_GPDMA_CxFCR:  return 0; /* FCR is write-only */
    case STM32_GPDMA_CxSR:   return ch->sr;
    case STM32_GPDMA_CxCR:   return ch->cr;
    case STM32_GPDMA_CxTR1:  return ch->tr1;
    case STM32_GPDMA_CxTR2:  return ch->tr2;
    case STM32_GPDMA_CxBR1:  return (ch->br1 & ~BR1_BNDT) | ch->remaining;
    case STM32_GPDMA_CxSAR:  return ch->sar;
    case STM32_GPDMA_CxDAR:  return ch->dar;
    case STM32_GPDMA_CxTR3:  return ch->tr3;
    case STM32_GPDMA_CxBR2:  return ch->br2;
    case STM32_GPDMA_CxLLR:  return ch->llr;
    default: return 0;
    }
}

mm_bool stm32_gpdma_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 *value_out)
{
    struct stm32_gpdma_state *d = (struct stm32_gpdma_state *)opaque;
    mm_u32 byte = offset & 3u;
    mm_u32 v = 0;

    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;

    offset &= ~3u;
    if (byte + size_bytes > 4u) {
        *value_out = 0u;
        return MM_TRUE;
    }
    if (offset == STM32_GPDMA_SECCFGR) {
        v = d->seccfgr;
    } else if (offset == STM32_GPDMA_PRIVCFGR) {
        v = d->privcfgr;
    } else if (offset == STM32_GPDMA_RCFGLOCKR) {
        v = d->rcfglockr;
    } else if (offset == STM32_GPDMA_MISR) {
        v = d->misr;
    } else if (offset == STM32_GPDMA_SMISR) {
        /* Secure read register (RM0517 16.8.5). */
        v = (mmio_active_sec() == MM_NONSECURE) ? 0u : d->smisr;
    } else if (offset >= STM32_GPDMA_CH_BASE) {
        mm_u32 ch_region = offset - STM32_GPDMA_CH_BASE;
        mm_u32 ch_idx = ch_region / STM32_GPDMA_CH_STRIDE;
        mm_u32 ch_offset = ch_region % STM32_GPDMA_CH_STRIDE;
        /* A secure channel reads as zero to non-secure agents (RM0517 16.4.15). */
        if (ch_idx < d->num_channels &&
            (!gpdma_ch_secure(d, ch_idx) || mmio_active_sec() != MM_NONSECURE)) {
            v = gpdma_ch_read_reg(&d->ch[ch_idx], ch_offset);
        }
    }

    *value_out = (v >> (byte * 8u)) & gpdma_width_mask(size_bytes);
    return MM_TRUE;
}

/* value holds only the written byte lanes (lanes); the other bytes keep
 * their contents and EN/RESET/FCR bits act only when written. */
static void gpdma_ch_write_reg(struct stm32_gpdma_state *d, mm_u32 ch_idx,
                               mm_u32 ch_offset, mm_u32 value, mm_u32 lanes)
{
    struct stm32_gpdma_channel *ch = &d->ch[ch_idx];
    mm_u32 merged = (gpdma_ch_read_reg(ch, ch_offset) & ~lanes) | value;

    if ((ch->cr & STM32_GPDMA_CxCR_EN) != 0u && ch_offset != STM32_GPDMA_CxFCR &&
        ch_offset != STM32_GPDMA_CxCR) {
        return;
    }
    switch (ch_offset) {
    case STM32_GPDMA_CxLBAR:
        ch->lbar = merged & 0xFFFF0000u; /* bits 31:16 */
        break;
    case STM32_GPDMA_CxFCR:
        /* Flag clear register: write 1 to clear corresponding SR bits */
        if (value & STM32_GPDMA_CxFCR_TCF)   ch->sr &= ~STM32_GPDMA_CxSR_TCF;
        if (value & STM32_GPDMA_CxFCR_HTF)   ch->sr &= ~STM32_GPDMA_CxSR_HTF;
        if (value & STM32_GPDMA_CxFCR_DTEF)  ch->sr &= ~STM32_GPDMA_CxSR_DTEF;
        if (value & STM32_GPDMA_CxFCR_ULEF)  ch->sr &= ~STM32_GPDMA_CxSR_ULEF;
        if (value & STM32_GPDMA_CxFCR_USEF)  ch->sr &= ~STM32_GPDMA_CxSR_USEF;
        if (value & STM32_GPDMA_CxFCR_SUSPF) ch->sr &= ~STM32_GPDMA_CxSR_SUSPF;
        if (value & STM32_GPDMA_CxFCR_TOF)   ch->sr &= ~STM32_GPDMA_CxSR_TOF;
        gpdma_irq_eval(d, ch_idx, MM_FALSE);
        break;
    case STM32_GPDMA_CxCR: {
        mm_bool was_en = (ch->cr & STM32_GPDMA_CxCR_EN) != 0u;
        mm_bool was_susp = was_en && (ch->cr & STM32_GPDMA_CxCR_SUSP) != 0u;
        mm_u32 keep = was_en ? CR_RO_WHEN_EN : 0u;
        mm_u32 cr = (merged & ~(STM32_GPDMA_CxCR_EN | STM32_GPDMA_CxCR_RESET | keep)) |
                    (ch->cr & keep);

        /* RESET acts only when disabled or suspended and keeps the
         * configuration registers (RM0517 16.8.9). */
        if ((value & STM32_GPDMA_CxCR_RESET) != 0u && (!was_en || was_susp)) {
            ch->cr = cr & ~STM32_GPDMA_CxCR_SUSP;
            ch->sr |= STM32_GPDMA_CxSR_IDLEF;
            ch->ht_done = 0u;
            ch->block_bytes = 0u;
        } else {
            /* Writing EN = 0 is ignored (RM0517 16.8.9). */
            ch->cr = cr | (was_en ? STM32_GPDMA_CxCR_EN : (value & STM32_GPDMA_CxCR_EN));
            if (!was_en && (ch->cr & STM32_GPDMA_CxCR_EN) != 0u) {
                gpdma_enable(d, ch_idx);
            }
            if ((ch->cr & STM32_GPDMA_CxCR_EN) != 0u) {
                if ((ch->cr & STM32_GPDMA_CxCR_SUSP) != 0u) {
                    if (!was_susp) {
                        ch->sr |= STM32_GPDMA_CxSR_IDLEF;
                        gpdma_flag(d, ch_idx, STM32_GPDMA_CxSR_SUSPF);
                    }
                } else if (!was_en || was_susp) {
                    ch->sr &= ~STM32_GPDMA_CxSR_IDLEF;
                    gpdma_start(d, ch_idx);
                }
            }
        }
        gpdma_irq_eval(d, ch_idx, MM_FALSE);
        break;
    }
    case STM32_GPDMA_CxTR1:
        ch->tr1 = merged;
        if (!gpdma_ch_secure(d, ch_idx)) {
            ch->tr1 &= ~(TR1_SSEC | TR1_DSEC);
        }
        break;
    case STM32_GPDMA_CxTR2:
        ch->tr2 = merged;
        break;
    case STM32_GPDMA_CxBR1:
        ch->br1 = merged;
        if ((lanes & BR1_BNDT) != 0u) {
            ch->remaining = merged & BR1_BNDT;
            ch->bndt_reload = ch->remaining;
        }
        break;
    case STM32_GPDMA_CxSAR:
        ch->sar = merged;
        break;
    case STM32_GPDMA_CxDAR:
        ch->dar = merged;
        break;
    case STM32_GPDMA_CxTR3:
        ch->tr3 = merged;
        break;
    case STM32_GPDMA_CxBR2:
        ch->br2 = merged;
        break;
    case STM32_GPDMA_CxLLR:
        ch->llr = merged & ((ch_idx >= (mm_u32)d->first_2d_channel) ?
                           (LLR_MASK_1D | LLR_UT3 | LLR_UB2) : LLR_MASK_1D);
        break;
    default:
        break;
    }
}

static mm_u32 gpdma_channel_mask(const struct stm32_gpdma_state *d)
{
    mm_u32 n = (mm_u32)d->num_channels;
    if (n > STM32_GPDMA_CH_COUNT) n = STM32_GPDMA_CH_COUNT;
    return (1u << n) - 1u;
}

/* SECCFGR/PRIVCFGR bits of enabled or locked channels are read-only
 * (RM0517 16.8.1/16.8.2). */
static mm_u32 gpdma_cfg_ro_mask(const struct stm32_gpdma_state *d)
{
    mm_u32 mask = d->rcfglockr;
    mm_u32 i;
    for (i = 0; i < d->num_channels && i < STM32_GPDMA_CH_COUNT; ++i) {
        if ((d->ch[i].cr & STM32_GPDMA_CxCR_EN) != 0u) {
            mask |= 1u << i;
        }
    }
    return mask;
}

mm_bool stm32_gpdma_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                          mm_u32 value)
{
    struct stm32_gpdma_state *d = (struct stm32_gpdma_state *)opaque;
    mm_bool ns = mmio_active_sec() == MM_NONSECURE;
    mm_u32 ch_mask = gpdma_channel_mask(d);
    mm_u32 byte = offset & 3u;
    mm_u32 lanes;

    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;

    /* RM0517 16.8 asks for 32-bit accesses; narrower ones act on their lanes. */
    if (byte + size_bytes > 4u) return MM_TRUE;
    offset &= ~3u;
    lanes = gpdma_width_mask(size_bytes) << (byte * 8u);
    value = (value << (byte * 8u)) & lanes;
    ch_mask &= lanes;

    if (offset == STM32_GPDMA_SECCFGR) {
        mm_u32 wr_mask = ch_mask & ~gpdma_cfg_ro_mask(d);
        mm_u32 i;
        if (ns) return MM_TRUE;
        d->seccfgr = (d->seccfgr & ~wr_mask) | (value & wr_mask);
        for (i = 0; i < d->num_channels && i < STM32_GPDMA_CH_COUNT; ++i) {
            if (!gpdma_ch_secure(d, i)) {
                d->ch[i].tr1 &= ~(TR1_SSEC | TR1_DSEC);
            }
        }
        gpdma_update_misr(d);
    } else if (offset == STM32_GPDMA_PRIVCFGR) {
        mm_u32 wr_mask = ch_mask & ~gpdma_cfg_ro_mask(d) & (ns ? ~d->seccfgr : ~0u);
        d->privcfgr = (d->privcfgr & ~wr_mask) | (value & wr_mask);
    } else if (offset == STM32_GPDMA_RCFGLOCKR) {
        /* Lock bits can only be set, not cleared */
        if (!ns) d->rcfglockr |= value & ch_mask;
    } else if (offset >= STM32_GPDMA_CH_BASE) {
        mm_u32 ch_region = offset - STM32_GPDMA_CH_BASE;
        mm_u32 ch_idx = ch_region / STM32_GPDMA_CH_STRIDE;
        mm_u32 ch_offset = ch_region % STM32_GPDMA_CH_STRIDE;
        if (ch_idx < d->num_channels && !(ns && gpdma_ch_secure(d, ch_idx))) {
            gpdma_ch_write_reg(d, ch_idx, ch_offset, value, lanes);
        }
    }

    return MM_TRUE;
}

void stm32_dma_router_init(struct stm32_dma_router *r,
                           struct stm32_gpdma_state *dma0, struct stm32_gpdma_state *dma1)
{
    if (r == 0) {
        return;
    }
    memset(r, 0, sizeof(*r));
    if (dma0 != 0) {
        dma0->router = r;
        r->dma[r->dma_count++] = dma0;
    }
    if (dma1 != 0) {
        dma1->router = r;
        r->dma[r->dma_count++] = dma1;
    }
}

void stm32_dma_router_bind(struct stm32_dma_router *r, mm_u32 line,
                           stm32_dma_ready_fn ready, void *ctx)
{
    if (r == 0 || line >= STM32_DMA_REQ_LINES) {
        return;
    }
    r->ready[line] = ready;
    r->ready_ctx[line] = ctx;
}

enum stm32_dma_req_result stm32_dma_request(struct stm32_dma_router *r, mm_u32 line)
{
    struct stm32_gpdma_state *hit = 0;
    mm_u32 hit_ch = 0;
    mm_u32 matches = 0;
    mm_u32 i;
    mm_u32 c;
    mm_u32 prev_line;
    mm_bool linked = MM_FALSE;
    enum stm32_dma_req_result res;

    if (r == 0 || line >= STM32_DMA_REQ_LINES) {
        return STM32_DMA_REQ_NONE;
    }
    for (i = 0; i < r->dma_count; ++i) {
        struct stm32_gpdma_state *d = r->dma[i];
        for (c = 0; c < d->num_channels && c < STM32_GPDMA_CH_COUNT; ++c) {
            const struct stm32_gpdma_channel *ch = &d->ch[c];
            if (!gpdma_ch_running(ch) || (ch->tr2 & TR2_SWREQ) != 0u ||
                TR2_REQSEL(ch->tr2) != line) {
                continue;
            }
            if (matches++ == 0u) {
                hit = d;
                hit_ch = c;
            }
        }
    }
    if (hit == 0) {
        return STM32_DMA_REQ_NONE;
    }
    if (matches > 1u) {
        gpdma_warn(hit, WARN_DUP_REQ, "several enabled channels share one REQSEL, the first serves");
    }
    prev_line = r->serving_line;
    r->serving_line = line;
    r->depth++;
    res = gpdma_beat(hit, hit_ch, &linked);
    if (linked) {
        gpdma_start(hit, hit_ch);
    }
    r->depth--;
    r->serving_line = prev_line;
    return res;
}

/* Raw copy of the registers and channel state: bump the region version when
 * stm32_gpdma_channel changes. Router, NVIC and IRQ wiring are not saved. */
struct gpdma_snapshot {
    mm_u32 seccfgr;
    mm_u32 privcfgr;
    mm_u32 rcfglockr;
    mm_u32 misr;
    mm_u32 smisr;
    mm_u32 warned;
    mm_u32 irq_held;
    struct stm32_gpdma_channel ch[STM32_GPDMA_CH_COUNT];
};

mm_bool stm32_gpdma_save(void *opaque, struct mm_snapshot_writer *w)
{
    struct stm32_gpdma_state *d = (struct stm32_gpdma_state *)opaque;
    struct gpdma_snapshot snap;
    if (d == 0 || w == 0) {
        return MM_FALSE;
    }
    memset(&snap, 0, sizeof(snap));
    snap.seccfgr = d->seccfgr;
    snap.privcfgr = d->privcfgr;
    snap.rcfglockr = d->rcfglockr;
    snap.misr = d->misr;
    snap.smisr = d->smisr;
    snap.warned = d->warned;
    snap.irq_held = d->irq_held;
    memcpy(snap.ch, d->ch, sizeof(snap.ch));
    return mm_snapshot_write(w, &snap, (mm_u32)sizeof(snap));
}

static mm_bool gpdma_snapshot_valid(const struct gpdma_snapshot *snap)
{
    mm_u32 i;
    for (i = 0; i < STM32_GPDMA_CH_COUNT; ++i) {
        const struct stm32_gpdma_channel *ch = &snap->ch[i];
        if (ch->remaining > BR1_BNDT || ch->bndt_reload > BR1_BNDT ||
            ch->block_bytes > BR1_BNDT || ch->ht_done > 1u) {
            return MM_FALSE;
        }
        if ((ch->cr & STM32_GPDMA_CxCR_EN) != 0u && ch->remaining > ch->block_bytes) {
            return MM_FALSE;
        }
    }
    return MM_TRUE;
}

mm_bool stm32_gpdma_load(void *opaque, struct mm_snapshot_reader *r)
{
    struct stm32_gpdma_state *d = (struct stm32_gpdma_state *)opaque;
    struct gpdma_snapshot snap;
    mm_u32 ch_mask;
    if (d == 0 || r == 0) {
        return MM_FALSE;
    }
    if ((r->size - r->offset) != (mm_u32)sizeof(snap)) {
        return MM_FALSE;
    }
    if (!mm_snapshot_read(r, &snap, (mm_u32)sizeof(snap)) || !gpdma_snapshot_valid(&snap)) {
        return MM_FALSE;
    }
    ch_mask = gpdma_channel_mask(d);
    d->seccfgr = snap.seccfgr & ch_mask;
    d->privcfgr = snap.privcfgr & ch_mask;
    d->rcfglockr = snap.rcfglockr & ch_mask;
    d->misr = snap.misr & ch_mask;
    d->smisr = snap.smisr & ch_mask;
    d->warned = snap.warned;
    d->irq_held = snap.irq_held & ch_mask;
    memcpy(d->ch, snap.ch, sizeof(d->ch));
    return MM_TRUE;
}
