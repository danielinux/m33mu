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
#include "m33mu/memmap.h"
#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "m33mu/snapshot.h"
#include "stm32_gpdma.h"

#define GP1       0x40020000u
#define GP1_S     0x50020000u
#define GP2       0x40021000u
#define PERIPH    0x40022000u
#define RAM_NS    0x20000000u
#define RAM_S     0x30000000u
#define RAM_SIZE  0x10000u
#define UNMAPPED  0x60000000u
#define IRQ1      27u

#define CH(c, r)  (STM32_GPDMA_CH_BASE + STM32_GPDMA_CH_STRIDE * (c) + (r))

#define SDW_B     0u
#define DDW_B     0u
#define SDW_H     1u
#define SDW_W     2u
#define SDW_BAD   3u
#define DDW_H     (1u << 16)
#define DDW_W     (2u << 16)
#define DDW_BAD   (3u << 16)
#define SINC      (1u << 3)
#define DINC      (1u << 19)
#define SSEC      (1u << 15)
#define DSEC      (1u << 31)
#define WW        (SDW_W | DDW_W)
#define SWREQ     (1u << 9)
#define TCEM(x)   ((mm_u32)(x) << 30)

#define UT1       (1u << 31)
#define UT2       (1u << 30)
#define UB1       (1u << 29)
#define USA       (1u << 28)
#define UDA       (1u << 27)
#define UT3       (1u << 26)
#define UB2       (1u << 25)
#define ULL       (1u << 16)
#define LA(a)     ((mm_u32)(a) & 0xFFFCu)
#define ALL_U     (UT1 | UT2 | UB1 | USA | UDA | ULL)

#define EN        STM32_GPDMA_CxCR_EN
#define SUSP      STM32_GPDMA_CxCR_SUSP
#define RESET     STM32_GPDMA_CxCR_RESET
#define TCIE      STM32_GPDMA_CxCR_TCIE
#define HTIE      STM32_GPDMA_CxCR_HTIE
#define USEIE     STM32_GPDMA_CxCR_USEIE
#define SUSPIE    STM32_GPDMA_CxCR_SUSPIE
#define IDLEF     STM32_GPDMA_CxSR_IDLEF
#define TCF       STM32_GPDMA_CxSR_TCF
#define HTF       STM32_GPDMA_CxSR_HTF
#define DTEF      STM32_GPDMA_CxSR_DTEF
#define ULEF      STM32_GPDMA_CxSR_ULEF
#define USEF      STM32_GPDMA_CxSR_USEF
#define SUSPF     STM32_GPDMA_CxSR_SUSPF

#define SERVED    STM32_DMA_REQ_SERVED
#define LAST      STM32_DMA_REQ_LAST
#define NONE      STM32_DMA_REQ_NONE

#define CHECK(c) do { if (!(c)) { printf("  line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static struct mm_memmap g_map;
static struct mmio_region g_regions[8];
static mm_u8 g_ram[RAM_SIZE];
static struct mm_nvic g_nvic;
static struct stm32_gpdma_state g_d1;
static struct stm32_gpdma_state g_d2;
static struct stm32_dma_router g_r;

static mm_u32 g_p_value;
static int g_p_reads;
static enum mm_sec_state g_p_read_sec;
static int g_p_writes;
static mm_u32 g_p_written;
static enum mm_sec_state g_p_write_sec;
static int g_intercepts;
static int g_ready_calls;

/* Returns the whole word whatever the size, like a 32-bit peripheral register. */
static mm_bool periph_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    (void)opaque;
    (void)offset;
    (void)size_bytes;
    g_p_reads++;
    g_p_read_sec = mmio_active_sec();
    *value_out = g_p_value++;
    return MM_TRUE;
}

static mm_bool periph_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    (void)opaque;
    (void)offset;
    (void)size_bytes;
    g_p_writes++;
    g_p_written = value;
    g_p_write_sec = mmio_active_sec();
    return MM_TRUE;
}

static mm_bool deny_all(void *opaque, enum mm_access_type type, enum mm_sec_state sec,
                        mm_u32 addr, mm_u32 size)
{
    (void)opaque;
    (void)type;
    (void)sec;
    (void)addr;
    (void)size;
    g_intercepts++;
    return MM_FALSE;
}

static void on_ready(void *ctx)
{
    (void)ctx;
    g_ready_calls++;
}

static void on_ready_count(void *ctx)
{
    (*(int *)ctx)++;
}

static void add_region(mm_u32 base, mm_u32 size, void *opaque, mmio_read_fn rd, mmio_write_fn wr)
{
    struct mmio_region reg;
    memset(&reg, 0, sizeof(reg));
    reg.base = base;
    reg.size = size;
    reg.opaque = opaque;
    reg.read = rd;
    reg.write = wr;
    (void)mmio_bus_register_region(&g_map.mmio, &reg);
}

static void setup(void)
{
    struct mm_target_cfg cfg;

    memset(&cfg, 0, sizeof(cfg));
    memset(g_ram, 0, sizeof(g_ram));
    memset(&g_d1, 0, sizeof(g_d1));
    memset(&g_d2, 0, sizeof(g_d2));
    cfg.ram_base_s = RAM_S;
    cfg.ram_size_s = RAM_SIZE;
    cfg.ram_base_ns = RAM_NS;
    cfg.ram_size_ns = RAM_SIZE;
    mm_memmap_init(&g_map, g_regions, sizeof(g_regions) / sizeof(g_regions[0]));
    (void)mm_memmap_configure_ram(&g_map, &cfg, g_ram, MM_TRUE);
    add_region(GP1, 0x1000u, &g_d1, stm32_gpdma_read, stm32_gpdma_write);
    add_region(GP1_S, 0x1000u, &g_d1, stm32_gpdma_read, stm32_gpdma_write);
    add_region(GP2, 0x1000u, &g_d2, stm32_gpdma_read, stm32_gpdma_write);
    add_region(PERIPH, 0x100u, 0, periph_read, periph_write);
    mm_memmap_set_interceptor(&g_map, deny_all, 0);
    mm_nvic_init(&g_nvic);

    g_d1.instance = 0;
    g_d1.num_channels = 8;
    g_d1.irq_base = IRQ1;
    g_d1.first_2d_channel = 6;
    g_d2.instance = 1;
    g_d2.num_channels = 8;
    g_d2.irq_base = 90;
    g_d2.first_2d_channel = 6;
    stm32_gpdma_set_nvic(&g_d1, &g_nvic);
    stm32_gpdma_set_nvic(&g_d2, &g_nvic);
    stm32_gpdma_reset(&g_d1);
    stm32_gpdma_reset(&g_d2);
    stm32_dma_router_init(&g_r, &g_d1, &g_d2);

    g_p_value = 0;
    g_p_reads = 0;
    g_p_read_sec = MM_SECURE;
    g_p_writes = 0;
    g_p_written = 0;
    g_p_write_sec = MM_SECURE;
    g_intercepts = 0;
    g_ready_calls = 0;
    mmio_set_active_sec(MM_SECURE);
}

static void wr(struct stm32_gpdma_state *d, mm_u32 off, mm_u32 v)
{
    (void)stm32_gpdma_write(d, off, 4u, v);
}

static mm_u32 rd(struct stm32_gpdma_state *d, mm_u32 off)
{
    mm_u32 v = 0;
    (void)stm32_gpdma_read(d, off, 4u, &v);
    return v;
}

static void wr_n(struct stm32_gpdma_state *d, mm_u32 off, mm_u32 size, mm_u32 v)
{
    (void)stm32_gpdma_write(d, off, size, v);
}

static mm_u32 rd_n(struct stm32_gpdma_state *d, mm_u32 off, mm_u32 size)
{
    mm_u32 v = 0xDEADBEEFu;
    (void)stm32_gpdma_read(d, off, size, &v);
    return v;
}

static mm_u32 ram_rd(mm_u32 addr)
{
    mm_u32 o = addr - RAM_NS;
    return (mm_u32)g_ram[o] | ((mm_u32)g_ram[o + 1u] << 8) |
           ((mm_u32)g_ram[o + 2u] << 16) | ((mm_u32)g_ram[o + 3u] << 24);
}

static void ram_wr(mm_u32 addr, mm_u32 v)
{
    mm_u32 o = addr - RAM_NS;
    g_ram[o] = (mm_u8)(v & 0xFFu);
    g_ram[o + 1u] = (mm_u8)((v >> 8) & 0xFFu);
    g_ram[o + 2u] = (mm_u8)((v >> 16) & 0xFFu);
    g_ram[o + 3u] = (mm_u8)((v >> 24) & 0xFFu);
}

static void prog(struct stm32_gpdma_state *d, mm_u32 c, mm_u32 tr1, mm_u32 tr2, mm_u32 bndt,
                 mm_u32 sar, mm_u32 dar, mm_u32 llr)
{
    wr(d, CH(c, STM32_GPDMA_CxLBAR), RAM_NS);
    wr(d, CH(c, STM32_GPDMA_CxTR1), tr1);
    wr(d, CH(c, STM32_GPDMA_CxTR2), tr2);
    wr(d, CH(c, STM32_GPDMA_CxBR1), bndt);
    wr(d, CH(c, STM32_GPDMA_CxSAR), sar);
    wr(d, CH(c, STM32_GPDMA_CxDAR), dar);
    wr(d, CH(c, STM32_GPDMA_CxLLR), llr);
}

static void enable(struct stm32_gpdma_state *d, mm_u32 c, mm_u32 cr_bits)
{
    wr(d, CH(c, STM32_GPDMA_CxCR), cr_bits | EN);
}

static mm_u32 sr(struct stm32_gpdma_state *d, mm_u32 c)
{
    return rd(d, CH(c, STM32_GPDMA_CxSR));
}

static mm_u32 bndt(struct stm32_gpdma_state *d, mm_u32 c)
{
    return rd(d, CH(c, STM32_GPDMA_CxBR1)) & 0xFFFFu;
}

static mm_bool enabled(struct stm32_gpdma_state *d, mm_u32 c)
{
    return (rd(d, CH(c, STM32_GPDMA_CxCR)) & EN) != 0u;
}

static enum stm32_dma_req_result req(mm_u32 line)
{
    return stm32_dma_request(&g_r, line);
}

static int test_swreq_block_copy(void)
{
    mm_u32 i;
    setup();
    for (i = 0; i < 4u; ++i) {
        ram_wr(RAM_NS + 0x100u + 4u * i, 0xA0u + i);
    }
    prog(&g_d1, 0, WW | SINC | DINC, SWREQ, 16, RAM_NS + 0x100u, RAM_NS + 0x200u, 0);
    enable(&g_d1, 0, 0);
    for (i = 0; i < 4u; ++i) {
        CHECK(ram_rd(RAM_NS + 0x200u + 4u * i) == 0xA0u + i);
    }
    CHECK((sr(&g_d1, 0) & (TCF | HTF | IDLEF)) == (TCF | HTF | IDLEF));
    CHECK(!enabled(&g_d1, 0));
    CHECK(bndt(&g_d1, 0) == 0u);
    CHECK(rd(&g_d1, CH(0, STM32_GPDMA_CxSAR)) == RAM_NS + 0x110u);
    CHECK(g_intercepts == 0);
    return 0;
}

static int test_swreq_self_linked_bound(void)
{
    setup();
    ram_wr(RAM_NS + 0x100u, 0x5Au);
    prog(&g_d1, 0, WW, SWREQ, 4, RAM_NS + 0x100u, RAM_NS + 0x200u, LA(0x300));
    enable(&g_d1, 0, 0);
    CHECK(enabled(&g_d1, 0));
    CHECK(g_d1.warned != 0u);
    CHECK(bndt(&g_d1, 0) == 4u);
    CHECK(ram_rd(RAM_NS + 0x200u) == 0x5Au);
    return 0;
}

static int test_one_beat_per_request(void)
{
    setup();
    g_p_value = 0x100u;
    prog(&g_d1, 0, WW | DINC, 5u, 12, PERIPH, RAM_NS + 0x200u, 0);
    enable(&g_d1, 0, 0);
    CHECK(g_p_reads == 0);
    CHECK(req(5) == SERVED);
    CHECK(g_p_reads == 1);
    CHECK(ram_rd(RAM_NS + 0x200u) == 0x100u);
    CHECK(bndt(&g_d1, 0) == 8u);
    CHECK(req(5) == SERVED);
    CHECK(req(5) == LAST);
    CHECK(ram_rd(RAM_NS + 0x208u) == 0x102u);
    CHECK(!enabled(&g_d1, 0));
    CHECK((sr(&g_d1, 0) & (TCF | IDLEF)) == (TCF | IDLEF));
    CHECK(req(5) == NONE);
    CHECK(g_p_reads == 3);
    return 0;
}

static int test_none_cases(void)
{
    setup();
    CHECK(req(5) == NONE);
    prog(&g_d1, 0, WW | DINC, 6u, 8, PERIPH, RAM_NS + 0x200u, 0);
    enable(&g_d1, 0, 0);
    CHECK(req(5) == NONE);
    prog(&g_d1, 1, WW, SWREQ | 5u, 4, RAM_NS + 0x100u, RAM_NS + 0x300u, LA(0x380));
    enable(&g_d1, 1, 0);
    CHECK(enabled(&g_d1, 1));
    CHECK(req(5) == NONE);
    CHECK(bndt(&g_d1, 1) == 4u);
    wr(&g_d1, CH(0, STM32_GPDMA_CxCR), EN | SUSP);
    CHECK(req(6) == NONE);
    CHECK(req(STM32_DMA_REQ_LINES) == NONE);
    CHECK(stm32_dma_request(0, 6) == NONE);
    CHECK(g_p_reads == 0);
    return 0;
}

static int test_duplicate_reqsel(void)
{
    setup();
    prog(&g_d2, 0, WW | DINC, 7u, 8, PERIPH, RAM_NS + 0x300u, 0);
    enable(&g_d2, 0, 0);
    prog(&g_d1, 3, WW | DINC, 7u, 8, PERIPH, RAM_NS + 0x400u, 0);
    enable(&g_d1, 3, 0);
    prog(&g_d1, 1, WW | DINC, 7u, 8, PERIPH, RAM_NS + 0x200u, 0);
    enable(&g_d1, 1, 0);
    CHECK(req(7) == SERVED);
    CHECK(bndt(&g_d1, 1) == 4u);
    CHECK(bndt(&g_d1, 3) == 8u);
    CHECK(bndt(&g_d2, 0) == 8u);
    CHECK(g_d1.warned != 0u);
    CHECK(g_d2.warned == 0u);
    return 0;
}

static int test_ready_calls(void)
{
    setup();
    stm32_dma_router_bind(&g_r, 5, on_ready, 0);
    prog(&g_d1, 0, WW | DINC, 5u, 8, PERIPH, RAM_NS + 0x200u, LA(0x300));
    enable(&g_d1, 0, 0);
    CHECK(g_ready_calls == 1);
    CHECK(req(5) == SERVED);
    CHECK(req(5) == SERVED);
    CHECK(enabled(&g_d1, 0));
    CHECK(bndt(&g_d1, 0) == 8u);
    CHECK(g_ready_calls == 1);
    wr(&g_d1, CH(0, STM32_GPDMA_CxCR), EN | SUSP);
    CHECK(g_ready_calls == 1);
    wr(&g_d1, CH(0, STM32_GPDMA_CxCR), EN);
    CHECK(g_ready_calls == 2);
    CHECK((sr(&g_d1, 0) & IDLEF) == 0u);
    wr(&g_d1, CH(0, STM32_GPDMA_CxCR), EN | SUSP);
    wr(&g_d1, CH(0, STM32_GPDMA_CxCR), EN | SUSP | RESET);

    /* a memory-to-memory LLI hands the channel over to request line 5 */
    ram_wr(RAM_NS + 0x400u, 5u);
    ram_wr(RAM_NS + 0x404u, 4u);
    ram_wr(RAM_NS + 0x408u, PERIPH);
    ram_wr(RAM_NS + 0x40Cu, RAM_NS + 0x500u);
    prog(&g_d1, 1, WW | SINC | DINC, SWREQ, 4, RAM_NS + 0x100u, RAM_NS + 0x180u,
         UT2 | UB1 | USA | UDA | LA(0x400));
    enable(&g_d1, 1, 0);
    CHECK(g_ready_calls == 3);
    CHECK(enabled(&g_d1, 1));
    CHECK((rd(&g_d1, CH(1, STM32_GPDMA_CxTR2)) & SWREQ) == 0u);
    CHECK(rd(&g_d1, CH(1, STM32_GPDMA_CxSAR)) == PERIPH);
    return 0;
}

static int test_ready_other_line(void)
{
    int ready7 = 0;
    setup();
    stm32_dma_router_bind(&g_r, 5, on_ready, 0);
    stm32_dma_router_bind(&g_r, 7, on_ready_count, &ready7);
    ram_wr(RAM_NS + 0x300u, 7u);
    ram_wr(RAM_NS + 0x304u, 4u);
    prog(&g_d1, 0, WW | DINC, 5u, 4, PERIPH, RAM_NS + 0x200u, UT2 | UB1 | LA(0x300));
    enable(&g_d1, 0, 0);
    CHECK(g_ready_calls == 1);
    CHECK(req(5) == SERVED);
    CHECK((rd(&g_d1, CH(0, STM32_GPDMA_CxTR2)) & 0xFFu) == 7u);
    CHECK(ready7 == 1);
    CHECK(req(5) == NONE);
    CHECK(req(7) == SERVED);
    CHECK(ready7 == 1);
    CHECK(g_ready_calls == 1);
    CHECK(g_p_reads == 2);
    return 0;
}

static int test_llr_write_mask(void)
{
    setup();
    ram_wr(RAM_NS + 0x100u, 0x77u);
    prog(&g_d1, 0, WW, SWREQ, 4, RAM_NS + 0x100u, RAM_NS + 0x200u, 0x00060003u);
    CHECK(rd(&g_d1, CH(0, STM32_GPDMA_CxLLR)) == 0u);
    enable(&g_d1, 0, 0);
    CHECK(!enabled(&g_d1, 0));
    CHECK((sr(&g_d1, 0) & (TCF | IDLEF)) == (TCF | IDLEF));
    CHECK(ram_rd(RAM_NS + 0x200u) == 0x77u);

    wr(&g_d1, CH(0, STM32_GPDMA_CxLLR), UT3 | UB2 | LA(0x300));
    CHECK(rd(&g_d1, CH(0, STM32_GPDMA_CxLLR)) == LA(0x300));
    wr(&g_d1, CH(6, STM32_GPDMA_CxLLR), UT3 | UB2 | (1u << 24) | LA(0x300));
    CHECK(rd(&g_d1, CH(6, STM32_GPDMA_CxLLR)) == (UT3 | UB2 | LA(0x300)));
    return 0;
}

static int test_llr_update_order(void)
{
    mm_u32 tr1v = WW | DINC;
    setup();
    ram_wr(RAM_NS + 0x300u, tr1v);
    ram_wr(RAM_NS + 0x304u, 9u);
    ram_wr(RAM_NS + 0x308u, 8u);
    ram_wr(RAM_NS + 0x30Cu, PERIPH);
    ram_wr(RAM_NS + 0x310u, RAM_NS + 0x600u);
    ram_wr(RAM_NS + 0x314u, UB1 | LA(0x340));
    prog(&g_d1, 0, 0, 9u, 0, 0, 0, ALL_U | LA(0x300));
    enable(&g_d1, 0, 0);
    CHECK(enabled(&g_d1, 0));
    CHECK((sr(&g_d1, 0) & (IDLEF | USEF)) == 0u);
    CHECK(rd(&g_d1, CH(0, STM32_GPDMA_CxTR1)) == tr1v);
    CHECK(rd(&g_d1, CH(0, STM32_GPDMA_CxTR2)) == 9u);
    CHECK(bndt(&g_d1, 0) == 8u);
    CHECK(rd(&g_d1, CH(0, STM32_GPDMA_CxSAR)) == PERIPH);
    CHECK(rd(&g_d1, CH(0, STM32_GPDMA_CxDAR)) == RAM_NS + 0x600u);
    CHECK(rd(&g_d1, CH(0, STM32_GPDMA_CxLLR)) == (UB1 | LA(0x340)));

    /* only the selected words are read, ULL = 0 keeps CxLLR */
    ram_wr(RAM_NS + 0x380u, 4u);
    ram_wr(RAM_NS + 0x384u, PERIPH);
    prog(&g_d1, 1, WW, 10u, 0, 0, RAM_NS + 0x700u, UB1 | USA | LA(0x380));
    enable(&g_d1, 1, 0);
    CHECK(enabled(&g_d1, 1));
    CHECK(bndt(&g_d1, 1) == 4u);
    CHECK(rd(&g_d1, CH(1, STM32_GPDMA_CxSAR)) == PERIPH);
    CHECK(rd(&g_d1, CH(1, STM32_GPDMA_CxTR1)) == WW);
    CHECK(rd(&g_d1, CH(1, STM32_GPDMA_CxLLR)) == (UB1 | USA | LA(0x380)));

    /* 2D channel: TR3 and BR2 sit between DAR and LLR */
    ram_wr(RAM_NS + 0x3C0u, WW);
    ram_wr(RAM_NS + 0x3C4u, 11u);
    ram_wr(RAM_NS + 0x3C8u, 8u);
    ram_wr(RAM_NS + 0x3CCu, PERIPH);
    ram_wr(RAM_NS + 0x3D0u, RAM_NS + 0x800u);
    ram_wr(RAM_NS + 0x3D4u, 0x11u);
    ram_wr(RAM_NS + 0x3D8u, 0x22u);
    ram_wr(RAM_NS + 0x3DCu, 0u);
    prog(&g_d1, 6, 0, 11u, 0, 0, 0, ALL_U | UT3 | UB2 | LA(0x3C0));
    enable(&g_d1, 6, 0);
    CHECK(enabled(&g_d1, 6));
    CHECK(rd(&g_d1, CH(6, STM32_GPDMA_CxTR3)) == 0x11u);
    CHECK(rd(&g_d1, CH(6, STM32_GPDMA_CxBR2)) == 0x22u);
    CHECK(rd(&g_d1, CH(6, STM32_GPDMA_CxLLR)) == 0u);

    /* 1D channel: UT3/UB2 consume no word */
    ram_wr(RAM_NS + 0x400u, 4u);
    ram_wr(RAM_NS + 0x404u, PERIPH);
    ram_wr(RAM_NS + 0x408u, UB1 | LA(0x440));
    ram_wr(RAM_NS + 0x40Cu, UB1 | LA(0x480));
    ram_wr(RAM_NS + 0x410u, UB1 | LA(0x4C0));
    prog(&g_d1, 2, WW, 12u, 0, 0, RAM_NS + 0x900u, UB1 | USA | UT3 | UB2 | ULL | LA(0x400));
    enable(&g_d1, 2, 0);
    CHECK(enabled(&g_d1, 2));
    CHECK(rd(&g_d1, CH(2, STM32_GPDMA_CxSAR)) == PERIPH);
    CHECK(rd(&g_d1, CH(2, STM32_GPDMA_CxLLR)) == (UB1 | LA(0x440)));
    CHECK(rd(&g_d1, CH(2, STM32_GPDMA_CxTR3)) == 0u);
    return 0;
}

static int test_ub1_restore(void)
{
    setup();
    ram_wr(RAM_NS + 0x300u, PERIPH);
    prog(&g_d1, 0, WW | DINC, 5u, 8, PERIPH, RAM_NS + 0x200u, USA | LA(0x300));
    enable(&g_d1, 0, 0);
    CHECK(req(5) == SERVED);
    CHECK(req(5) == SERVED);
    CHECK(enabled(&g_d1, 0));
    CHECK(bndt(&g_d1, 0) == 8u);
    CHECK(rd(&g_d1, CH(0, STM32_GPDMA_CxDAR)) == RAM_NS + 0x208u);
    CHECK((sr(&g_d1, 0) & TCF) != 0u);
    CHECK(req(5) == SERVED);
    CHECK(ram_rd(RAM_NS + 0x208u) == 2u);
    return 0;
}

static void hal_node(mm_u32 node, mm_u32 llr)
{
    ram_wr(RAM_NS + node, WW | DINC);
    ram_wr(RAM_NS + node + 4u, 0u);
    ram_wr(RAM_NS + node + 8u, 16u);
    ram_wr(RAM_NS + node + 12u, PERIPH);
    ram_wr(RAM_NS + node + 16u, RAM_NS + 0x200u);
    ram_wr(RAM_NS + node + 20u, llr);
}

static int test_hal_start(void)
{
    mm_u32 i;
    setup();
    stm32_dma_router_bind(&g_r, 0, on_ready, 0);
    hal_node(0x300u, 0u);
    prog(&g_d1, 0, 0, 0, 0, 0, 0, ALL_U | LA(0x300));
    g_p_value = 0x500u;
    enable(&g_d1, 0, TCIE | HTIE);
    CHECK(g_ready_calls == 1);
    CHECK((sr(&g_d1, 0) & (TCF | HTF)) == 0u);
    CHECK(req(0) == SERVED);
    CHECK((sr(&g_d1, 0) & HTF) == 0u);
    CHECK(req(0) == SERVED);
    CHECK((sr(&g_d1, 0) & HTF) != 0u);
    CHECK(req(0) == SERVED);
    CHECK(req(0) == LAST);
    for (i = 0; i < 4u; ++i) {
        CHECK(ram_rd(RAM_NS + 0x200u + 4u * i) == 0x500u + i);
    }
    CHECK((sr(&g_d1, 0) & TCF) != 0u);
    CHECK(!enabled(&g_d1, 0));
    CHECK(mm_nvic_is_pending(&g_nvic, IRQ1));
    return 0;
}

static int test_circular(void)
{
    mm_u32 lap;
    mm_u32 i;
    setup();
    hal_node(0x300u, ALL_U | LA(0x300));
    prog(&g_d1, 0, 0, 0, 0, 0, 0, ALL_U | LA(0x300));
    enable(&g_d1, 0, TCIE | HTIE);
    for (lap = 0; lap < 2u; ++lap) {
        CHECK(req(0) == SERVED);
        CHECK(req(0) == SERVED);
        CHECK((sr(&g_d1, 0) & (HTF | TCF)) == HTF);
        CHECK(req(0) == SERVED);
        CHECK(req(0) == SERVED);
        CHECK((sr(&g_d1, 0) & TCF) != 0u);
        CHECK(enabled(&g_d1, 0));
        CHECK(bndt(&g_d1, 0) == 16u);
        CHECK(rd(&g_d1, CH(0, STM32_GPDMA_CxDAR)) == RAM_NS + 0x200u);
        wr(&g_d1, CH(0, STM32_GPDMA_CxFCR), TCF | HTF);
        CHECK(rd(&g_d1, STM32_GPDMA_MISR) == 0u);
    }
    for (i = 0; i < 4u; ++i) {
        CHECK(ram_rd(RAM_NS + 0x200u + 4u * i) == 4u + i);
    }
    return 0;
}

static int test_tcem_flags(void)
{
    setup();
    /* TCEM 00: TCF at the block end, before a failing link */
    prog(&g_d1, 0, WW, TCEM(0) | 5u, 4, PERIPH, RAM_NS + 0x200u, USA | LA(0x300));
    wr(&g_d1, CH(0, STM32_GPDMA_CxLBAR), UNMAPPED);
    enable(&g_d1, 0, 0);
    CHECK(req(5) == SERVED);
    CHECK((sr(&g_d1, 0) & (TCF | ULEF | IDLEF)) == (TCF | ULEF | IDLEF));
    CHECK(!enabled(&g_d1, 0));

    /* TCEM 01 on a 1D channel: same as 00 */
    prog(&g_d2, 0, WW, TCEM(1) | 11u, 4, PERIPH, RAM_NS + 0x200u, USA | LA(0x300));
    wr(&g_d2, CH(0, STM32_GPDMA_CxLBAR), UNMAPPED);
    enable(&g_d2, 0, 0);
    CHECK(req(11) == SERVED);
    CHECK((sr(&g_d2, 0) & (TCF | ULEF | IDLEF)) == (TCF | ULEF | IDLEF));

    /* TCEM 10: TCF only once the next LLI is loaded */
    prog(&g_d1, 1, WW, TCEM(2) | 6u, 4, PERIPH, RAM_NS + 0x200u, USA | LA(0x300));
    wr(&g_d1, CH(1, STM32_GPDMA_CxLBAR), UNMAPPED);
    enable(&g_d1, 1, 0);
    CHECK(req(6) == SERVED);
    CHECK((sr(&g_d1, 1) & (TCF | ULEF)) == ULEF);
    ram_wr(RAM_NS + 0x300u, PERIPH);
    prog(&g_d1, 2, WW, TCEM(2) | 7u, 4, PERIPH, RAM_NS + 0x200u, USA | LA(0x300));
    enable(&g_d1, 2, 0);
    CHECK(req(7) == SERVED);
    CHECK((sr(&g_d1, 2) & (TCF | ULEF)) == TCF);

    /* TCEM 11: HTF/TCF only in the last LLI */
    ram_wr(RAM_NS + 0x340u, 8u);
    ram_wr(RAM_NS + 0x344u, 0u);
    prog(&g_d1, 3, WW, TCEM(3) | 8u, 8, PERIPH, RAM_NS + 0x200u, UB1 | ULL | LA(0x340));
    enable(&g_d1, 3, 0);
    CHECK(req(8) == SERVED);
    CHECK(req(8) == SERVED);
    CHECK((sr(&g_d1, 3) & (TCF | HTF)) == 0u);
    CHECK(req(8) == SERVED);
    CHECK((sr(&g_d1, 3) & (TCF | HTF)) == HTF);
    CHECK(req(8) == LAST);
    CHECK((sr(&g_d1, 3) & TCF) != 0u);

    /* null LLI0 with TCEM 10: TCF once LLI1 is loaded, no HTF */
    ram_wr(RAM_NS + 0x380u, 4u);
    prog(&g_d1, 4, WW, TCEM(2) | 9u, 0, PERIPH, RAM_NS + 0x200u, UB1 | LA(0x380));
    enable(&g_d1, 4, 0);
    CHECK(enabled(&g_d1, 4));
    CHECK((sr(&g_d1, 4) & (TCF | HTF)) == TCF);

    /* null LLI0 with TCEM 00: no flag for LLI0 */
    prog(&g_d1, 5, WW, TCEM(0) | 10u, 0, PERIPH, RAM_NS + 0x200u, UB1 | LA(0x380));
    enable(&g_d1, 5, 0);
    CHECK(enabled(&g_d1, 5));
    CHECK((sr(&g_d1, 5) & (TCF | HTF)) == 0u);
    return 0;
}

static int test_byte_beats_ht_rounding(void)
{
    mm_u32 i;
    setup();
    g_p_value = 0x41u;
    prog(&g_d1, 0, SDW_B | DDW_B | DINC, 5u, 5, PERIPH, RAM_NS + 0x200u, 0);
    enable(&g_d1, 0, HTIE);
    CHECK(req(5) == SERVED);
    CHECK(req(5) == SERVED);
    CHECK((sr(&g_d1, 0) & HTF) == 0u);
    CHECK(req(5) == SERVED);
    CHECK((sr(&g_d1, 0) & (HTF | TCF)) == HTF);
    CHECK(req(5) == SERVED);
    CHECK(req(5) == LAST);
    CHECK((sr(&g_d1, 0) & (TCF | IDLEF)) == (TCF | IDLEF));
    for (i = 0; i < 5u; ++i) {
        CHECK(g_ram[0x200u + i] == (mm_u8)(0x41u + i));
    }
    CHECK(g_ram[0x205u] == 0u);
    CHECK(rd(&g_d1, CH(0, STM32_GPDMA_CxDAR)) == RAM_NS + 0x205u);
    return 0;
}

static int check_usef(mm_u32 tr1, mm_u32 bndt_v, mm_u32 sar, mm_u32 dar, mm_u32 llr)
{
    wr(&g_d1, CH(2, STM32_GPDMA_CxCR), RESET);
    wr(&g_d1, CH(2, STM32_GPDMA_CxFCR), 0x7F00u);
    CHECK(sr(&g_d1, 2) == IDLEF);
    prog(&g_d1, 2, tr1, 5u, bndt_v, sar, dar, llr);
    enable(&g_d1, 2, USEIE);
    CHECK((sr(&g_d1, 2) & (USEF | IDLEF)) == (USEF | IDLEF));
    CHECK(!enabled(&g_d1, 2));
    CHECK(mm_nvic_is_pending(&g_nvic, IRQ1 + 2u));
    mm_nvic_set_pending(&g_nvic, IRQ1 + 2u, MM_FALSE);
    return 0;
}

static int test_errors(void)
{
    setup();
    prog(&g_d1, 0, WW, SWREQ, 4, UNMAPPED, RAM_NS + 0x200u, 0);
    enable(&g_d1, 0, 0);
    CHECK((sr(&g_d1, 0) & (DTEF | IDLEF)) == (DTEF | IDLEF));
    CHECK(!enabled(&g_d1, 0));

    prog(&g_d1, 1, WW, 5u, 4, PERIPH, UNMAPPED, 0);
    enable(&g_d1, 1, 0);
    CHECK(req(5) == NONE);
    CHECK((sr(&g_d1, 1) & (DTEF | IDLEF)) == (DTEF | IDLEF));

    prog(&g_d1, 3, WW, 6u, 0, PERIPH, RAM_NS, UB1 | LA(0));
    wr(&g_d1, CH(3, STM32_GPDMA_CxLBAR), UNMAPPED);
    enable(&g_d1, 3, 0);
    CHECK((sr(&g_d1, 3) & (ULEF | IDLEF)) == (ULEF | IDLEF));
    CHECK(!enabled(&g_d1, 3));

    if (check_usef(SDW_BAD | DDW_W, 4, RAM_NS, RAM_NS + 0x200u, 0) != 0) return 1;
    if (check_usef(SDW_W | DDW_BAD, 4, RAM_NS, RAM_NS + 0x200u, 0) != 0) return 1;
    if (check_usef(WW, 4, RAM_NS + 2u, RAM_NS + 0x200u, 0) != 0) return 1;
    if (check_usef(SDW_W | DDW_H, 4, RAM_NS, RAM_NS + 0x201u, 0) != 0) return 1;
    if (check_usef(WW, 6, RAM_NS, RAM_NS + 0x200u, 0) != 0) return 1;
    if (check_usef(WW, 0, RAM_NS, RAM_NS + 0x200u, 0) != 0) return 1;
    ram_wr(RAM_NS + 0x300u, SDW_BAD);
    ram_wr(RAM_NS + 0x304u, 4u);
    if (check_usef(WW, 0, RAM_NS, RAM_NS + 0x200u, UT1 | UB1 | LA(0x300)) != 0) return 1;
    /* BNDT = 0 with UB1 = 0: no link transfer, even from a bad LLI address */
    if (check_usef(WW, 0, RAM_NS, RAM_NS + 0x200u, USA | LA(0x300)) != 0) return 1;
    prog(&g_d1, 4, WW, 5u, 0, RAM_NS, RAM_NS + 0x200u, UT1 | USA | LA(0x300));
    wr(&g_d1, CH(4, STM32_GPDMA_CxLBAR), UNMAPPED);
    enable(&g_d1, 4, 0);
    CHECK((sr(&g_d1, 4) & (USEF | ULEF | IDLEF)) == (USEF | IDLEF));
    CHECK(rd(&g_d1, CH(4, STM32_GPDMA_CxTR1)) == WW);
    /* LLI1 or a later LLI loading BNDT = 0 */
    ram_wr(RAM_NS + 0x320u, 0u);
    if (check_usef(WW, 0, RAM_NS, RAM_NS + 0x200u, UB1 | LA(0x320)) != 0) return 1;
    prog(&g_d1, 5, WW, SWREQ, 4, RAM_NS, RAM_NS + 0x200u, UB1 | LA(0x320));
    enable(&g_d1, 5, USEIE);
    CHECK((sr(&g_d1, 5) & (TCF | USEF | IDLEF)) == (TCF | USEF | IDLEF));
    CHECK(!enabled(&g_d1, 5));
    CHECK(mm_nvic_is_pending(&g_nvic, IRQ1 + 5u));
    CHECK(g_intercepts == 0);
    return 0;
}

static int test_pam_widths(void)
{
    setup();
    ram_wr(RAM_NS + 0x100u, 0x1234ABCDu);
    ram_wr(RAM_NS + 0x200u, 0xFFFFFFFFu);
    ram_wr(RAM_NS + 0x204u, 0xFFFFFFFFu);
    prog(&g_d1, 0, SDW_H | DDW_W | SINC | DINC, SWREQ, 4, RAM_NS + 0x100u, RAM_NS + 0x200u, 0);
    enable(&g_d1, 0, 0);
    CHECK(ram_rd(RAM_NS + 0x200u) == 0x0000ABCDu);
    CHECK(ram_rd(RAM_NS + 0x204u) == 0x00001234u);
    CHECK(rd(&g_d1, CH(0, STM32_GPDMA_CxDAR)) == RAM_NS + 0x208u);

    ram_wr(RAM_NS + 0x110u, 0x11223344u);
    ram_wr(RAM_NS + 0x114u, 0x55667788u);
    ram_wr(RAM_NS + 0x300u, 0xFFFFFFFFu);
    ram_wr(RAM_NS + 0x304u, 0xFFFFFFFFu);
    prog(&g_d1, 1, SDW_W | DDW_H | SINC | DINC, SWREQ, 8, RAM_NS + 0x110u, RAM_NS + 0x300u, 0);
    enable(&g_d1, 1, 0);
    CHECK(ram_rd(RAM_NS + 0x300u) == 0x77883344u);
    CHECK(ram_rd(RAM_NS + 0x304u) == 0xFFFFFFFFu);

    g_p_value = 0xDEAD1234u;
    prog(&g_d1, 2, SDW_H | DDW_W | DINC, 5u, 2, PERIPH, RAM_NS + 0x400u, 0);
    enable(&g_d1, 2, 0);
    CHECK(req(5) == LAST);
    CHECK(ram_rd(RAM_NS + 0x400u) == 0x00001234u);
    return 0;
}

static int test_security(void)
{
    mm_u32 v = 0;
    setup();
    CHECK(!mm_memmap_read(&g_map, MM_NONSECURE, RAM_NS, 4u, &v));
    CHECK(g_intercepts == 1);
    g_intercepts = 0;

    prog(&g_d1, 0, WW | DINC, 5u, 8, PERIPH, RAM_NS + 0x200u, 0);
    enable(&g_d1, 0, 0);
    CHECK(req(5) == SERVED);
    CHECK(g_p_read_sec == MM_NONSECURE);

    wr(&g_d1, STM32_GPDMA_SECCFGR, 1u << 1);
    prog(&g_d1, 1, WW | DINC | SSEC, 6u, 8, PERIPH, RAM_NS + 0x300u, 0);
    CHECK((rd(&g_d1, CH(1, STM32_GPDMA_CxTR1)) & SSEC) != 0u);
    enable(&g_d1, 1, 0);
    CHECK(req(6) == SERVED);
    CHECK(g_p_read_sec == MM_SECURE);

    wr(&g_d1, STM32_GPDMA_SECCFGR, (1u << 1) | (1u << 2));
    ram_wr(RAM_NS + 0x100u, 0xC0DEu);
    prog(&g_d1, 2, WW | SINC | DSEC, SWREQ, 4, RAM_NS + 0x100u, PERIPH, 0);
    enable(&g_d1, 2, 0);
    CHECK(g_p_writes == 1);
    CHECK(g_p_written == 0xC0DEu);
    CHECK(g_p_write_sec == MM_SECURE);

    prog(&g_d1, 3, WW | SSEC | DSEC, 7u, 4, PERIPH, RAM_NS, 0);
    CHECK((rd(&g_d1, CH(3, STM32_GPDMA_CxTR1)) & (SSEC | DSEC)) == 0u);
    enable(&g_d1, 3, 0);
    CHECK(req(7) == LAST);
    CHECK(g_p_read_sec == MM_NONSECURE);

    wr(&g_d1, CH(1, STM32_GPDMA_CxCR), EN | SUSP);
    wr(&g_d1, CH(1, STM32_GPDMA_CxCR), EN | SUSP | RESET);
    wr(&g_d1, STM32_GPDMA_SECCFGR, 1u << 1);
    prog(&g_d1, 1, WW | SSEC, 6u, 4, PERIPH, RAM_NS, 0);
    CHECK((rd(&g_d1, CH(1, STM32_GPDMA_CxTR1)) & SSEC) != 0u);
    wr(&g_d1, STM32_GPDMA_SECCFGR, 0u);
    CHECK((rd(&g_d1, CH(1, STM32_GPDMA_CxTR1)) & SSEC) == 0u);

    /* link transfers carry the channel security */
    wr(&g_d1, STM32_GPDMA_SECCFGR, 1u << 4);
    prog(&g_d1, 4, WW, 9u, 0, 0, RAM_NS, UB1 | USA | LA(PERIPH - GP1));
    wr(&g_d1, CH(4, STM32_GPDMA_CxLBAR), GP1);
    g_p_reads = 0;
    enable(&g_d1, 4, 0);
    CHECK(g_p_reads == 2);
    CHECK(g_p_read_sec == MM_SECURE);
    prog(&g_d1, 5, WW, 10u, 0, 0, RAM_NS, UB1 | USA | LA(PERIPH - GP1));
    wr(&g_d1, CH(5, STM32_GPDMA_CxLBAR), GP1);
    enable(&g_d1, 5, 0);
    CHECK(g_p_reads == 4);
    CHECK(g_p_read_sec == MM_NONSECURE);

    /* non-secure agents: secure channels read as zero and ignore writes */
    wr(&g_d1, STM32_GPDMA_SECCFGR, (1u << 4) | (1u << 6));
    prog(&g_d1, 6, WW | SSEC, 12u, 4, PERIPH, RAM_NS, 0);
    mmio_set_active_sec(MM_NONSECURE);
    CHECK(rd(&g_d1, CH(6, STM32_GPDMA_CxTR1)) == 0u);
    CHECK(rd(&g_d1, CH(6, STM32_GPDMA_CxSAR)) == 0u);
    CHECK(rd(&g_d1, STM32_GPDMA_SECCFGR) == ((1u << 4) | (1u << 6)));
    wr(&g_d1, CH(6, STM32_GPDMA_CxSAR), RAM_NS);
    enable(&g_d1, 6, 0);
    wr(&g_d1, STM32_GPDMA_SECCFGR, 1u << 7);
    wr(&g_d1, STM32_GPDMA_RCFGLOCKR, 1u << 7);
    wr(&g_d1, STM32_GPDMA_PRIVCFGR, 0xFFu);
    wr(&g_d1, CH(7, STM32_GPDMA_CxSAR), RAM_NS + 4u);
    CHECK(rd(&g_d1, CH(7, STM32_GPDMA_CxSAR)) == RAM_NS + 4u);
    CHECK(rd(&g_d1, STM32_GPDMA_SECCFGR) == ((1u << 4) | (1u << 6)));
    CHECK(rd(&g_d1, STM32_GPDMA_RCFGLOCKR) == 0u);
    CHECK((rd(&g_d1, STM32_GPDMA_PRIVCFGR) & 0xC0u) == 0x80u);
    mmio_set_active_sec(MM_SECURE);
    CHECK(rd(&g_d1, CH(6, STM32_GPDMA_CxSAR)) == PERIPH);
    CHECK((rd(&g_d1, CH(6, STM32_GPDMA_CxTR1)) & SSEC) != 0u);
    CHECK(!enabled(&g_d1, 6));

    CHECK(g_intercepts == 0);
    return 0;
}

static int test_cfg_locked_while_enabled(void)
{
    setup();
    wr(&g_d1, STM32_GPDMA_SECCFGR, 1u << 1);
    wr(&g_d1, STM32_GPDMA_PRIVCFGR, 1u << 1);
    prog(&g_d1, 1, WW | DINC | SSEC, 6u, 8, PERIPH, RAM_NS + 0x300u, 0);
    enable(&g_d1, 1, 3u << 22);
    wr(&g_d1, STM32_GPDMA_SECCFGR, 1u << 3);
    wr(&g_d1, STM32_GPDMA_PRIVCFGR, 1u << 3);
    CHECK(rd(&g_d1, STM32_GPDMA_SECCFGR) == ((1u << 1) | (1u << 3)));
    CHECK(rd(&g_d1, STM32_GPDMA_PRIVCFGR) == ((1u << 1) | (1u << 3)));
    CHECK((rd(&g_d1, CH(1, STM32_GPDMA_CxTR1)) & SSEC) != 0u);
    CHECK(req(6) == SERVED);
    CHECK(g_p_read_sec == MM_SECURE);

    wr(&g_d1, CH(1, STM32_GPDMA_CxCR), EN | (1u << 16));
    CHECK((rd(&g_d1, CH(1, STM32_GPDMA_CxCR)) & 0x00FF0000u) == (3u << 22));
    wr(&g_d1, CH(2, STM32_GPDMA_CxCR), 3u << 22);
    CHECK(rd(&g_d1, CH(2, STM32_GPDMA_CxCR)) == (3u << 22));

    /* locked channels keep SECx/PRIVx even while disabled */
    wr(&g_d1, STM32_GPDMA_SECCFGR, (1u << 3) | (1u << 4));
    wr(&g_d1, STM32_GPDMA_RCFGLOCKR, 0xFFFF0000u | (1u << 4));
    CHECK(rd(&g_d1, STM32_GPDMA_RCFGLOCKR) == (1u << 4));
    wr(&g_d1, STM32_GPDMA_RCFGLOCKR, 0u);
    CHECK(rd(&g_d1, STM32_GPDMA_RCFGLOCKR) == (1u << 4));
    wr(&g_d1, STM32_GPDMA_SECCFGR, 0u);
    CHECK(rd(&g_d1, STM32_GPDMA_SECCFGR) == ((1u << 1) | (1u << 4)));
    wr(&g_d1, STM32_GPDMA_SECCFGR, 0xFFFFFFFFu);
    CHECK(rd(&g_d1, STM32_GPDMA_SECCFGR) == 0xFFu);
    wr(&g_d1, STM32_GPDMA_PRIVCFGR, 0xFFFFFFFFu);
    CHECK(rd(&g_d1, STM32_GPDMA_PRIVCFGR) == 0xEFu);
    return 0;
}

static int test_active_sec_restored(void)
{
    setup();
    prog(&g_d1, 0, WW | DINC, 5u, 8, PERIPH, RAM_NS + 0x200u, 0);
    enable(&g_d1, 0, 0);
    mmio_set_active_sec(MM_SECURE);
    CHECK(req(5) == SERVED);
    CHECK(g_p_read_sec == MM_NONSECURE);
    CHECK(mmio_active_sec() == MM_SECURE);

    wr(&g_d1, STM32_GPDMA_SECCFGR, 1u << 1);
    prog(&g_d1, 1, WW | DINC | SSEC, 6u, 8, PERIPH, RAM_NS + 0x300u, 0);
    enable(&g_d1, 1, 0);
    mmio_set_active_sec(MM_NONSECURE);
    CHECK(req(6) == SERVED);
    CHECK(g_p_read_sec == MM_SECURE);
    CHECK(mmio_active_sec() == MM_NONSECURE);
    return 0;
}

static int test_suspend_reset_abort(void)
{
    setup();
    prog(&g_d1, 0, WW | DINC, 5u, 16, PERIPH, RAM_NS + 0x200u, 0);
    enable(&g_d1, 0, SUSPIE);
    CHECK(req(5) == SERVED);
    wr(&g_d1, CH(0, STM32_GPDMA_CxCR), SUSPIE);
    CHECK(enabled(&g_d1, 0));
    CHECK(req(5) == SERVED);
    wr(&g_d1, CH(0, STM32_GPDMA_CxCR), EN | SUSPIE | SUSP);
    CHECK((sr(&g_d1, 0) & (SUSPF | IDLEF)) == (SUSPF | IDLEF));
    CHECK(enabled(&g_d1, 0));
    CHECK(mm_nvic_is_pending(&g_nvic, IRQ1));
    CHECK(req(5) == NONE);
    CHECK(bndt(&g_d1, 0) == 8u);
    wr(&g_d1, CH(0, STM32_GPDMA_CxCR), EN | SUSPIE | SUSP | RESET);
    CHECK(rd(&g_d1, CH(0, STM32_GPDMA_CxCR)) == SUSPIE);
    CHECK(sr(&g_d1, 0) == (HTF | SUSPF | IDLEF));
    CHECK(rd(&g_d1, CH(0, STM32_GPDMA_CxTR1)) == (WW | DINC));
    CHECK(rd(&g_d1, CH(0, STM32_GPDMA_CxTR2)) == 5u);
    CHECK(rd(&g_d1, CH(0, STM32_GPDMA_CxLBAR)) == RAM_NS);
    CHECK(req(5) == NONE);

    /* HAL restart: only BR1, SAR, DAR are reprogrammed */
    wr(&g_d1, CH(0, STM32_GPDMA_CxFCR), 0x7F00u);
    wr(&g_d1, CH(0, STM32_GPDMA_CxBR1), 8u);
    wr(&g_d1, CH(0, STM32_GPDMA_CxSAR), PERIPH);
    wr(&g_d1, CH(0, STM32_GPDMA_CxDAR), RAM_NS + 0x400u);
    g_p_value = 0x900u;
    wr(&g_d1, CH(0, STM32_GPDMA_CxCR), SUSPIE | EN);
    CHECK(req(5) == SERVED);
    CHECK(req(5) == LAST);
    CHECK(ram_rd(RAM_NS + 0x400u) == 0x900u);
    CHECK(ram_rd(RAM_NS + 0x404u) == 0x901u);

    /* RESET on an active, not suspended channel has no effect */
    prog(&g_d1, 1, WW | DINC, 6u, 8, PERIPH, RAM_NS + 0x500u, 0);
    enable(&g_d1, 1, 0);
    wr(&g_d1, CH(1, STM32_GPDMA_CxCR), EN | RESET);
    CHECK(enabled(&g_d1, 1));
    CHECK(req(6) == SERVED);
    CHECK(bndt(&g_d1, 1) == 4u);
    return 0;
}

static int test_misr_smisr(void)
{
    setup();
    wr(&g_d1, STM32_GPDMA_SECCFGR, 1u << 1);
    prog(&g_d1, 0, WW | SINC | DINC, SWREQ, 8, RAM_NS + 0x100u, RAM_NS + 0x200u, 0);
    enable(&g_d1, 0, TCIE);
    prog(&g_d1, 1, WW | SINC | DINC, SWREQ, 8, RAM_NS + 0x100u, RAM_NS + 0x300u, 0);
    enable(&g_d1, 1, TCIE);
    prog(&g_d1, 2, WW | SINC | DINC, SWREQ, 8, RAM_NS + 0x100u, RAM_NS + 0x400u, 0);
    enable(&g_d1, 2, 0);
    CHECK((sr(&g_d1, 2) & TCF) != 0u);
    CHECK(rd(&g_d1, STM32_GPDMA_MISR) == 1u);
    CHECK(rd(&g_d1, STM32_GPDMA_SMISR) == 2u);
    mmio_set_active_sec(MM_NONSECURE);
    CHECK(rd(&g_d1, STM32_GPDMA_SMISR) == 0u && rd(&g_d1, STM32_GPDMA_MISR) == 1u);
    mmio_set_active_sec(MM_SECURE);
    CHECK(mm_nvic_is_pending(&g_nvic, IRQ1));
    CHECK(mm_nvic_is_pending(&g_nvic, IRQ1 + 1u));
    CHECK(!mm_nvic_is_pending(&g_nvic, IRQ1 + 2u));
    wr(&g_d1, CH(0, STM32_GPDMA_CxFCR), TCF);
    CHECK(rd(&g_d1, STM32_GPDMA_MISR) == 0u);
    wr(&g_d1, CH(1, STM32_GPDMA_CxFCR), TCF);
    CHECK(rd(&g_d1, STM32_GPDMA_SMISR) == 0u);
    return 0;
}

static int test_save_load_mid_block(void)
{
    static mm_u8 buf[4096];
    struct mm_snapshot_writer w;
    struct mm_snapshot_reader r;
    setup();
    prog(&g_d1, 0, WW | DINC, 5u, 16, PERIPH, RAM_NS + 0x200u, 0);
    enable(&g_d1, 0, HTIE);
    CHECK(req(5) == SERVED);
    CHECK(req(5) == SERVED);
    CHECK((sr(&g_d1, 0) & HTF) != 0u);
    mm_snapshot_writer_init(&w, buf, (mm_u32)sizeof(buf));
    CHECK(stm32_gpdma_save(&g_d1, &w));
    CHECK(req(5) == SERVED);
    CHECK(req(5) == LAST);
    CHECK(!enabled(&g_d1, 0));

    mm_snapshot_reader_init(&r, buf, mm_snapshot_bytes_used(&w) - 4u);
    CHECK(!stm32_gpdma_load(&g_d1, &r));
    mm_snapshot_reader_init(&r, buf, mm_snapshot_bytes_used(&w));
    CHECK(stm32_gpdma_load(&g_d1, &r));
    CHECK(enabled(&g_d1, 0));
    CHECK(bndt(&g_d1, 0) == 8u);
    CHECK((sr(&g_d1, 0) & (HTF | TCF)) == HTF);
    CHECK(rd(&g_d1, CH(0, STM32_GPDMA_CxDAR)) == RAM_NS + 0x208u);
    CHECK(g_d1.router == &g_r);
    CHECK(req(5) == SERVED);
    CHECK(req(5) == LAST);
    CHECK(ram_rd(RAM_NS + 0x20Cu) == 5u);
    return 0;
}

static int test_load_checks(void)
{
    static mm_u8 buf[4096];
    struct mm_snapshot_writer w;
    struct mm_snapshot_reader r;
    setup();
    prog(&g_d1, 0, WW | DINC, 5u, 16, PERIPH, RAM_NS + 0x200u, 0);
    enable(&g_d1, 0, HTIE);
    CHECK(req(5) == SERVED);
    CHECK(bndt(&g_d1, 0) == 12u);

    g_d1.ch[0].remaining = g_d1.ch[0].block_bytes + 4u;
    mm_snapshot_writer_init(&w, buf, (mm_u32)sizeof(buf));
    CHECK(stm32_gpdma_save(&g_d1, &w));
    g_d1.ch[0].remaining = 12u;
    mm_snapshot_reader_init(&r, buf, mm_snapshot_bytes_used(&w));
    CHECK(!stm32_gpdma_load(&g_d1, &r));
    CHECK(enabled(&g_d1, 0) && bndt(&g_d1, 0) == 12u);

    g_d1.seccfgr = 0xFFFFFF00u;
    g_d1.privcfgr = 0xFFFFFF00u;
    g_d1.rcfglockr = 0xFFFFFF00u;
    g_d1.misr = 0xFFFFFF00u;
    g_d1.smisr = 0xFFFFFF00u;
    mm_snapshot_writer_init(&w, buf, (mm_u32)sizeof(buf));
    CHECK(stm32_gpdma_save(&g_d1, &w));
    mm_snapshot_reader_init(&r, buf, mm_snapshot_bytes_used(&w));
    CHECK(stm32_gpdma_load(&g_d1, &r));
    CHECK(rd(&g_d1, STM32_GPDMA_SECCFGR) == 0u && rd(&g_d1, STM32_GPDMA_PRIVCFGR) == 0u);
    CHECK(rd(&g_d1, STM32_GPDMA_RCFGLOCKR) == 0u && rd(&g_d1, STM32_GPDMA_MISR) == 0u);
    CHECK(rd(&g_d1, STM32_GPDMA_SMISR) == 0u);
    CHECK(req(5) == SERVED && bndt(&g_d1, 0) == 8u);
    return 0;
}

static int test_sub_word_writes(void)
{
    mm_u32 cr0 = CH(0, STM32_GPDMA_CxCR);
    mm_u32 cr2 = CH(2, STM32_GPDMA_CxCR);
    mm_u32 fcr1 = CH(1, STM32_GPDMA_CxFCR);
    setup();
    wr(&g_d1, cr0, TCIE | HTIE);
    wr_n(&g_d1, cr0, 1u, 0u);
    CHECK(rd(&g_d1, cr0) == (TCIE | HTIE));
    wr_n(&g_d1, cr0 + 1u, 1u, 0x01u);
    CHECK(rd(&g_d1, cr0) == TCIE);
    wr_n(&g_d1, cr0 + 2u, 2u, 0x00C1u);
    CHECK(rd(&g_d1, cr0) == (TCIE | (3u << 22) | (1u << 16)));
    CHECK(!enabled(&g_d1, 0));

    /* a half-word write of EN enables and keeps the upper half */
    prog(&g_d1, 1, WW | SINC | DINC, SWREQ, 8, RAM_NS + 0x100u, RAM_NS + 0x200u, 0);
    wr(&g_d1, CH(1, STM32_GPDMA_CxCR), 3u << 22);
    wr_n(&g_d1, CH(1, STM32_GPDMA_CxCR), 2u, EN | TCIE);
    CHECK((sr(&g_d1, 1) & (TCF | HTF | IDLEF)) == (TCF | HTF | IDLEF));
    CHECK(rd(&g_d1, CH(1, STM32_GPDMA_CxCR)) == (TCIE | (3u << 22)));
    CHECK(mm_nvic_is_pending(&g_nvic, IRQ1 + 1u));

    /* CxFCR clears only the flags of the byte written */
    wr_n(&g_d1, fcr1, 1u, 0xFFu);
    wr_n(&g_d1, fcr1 + 2u, 2u, 0xFFFFu);
    CHECK((sr(&g_d1, 1) & (TCF | HTF)) == (TCF | HTF));
    wr_n(&g_d1, fcr1 + 1u, 1u, HTF >> 8);
    CHECK((sr(&g_d1, 1) & (TCF | HTF)) == TCF);

    /* writes without byte 0 leave a suspended channel as it is */
    prog(&g_d1, 2, WW | DINC, 5u, 8, PERIPH, RAM_NS + 0x300u, 0);
    enable(&g_d1, 2, 0);
    wr(&g_d1, cr2, EN | SUSP);
    wr_n(&g_d1, cr2 + 2u, 2u, 0x0006u);
    wr_n(&g_d1, cr2 + 1u, 1u, 0u);
    CHECK(rd(&g_d1, cr2) == (EN | SUSP));
    CHECK(req(5) == NONE);

    /* BR1 keeps BNDT unless its bytes are written */
    prog(&g_d1, 3, WW, 6u, 8, PERIPH, RAM_NS + 0x300u, 0);
    wr_n(&g_d1, CH(3, STM32_GPDMA_CxBR1) + 2u, 2u, 0u);
    CHECK(bndt(&g_d1, 3) == 8u);
    wr_n(&g_d1, CH(3, STM32_GPDMA_CxBR1), 1u, 4u);
    CHECK(bndt(&g_d1, 3) == 4u);

    wr(&g_d1, STM32_GPDMA_SECCFGR, 1u << 6);
    wr_n(&g_d1, STM32_GPDMA_SECCFGR + 1u, 1u, 0u);
    CHECK(rd(&g_d1, STM32_GPDMA_SECCFGR) == (1u << 6));
    wr_n(&g_d1, STM32_GPDMA_SECCFGR, 1u, 0x05u);
    CHECK(rd(&g_d1, STM32_GPDMA_SECCFGR) == 0x01u);
    return 0;
}

static int test_sub_word_reads(void)
{
    mm_u32 cr = CH(0, STM32_GPDMA_CxCR);
    setup();
    wr(&g_d1, cr, TCIE | SUSPIE | (3u << 22) | (1u << 16));
    CHECK(rd_n(&g_d1, cr, 1u) == 0u);
    CHECK(rd_n(&g_d1, cr + 1u, 1u) == ((TCIE | SUSPIE) >> 8));
    CHECK(rd_n(&g_d1, cr + 2u, 1u) == 0xC1u);
    CHECK(rd_n(&g_d1, cr + 3u, 1u) == 0u);
    CHECK(rd_n(&g_d1, cr, 2u) == (TCIE | SUSPIE));
    CHECK(rd_n(&g_d1, cr + 2u, 2u) == 0x00C1u);
    CHECK(rd_n(&g_d1, cr + 3u, 2u) == 0u);
    CHECK(rd_n(&g_d1, CH(0, STM32_GPDMA_CxSR), 1u) == IDLEF);
    return 0;
}

static int test_irq_level(void)
{
    mm_u32 cr = CH(0, STM32_GPDMA_CxCR);
    mm_u32 fcr = CH(0, STM32_GPDMA_CxFCR);
    setup();
    prog(&g_d1, 0, WW | SINC | DINC, SWREQ, 8, RAM_NS + 0x100u, RAM_NS + 0x200u, 0);
    enable(&g_d1, 0, 0);
    CHECK((sr(&g_d1, 0) & TCF) != 0u);
    CHECK(!mm_nvic_is_pending(&g_nvic, IRQ1));
    wr(&g_d1, cr, TCIE);
    CHECK(mm_nvic_is_pending(&g_nvic, IRQ1));
    CHECK(rd(&g_d1, STM32_GPDMA_MISR) == 1u);
    mm_nvic_set_pending(&g_nvic, IRQ1, MM_FALSE);
    wr(&g_d1, fcr, TCF);
    CHECK(!mm_nvic_is_pending(&g_nvic, IRQ1));
    CHECK(rd(&g_d1, STM32_GPDMA_MISR) == 0u);

    /* from the active handler: pend while the line stays high, retract when it drops */
    wr(&g_d1, CH(0, STM32_GPDMA_CxBR1), 8u);
    wr(&g_d1, CH(0, STM32_GPDMA_CxSAR), RAM_NS + 0x100u);
    wr(&g_d1, CH(0, STM32_GPDMA_CxDAR), RAM_NS + 0x200u);
    wr(&g_d1, cr, TCIE | EN);
    CHECK(mm_nvic_is_pending(&g_nvic, IRQ1));
    mm_nvic_set_pending(&g_nvic, IRQ1, MM_FALSE);
    mm_nvic_set_active(&g_nvic, IRQ1, MM_TRUE);
    wr(&g_d1, cr, TCIE | HTIE);
    CHECK(mm_nvic_is_pending(&g_nvic, IRQ1));
    wr(&g_d1, fcr, TCF);
    CHECK(mm_nvic_is_pending(&g_nvic, IRQ1));
    wr(&g_d1, fcr, HTF);
    CHECK(!mm_nvic_is_pending(&g_nvic, IRQ1));
    mm_nvic_set_active(&g_nvic, IRQ1, MM_FALSE);
    CHECK(g_d1.irq_held == 0u);

    /* outside the handler the pend latches */
    wr(&g_d1, CH(0, STM32_GPDMA_CxBR1), 8u);
    wr(&g_d1, CH(0, STM32_GPDMA_CxSAR), RAM_NS + 0x100u);
    wr(&g_d1, CH(0, STM32_GPDMA_CxDAR), RAM_NS + 0x200u);
    wr(&g_d1, cr, TCIE | EN);
    CHECK(mm_nvic_is_pending(&g_nvic, IRQ1));
    wr(&g_d1, fcr, TCF | HTF);
    CHECK(mm_nvic_is_pending(&g_nvic, IRQ1));
    mm_nvic_set_pending(&g_nvic, IRQ1, MM_FALSE);
    wr(&g_d1, fcr, TCF);
    CHECK(!mm_nvic_is_pending(&g_nvic, IRQ1));
    return 0;
}

int main(void)
{
    static const struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        { "swreq_block_copy", test_swreq_block_copy },
        { "swreq_self_linked_bound", test_swreq_self_linked_bound },
        { "one_beat_per_request", test_one_beat_per_request },
        { "none_cases", test_none_cases },
        { "duplicate_reqsel", test_duplicate_reqsel },
        { "ready_calls", test_ready_calls },
        { "ready_other_line", test_ready_other_line },
        { "llr_write_mask", test_llr_write_mask },
        { "llr_update_order", test_llr_update_order },
        { "ub1_restore", test_ub1_restore },
        { "hal_start", test_hal_start },
        { "circular", test_circular },
        { "tcem_flags", test_tcem_flags },
        { "byte_beats_ht_rounding", test_byte_beats_ht_rounding },
        { "errors", test_errors },
        { "pam_widths", test_pam_widths },
        { "security", test_security },
        { "cfg_locked_while_enabled", test_cfg_locked_while_enabled },
        { "active_sec_restored", test_active_sec_restored },
        { "suspend_reset_abort", test_suspend_reset_abort },
        { "misr_smisr", test_misr_smisr },
        { "save_load_mid_block", test_save_load_mid_block },
        { "load_checks", test_load_checks },
        { "sub_word_writes", test_sub_word_writes },
        { "sub_word_reads", test_sub_word_reads },
        { "irq_level", test_irq_level },
    };
    size_t i;
    int failures = 0;

    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        int rc = tests[i].fn();
        printf("%-45s %s\n", tests[i].name, rc == 0 ? "PASS" : "FAIL");
        if (rc != 0) failures++;
    }
    return (failures == 0) ? 0 : 1;
}
