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

/* ADC -> request router -> GPDMA -> RAM on the H5 memory map. */

#include <stdio.h>
#include <string.h>
#include "m33mu/cpu_db.h"
#include "m33mu/memmap.h"
#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "m33mu/signal.h"
#include "m33mu/snapshot.h"
#include "m33mu/target.h"
#include "m33mu/timer.h"
#include "stm32_gpdma.h"
#include "stm32h5_adc.h"
#include "stm32h563/stm32h563_mmio.h"
#include "stm32h533/stm32h533_mmio.h"
#include "stm32h5f4/stm32h5f4_mmio.h"

#define ADC1         0x42028000u
#define ADC_COMMON   0x42028300u
#define ADC3         0x4202D800u
#define ADC3_COMMON  0x4202DB00u

#define ISR     0x00u
#define CR      0x08u
#define CFGR    0x0Cu
#define SQR1    0x30u
#define DR      0x40u
#define CCR     0x08u

#define ADEN     (1u << 0)
#define ADDIS    (1u << 1)
#define ADSTART  (1u << 2)
#define JADSTART (1u << 3)
#define ADSTP    (1u << 4)
#define JADSTP   (1u << 5)
#define ADVREGEN (1u << 28)
#define DEEPPWD  (1u << 29)
#define ADCAL    (1u << 31)
#define RS       (ADCAL | ADEN | ADDIS | JADSTART | JADSTP | ADSTART | ADSTP)

#define ADRDY    (1u << 0)
#define EOC      (1u << 2)
#define EOS      (1u << 3)
#define OVR      (1u << 4)
#define ISR_ALL  0x7FFu

#define DMAEN    (1u << 0)
#define DMACFG   (1u << 1)
#define CONT     (1u << 13)

#define RCC_AHB2ENR    0x44020C8Cu
#define GPIOAEN        (1u << 0)
#define GPIOBEN        (1u << 1)
#define ADCEN          (1u << 10)
#define ADC3EN         (1u << 24)
#define TZSC_SECCFGR3  0x50032418u
#define ADC12SEC       (1u << 14)

#define GP1      0x40020000u
#define GP2      0x40021000u
#define GP1_CH0_IRQ 27u
#define CH(c, r) (STM32_GPDMA_CH_BASE + STM32_GPDMA_CH_STRIDE * (c) + (r))
#define HW_DINC  ((1u << 0) | (1u << 16) | (1u << 19)) /* SDW = DDW = halfword, DINC */
#define UT1      (1u << 31)
#define UT2      (1u << 30)
#define UB1      (1u << 29)
#define USA      (1u << 28)
#define UDA      (1u << 27)
#define ULL      (1u << 16)
#define ALL_U    (UT1 | UT2 | UB1 | USA | UDA | ULL)
#define EN       STM32_GPDMA_CxCR_EN
#define SUSP     STM32_GPDMA_CxCR_SUSP
#define TCIE     STM32_GPDMA_CxCR_TCIE
#define HTIE     STM32_GPDMA_CxCR_HTIE
#define IDLEF    STM32_GPDMA_CxSR_IDLEF
#define TCF      STM32_GPDMA_CxSR_TCF
#define HTF      STM32_GPDMA_CxSR_HTF
#define DTEF     STM32_GPDMA_CxSR_DTEF
#define USEF     STM32_GPDMA_CxSR_USEF
#define SUSPF    STM32_GPDMA_CxSR_SUSPF

/* SQ1..SQ3 = IN0 (PA0), IN1 (PA1), IN3 (PA6); L = 2. */
#define SCAN3    (2u | (0u << 6) | (1u << 12) | (3u << 18))
#define ONE_CH0  0u
#define C0       620u   /* 500 mV */
#define C1       1241u  /* 1000 mV */
#define C3       2482u  /* 2000 mV */
#define C_PB2    1861u  /* 1500 mV */
#define SLOT     60u    /* HCLK/4, 2.5 + 12.5 ADC cycles */

#define RAM_SIZE 0x10000u
#define NO_EVENT ((mm_u64)-1)

#define CHECK(c) do { if (!(c)) { printf("  line %d: %s\n", __LINE__, #c); goto out; } } while (0)

struct part {
    const char *name;
    void (*mmio_reset)(void);
    mm_bool (*register_mmio)(struct mmio_bus *bus);
    void (*gpdma_set_nvic)(struct mm_nvic *nvic);
    mm_bool has_adc3;
};

static const struct part g_parts[] = {
    { "stm32h5f4", mm_stm32h5f4_mmio_reset, mm_stm32h5f4_register_mmio,
      mm_stm32h5f4_gpdma_set_nvic, MM_TRUE },
    { "stm32h563", mm_stm32h563_mmio_reset, mm_stm32h563_register_mmio,
      mm_stm32h563_gpdma_set_nvic, MM_FALSE },
    { "stm32h533", mm_stm32h533_mmio_reset, mm_stm32h533_register_mmio,
      mm_stm32h533_gpdma_set_nvic, MM_FALSE },
};

static const char *g_tmp_path = "/tmp/stm32h5_adc_dma_test_fixture.sigc";

static struct mm_target_cfg g_cfg;
static struct mm_memmap g_map;
static struct mmio_region g_regions[512];
static mm_u8 g_ram[RAM_SIZE];
static struct mm_nvic g_nvic;
static mm_u32 g_base;
static mm_u32 g_buf;
static mm_u32 g_node;

static void wr(mm_u32 addr, mm_u32 v)
{
    (void)mmio_bus_write(&g_map.mmio, addr, 4, v);
}

static mm_u32 rd(mm_u32 addr)
{
    mm_u32 v = 0xDEADBEEFu;
    if (!mmio_bus_read(&g_map.mmio, addr, 4, &v)) return 0xDEADBEEFu;
    return v;
}

static mm_u32 ram16(mm_u32 addr)
{
    mm_u32 o = addr - g_base;
    return (mm_u32)g_ram[o] | ((mm_u32)g_ram[o + 1u] << 8);
}

static void ram_fill16(mm_u32 addr, mm_u32 count, mm_u32 v)
{
    mm_u32 i;
    for (i = 0; i < count; ++i) {
        g_ram[addr - g_base + 2u * i] = (mm_u8)(v & 0xFFu);
        g_ram[addr - g_base + 2u * i + 1u] = (mm_u8)((v >> 8) & 0xFFu);
    }
}

static void ram_wr32(mm_u32 addr, mm_u32 v)
{
    mm_u32 o = addr - g_base;
    g_ram[o] = (mm_u8)(v & 0xFFu);
    g_ram[o + 1u] = (mm_u8)((v >> 8) & 0xFFu);
    g_ram[o + 2u] = (mm_u8)((v >> 16) & 0xFFu);
    g_ram[o + 3u] = (mm_u8)((v >> 24) & 0xFFu);
}

static void tk(mm_u64 cycles)
{
    mm_timer_tick(&g_cfg, cycles);
}

static void put_le(FILE *f, mm_u64 v, int bytes)
{
    int i;
    for (i = 0; i < bytes; ++i) {
        fputc((int)((v >> (8 * i)) & 0xFFu), f);
    }
}

/* Flat traces: 500 mV on PA0, 1000 mV on PA1, 2000 mV on PA6, 1500 mV on PB2. */
static mm_bool bind_inputs(void)
{
    static const struct {
        const char *name;
        mm_i16 mv;
        int bank;
        int pin;
    } tr[] = {
        { "a0", 500, 0, 0 }, { "a1", 1000, 0, 1 }, { "a6", 2000, 0, 6 }, { "b2", 1500, 1, 2 }
    };
    FILE *f;
    char name[MM_SIGNAL_TRACE_NAME_LEN];
    mm_u32 i;
    const mm_u32 n = (mm_u32)(sizeof(tr) / sizeof(tr[0]));

    mm_signal_reset();
    f = fopen(g_tmp_path, "wb");
    if (f == 0) return MM_FALSE;
    fwrite("SIGC", 1, 4, f);
    put_le(f, 1, 4);
    put_le(f, 1, 4);
    put_le(f, n, 4);
    put_le(f, 1, 4);
    put_le(f, 1, 4);
    put_le(f, 1000u, 4);
    for (i = 0; i < n; ++i) {
        memset(name, 0, sizeof(name));
        snprintf(name, sizeof(name), "%s", tr[i].name);
        put_le(f, i + 1u, 4);
        fwrite(name, 1, sizeof(name), f);
        put_le(f, 1, 4);
        put_le(f, 3300, 4);
        put_le(f, (mm_u16)tr[i].mv, 2);
    }
    fclose(f);
    if (!mm_signal_load(g_tmp_path)) return MM_FALSE;
    for (i = 0; i < n; ++i) {
        if (mm_signal_bind(tr[i].name, tr[i].bank, tr[i].pin, MM_SIGNAL_ROLE_GPIO) < 0) {
            return MM_FALSE;
        }
    }
    return MM_TRUE;
}

static void ll_cr(mm_u32 adc, mm_u32 bit)
{
    wr(adc + CR, (rd(adc + CR) & ~RS) | bit);
}

/* CKMODE = HCLK/4, regulator on, ADEN, ADRDY. */
static mm_bool bring_up(mm_u32 adc, mm_u32 common)
{
    wr(adc + CR, rd(adc + CR) & ~(DEEPPWD | RS));
    ll_cr(adc, ADVREGEN);
    wr(common + CCR, (rd(common + CCR) & ~(0x3u << 16)) | (3u << 16));
    ll_cr(adc, ADEN);
    tk(1);
    tk(60);
    return (rd(adc + ISR) & ADRDY) != 0u ? MM_TRUE : MM_FALSE;
}

static void power_up(void)
{
    wr(RCC_AHB2ENR, rd(RCC_AHB2ENR) | GPIOAEN | GPIOBEN | ADCEN);
}

/* The way mm_execution_state brings a part up: SoC reset, MMIO, then timer/ADC init. */
static int setup(const struct part *p)
{
    struct mm_target_cfg ram;

    if (!mm_cpu_lookup(p->name, &g_cfg)) return 1;
    mmio_set_active_sec(MM_SECURE);
    memset(g_ram, 0, sizeof(g_ram));
    mm_memmap_init(&g_map, g_regions, sizeof(g_regions) / sizeof(g_regions[0]));
    mmio_bus_enable_step_hooks(&g_map.mmio);
    ram = g_cfg;
    ram.ram_regions = 0;
    ram.ram_region_count = 0u;
    ram.ram_size_s = RAM_SIZE;
    ram.ram_size_ns = RAM_SIZE;
    if (!mm_memmap_configure_ram(&g_map, &ram, g_ram, MM_TRUE)) return 1;
    g_base = ram.ram_base_ns;
    g_buf = g_base + 0x1000u;
    g_node = g_base + 0x800u;
    p->mmio_reset();
    if (!p->register_mmio(&g_map.mmio)) return 1;
    mm_nvic_init(&g_nvic);
    mm_nvic_set_lines(&g_nvic, g_cfg.irq_lines);
    p->gpdma_set_nvic(&g_nvic);
    mm_timer_reset(&g_cfg);
    mm_timer_init(&g_cfg, &g_map.mmio, &g_nvic);
    if (!bind_inputs()) return 1;
    power_up();
    return bring_up(ADC1, ADC_COMMON) ? 0 : 1;
}

static void teardown(void)
{
    mm_signal_reset();
    remove(g_tmp_path);
    mmio_set_active_sec(MM_SECURE);
}

static void dma_prog(mm_u32 dma, mm_u32 c, mm_u32 line, mm_u32 sar, mm_u32 bytes, mm_u32 dar,
                     mm_u32 llr)
{
    wr(dma + CH(c, STM32_GPDMA_CxLBAR), g_base & 0xFFFF0000u);
    wr(dma + CH(c, STM32_GPDMA_CxTR1), HW_DINC);
    wr(dma + CH(c, STM32_GPDMA_CxTR2), line);
    wr(dma + CH(c, STM32_GPDMA_CxBR1), bytes);
    wr(dma + CH(c, STM32_GPDMA_CxSAR), sar);
    wr(dma + CH(c, STM32_GPDMA_CxDAR), dar);
    wr(dma + CH(c, STM32_GPDMA_CxLLR), llr);
}

static void dma_cr(mm_u32 dma, mm_u32 c, mm_u32 v)
{
    wr(dma + CH(c, STM32_GPDMA_CxCR), v);
}

static mm_u32 dma_sr(mm_u32 dma, mm_u32 c)
{
    return rd(dma + CH(c, STM32_GPDMA_CxSR));
}

static mm_u32 dma_bndt(mm_u32 dma, mm_u32 c)
{
    return rd(dma + CH(c, STM32_GPDMA_CxBR1)) & 0xFFFFu;
}

static mm_bool dma_on(mm_u32 dma, mm_u32 c)
{
    return (rd(dma + CH(c, STM32_GPDMA_CxCR)) & EN) != 0u ? MM_TRUE : MM_FALSE;
}

/* Programs CFGR and the sequence, then ADSTART; the sequence starts at the next tick. */
static void adc_start(mm_u32 adc, mm_u32 cfgr, mm_u32 sqr1)
{
    wr(adc + CFGR, (rd(adc + CFGR) & ~(DMAEN | DMACFG | CONT)) | cfgr);
    wr(adc + SQR1, sqr1);
    wr(adc + ISR, ISR_ALL);
    ll_cr(adc, ADSTART);
    tk(1);
}

static const struct stm32_adc_inst *adc1(void)
{
    return &stm32h5_adc_state()->inst[0];
}

static int test_scan_into_buffer(const struct part *p)
{
    int rc = 1;
    if (setup(p)) goto out;
    dma_prog(GP1, 0, 0u, ADC1 + DR, 6u, g_buf, 0u);
    dma_cr(GP1, 0, EN);
    adc_start(ADC1, DMAEN, SCAN3);
    tk(3u * SLOT);
    CHECK(ram16(g_buf) == C0 && ram16(g_buf + 2u) == C1 && ram16(g_buf + 4u) == C3);
    CHECK(!dma_on(GP1, 0));
    CHECK((dma_sr(GP1, 0) & (TCF | HTF | IDLEF)) == (TCF | HTF | IDLEF));
    CHECK(dma_bndt(GP1, 0) == 0u);
    CHECK(!mm_nvic_is_pending(&g_nvic, GP1_CH0_IRQ));
    CHECK((rd(ADC1 + ISR) & (EOC | EOS | OVR)) == EOS);
    CHECK(adc1()->dma_stopped && adc1()->fifo_count == 0u);
    CHECK((stm32h5_adc_state()->warned & STM32_ADC_WARN_DMA) == 0u);
    CHECK(mmio_active_sec() == MM_SECURE);
    CHECK(rd(ADC1 + DR) == C3);
    rc = 0;
out:
    teardown();
    return rc;
}

/* HAL_DMAEx_List_Start_IT shape: BNDT = 0 with every update bit, node in RAM linking to itself. */
static int test_circular_linked_list(const struct part *p)
{
    int rc = 1;
    if (setup(p)) goto out;
    ram_wr32(g_node, HW_DINC);
    ram_wr32(g_node + 4u, 0u);
    ram_wr32(g_node + 8u, 12u);
    ram_wr32(g_node + 12u, ADC1 + DR);
    ram_wr32(g_node + 16u, g_buf);
    ram_wr32(g_node + 20u, ALL_U | (g_node & 0xFFFCu));
    dma_prog(GP1, 0, 0u, 0u, 0u, 0u, ALL_U | (g_node & 0xFFFCu));
    wr(GP1 + CH(0, STM32_GPDMA_CxTR1), 0u);
    dma_cr(GP1, 0, EN | HTIE | TCIE);
    CHECK(dma_on(GP1, 0));
    CHECK(dma_bndt(GP1, 0) == 12u);
    CHECK(rd(GP1 + CH(0, STM32_GPDMA_CxDAR)) == g_buf);
    CHECK((dma_sr(GP1, 0) & (USEF | TCF | HTF)) == 0u);

    adc_start(ADC1, DMAEN | DMACFG | CONT, SCAN3);
    tk(3u * SLOT);
    CHECK((dma_sr(GP1, 0) & (TCF | HTF)) == HTF);
    CHECK(mm_nvic_is_pending(&g_nvic, GP1_CH0_IRQ));
    mm_nvic_set_pending(&g_nvic, GP1_CH0_IRQ, MM_FALSE);
    tk(3u * SLOT);
    CHECK((dma_sr(GP1, 0) & (TCF | HTF)) == (TCF | HTF));
    CHECK(mm_nvic_is_pending(&g_nvic, GP1_CH0_IRQ));
    CHECK(ram16(g_buf) == C0 && ram16(g_buf + 2u) == C1 && ram16(g_buf + 4u) == C3);
    CHECK(ram16(g_buf + 6u) == C0 && ram16(g_buf + 8u) == C1 && ram16(g_buf + 10u) == C3);
    CHECK(dma_on(GP1, 0) && dma_bndt(GP1, 0) == 12u);
    CHECK(rd(GP1 + CH(0, STM32_GPDMA_CxDAR)) == g_buf);

    wr(GP1 + CH(0, STM32_GPDMA_CxFCR), TCF | HTF);
    ram_fill16(g_buf, 6u, 0xFFFFu);
    tk(6u * SLOT);
    CHECK((dma_sr(GP1, 0) & (TCF | HTF)) == (TCF | HTF));
    CHECK(ram16(g_buf) == C0 && ram16(g_buf + 4u) == C3 && ram16(g_buf + 10u) == C3);
    CHECK((rd(ADC1 + ISR) & OVR) == 0u);
    CHECK((rd(ADC1 + CR) & ADSTART) != 0u);
    rc = 0;
out:
    teardown();
    return rc;
}

/* DMACFG = 0: the last beat stops the ADC with DR frozen; ADSTP + ADSTART re-arms it. */
static int test_oneshot_end(const struct part *p)
{
    int rc = 1;
    if (setup(p)) goto out;
    dma_prog(GP1, 0, 0u, ADC1 + DR, 8u, g_buf, 0u);
    dma_cr(GP1, 0, EN);
    adc_start(ADC1, DMAEN | CONT, SCAN3);
    tk(4u * SLOT);
    CHECK(!dma_on(GP1, 0));
    CHECK(ram16(g_buf + 6u) == C0);
    tk(10u * SLOT);
    CHECK((rd(ADC1 + ISR) & (EOC | OVR)) == 0u);
    CHECK((rd(ADC1 + CR) & ADSTART) != 0u);
    CHECK(mm_timer_adc_cycles_until_event(&g_cfg) == NO_EVENT);
    CHECK(adc1()->dma_stopped && adc1()->rank == 0u);
    CHECK(rd(ADC1 + DR) == C0);

    ram_fill16(g_buf, 4u, 0xFFFFu);
    dma_prog(GP1, 0, 0u, ADC1 + DR, 8u, g_buf, 0u);
    dma_cr(GP1, 0, EN);
    ll_cr(ADC1, ADSTP);
    ll_cr(ADC1, ADSTART);
    tk(1);
    CHECK(!adc1()->dma_stopped);
    tk(4u * SLOT);
    CHECK(!dma_on(GP1, 0));
    CHECK(ram16(g_buf) == C0 && ram16(g_buf + 2u) == C1 && ram16(g_buf + 4u) == C3 &&
          ram16(g_buf + 6u) == C0);
    rc = 0;
out:
    teardown();
    return rc;
}

static int test_dmaen_without_channel_overruns(const struct part *p)
{
    int rc = 1;
    if (setup(p)) goto out;
    adc_start(ADC1, DMAEN | DMACFG | CONT, ONE_CH0);
    tk(3u * SLOT);
    CHECK(adc1()->fifo_count == 3u);
    CHECK((rd(ADC1 + ISR) & OVR) == 0u);
    tk(SLOT);
    CHECK((rd(ADC1 + ISR) & OVR) != 0u);
    CHECK((stm32h5_adc_state()->warned & STM32_ADC_WARN_DMA) == 0u);
    rc = 0;
out:
    teardown();
    return rc;
}

static int test_late_channel_enable(const struct part *p)
{
    int rc = 1;
    if (setup(p)) goto out;
    adc_start(ADC1, DMAEN | DMACFG, ONE_CH0);
    tk(SLOT);
    CHECK((rd(ADC1 + ISR) & EOC) != 0u && adc1()->fifo_count == 1u);
    ram_fill16(g_buf, 4u, 0xFFFFu);
    dma_prog(GP1, 0, 0u, ADC1 + DR, 8u, g_buf, 0u);
    CHECK(adc1()->fifo_count == 1u && dma_bndt(GP1, 0) == 8u);
    dma_cr(GP1, 0, EN);
    CHECK(ram16(g_buf) == C0 && ram16(g_buf + 2u) == 0xFFFFu);
    CHECK(dma_bndt(GP1, 0) == 6u);
    CHECK(adc1()->fifo_count == 0u && (rd(ADC1 + ISR) & EOC) == 0u);
    rc = 0;
out:
    teardown();
    return rc;
}

static int test_ovr_clear_resumes(const struct part *p)
{
    int rc = 1;
    if (setup(p)) goto out;
    adc_start(ADC1, DMAEN | DMACFG | CONT, ONE_CH0);
    tk(4u * SLOT);
    CHECK((rd(ADC1 + ISR) & OVR) != 0u && adc1()->fifo_count == 3u);
    ram_fill16(g_buf, 4u, 0xFFFFu);
    dma_prog(GP1, 0, 0u, ADC1 + DR, 8u, g_buf, 0u);
    dma_cr(GP1, 0, EN);
    CHECK(dma_bndt(GP1, 0) == 8u && ram16(g_buf) == 0xFFFFu);
    wr(ADC1 + ISR, OVR);
    CHECK(dma_bndt(GP1, 0) == 2u && adc1()->fifo_count == 0u);
    CHECK(ram16(g_buf) == C0 && ram16(g_buf + 4u) == C0 && ram16(g_buf + 6u) == 0xFFFFu);
    tk(SLOT);
    CHECK(!dma_on(GP1, 0) && (dma_sr(GP1, 0) & TCF) != 0u);
    CHECK(ram16(g_buf + 6u) == C0);
    CHECK(!adc1()->dma_stopped);
    rc = 0;
out:
    teardown();
    return rc;
}

/* A non-secure channel reading the TZSC-secured ADC reads 0, as for CPU
 * accesses (RM0481 section 5.4.2 gives a bus error only for fetches); DR is
 * never read, so the FIFO fills and OVR follows. */
static int test_secure_adc_nonsecure_channel(const struct part *p)
{
    int rc = 1;
    if (setup(p)) goto out;
    wr(TZSC_SECCFGR3, rd(TZSC_SECCFGR3) | ADC12SEC);
    ram_fill16(g_buf, 4u, 0xFFFFu);
    dma_prog(GP1, 0, 0u, ADC1 + DR, 8u, g_buf, 0u);
    dma_cr(GP1, 0, EN);
    adc_start(ADC1, DMAEN | DMACFG | CONT, ONE_CH0);
    tk(SLOT);
    CHECK(ram16(g_buf) == 0u && dma_bndt(GP1, 0) == 6u && adc1()->fifo_count == 1u);
    CHECK((dma_sr(GP1, 0) & DTEF) == 0u);
    CHECK(mmio_active_sec() == MM_SECURE);
    tk(3u * SLOT);
    CHECK((rd(ADC1 + ISR) & OVR) != 0u && adc1()->fifo_count == 3u);
    CHECK(dma_bndt(GP1, 0) == 2u && ram16(g_buf + 4u) == 0u && ram16(g_buf + 6u) == 0xFFFFu);
    CHECK(dma_on(GP1, 0) && (dma_sr(GP1, 0) & DTEF) == 0u);
    tk(4u * SLOT);
    CHECK(dma_bndt(GP1, 0) == 2u);
    CHECK(mmio_active_sec() == MM_SECURE);
    rc = 0;
out:
    teardown();
    return rc;
}

/* A SoC reset clears the GPDMA but neither re-registers MMIO nor re-inits the ADC. */
static int test_reset_keeps_bindings(const struct part *p)
{
    int rc = 1;
    if (setup(p)) goto out;
    dma_prog(GP1, 0, 0u, ADC1 + DR, 8u, g_buf, 0u);
    dma_cr(GP1, 0, EN);
    adc_start(ADC1, DMAEN | DMACFG | CONT, ONE_CH0);
    tk(2u * SLOT);
    CHECK(dma_bndt(GP1, 0) == 4u);

    p->mmio_reset();
    mm_timer_reset(&g_cfg);
    CHECK(!dma_on(GP1, 0) && dma_bndt(GP1, 0) == 0u && dma_sr(GP1, 0) == IDLEF);
    power_up();
    CHECK(bring_up(ADC1, ADC_COMMON));
    adc_start(ADC1, DMAEN | DMACFG, ONE_CH0);
    tk(SLOT);
    CHECK(adc1()->fifo_count == 1u);
    ram_fill16(g_buf, 4u, 0xFFFFu);
    dma_prog(GP1, 0, 0u, ADC1 + DR, 8u, g_buf, 0u);
    dma_cr(GP1, 0, EN);
    CHECK(ram16(g_buf) == C0 && dma_bndt(GP1, 0) == 6u && adc1()->fifo_count == 0u);
    rc = 0;
out:
    teardown();
    return rc;
}

/* Suspended channel half way through its block, one result waiting in the ADC FIFO. */
static int test_snapshot_mid_block(const struct part *p)
{
    static mm_u8 buf[262144];
    struct mm_snapshot_writer w;
    struct mm_snapshot_reader r;
    int rc = 1;
    if (setup(p)) goto out;
    dma_prog(GP1, 0, 0u, ADC1 + DR, 8u, g_buf, 0u);
    dma_cr(GP1, 0, EN | HTIE);
    adc_start(ADC1, DMAEN | DMACFG | CONT, ONE_CH0);
    tk(2u * SLOT);
    CHECK(dma_bndt(GP1, 0) == 4u && (dma_sr(GP1, 0) & HTF) != 0u);
    dma_cr(GP1, 0, EN | HTIE | SUSP);
    CHECK((dma_sr(GP1, 0) & (SUSPF | IDLEF)) == (SUSPF | IDLEF));
    tk(SLOT);
    CHECK(adc1()->fifo_count == 1u && dma_bndt(GP1, 0) == 4u);
    mm_snapshot_writer_init(&w, buf, (mm_u32)sizeof(buf));
    CHECK(mmio_bus_save(&g_map.mmio, &w));

    dma_cr(GP1, 0, EN | HTIE);
    tk(6u * SLOT);
    CHECK(!dma_on(GP1, 0) && adc1()->fifo_count != 1u);

    mm_snapshot_reader_init(&r, buf, mm_snapshot_bytes_used(&w));
    CHECK(mmio_bus_load(&g_map.mmio, &r));
    CHECK(dma_on(GP1, 0) && dma_bndt(GP1, 0) == 4u);
    CHECK((rd(GP1 + CH(0, STM32_GPDMA_CxCR)) & SUSP) != 0u);
    CHECK((dma_sr(GP1, 0) & (HTF | SUSPF | TCF)) == (HTF | SUSPF));
    CHECK(adc1()->fifo_count == 1u && (rd(ADC1 + ISR) & OVR) == 0u);
    ram_fill16(g_buf + 4u, 2u, 0xFFFFu);
    dma_cr(GP1, 0, EN | HTIE);
    CHECK(ram16(g_buf + 4u) == C0 && ram16(g_buf + 6u) == 0xFFFFu);
    CHECK(dma_bndt(GP1, 0) == 2u && adc1()->fifo_count == 0u);
    rc = 0;
out:
    teardown();
    return rc;
}

/* ADC3 raises request 142 (RM0517), served here by a GPDMA2 channel. */
static int test_adc3_line(const struct part *p)
{
    int rc = 1;
    if (!p->has_adc3) return 0;
    if (setup(p)) goto out;
    wr(RCC_AHB2ENR, rd(RCC_AHB2ENR) | ADC3EN);
    CHECK(bring_up(ADC3, ADC3_COMMON));
    ram_fill16(g_buf, 4u, 0xFFFFu);
    dma_prog(GP2, 3, 142u, ADC3 + DR, 4u, g_buf, 0u);
    dma_prog(GP1, 0, 0u, ADC1 + DR, 4u, g_buf + 4u, 0u);
    dma_cr(GP2, 3, EN);
    dma_cr(GP1, 0, EN);
    adc_start(ADC3, DMAEN | DMACFG, ONE_CH0);
    tk(SLOT);
    CHECK(ram16(g_buf) == C_PB2 && dma_bndt(GP2, 3) == 2u);
    CHECK(dma_bndt(GP1, 0) == 4u && ram16(g_buf + 4u) == 0xFFFFu);
    CHECK((rd(ADC3 + ISR) & EOC) == 0u);
    rc = 0;
out:
    teardown();
    return rc;
}

int main(void)
{
    struct {
        const char *name;
        int (*fn)(const struct part *p);
    } tests[] = {
        { "scan_into_buffer", test_scan_into_buffer },
        { "circular_linked_list", test_circular_linked_list },
        { "oneshot_end", test_oneshot_end },
        { "dmaen_without_channel_overruns", test_dmaen_without_channel_overruns },
        { "late_channel_enable", test_late_channel_enable },
        { "ovr_clear_resumes", test_ovr_clear_resumes },
        { "secure_adc_nonsecure_channel", test_secure_adc_nonsecure_channel },
        { "reset_keeps_bindings", test_reset_keeps_bindings },
        { "snapshot_mid_block", test_snapshot_mid_block },
        { "adc3_line", test_adc3_line },
    };
    size_t i;
    size_t j;
    int failures = 0;
    int rc;

    for (j = 0; j < sizeof(g_parts) / sizeof(g_parts[0]); ++j) {
        for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
            rc = tests[i].fn(&g_parts[j]);
            printf("%-10s %-35s %s\n", g_parts[j].name, tests[i].name, rc == 0 ? "PASS" : "FAIL");
            if (rc != 0) failures++;
        }
    }
    return (failures == 0) ? 0 : 1;
}
