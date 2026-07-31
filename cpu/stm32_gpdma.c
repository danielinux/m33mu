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

#include <string.h>
#include "stm32_gpdma.h"
#include "m33mu/memmap.h"

static void gpdma_execute_transfer(struct stm32_gpdma_state *d, mm_u32 ch_idx);

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
    for (i = 0; i < d->num_channels; ++i) {
        memset(&d->ch[i], 0, sizeof(d->ch[i]));
        d->ch[i].sr = STM32_GPDMA_CxSR_IDLEF;
    }
}

void stm32_gpdma_set_nvic(struct stm32_gpdma_state *d, struct mm_nvic *nvic)
{
    d->nvic = nvic;
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
    case STM32_GPDMA_CxBR1:  return ch->br1;
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
    mm_u32 v = 0;

    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;

    if (offset == STM32_GPDMA_SECCFGR) {
        v = d->seccfgr;
    } else if (offset == STM32_GPDMA_PRIVCFGR) {
        v = d->privcfgr;
    } else if (offset == STM32_GPDMA_RCFGLOCKR) {
        v = d->rcfglockr;
    } else if (offset == STM32_GPDMA_MISR) {
        v = d->misr;
    } else if (offset == STM32_GPDMA_SMISR) {
        v = d->smisr;
    } else if (offset >= STM32_GPDMA_CH_BASE) {
        mm_u32 ch_region = offset - STM32_GPDMA_CH_BASE;
        mm_u32 ch_idx = ch_region / STM32_GPDMA_CH_STRIDE;
        mm_u32 ch_offset = ch_region % STM32_GPDMA_CH_STRIDE;
        if (ch_idx < d->num_channels) {
            v = gpdma_ch_read_reg(&d->ch[ch_idx], ch_offset);
        }
    }

    *value_out = v;
    return MM_TRUE;
}

static void gpdma_ch_write_reg(struct stm32_gpdma_state *d, mm_u32 ch_idx,
                               mm_u32 ch_offset, mm_u32 value)
{
    struct stm32_gpdma_channel *ch = &d->ch[ch_idx];

    switch (ch_offset) {
    case STM32_GPDMA_CxLBAR:
        ch->lbar = value & 0xFFFF0000u; /* bits 31:16 */
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
        break;
    case STM32_GPDMA_CxCR:
        if (value & STM32_GPDMA_CxCR_RESET) {
            /* Channel reset */
            memset(ch, 0, sizeof(*ch));
            ch->sr = STM32_GPDMA_CxSR_IDLEF;
        } else {
            mm_bool was_enabled = (ch->cr & STM32_GPDMA_CxCR_EN) != 0u;
            ch->cr = value;
            if (!was_enabled && (value & STM32_GPDMA_CxCR_EN)) {
                /* Channel just enabled - start transfer */
                ch->sr &= ~STM32_GPDMA_CxSR_IDLEF;
                ch->remaining = ch->br1 & 0xFFFFu;
                gpdma_execute_transfer(d, ch_idx);
            }
            if (value & STM32_GPDMA_CxCR_SUSP) {
                ch->sr |= STM32_GPDMA_CxSR_SUSPF;
            }
        }
        break;
    case STM32_GPDMA_CxTR1:
        ch->tr1 = value;
        break;
    case STM32_GPDMA_CxTR2:
        ch->tr2 = value;
        break;
    case STM32_GPDMA_CxBR1:
        ch->br1 = value;
        ch->remaining = value & 0xFFFFu;
        break;
    case STM32_GPDMA_CxSAR:
        ch->sar = value;
        break;
    case STM32_GPDMA_CxDAR:
        ch->dar = value;
        break;
    case STM32_GPDMA_CxTR3:
        ch->tr3 = value;
        break;
    case STM32_GPDMA_CxBR2:
        ch->br2 = value;
        break;
    case STM32_GPDMA_CxLLR:
        ch->llr = value;
        break;
    default:
        break;
    }
}

/* Execute DMA transfer for a channel (memory-to-memory supported) */
static void gpdma_execute_transfer(struct stm32_gpdma_state *d, mm_u32 ch_idx)
{
    struct stm32_gpdma_channel *ch = &d->ch[ch_idx];
    struct mm_memmap *map = mm_memmap_current();
    mm_u32 src_width, dst_width, count;
    mm_u32 sinc, dinc;
    mm_u32 sar, dar;
    mm_u32 i;

    if ((ch->cr & STM32_GPDMA_CxCR_EN) == 0u) {
        return;
    }

    /* TR1: bits 1:0 = SDW (source data width), bits 17:16 = DDW (dest width) */
    src_width = 1u << ((ch->tr1 >> 0) & 0x3u);
    dst_width = 1u << ((ch->tr1 >> 16) & 0x3u);
    /* TR1: bit 3 = SINC, bit 19 = DINC */
    sinc = (ch->tr1 & (1u << 3)) ? src_width : 0u;
    dinc = (ch->tr1 & (1u << 19)) ? dst_width : 0u;

    count = ch->remaining;
    sar = ch->sar;
    dar = ch->dar;

    /* Execute transfer (simplified: no burst, no request signal) */
    for (i = 0; i < count && (ch->cr & STM32_GPDMA_CxCR_SUSP) == 0u; ) {
        mm_u32 val = 0;
        mm_u32 xfer_size = (src_width < dst_width) ? src_width : dst_width;

        if (!mm_memmap_read(map, MM_NONSECURE, sar, xfer_size, &val)) {
            ch->sr |= STM32_GPDMA_CxSR_DTEF;
            break;
        }
        if (!mm_memmap_write(map, MM_NONSECURE, dar, xfer_size, val)) {
            ch->sr |= STM32_GPDMA_CxSR_DTEF;
            break;
        }

        sar += sinc;
        dar += dinc;
        i += xfer_size;

        /* Half transfer check */
        if (i >= count / 2 && (ch->sr & STM32_GPDMA_CxSR_HTF) == 0u) {
            ch->sr |= STM32_GPDMA_CxSR_HTF;
            if ((ch->cr & STM32_GPDMA_CxCR_HTIE) && d->nvic) {
                mm_nvic_set_pending(d->nvic, gpdma_channel_irq(d, ch_idx), MM_TRUE);
            }
        }
    }

    ch->sar = sar;
    ch->dar = dar;
    ch->remaining = count - i;

    if (ch->remaining == 0 && (ch->sr & STM32_GPDMA_CxSR_DTEF) == 0u) {
        /* Transfer complete */
        ch->sr |= STM32_GPDMA_CxSR_TCF | STM32_GPDMA_CxSR_IDLEF;
        ch->cr &= ~STM32_GPDMA_CxCR_EN;
        d->misr |= (1u << ch_idx);

        if ((ch->cr & STM32_GPDMA_CxCR_TCIE) && d->nvic) {
            mm_nvic_set_pending(d->nvic, gpdma_channel_irq(d, ch_idx), MM_TRUE);
        }

        /* Linked list handling */
        if (ch->llr != 0u) {
            mm_u32 lli_addr = ch->lbar | ((ch->llr >> 2) << 2);
            mm_u32 lli_data[4];
            mm_u32 lli_idx;
            mm_bool lli_ok = MM_TRUE;

            for (lli_idx = 0; lli_idx < 4 && lli_ok; ++lli_idx) {
                if (!mm_memmap_read(map, MM_NONSECURE,
                                    lli_addr + lli_idx * 4, 4, &lli_data[lli_idx])) {
                    lli_ok = MM_FALSE;
                }
            }

            if (lli_ok) {
                ch->tr1 = lli_data[0];
                ch->tr2 = lli_data[1];
                ch->br1 = lli_data[2];
                ch->sar = lli_data[3];
                ch->remaining = ch->br1 & 0xFFFFu;
                ch->sr &= ~STM32_GPDMA_CxSR_IDLEF;
                ch->cr |= STM32_GPDMA_CxCR_EN;
                gpdma_execute_transfer(d, ch_idx);
            }
        }
    }
}

mm_bool stm32_gpdma_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                          mm_u32 value)
{
    struct stm32_gpdma_state *d = (struct stm32_gpdma_state *)opaque;

    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;

    if (offset == STM32_GPDMA_SECCFGR) {
        d->seccfgr = value;
    } else if (offset == STM32_GPDMA_PRIVCFGR) {
        d->privcfgr = value;
    } else if (offset == STM32_GPDMA_RCFGLOCKR) {
        /* Lock bits can only be set, not cleared */
        d->rcfglockr |= value;
    } else if (offset >= STM32_GPDMA_CH_BASE) {
        mm_u32 ch_region = offset - STM32_GPDMA_CH_BASE;
        mm_u32 ch_idx = ch_region / STM32_GPDMA_CH_STRIDE;
        mm_u32 ch_offset = ch_region % STM32_GPDMA_CH_STRIDE;
        if (ch_idx < d->num_channels) {
            gpdma_ch_write_reg(d, ch_idx, ch_offset, value);
        }
    }

    return MM_TRUE;
}
