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

#ifndef M33MU_STM32_GPDMA_H
#define M33MU_STM32_GPDMA_H

#include "m33mu/types.h"
#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "m33mu/snapshot.h"
#include "stm32_dma_req.h"

/* GPDMA global register offsets */
#define STM32_GPDMA_SECCFGR      0x000u
#define STM32_GPDMA_PRIVCFGR     0x004u
#define STM32_GPDMA_RCFGLOCKR    0x008u
#define STM32_GPDMA_MISR         0x00Cu
#define STM32_GPDMA_SMISR        0x010u

/* Channel layout (16 channels max, each 0x80 bytes starting at 0x50) */
#define STM32_GPDMA_CH_BASE      0x050u
#define STM32_GPDMA_CH_STRIDE    0x080u
#define STM32_GPDMA_CH_COUNT     16u

/* Per-channel register offsets from channel base */
#define STM32_GPDMA_CxLBAR       0x000u
#define STM32_GPDMA_CxFCR        0x00Cu
#define STM32_GPDMA_CxSR         0x010u
#define STM32_GPDMA_CxCR         0x014u
#define STM32_GPDMA_CxTR1        0x040u
#define STM32_GPDMA_CxTR2        0x044u
#define STM32_GPDMA_CxBR1        0x048u
#define STM32_GPDMA_CxSAR        0x04Cu
#define STM32_GPDMA_CxDAR        0x050u
#define STM32_GPDMA_CxTR3        0x054u
#define STM32_GPDMA_CxBR2        0x058u
#define STM32_GPDMA_CxLLR        0x07Cu

/* GPDMA control bits */
#define STM32_GPDMA_CxCR_EN      (1u << 0)
#define STM32_GPDMA_CxCR_RESET   (1u << 1)
#define STM32_GPDMA_CxCR_SUSP    (1u << 2)
#define STM32_GPDMA_CxCR_TCIE    (1u << 8)
#define STM32_GPDMA_CxCR_HTIE    (1u << 9)
#define STM32_GPDMA_CxCR_DTEIE   (1u << 10)
#define STM32_GPDMA_CxCR_ULEIE   (1u << 11)
#define STM32_GPDMA_CxCR_USEIE   (1u << 12)
#define STM32_GPDMA_CxCR_SUSPIE  (1u << 13)
#define STM32_GPDMA_CxCR_TOIE    (1u << 14)

/* GPDMA status bits */
#define STM32_GPDMA_CxSR_IDLEF   (1u << 0)
#define STM32_GPDMA_CxSR_TCF     (1u << 8)
#define STM32_GPDMA_CxSR_HTF     (1u << 9)
#define STM32_GPDMA_CxSR_DTEF    (1u << 10)
#define STM32_GPDMA_CxSR_ULEF    (1u << 11)
#define STM32_GPDMA_CxSR_USEF    (1u << 12)
#define STM32_GPDMA_CxSR_SUSPF   (1u << 13)
#define STM32_GPDMA_CxSR_TOF     (1u << 14)

/* GPDMA flag clear bits in FCR */
#define STM32_GPDMA_CxFCR_TCF    (1u << 8)
#define STM32_GPDMA_CxFCR_HTF    (1u << 9)
#define STM32_GPDMA_CxFCR_DTEF   (1u << 10)
#define STM32_GPDMA_CxFCR_ULEF   (1u << 11)
#define STM32_GPDMA_CxFCR_USEF   (1u << 12)
#define STM32_GPDMA_CxFCR_SUSPF  (1u << 13)
#define STM32_GPDMA_CxFCR_TOF    (1u << 14)

struct stm32_gpdma_channel {
    mm_u32 lbar;
    mm_u32 fcr;
    mm_u32 sr;
    mm_u32 cr;
    mm_u32 tr1;
    mm_u32 tr2;
    mm_u32 br1;
    mm_u32 sar;
    mm_u32 dar;
    mm_u32 tr3;
    mm_u32 br2;
    mm_u32 llr;
    mm_u32 remaining;   /* live BNDT: bytes left in the current block */
    mm_u32 bndt_reload; /* programmed BNDT, restored when LLR.UB1 = 0 */
    mm_u32 block_bytes;
    mm_u32 ht_done;
};

struct stm32_dma_router;

struct stm32_gpdma_state {
    mm_u32 seccfgr;
    mm_u32 privcfgr;
    mm_u32 rcfglockr;
    mm_u32 misr;
    mm_u32 smisr;
    mm_u32 irq_held;   /* channels pended from their active handler */
    struct stm32_gpdma_channel ch[STM32_GPDMA_CH_COUNT];
    struct mm_nvic *nvic;
    mm_u32 irq_base;   /* Base IRQ number for this DMA controller */
    /* Optional per-channel IRQ table, for parts whose GPDMA channel
     * interrupts are not numbered contiguously from irq_base (the
     * STM32H5F4 places channels 8..11 far above channels 0..7). When
     * null, the IRQ number is irq_base + channel index. */
    const mm_u16 *irq_map;
    mm_u8 instance;    /* 0 = GPDMA1, 1 = GPDMA2 */
    mm_u8 num_channels; /* Actual number of channels (8, 12 or 16) */
    mm_u8 first_2d_channel; /* channels >= this one load TR3/BR2 from an LLI */
    mm_u32 warned;
    struct stm32_dma_router *router; /* NULL: no request lines */
};

#define STM32_DMA_REQ_LINES 256u

typedef void (*stm32_dma_ready_fn)(void *ctx);

struct stm32_dma_router {
    struct stm32_gpdma_state *dma[2];
    mm_u32 dma_count;
    mm_u32 depth;                        /* > 0 while serving a request */
    mm_u32 serving_line;                 /* line of the innermost request */
    stm32_dma_ready_fn ready[STM32_DMA_REQ_LINES];
    void *ready_ctx[STM32_DMA_REQ_LINES];
};

void stm32_gpdma_reset(struct stm32_gpdma_state *d);
void stm32_gpdma_set_nvic(struct stm32_gpdma_state *d, struct mm_nvic *nvic);

mm_bool stm32_gpdma_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 *value_out);
mm_bool stm32_gpdma_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                          mm_u32 value);
mm_bool stm32_gpdma_save(void *opaque, struct mm_snapshot_writer *w);
mm_bool stm32_gpdma_load(void *opaque, struct mm_snapshot_reader *r);
/* Registers base (with save/load as name) and its secure alias at base + 0x10000000. */
mm_bool stm32_gpdma_register(struct mmio_bus *bus, struct stm32_gpdma_state *d, mm_u32 base,
                             const char *name);

/* Runs once at MMIO registration; reset leaves the router alone. */
void stm32_dma_router_init(struct stm32_dma_router *r,
                           struct stm32_gpdma_state *dma0, struct stm32_gpdma_state *dma1);
void stm32_dma_router_bind(struct stm32_dma_router *r, mm_u32 line,
                           stm32_dma_ready_fn ready, void *ctx);
/* Serves one beat on the first ready channel with SWREQ = 0 and REQSEL = line. */
enum stm32_dma_req_result stm32_dma_request(struct stm32_dma_router *r, mm_u32 line);

#endif /* M33MU_STM32_GPDMA_H */
