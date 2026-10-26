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
#include "m33mu/signal.h"
#include "stm32_adc.h"

#define ADC12      0x42028000u
#define ADC12_SEC  0x52028000u
#define COMMON     0x300u

#define ISR     0x00u
#define IER     0x04u
#define CR      0x08u
#define CFGR    0x0Cu
#define CFGR2   0x10u
#define SMPR1   0x14u
#define SMPR2   0x18u
#define TR1     0x20u
#define TR2     0x24u
#define TR3     0x28u
#define SQR1    0x30u
#define DR      0x40u
#define OFR1    0x60u
#define JDR1    0x80u
#define DIFSEL  0xB0u
#define OR      0xC8u
#define CSR     0x00u
#define CCR     0x08u
#define CDR     0x0Cu

#define ADEN     (1u << 0)
#define ADDIS    (1u << 1)
#define ADSTART  (1u << 2)
#define JADSTART (1u << 3)
#define ADSTP    (1u << 4)
#define JADSTP   (1u << 5)
#define ADVREGEN (1u << 28)
#define DEEPPWD  (1u << 29)
#define ADCALDIF (1u << 30)
#define ADCAL    (1u << 31)
#define RS       (ADCAL | ADEN | ADDIS | JADSTART | JADSTP | ADSTART | ADSTP)

#define ADRDY    (1u << 0)
#define EOSMP    (1u << 1)
#define EOC      (1u << 2)
#define EOS      (1u << 3)
#define OVR      (1u << 4)

#define CFGR_DMAEN   (1u << 0)
#define CFGR_RES     (3u << 3)
#define CFGR_OVRMOD  (1u << 12)
#define CFGR_CONT    (1u << 13)
#define CFGR_ALIGN   (1u << 15)
#define CFGR2_ROVSE  (1u << 0)
#define OFR_EN       (1u << 31)
#define OFR_SATEN    (1u << 25)
#define OFR_POS      (1u << 24)
#define OFR_CH(c)    ((mm_u32)(c) << 26)
#define SMPPLUS      (1u << 31)
#define CCR_CKMODE   (3u << 16)
#define CCR_PRESC    (0xFu << 18)

#define LOOP_MAX 100000

static const char *g_tmp_path = "/tmp/stm32_adc_test_fixture.sigc";

static struct mmio_region g_regions[8];
static struct mmio_bus g_bus;
static struct mm_nvic g_nvic;
static struct stm32_adc_state g_adc;
static struct stm32_adc_soc g_soc;
static struct stm32_adc_input g_in1[STM32_ADC_NUM_CHANNELS];
static struct stm32_adc_input g_in2[STM32_ADC_NUM_CHANNELS];
static mm_bool g_clk[2];
static mm_bool g_analog[8][16];
static mm_i32 g_mv[8][16];
static mm_bool g_unbound[8][16];
static mm_u64 g_hclk;
static int g_samples;
static mm_u64 g_last_ahead;
static mm_u64 g_aheads[8];
static mm_bool g_with_signal;
static mm_u64 g_poll;
static int g_enable_writes;
static mm_bool g_access;

static mm_bool fake_access(void *ctx)
{
    (void)ctx;
    return g_access;
}

static mm_bool fake_clock(void *ctx, mm_u32 inst)
{
    (void)ctx;
    return g_clk[inst];
}

static mm_bool fake_analog(void *ctx, int bank, int pin)
{
    (void)ctx;
    return g_analog[bank][pin];
}

static mm_u64 fake_hclk(void *ctx)
{
    (void)ctx;
    return g_hclk;
}

static mm_bool fake_sample(void *ctx, int bank, int pin, mm_u64 cycles_ahead, mm_i32 *mv_out)
{
    (void)ctx;
    if (g_samples < 8) g_aheads[g_samples] = cycles_ahead;
    g_samples++;
    g_last_ahead = cycles_ahead;
    *mv_out = g_mv[bank][pin];
    return g_unbound[bank][pin] ? MM_FALSE : MM_TRUE;
}

static void set_input(struct stm32_adc_input *tab, mm_u32 ch, mm_u8 kind, mm_u8 bank, mm_u8 pin)
{
    tab[ch].kind = kind;
    tab[ch].bank = bank;
    tab[ch].pin = pin;
}

static void setup(void)
{
    int b, p;
    memset(&g_adc, 0, sizeof(g_adc));
    memset(&g_soc, 0, sizeof(g_soc));
    memset(g_in1, 0, sizeof(g_in1));
    memset(g_in2, 0, sizeof(g_in2));
    memset(g_mv, 0, sizeof(g_mv));
    memset(g_unbound, 0, sizeof(g_unbound));
    memset(g_aheads, 0, sizeof(g_aheads));
    for (b = 0; b < 8; ++b) {
        for (p = 0; p < 16; ++p) g_analog[b][p] = MM_TRUE;
    }
    g_clk[0] = MM_TRUE;
    g_clk[1] = MM_TRUE;
    g_hclk = 64000000u;
    g_samples = 0;
    g_last_ahead = 0u;
    g_with_signal = MM_FALSE;
    g_poll = 5u;
    g_enable_writes = 0;

    set_input(g_in1, 0, STM32_ADC_IN_PIN, 0, 0);
    set_input(g_in1, 1, STM32_ADC_IN_PIN, 0, 1);
    set_input(g_in1, 5, STM32_ADC_IN_PIN, 1, 1);
    set_input(g_in1, 17, STM32_ADC_IN_INTERNAL, 0, 0);
    set_input(g_in2, 0, STM32_ADC_IN_PIN, 0, 0);
    set_input(g_in2, 2, STM32_ADC_IN_PIN, 5, 13);

    g_soc.base = ADC12;
    g_soc.inst_count = 2u;
    g_soc.irq[0] = 37;
    g_soc.irq[1] = 69;
    g_soc.inputs[0] = g_in1;
    g_soc.inputs[1] = g_in2;
    g_soc.clock_enabled = fake_clock;
    g_soc.pin_is_analog = fake_analog;
    g_soc.hclk_hz = fake_hclk;
    g_soc.sample_mv = fake_sample;

    mmio_bus_init(&g_bus, g_regions, sizeof(g_regions) / sizeof(g_regions[0]));
    mmio_bus_enable_step_hooks(&g_bus);
    mm_nvic_init(&g_nvic);
    stm32_adc_init(&g_adc, &g_soc, &g_bus, &g_nvic);
}

static void wr(mm_u32 inst, mm_u32 off, mm_u32 v)
{
    (void)mmio_bus_write(&g_bus, ADC12 + inst * 0x100u + off, 4, v);
}

static mm_u32 rd(mm_u32 inst, mm_u32 off)
{
    mm_u32 v = 0;
    (void)mmio_bus_read(&g_bus, ADC12 + inst * 0x100u + off, 4, &v);
    return v;
}

static void modify(mm_u32 inst, mm_u32 off, mm_u32 clear, mm_u32 set)
{
    wr(inst, off, (rd(inst, off) & ~clear) | set);
}

static void wr_common(mm_u32 off, mm_u32 v)
{
    (void)mmio_bus_write(&g_bus, ADC12 + COMMON + off, 4, v);
}

static mm_u32 rd_common(mm_u32 off)
{
    mm_u32 v = 0;
    (void)mmio_bus_read(&g_bus, ADC12 + COMMON + off, 4, &v);
    return v;
}

static void tk(mm_u64 cycles)
{
    stm32_adc_tick(&g_adc, cycles);
    if (g_with_signal) {
        mm_signal_advance(cycles);
    }
}

static mm_u64 next_event(void)
{
    return stm32_adc_cycles_until_event(&g_adc);
}

/* LL_ADC_Enable, _Disable, _REG_StartConversion, ...: MODIFY_REG(CR, RS, bit). */
static void ll_cr(mm_u32 inst, mm_u32 bit)
{
    modify(inst, CR, RS, bit);
}

static void ll_cal(mm_u32 inst)
{
    modify(inst, CR, ADCALDIF | RS, ADCAL);
}

static void set_ckmode(mm_u32 ckmode)
{
    wr_common(CCR, (rd_common(CCR) & ~(CCR_CKMODE | CCR_PRESC)) | (ckmode << 16));
}

static void bring_up(mm_u32 inst, mm_u32 ckmode)
{
    wr(inst, CR, rd(inst, CR) & ~(DEEPPWD | RS));
    ll_cr(inst, ADVREGEN);
    set_ckmode(ckmode);
}

static int enabled(mm_u32 inst, mm_u32 ckmode)
{
    mm_u64 div = (ckmode == 0u) ? 1u : (1u << (ckmode - 1u));
    bring_up(inst, ckmode);
    ll_cr(inst, ADEN);
    tk(1);
    tk(15u * div);
    return (rd(inst, ISR) & ADRDY) != 0u ? 0 : 1;
}

/* One software-started conversion; 0xFFFFFFFF if EOC never set. */
static mm_u32 convert(mm_u32 inst)
{
    wr(inst, ISR, EOSMP | EOC | EOS);
    ll_cr(inst, ADSTART);
    tk(1);
    tk(4000);
    if ((rd(inst, ISR) & EOC) == 0u) {
        return 0xFFFFFFFFu;
    }
    return rd(inst, DR);
}

/* HAL_ADC_Init: deep power-down exit, regulator, CCR only when ADC1 and ADC2 are off. */
static int hal_init(mm_u32 inst, mm_u32 ckmode, mm_u32 res, mm_bool *ccr_written)
{
    int n;
    *ccr_written = MM_FALSE;
    if ((rd(inst, CR) & DEEPPWD) != 0u) {
        wr(inst, CR, rd(inst, CR) & ~(DEEPPWD | RS));
    }
    if ((rd(inst, CR) & ADVREGEN) == 0u) {
        ll_cr(inst, ADVREGEN);
        for (n = 0; n < 20; ++n) tk(g_poll);
    }
    if ((rd(inst, CR) & ADVREGEN) == 0u) return 1;
    if ((rd(inst, CR) & ADSTART) == 0u) {
        if (((rd(0, CR) | rd(1, CR)) & ADEN) == 0u) {
            set_ckmode(ckmode);
            *ccr_written = MM_TRUE;
        }
        modify(inst, CFGR, CFGR_RES | CFGR_CONT, res << 3);
    }
    modify(inst, SQR1, 0xFu, 0u);
    return 0;
}

static int hal_disable(mm_u32 inst)
{
    mm_u32 cr = rd(inst, CR);
    int n;
    if ((cr & ADEN) != 0u && (cr & ADDIS) == 0u) {
        if ((cr & (JADSTART | ADSTART | ADEN)) != ADEN) return 1;
        ll_cr(inst, ADDIS);
        wr(inst, ISR, EOSMP | ADRDY);
        for (n = 0; (rd(inst, CR) & ADEN) != 0u; ++n) {
            if (n > LOOP_MAX) return 1;
            tk(g_poll);
        }
    }
    return 0;
}

static int hal_calibrate(mm_u32 inst)
{
    int n;
    if (hal_disable(inst)) return 1;
    ll_cal(inst);
    for (n = 0; (rd(inst, CR) & ADCAL) != 0u; ++n) {
        if (n > LOOP_MAX) return 1;
        tk(g_poll);
    }
    return 0;
}

/* HAL_ADC_ConfigChannel for ranks 1..4, single-ended, no offset. */
static void hal_config_channel(mm_u32 inst, mm_u32 rank, mm_u32 ch, mm_u32 smp)
{
    mm_u32 k;
    if (ch == 0u) {
        modify(inst, OR, 0u, 1u);
    }
    modify(inst, SQR1, 0x1Fu << (6u * rank), ch << (6u * rank));
    if (ch < 10u) {
        modify(inst, SMPR1, 7u << (3u * ch), smp << (3u * ch));
    } else {
        modify(inst, SMPR2, 7u << (3u * (ch - 10u)), smp << (3u * (ch - 10u)));
    }
    modify(inst, SMPR1, SMPPLUS, 0u);
    for (k = 0; k < 4u; ++k) {
        if (((rd(inst, OFR1 + 4u * k) >> 26) & 0x1Fu) == ch) {
            modify(inst, OFR1 + 4u * k, 1u << 31, 0u);
        }
    }
    if ((rd(inst, CR) & ADEN) == 0u) {
        modify(inst, DIFSEL, 1u << ch, 0u);
    }
}

/* ADC_Enable, including the ADEN retry loop for the 4 clocks after ADCAL. */
static int hal_enable(mm_u32 inst)
{
    int n;
    if ((rd(inst, CR) & ADEN) != 0u) return 0;
    if ((rd(inst, CR) & (ADCAL | JADSTP | ADSTP | JADSTART | ADSTART | ADDIS | ADEN)) != 0u) return 1;
    ll_cr(inst, ADEN);
    g_enable_writes++;
    for (n = 0; (rd(inst, ISR) & ADRDY) == 0u; ++n) {
        if ((rd(inst, CR) & ADEN) == 0u) {
            ll_cr(inst, ADEN);
            g_enable_writes++;
        }
        if (n > LOOP_MAX) return 1;
        tk(g_poll);
    }
    return 0;
}

static int hal_start(mm_u32 inst)
{
    if ((rd(inst, CR) & ADSTART) != 0u) return 1;
    if (hal_enable(inst)) return 1;
    wr(inst, ISR, EOC | EOS | OVR);
    ll_cr(inst, ADSTART);
    return 0;
}

/* HAL_ADC_PollForConversion with EOCSelection = single conversion. */
static int hal_poll(mm_u32 inst)
{
    int n;
    for (n = 0; (rd(inst, ISR) & EOC) == 0u; ++n) {
        if (n > LOOP_MAX) return 1;
        tk(g_poll);
    }
    wr(inst, ISR, EOC | EOS);
    return 0;
}

static int hal_stop(mm_u32 inst)
{
    int n;
    if ((rd(inst, CR) & ADSTART) != 0u) {
        ll_cr(inst, ADSTP);
        for (n = 0; (rd(inst, CR) & ADSTART) != 0u; ++n) {
            if (n > LOOP_MAX) return 1;
            tk(g_poll);
        }
    }
    return hal_disable(inst);
}

static void put_le(FILE *f, mm_u64 v, int bytes)
{
    int i;
    for (i = 0; i < bytes; ++i) {
        fputc((int)((v >> (8 * i)) & 0xFFu), f);
    }
}

/* One uniform timebase, one trace. */
static mm_bool write_fixture(const char *path, const char *trace_name, const mm_i16 *samples,
                             mm_u32 count, mm_u32 period_ns)
{
    FILE *f = fopen(path, "wb");
    char name[32];
    mm_u32 i;

    if (f == 0) return MM_FALSE;
    fwrite("SIGC", 1, 4, f);
    put_le(f, 1, 4);
    put_le(f, 1, 4);
    put_le(f, 1, 4);
    put_le(f, 1, 4);
    put_le(f, count, 4);
    put_le(f, period_ns, 4);
    memset(name, 0, sizeof(name));
    snprintf(name, sizeof(name), "%s", trace_name);
    put_le(f, 1, 4);
    fwrite(name, 1, sizeof(name), f);
    put_le(f, 1, 4);
    put_le(f, 3300, 4);
    for (i = 0; i < count; ++i) put_le(f, (mm_u16)samples[i], 2);
    fclose(f);
    return MM_TRUE;
}

static int check_reset_values(void)
{
    mm_u32 i;
    for (i = 0; i < 2u; ++i) {
        if (rd(i, CR) != 0x20000000u || rd(i, CFGR) != 0x80000000u) return 1;
        if (rd(i, TR1) != 0x0FFF0000u || rd(i, TR2) != 0x00FF0000u || rd(i, TR3) != 0x00FF0000u) return 1;
        if (rd(i, ISR) != 0u || rd(i, SQR1) != 0u || rd(i, DR) != 0u) return 1;
        if (rd(i, CFGR2) != 0u || rd(i, SMPR1) != 0u || rd(i, DIFSEL) != 0u) return 1;
    }
    if (rd_common(CSR) != 0u || rd_common(CCR) != 0u) return 1;
    if (next_event() != STM32_ADC_NO_EVENT) return 1;
    return 0;
}

static int test_reset_values(void)
{
    setup();
    return check_reset_values();
}

static int test_reset_clears_dirty_state(void)
{
    setup();
    if (enabled(0, 1)) return 1;
    g_mv[0][0] = 1000;
    if (convert(0) != 1241u) return 1;
    wr(0, CFGR, 0x3018u);
    wr(0, SQR1, 0x41u);
    wr(0, SMPR1, 7u);
    wr(0, DIFSEL, 3u);
    wr(0, CFGR2, 5u);
    ll_cr(0, ADSTART);
    bring_up(1, 2);
    ll_cal(1);
    wr_common(CCR, 0x01C3C000u | (3u << 16));
    tk(1);
    if (next_event() == STM32_ADC_NO_EVENT) return 1;
    stm32_adc_reset(&g_adc);
    if (check_reset_values()) return 1;
    tk(5000);
    if (rd(0, ISR) != 0u || rd(1, ISR) != 0u || rd(1, CR) != 0x20000000u) return 1;
    return 0;
}

static int test_storage_and_plain_cr_bits(void)
{
    static const mm_u32 offs[] = { CFGR2, 0x1Cu, 0x2Cu, 0x4Cu, 0x60u, 0xA0u, DIFSEL, 0xB4u, OR, 0xFCu };
    mm_u32 i;
    setup();
    wr(0, CR, rd(0, CR) & ~(DEEPPWD | RS));
    if (rd(0, CR) != 0u) return 1;
    ll_cr(0, ADVREGEN);
    if (rd(0, CR) != ADVREGEN) return 1;
    wr(0, CR, DEEPPWD | ADVREGEN);
    if (rd(0, CR) != DEEPPWD) return 1;
    for (i = 0; i < sizeof(offs) / sizeof(offs[0]); ++i) {
        wr(1, offs[i], 0x12340000u | offs[i]);
        if (rd(1, offs[i]) != (0x12340000u | offs[i])) return 1;
    }
    wr(0, DR, 5u);
    wr(0, JDR1, 5u);
    if (rd(0, DR) != 0u || rd(0, JDR1) != 0u) return 1;
    wr(0, CFGR2, 0u);
    (void)mmio_bus_write(&g_bus, ADC12 + CFGR2 + 1u, 1, 0xABu);
    if (rd(0, CFGR2) != 0xAB00u) return 1;
    wr_common(CCR, 0x01C3C000u | (2u << 16));
    if (rd_common(CCR) != (0x01C3C000u | (2u << 16))) return 1;
    wr_common(CSR, 0xFFFFFFFFu);
    wr_common(CDR, 0xFFFFFFFFu);
    if (rd_common(CSR) != 0u || rd_common(CDR) != 0u) return 1;
    return 0;
}

static int test_calibration_timing(void)
{
    setup();
    bring_up(0, 1);
    ll_cal(0);
    if ((rd(0, CR) & ADCAL) == 0u) return 1;
    tk(10);
    tk(1334);
    if ((rd(0, CR) & ADCAL) == 0u) return 1;
    tk(1);
    if ((rd(0, CR) & ADCAL) != 0u) return 1;
    tk(3);
    ll_cr(0, ADEN);
    if ((rd(0, CR) & ADEN) != 0u) return 1;
    tk(1);
    ll_cr(0, ADEN);
    if ((rd(0, CR) & ADEN) == 0u) return 1;

    setup();
    bring_up(0, 2);
    ll_cal(0);
    tk(1);
    tk(2669);
    if ((rd(0, CR) & ADCAL) == 0u) return 1;
    tk(1);
    if ((rd(0, CR) & ADCAL) != 0u) return 1;
    tk(7);
    ll_cr(0, ADEN);
    if ((rd(0, CR) & ADEN) != 0u) return 1;
    tk(1);
    ll_cr(0, ADEN);
    if ((rd(0, CR) & ADEN) == 0u) return 1;

    /* A read-modify-write carrying ADCAL=1 does not restart t_CAL. */
    setup();
    bring_up(0, 1);
    ll_cal(0);
    tk(1);
    tk(500);
    wr(0, CR, rd(0, CR) | ADCALDIF);
    if ((rd(0, CR) & (ADCAL | ADCALDIF)) != (ADCAL | ADCALDIF) || next_event() != 835u) return 1;
    tk(834);
    if ((rd(0, CR) & ADCAL) == 0u) return 1;
    tk(1);
    if ((rd(0, CR) & ADCAL) != 0u || (rd(0, CR) & ADCALDIF) == 0u) return 1;
    return 0;
}

static int test_adrdy_delay(void)
{
    setup();
    bring_up(0, 1);
    ll_cr(0, ADEN);
    if ((rd(0, CR) & ADEN) == 0u) return 1;
    tk(5);
    tk(14);
    if ((rd(0, ISR) & ADRDY) != 0u) return 1;
    tk(1);
    if ((rd(0, ISR) & ADRDY) == 0u) return 1;

    setup();
    bring_up(0, 1);
    ll_cr(0, ADEN);
    tk(5);
    ll_cr(0, ADEN);
    tk(14);
    if ((rd(0, ISR) & ADRDY) != 0u) return 1;
    tk(1);
    if ((rd(0, ISR) & ADRDY) == 0u) return 1;

    setup();
    bring_up(0, 3);
    ll_cr(0, ADEN);
    tk(1);
    tk(59);
    if ((rd(0, ISR) & ADRDY) != 0u) return 1;
    tk(1);
    if ((rd(0, ISR) & ADRDY) == 0u) return 1;
    return 0;
}

static int test_refused_writes(void)
{
    setup();
    if (enabled(0, 1)) return 1;
    ll_cal(0);
    if ((rd(0, CR) & ADCAL) != 0u) return 1;
    wr(0, CR, rd(0, CR) & ~ADEN);
    if ((rd(0, CR) & ADEN) == 0u) return 1;
    ll_cr(0, ADSTP);
    if ((rd(0, CR) & (ADSTP | ADSTART)) != 0u) return 1;
    ll_cr(0, JADSTART);
    if ((rd(0, CR) & JADSTART) != 0u) return 1;
    ll_cr(0, JADSTP);
    if ((rd(0, CR) & JADSTP) != 0u) return 1;
    ll_cr(0, ADSTART);
    ll_cr(0, ADDIS);
    if ((rd(0, CR) & (ADEN | ADSTART | ADDIS)) != (ADEN | ADSTART)) return 1;

    setup();
    bring_up(0, 1);
    ll_cr(0, ADSTART);
    if ((rd(0, CR) & ADSTART) != 0u) return 1;
    ll_cal(0);
    ll_cr(0, ADEN);
    if ((rd(0, CR) & ADEN) != 0u) return 1;
    return 0;
}

static int test_single_conversion(void)
{
    mm_u32 isr;
    setup();
    if (enabled(0, 1)) return 1;
    g_mv[0][0] = 1000;
    ll_cr(0, ADSTART);
    if ((rd(0, CR) & ADSTART) == 0u) return 1;
    tk(7);
    if ((rd(0, ISR) & EOSMP) != 0u || g_samples != 0) return 1;
    tk(3);
    if ((rd(0, ISR) & EOSMP) == 0u || g_samples != 1 || g_last_ahead != 3u) return 1;
    tk(11);
    if ((rd(0, ISR) & EOC) != 0u) return 1;
    tk(1);
    isr = rd(0, ISR);
    if ((isr & (EOC | EOS)) != (EOC | EOS) || (rd(0, CR) & ADSTART) != 0u) return 1;
    if (rd(0, DR) != 1241u || (rd(0, ISR) & EOC) != 0u) return 1;

    /* Sampling instant inside a long batch, batch start = sequence start. */
    wr(0, ISR, EOSMP | EOS);
    ll_cr(0, ADSTART);
    tk(1);
    tk(100);
    if (g_samples != 2 || g_last_ahead != 3u || (rd(0, ISR) & EOC) == 0u) return 1;

    /* Batch start one cycle after the sequence start. */
    wr(0, ISR, EOSMP | EOC | EOS);
    ll_cr(0, ADSTART);
    tk(7);
    tk(1);
    tk(100);
    if (g_samples != 3 || g_last_ahead != 2u) return 1;

    /* Rewriting ADSTART during a conversion does not restart it. */
    wr(0, ISR, EOSMP | EOC | EOS);
    ll_cr(0, ADSTART);
    tk(1);
    ll_cr(0, ADSTART);
    tk(2);
    if ((rd(0, ISR) & EOSMP) != 0u) return 1;
    tk(1);
    if ((rd(0, ISR) & EOSMP) == 0u) return 1;
    tk(11);
    if ((rd(0, ISR) & EOC) != 0u) return 1;
    tk(1);
    if ((rd(0, ISR) & EOC) == 0u) return 1;
    return 0;
}

static int measure(mm_u32 ckmode, mm_u32 res, mm_u32 smpr1, mm_u32 smpr2, mm_u32 ch,
                   mm_u64 *eosmp, mm_u64 *eoc)
{
    mm_u64 n;
    setup();
    if (enabled(0, ckmode)) return 1;
    modify(0, CFGR, CFGR_RES, res << 3);
    wr(0, SMPR1, smpr1);
    wr(0, SMPR2, smpr2);
    wr(0, SQR1, ch << 6);
    wr(0, ISR, EOSMP | EOC | EOS);
    ll_cr(0, ADSTART);
    tk(1);
    *eosmp = 0u;
    *eoc = 0u;
    for (n = 1u; n <= 5000u && *eoc == 0u; ++n) {
        tk(1);
        if (*eosmp == 0u && (rd(0, ISR) & EOSMP) != 0u) *eosmp = n;
        if ((rd(0, ISR) & EOC) != 0u) *eoc = n;
    }
    return 0;
}

static int test_timing_matrix(void)
{
    static const struct {
        mm_u32 ckmode, res, smpr1, smpr2, ch;
        mm_u64 eosmp, eoc;
    } cases[] = {
        { 1, 0, 0, 0, 0, 3, 15 },
        { 2, 0, 0, 0, 0, 5, 30 },
        { 3, 0, 0, 0, 0, 10, 60 },
        { 1, 0, 2, 0, 0, 13, 25 },
        { 1, 0, 3, 0, 0, 25, 37 },
        { 1, 0, 4, 0, 0, 48, 60 },
        { 1, 0, 5, 0, 0, 93, 105 },
        { 1, 0, 6, 0, 0, 248, 260 },
        { 1, 0, 7, 0, 0, 641, 653 },
        { 1, 0, SMPPLUS, 0, 0, 4, 16 },
        { 1, 0, SMPPLUS | 1u, 0, 0, 7, 19 },
        { 1, 1, 0, 0, 0, 3, 13 },
        { 1, 2, 0, 0, 0, 3, 11 },
        { 1, 3, 0, 0, 0, 3, 9 },
        { 1, 0, 0, 1u << 6, 12, 7, 19 },
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        mm_u64 eosmp, eoc;
        if (measure(cases[i].ckmode, cases[i].res, cases[i].smpr1, cases[i].smpr2, cases[i].ch,
                    &eosmp, &eoc)) return 1;
        if (eosmp != cases[i].eosmp || eoc != cases[i].eoc) {
            printf("  case %u: EOSMP %llu EOC %llu\n", (unsigned)i,
                   (unsigned long long)eosmp, (unsigned long long)eoc);
            return 1;
        }
    }
    return 0;
}

static int conv_mv(mm_i32 mv, mm_u32 expected)
{
    g_mv[0][0] = mv;
    return convert(0) == expected ? 0 : 1;
}

static int test_quantizer(void)
{
    setup();
    if (enabled(0, 1)) return 1;
    if (conv_mv(1000, 1241u) || conv_mv(3300, 4095u) || conv_mv(4000, 4095u)) return 1;
    if (conv_mv(-100, 0u) || conv_mv(1650, 2048u) || conv_mv(0, 0u)) return 1;
    modify(0, CFGR, CFGR_RES, 1u << 3);
    if (conv_mv(1000, 310u) || conv_mv(5000, 1023u)) return 1;
    modify(0, CFGR, CFGR_RES, 2u << 3);
    if (conv_mv(1000, 77u)) return 1;
    modify(0, CFGR, CFGR_RES, 3u << 3);
    if (conv_mv(1000, 19u) || conv_mv(3300, 63u)) return 1;
    modify(0, CFGR, CFGR_RES, 0u);
    /* 1462.5 and 877.5: halves round up */
    stm32_adc_set_vref_mv(&g_adc, 2800);
    if (conv_mv(1000, 1463u) || conv_mv(600, 878u) || conv_mv(999, 1461u)) return 1;
    stm32_adc_set_vref_mv(&g_adc, 2500);
    if (conv_mv(1000, 1638u)) return 1;
    stm32_adc_set_vref_mv(&g_adc, 0);
    stm32_adc_set_vref_mv(&g_adc, -5);
    if (conv_mv(1000, 1638u)) return 1;
    return 0;
}

static int test_noise_is_seeded(void)
{
    mm_u32 codes[2][8];
    mm_bool differs = MM_FALSE;
    int run, k;
    for (run = 0; run < 2; ++run) {
        setup();
        stm32_adc_set_noise(&g_adc, 2u, 7u);
        if (enabled(0, 1)) return 1;
        g_mv[0][0] = 1000;
        for (k = 0; k < 8; ++k) {
            mm_u32 c = convert(0);
            if (c < 1239u || c > 1243u) return 1;
            if (c != 1241u) differs = MM_TRUE;
            codes[run][k] = c;
        }
    }
    if (!differs || memcmp(codes[0], codes[1], sizeof(codes[0])) != 0) return 1;

    /* stm32_adc_reset() reseeds without a new init. */
    stm32_adc_reset(&g_adc);
    if (enabled(0, 1)) return 1;
    for (k = 0; k < 8; ++k) {
        if (convert(0) != codes[0][k]) return 1;
    }

    /* Noise is added before the clamp. */
    for (k = 0; k < 8; ++k) {
        mm_u32 hi, lo;
        g_mv[0][0] = 3300;
        hi = convert(0);
        g_mv[0][0] = 0;
        lo = convert(0);
        if (hi < 4093u || hi > 4095u || lo > 2u) return 1;
    }
    setup();
    stm32_adc_set_noise(&g_adc, 0u, 7u);
    if (enabled(0, 1) || conv_mv(1000, 1241u)) return 1;
    return 0;
}

static int test_dr_peek_and_isr(void)
{
    mm_u8 buf[4];
    mm_u32 v = 0;
    setup();
    if (enabled(0, 1)) return 1;
    g_mv[0][0] = 1000;
    ll_cr(0, ADSTART);
    tk(1);
    tk(100);
    if ((rd(0, ISR) & EOC) == 0u) return 1;
    if (mmio_bus_peek(&g_bus, ADC12 + DR, 4, buf) != MMIO_PEEK_OK) return 1;
    v = (mm_u32)buf[0] | ((mm_u32)buf[1] << 8) | ((mm_u32)buf[2] << 16) | ((mm_u32)buf[3] << 24);
    if (v != 1241u || (rd(0, ISR) & EOC) == 0u) return 1;
    mmio_set_peek_mode(MM_TRUE);
    v = rd(0, DR);
    mmio_set_peek_mode(MM_FALSE);
    if (v != 1241u || (rd(0, ISR) & EOC) == 0u) return 1;
    if (!mmio_bus_read(&g_bus, ADC12 + DR + 1u, 1, &v) || v != (1241u >> 8)) return 1;
    if ((rd(0, ISR) & EOC) != 0u) return 1;

    if ((rd(0, ISR) & (EOSMP | EOS)) != (EOSMP | EOS)) return 1;
    wr(0, ISR, 0u);
    if ((rd(0, ISR) & (EOSMP | EOS)) != (EOSMP | EOS)) return 1;
    wr(0, ISR, EOSMP);
    if ((rd(0, ISR) & (EOSMP | EOS)) != EOS) return 1;
    (void)mmio_bus_write(&g_bus, ADC12 + ISR, 1, EOS);
    if ((rd(0, ISR) & EOS) != 0u || (rd(0, ISR) & ADRDY) == 0u) return 1;
    return 0;
}

static int test_pin_path(void)
{
    int n;
    setup();
    if (enabled(0, 1)) return 1;
    g_mv[0][0] = 1000;
    g_analog[0][0] = MM_FALSE;
    n = g_samples;
    if (convert(0) != 0u || g_samples != n) return 1;
    g_analog[0][0] = MM_TRUE;

    wr(0, SQR1, 17u << 6);
    if (convert(0) != 0u || g_samples != n) return 1;
    if ((g_adc.warned & STM32_ADC_WARN_INTERNAL) == 0u) return 1;
    if (convert(0) != 0u) return 1;

    wr(0, SQR1, 2u << 6);
    if (convert(0) != 0u || g_samples != n) return 1;
    wr(0, SQR1, 25u << 6);
    if (convert(0) != 0u || g_samples != n) return 1;

    g_mv[0][1] = 1500;
    g_unbound[0][1] = MM_TRUE;
    wr(0, SQR1, 1u << 6);
    if (convert(0) != 0u || g_samples != n + 1) return 1;

    wr(0, SQR1, 0u);
    wr(0, DIFSEL, 1u << 1);
    if (convert(0) != 1241u || (g_adc.warned & STM32_ADC_WARN_DIFSEL) != 0u) return 1;
    wr(0, DIFSEL, 1u);
    if (convert(0) != 1241u || (g_adc.warned & STM32_ADC_WARN_DIFSEL) == 0u) return 1;
    wr(0, DIFSEL, 0u);

    g_mv[1][1] = 2000;
    wr(0, SQR1, 5u << 6);
    if (convert(0) != 2482u) return 1;

    if (enabled(1, 1)) return 1;
    g_mv[5][13] = 500;
    wr(1, SQR1, 2u << 6);
    if (convert(1) != 620u) return 1;
    return 0;
}

static int test_adstp_aborts(void)
{
    setup();
    if (enabled(0, 1)) return 1;
    if (conv_mv(1000, 1241u)) return 1;
    g_mv[0][0] = 2000;
    wr(0, ISR, EOSMP | EOC | EOS);
    ll_cr(0, ADSTART);
    tk(1);
    tk(3);
    if ((rd(0, ISR) & EOSMP) == 0u) return 1;
    ll_cr(0, ADSTP);
    if ((rd(0, CR) & (ADSTART | ADSTP)) != 0u) return 1;
    tk(100);
    if ((rd(0, ISR) & (EOC | EOS)) != 0u || rd(0, DR) != 1241u) return 1;
    if (next_event() != STM32_ADC_NO_EVENT) return 1;
    if (conv_mv(2000, 2482u)) return 1;
    return 0;
}

static int test_addis_and_recalibrate(void)
{
    setup();
    if (enabled(0, 1)) return 1;
    ll_cr(0, ADDIS);
    if ((rd(0, CR) & (ADEN | ADDIS)) != 0u || (rd(0, ISR) & ADRDY) == 0u) return 1;
    wr(0, ISR, ADRDY);
    g_poll = 1u;
    if (hal_calibrate(0) || hal_enable(0)) return 1;
    if (conv_mv(1000, 1241u)) return 1;
    return 0;
}

static int test_rmw_adstp_does_not_restart(void)
{
    setup();
    if (enabled(0, 1)) return 1;
    if (conv_mv(1000, 1241u)) return 1;
    wr(0, ISR, EOSMP | EOC | EOS);
    ll_cr(0, ADSTART);
    tk(1);
    tk(3);
    wr(0, CR, rd(0, CR) | ADSTP);
    if ((rd(0, CR) & (ADSTART | ADSTP)) != 0u || (rd(0, CR) & ADEN) == 0u) return 1;
    if (next_event() != STM32_ADC_NO_EVENT) return 1;
    tk(100);
    if ((rd(0, ISR) & (EOC | EOS)) != 0u || (rd(0, CR) & ADSTART) != 0u) return 1;
    return 0;
}

static int test_rmw_addis_does_not_reenable(void)
{
    setup();
    if (enabled(0, 1)) return 1;
    wr(0, CR, rd(0, CR) | ADDIS);
    if ((rd(0, CR) & (ADEN | ADDIS)) != 0u) return 1;
    if (next_event() != STM32_ADC_NO_EVENT) return 1;
    wr(0, ISR, ADRDY);
    tk(100);
    if ((rd(0, CR) & ADEN) != 0u || (rd(0, ISR) & ADRDY) != 0u) return 1;
    return 0;
}

static int test_clock_gating(void)
{
    setup();
    bring_up(0, 1);
    g_clk[0] = MM_FALSE;
    if (rd(0, CR) != 0u || rd_common(CCR) != 0u) return 1;
    wr(0, CFGR2, 5u);
    wr_common(CCR, 3u << 16);
    g_clk[0] = MM_TRUE;
    if (rd(0, CFGR2) != 0u || rd_common(CCR) != (1u << 16)) return 1;
    g_clk[1] = MM_FALSE;
    if (rd(0, CR) != ADVREGEN || rd(1, CR) != 0u) return 1;
    g_clk[1] = MM_TRUE;
    if (rd(1, CR) != DEEPPWD) return 1;

    ll_cal(0);
    tk(1);
    tk(500);
    g_clk[0] = MM_FALSE;
    tk(1000);
    if (next_event() != STM32_ADC_NO_EVENT) return 1;
    g_clk[0] = MM_TRUE;
    if ((rd(0, CR) & ADCAL) == 0u || next_event() != 835u) return 1;
    tk(834);
    if ((rd(0, CR) & ADCAL) == 0u) return 1;
    tk(1);
    if ((rd(0, CR) & ADCAL) != 0u) return 1;
    return 0;
}

static int test_access_denied_raz_wi(void)
{
    mm_u8 buf[4];
    mm_u32 v = 0u;

    setup();
    g_soc.access_allowed = fake_access;
    g_access = MM_TRUE;
    wr(0, SMPR1, 0x123u);
    g_access = MM_FALSE;
    if (rd(0, SMPR1) != 0u || rd(0, CR) != 0u) return 1;
    wr(0, SMPR1, 0x456u);
    wr(0, CR, ADVREGEN);
    wr_common(CCR, 3u << 16);
    if (mmio_bus_peek(&g_bus, ADC12 + SMPR1, 4, buf) != MMIO_PEEK_OK) return 1;
    if (buf[0] != 0x23u || buf[1] != 0x01u) return 1;
    mmio_set_peek_mode(MM_TRUE);
    (void)mmio_bus_read(&g_bus, ADC12 + SMPR1, 4, &v);
    mmio_set_peek_mode(MM_FALSE);
    if (v != 0x123u) return 1;
    g_access = MM_TRUE;
    if (rd(0, SMPR1) != 0x123u || rd(0, CR) != DEEPPWD || rd_common(CCR) != 0u) return 1;
    return 0;
}

static int test_next_event(void)
{
    setup();
    bring_up(0, 1);
    if (next_event() != STM32_ADC_NO_EVENT) return 1;
    ll_cal(0);
    if (next_event() != 0u) return 1;
    tk(1);
    if (next_event() != 1335u) return 1;
    tk(1330);
    if (next_event() != 5u) return 1;
    tk(5);
    if ((rd(0, CR) & ADCAL) != 0u || next_event() != STM32_ADC_NO_EVENT) return 1;
    tk(4);
    ll_cr(0, ADEN);
    if ((rd(0, CR) & ADEN) == 0u || next_event() != 0u) return 1;
    tk(1);
    if (next_event() != 15u) return 1;
    tk(15);
    if ((rd(0, ISR) & ADRDY) == 0u || next_event() != STM32_ADC_NO_EVENT) return 1;
    ll_cr(0, ADSTART);
    if (next_event() != 0u) return 1;
    tk(1);
    if (next_event() != 3u) return 1;
    tk(3);
    if ((rd(0, ISR) & EOSMP) == 0u || next_event() != 12u) return 1;
    tk(12);
    if ((rd(0, ISR) & EOC) == 0u || next_event() != STM32_ADC_NO_EVENT) return 1;

    /* ADSTART before ADRDY: the bound is ADRDY, then the sampling instant. */
    setup();
    bring_up(0, 1);
    ll_cr(0, ADEN);
    ll_cr(0, ADSTART);
    if (next_event() != 0u) return 1;
    tk(1);
    if (next_event() != 15u) return 1;
    tk(15);
    if ((rd(0, ISR) & ADRDY) == 0u || next_event() != 3u) return 1;
    tk(3);
    if ((rd(0, ISR) & EOSMP) == 0u || next_event() != 12u) return 1;
    tk(12);
    if ((rd(0, ISR) & EOC) == 0u || next_event() != STM32_ADC_NO_EVENT) return 1;
    return 0;
}

static int test_clock_warnings(void)
{
    setup();
    bring_up(0, 0);
    ll_cal(0);
    if ((g_adc.warned & STM32_ADC_WARN_CKMODE0) == 0u) return 1;
    tk(1);
    tk(1334);
    if ((rd(0, CR) & ADCAL) == 0u) return 1;
    tk(1);
    if ((rd(0, CR) & ADCAL) != 0u || (g_adc.warned & STM32_ADC_WARN_FREQ) != 0u) return 1;

    setup();
    g_hclk = 250000000u;
    bring_up(0, 1);
    ll_cal(0);
    if ((g_adc.warned & STM32_ADC_WARN_FREQ) == 0u) return 1;

    setup();
    g_hclk = 250000000u;
    bring_up(0, 3);
    ll_cal(0);
    if ((g_adc.warned & (STM32_ADC_WARN_FREQ | STM32_ADC_WARN_CKMODE0)) != 0u) return 1;

    setup();
    g_hclk = 4000000u;
    bring_up(0, 3);
    ll_cal(0);
    if ((g_adc.warned & STM32_ADC_WARN_FREQ) == 0u) return 1;
    return 0;
}

static int test_regulator_warning(void)
{
    setup();
    set_ckmode(1);
    ll_cr(0, ADEN);
    if ((rd(0, CR) & ADEN) == 0u || (g_adc.warned & STM32_ADC_WARN_REGULATOR) == 0u) return 1;
    tk(1);
    tk(15);
    if ((rd(0, ISR) & ADRDY) == 0u) return 1;

    setup();
    if (enabled(0, 1) || (g_adc.warned & STM32_ADC_WARN_REGULATOR) != 0u) return 1;
    return 0;
}

static int test_oversampling_warning(void)
{
    setup();
    if (enabled(0, 1)) return 1;
    modify(0, CFGR2, 0u, CFGR2_ROVSE);
    if (conv_mv(1000, 1241u)) return 1;
    if ((rd(0, CR) & ADSTART) != 0u || next_event() != STM32_ADC_NO_EVENT) return 1;
    if ((g_adc.warned & STM32_ADC_WARN_OVERSAMPLING) == 0u) return 1;
    return 0;
}

static int test_adstart_before_adrdy_waits(void)
{
    setup();
    bring_up(0, 1);
    g_mv[0][0] = 1000;
    ll_cr(0, ADEN);
    ll_cr(0, ADSTART);
    if ((rd(0, CR) & ADSTART) == 0u) return 1;
    tk(1);
    tk(14);
    if ((rd(0, ISR) & (ADRDY | EOSMP)) != 0u) return 1;
    tk(1);
    if ((rd(0, ISR) & (ADRDY | EOSMP)) != ADRDY) return 1;
    tk(2);
    if ((rd(0, ISR) & EOSMP) != 0u) return 1;
    tk(1);
    if ((rd(0, ISR) & EOSMP) == 0u) return 1;
    tk(12);
    if ((rd(0, ISR) & EOC) == 0u || rd(0, DR) != 1241u) return 1;
    return 0;
}

static int test_two_rank_scan(void)
{
    mm_u64 at[4] = { 0, 0, 0, 0 };
    mm_u32 codes[2] = { 0, 0 };
    mm_bool eos_early = MM_FALSE;
    mm_u64 n;
    int eoc = 0;

    setup();
    if (enabled(0, 1)) return 1;
    g_mv[0][0] = 1000;
    g_mv[0][1] = 2000;
    wr(0, SQR1, 1u | (0u << 6) | (1u << 12));
    wr(0, SMPR1, 1u << 3);
    wr(0, ISR, EOSMP | EOC | EOS);
    ll_cr(0, ADSTART);
    tk(1);
    for (n = 1u; n <= 100u && eoc < 2; ++n) {
        tk(1);
        if (g_samples >= 1 && at[0] == 0u) at[0] = n;
        if (g_samples >= 2 && at[2] == 0u) at[2] = n;
        if ((rd(0, ISR) & EOC) != 0u) {
            if (eoc == 0 && (rd(0, ISR) & EOS) != 0u) eos_early = MM_TRUE;
            at[eoc * 2 + 1] = n;
            codes[eoc++] = rd(0, DR);
        }
    }
    if (at[0] != 3u || at[1] != 15u || at[2] != 22u || at[3] != 34u) return 1;
    if (codes[0] != 1241u || codes[1] != 2482u || eos_early) return 1;
    if ((rd(0, ISR) & EOS) == 0u || (rd(0, CR) & ADSTART) != 0u) return 1;
    return 0;
}

/* Ranks 5 and 6 come from SQR2. */
static int test_six_rank_scan(void)
{
    static const mm_u32 expected[6] = { 2482u, 1241u, 4095u, 2482u, 1241u, 4095u };
    mm_u32 codes[6];
    int eoc = 0;
    int n;

    setup();
    if (enabled(0, 1)) return 1;
    g_mv[0][0] = 1000;
    g_mv[0][1] = 2000;
    g_mv[1][1] = 3300;
    wr(0, SQR1, 5u | (1u << 6) | (0u << 12) | (5u << 18) | (1u << 24));
    wr(0, 0x34u, 0u | (5u << 6));
    wr(0, ISR, EOSMP | EOC | EOS);
    ll_cr(0, ADSTART);
    tk(1);
    for (n = 0; n < 200 && eoc < 6; ++n) {
        tk(1);
        if ((rd(0, ISR) & EOC) != 0u) {
            if (eoc < 5 && (rd(0, ISR) & EOS) != 0u) return 1;
            codes[eoc++] = rd(0, DR);
        }
    }
    if (eoc != 6) return 1;
    for (n = 0; n < 6; ++n) {
        if (codes[n] != expected[n]) return 1;
    }
    if ((rd(0, ISR) & EOS) == 0u || (rd(0, CR) & ADSTART) != 0u) return 1;
    return 0;
}

static int test_clock_gating_mid_conversion(void)
{
    setup();
    if (enabled(0, 1)) return 1;
    g_mv[0][0] = 1000;
    wr(0, ISR, EOSMP | EOC | EOS);
    ll_cr(0, ADSTART);
    tk(1);
    tk(5);
    g_clk[0] = MM_FALSE;
    tk(1000);
    g_clk[0] = MM_TRUE;
    if ((rd(0, ISR) & EOC) != 0u || next_event() != 10u) return 1;
    tk(9);
    if ((rd(0, ISR) & EOC) != 0u) return 1;
    tk(1);
    if ((rd(0, ISR) & EOC) == 0u || rd(0, DR) != 1241u) return 1;
    return 0;
}

static int test_init_reads_settings(void)
{
    int rc = 1;
    mm_signal_reset();
    mm_signal_set_adc_vref_mv(2500);
    setup();
    if (g_adc.vref_mv != 2500) goto out;
    if (enabled(0, 1) || conv_mv(1000, 1638u)) goto out;
    mm_signal_set_vdd_mv(3000);
    stm32_adc_reset(&g_adc);
    stm32_adc_init(&g_adc, &g_soc, 0, &g_nvic);
    if (g_adc.vref_mv != 2500) goto out;
    mm_signal_set_adc_vref_mv(0);
    stm32_adc_init(&g_adc, &g_soc, 0, &g_nvic);
    if (g_adc.vref_mv != 3000) goto out;

    mm_signal_reset();
    stm32_adc_init(&g_adc, 0, &g_bus, &g_nvic);
    stm32_adc_init(&g_adc, &g_soc, 0, &g_nvic);
    if (g_adc.vref_mv != 3300) goto out;
    if (enabled(0, 1) || conv_mv(1000, 1241u)) goto out;
    rc = 0;
out:
    mm_signal_reset();
    return rc;
}

static int test_reboot_replays_noise(void)
{
    mm_u32 codes[8];
    mm_bool differs = MM_FALSE;
    int rc = 1;
    int k;
    mm_signal_reset();
    mm_signal_set_adc_noise(2u, 7u);
    setup();
    if (g_adc.noise_lsb != 2u || g_adc.noise_seed != 7u) goto out;
    if (enabled(0, 1)) goto out;
    g_mv[0][0] = 1000;
    for (k = 0; k < 8; ++k) {
        codes[k] = convert(0);
        if (codes[k] < 1239u || codes[k] > 1243u) goto out;
    }

    /* main.c reboot order: timer reset, then timer init. */
    stm32_adc_reset(&g_adc);
    stm32_adc_init(&g_adc, &g_soc, 0, &g_nvic);
    if (enabled(0, 1)) goto out;
    for (k = 0; k < 8; ++k) {
        if (convert(0) != codes[k]) goto out;
    }

    mm_signal_set_adc_noise(2u, 8u);
    stm32_adc_init(&g_adc, &g_soc, 0, &g_nvic);
    if (enabled(0, 1)) goto out;
    for (k = 0; k < 8; ++k) {
        mm_u32 c = convert(0);
        if (c < 1239u || c > 1243u) goto out;
        if (c != codes[k]) differs = MM_TRUE;
    }
    if (!differs) goto out;
    rc = 0;
out:
    mm_signal_reset();
    return rc;
}

static int test_ckmode_locked_while_enabled(void)
{
    setup();
    if (enabled(0, 1)) return 1;
    wr_common(CCR, (3u << 16) | (2u << 18) | (1u << 22));
    if ((rd_common(CCR) & CCR_CKMODE) != (1u << 16)) return 1;
    if ((rd_common(CCR) & CCR_PRESC) != 0u || (rd_common(CCR) & (1u << 22)) == 0u) return 1;
    ll_cr(0, ADDIS);
    if ((rd(0, CR) & ADEN) != 0u) return 1;
    set_ckmode(3);
    if ((rd_common(CCR) & CCR_CKMODE) != (3u << 16)) return 1;
    wr_common(CCR, (rd_common(CCR) & ~CCR_PRESC) | (2u << 18));
    if ((rd_common(CCR) & CCR_PRESC) != (2u << 18)) return 1;
    return 0;
}

/* ADRDY, the deferred start and both ranks inside one long batch. */
static int test_one_tick_crosses_deadlines(void)
{
    static const struct {
        mm_u32 ckmode;
        mm_u64 ahead0, ahead1;
    } cases[] = {
        { 1, 15 + 3, 15 + 22 },
        { 3, 60 + 10, 60 + 86 },
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        setup();
        bring_up(0, cases[i].ckmode);
        g_mv[0][0] = 1000;
        g_mv[0][1] = 2000;
        wr(0, SQR1, 1u | (0u << 6) | (1u << 12));
        wr(0, SMPR1, 1u << 3);
        ll_cr(0, ADEN);
        ll_cr(0, ADSTART);
        tk(1);
        tk(10000);
        if (g_samples != 2 || g_aheads[0] != cases[i].ahead0 || g_aheads[1] != cases[i].ahead1) {
            printf("  case %u: aheads %llu %llu\n", (unsigned)i,
                   (unsigned long long)g_aheads[0], (unsigned long long)g_aheads[1]);
            return 1;
        }
        if ((rd(0, ISR) & (ADRDY | EOSMP | EOC | EOS)) != (ADRDY | EOSMP | EOC | EOS)) return 1;
        if (rd(0, DR) != 1241u || rd(0, DR) != 2482u || (rd(0, CR) & ADSTART) != 0u) return 1;
        if (next_event() != STM32_ADC_NO_EVENT) return 1;
    }
    return 0;
}

static int test_hal_flow(void)
{
    mm_bool ccr = MM_FALSE;
    setup();
    if (hal_init(0, 1, 0, &ccr) || !ccr) return 1;
    if (hal_calibrate(0)) return 1;
    hal_config_channel(0, 1, 0, 0);
    g_mv[0][0] = 1000;
    if (hal_start(0) || hal_poll(0) || rd(0, DR) != 1241u) return 1;
    if (hal_stop(0) || (rd(0, CR) & ADEN) != 0u) return 1;
    g_mv[0][0] = 2000;
    if (hal_start(0) || hal_poll(0) || rd(0, DR) != 2482u) return 1;
    g_mv[0][0] = 500;
    if (hal_start(0) || hal_poll(0) || rd(0, DR) != 620u) return 1;
    if (hal_stop(0) || (rd(0, CR) & (ADEN | ADSTART)) != 0u) return 1;
    return 0;
}

static int test_hal_enable_retry_after_calibration(void)
{
    mm_bool ccr = MM_FALSE;
    setup();
    g_poll = 1u;
    if (hal_init(0, 1, 0, &ccr) || hal_calibrate(0)) return 1;
    g_enable_writes = 0;
    if (hal_enable(0)) return 1;
    if (g_enable_writes < 2 || (rd(0, ISR) & ADRDY) == 0u) return 1;
    return 0;
}

static int test_hal_init_skips_ccr_when_other_enabled(void)
{
    mm_bool ccr = MM_FALSE;
    setup();
    if (hal_init(1, 1, 0, &ccr) || !ccr || hal_enable(1)) return 1;
    if (hal_init(0, 2, 0, &ccr) || ccr) return 1;
    if (rd_common(CCR) != (1u << 16)) return 1;
    return 0;
}

static int test_common_block(void)
{
    mm_u32 v = 0;
    setup();
    if (enabled(0, 1)) return 1;
    if (rd_common(CSR) != ADRDY) return 1;
    if (enabled(1, 1)) return 1;
    if (rd_common(CSR) != (ADRDY | (ADRDY << 16))) return 1;
    g_mv[0][0] = 1000;
    wr(1, SQR1, 0u);
    ll_cr(1, ADSTART);
    tk(1);
    tk(100);
    if ((rd_common(CSR) & (EOC << 16)) == 0u || (rd_common(CSR) & EOC) != 0u) return 1;
    if (rd(1, DR) != 1241u || (rd_common(CSR) & (EOC << 16)) != 0u) return 1;

    if (!mmio_bus_read(&g_bus, ADC12_SEC + CR, 4, &v) || v != rd(0, CR)) return 1;
    if (!mmio_bus_write(&g_bus, ADC12_SEC + 0x100u + CFGR2, 4, 0x55u) || rd(1, CFGR2) != 0x55u) return 1;
    if (!mmio_bus_read(&g_bus, ADC12_SEC + COMMON + CCR, 4, &v) || v != (1u << 16)) return 1;
    if (!mmio_bus_write(&g_bus, ADC12 + 0x200u, 4, 0x1234u)) return 1;
    if (!mmio_bus_read(&g_bus, ADC12 + 0x200u, 4, &v) || v != 0u) return 1;
    if (rd_common(CDR) != 0u) return 1;
    return 0;
}

static int test_real_signal_path(void)
{
    mm_i16 samples[2] = { 500, 2000 };
    int rc = 1;

    setup();
    g_soc.sample_mv = 0;
    mm_signal_reset();
    mm_signal_set_cpu_hz(64000000ull);
    if (!write_fixture(g_tmp_path, "ain", samples, 2, 1000000u)) return 1;
    if (!mm_signal_load(g_tmp_path)) return 1;
    if (mm_signal_bind("ain", 0, 0, MM_SIGNAL_ROLE_GPIO) < 0) return 1;
    mm_signal_set_trigger(0x08001000u);
    g_with_signal = MM_TRUE;

    /* 1 ms = 64000 cycles; the step is at 64000 cycles after the trigger. */
    if (enabled(0, 1) || convert(0) != 620u) goto out;
    mm_signal_check_trigger(0x08001000u);
    wr(0, ISR, EOSMP | EOC | EOS);
    ll_cr(0, ADSTART);
    tk(63996);
    tk(100);
    if ((rd(0, ISR) & EOC) == 0u || rd(0, DR) != 620u) goto out;
    wr(0, ISR, EOSMP | EOC | EOS);
    ll_cr(0, ADSTART);
    tk(1);
    tk(100);
    if ((rd(0, ISR) & EOC) == 0u || rd(0, DR) != 2482u) goto out;
    rc = 0;
out:
    g_with_signal = MM_FALSE;
    mm_signal_reset();
    remove(g_tmp_path);
    return rc;
}

static mm_bool pend(mm_u32 irq)
{
    return mm_nvic_is_pending(&g_nvic, irq) ? MM_TRUE : MM_FALSE;
}

static void unpend(mm_u32 irq)
{
    mm_nvic_set_pending(&g_nvic, irq, MM_FALSE);
}

/* One software-started conversion run to its end; nothing is read. */
static void run_conv(mm_u32 inst, mm_i32 mv)
{
    g_mv[0][0] = mv;
    ll_cr(inst, ADSTART);
    tk(1);
    tk(100);
}

static mm_u32 peek_dr(void)
{
    mm_u8 b[4];
    if (mmio_bus_peek(&g_bus, ADC12 + DR, 4, b) != MMIO_PEEK_OK) return 0xFFFFFFFFu;
    return (mm_u32)b[0] | ((mm_u32)b[1] << 8) | ((mm_u32)b[2] << 16) | ((mm_u32)b[3] << 24);
}

static int test_irq_level_ier_write(void)
{
    setup();
    if (enabled(0, 1) || pend(37)) return 1;
    wr(0, IER, ADRDY);
    if (!pend(37)) return 1;
    unpend(37);
    wr(0, IER, 0u);
    wr(0, ISR, ADRDY);
    run_conv(0, 1000);
    if ((rd(0, ISR) & EOC) == 0u || pend(37)) return 1;
    wr(0, IER, EOC);
    if (!pend(37)) return 1;
    unpend(37);
    wr(0, ISR, EOSMP);
    if (!pend(37)) return 1;
    unpend(37);
    if (rd(0, DR) != 1241u || pend(37)) return 1;
    wr(0, IER, EOC);
    if (pend(37) || pend(69)) return 1;
    return 0;
}

/* Cycle of the first pend after the start, 0 if none within 100 cycles. */
static mm_u64 first_pend(mm_u32 inst, mm_u32 irq, mm_u32 ier, mm_u32 cfgr)
{
    mm_u64 n;
    modify(inst, CFGR, 0u, cfgr);
    wr(inst, ISR, ADRDY | EOSMP | EOC | EOS | OVR);
    wr(inst, IER, ier);
    unpend(irq);
    g_mv[0][0] = 1000;
    ll_cr(inst, ADSTART);
    tk(1);
    for (n = 1u; n <= 100u; ++n) {
        tk(1);
        if (pend(irq)) return n;
    }
    return 0u;
}

static int test_irq_each_flag(void)
{
    static const struct {
        mm_u32 ier, cfgr;
        mm_u64 at;
    } cases[] = {
        { EOSMP, 0u, 3u },
        { EOC, 0u, 15u },
        { EOS, 0u, 15u },
        { OVR, CFGR_CONT | CFGR_OVRMOD, 30u },
        { OVR, CFGR_CONT, 60u },
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        mm_u64 at;
        setup();
        if (enabled(0, 1)) return 1;
        at = first_pend(0, 37, cases[i].ier, cases[i].cfgr);
        if (at != cases[i].at || pend(69)) {
            printf("  case %u: pend at %llu\n", (unsigned)i, (unsigned long long)at);
            return 1;
        }
    }
    setup();
    if (enabled(1, 1)) return 1;
    if (first_pend(1, 69, EOC, 0u) != 15u || pend(37)) return 1;

    setup();
    bring_up(0, 1);
    wr(0, IER, ADRDY);
    ll_cr(0, ADEN);
    tk(1);
    tk(14);
    if (pend(37)) return 1;
    tk(1);
    if (!pend(37) || pend(69)) return 1;
    return 0;
}

static int test_irq_repend_fifo(void)
{
    setup();
    if (enabled(0, 1)) return 1;
    wr(0, IER, EOC);
    run_conv(0, 1000);
    run_conv(0, 2000);
    unpend(37);
    if (rd(0, DR) != 1241u || (rd(0, ISR) & EOC) == 0u || !pend(37)) return 1;
    unpend(37);
    if (rd(0, DR) != 2482u || (rd(0, ISR) & EOC) != 0u || pend(37)) return 1;
    return 0;
}

/* A request raised while the handler runs is held: cleared flags drop it, flags
 * still set at return pend at the next tick; outside the handler it stays latched. */
static int test_irq_held_while_active(void)
{
    setup();
    if (enabled(0, 1)) return 1;
    g_mv[0][1] = 2000;
    g_mv[1][1] = 3300;
    wr(0, SQR1, 2u | (5u << 6) | (0u << 12) | (1u << 18));
    wr(0, IER, EOS);
    run_conv(0, 1000);
    if (!pend(37)) return 1;
    unpend(37);
    mm_nvic_set_active(&g_nvic, 37, MM_TRUE);
    if (rd(0, DR) != 4095u || pend(37)) return 1;
    wr(0, ISR, EOC | EOS);
    mm_nvic_set_active(&g_nvic, 37, MM_FALSE);
    if ((rd(0, ISR) & EOC) != 0u || rd(0, DR) != 1241u || pend(37)) return 1;
    tk(1);
    if (pend(37)) return 1;
    run_conv(0, 1000);
    if (!pend(37)) return 1;
    unpend(37);
    mm_nvic_set_active(&g_nvic, 37, MM_TRUE);
    wr(0, IER, EOS);
    tk(1);
    if (pend(37)) return 1;
    mm_nvic_set_active(&g_nvic, 37, MM_FALSE);
    tk(1);
    if (!pend(37)) return 1;
    unpend(37);
    wr(0, ISR, EOC | EOS);
    run_conv(0, 1000);
    if (!pend(37)) return 1;
    wr(0, ISR, EOC | EOS);
    if (!pend(37)) return 1;
    return 0;
}

struct hal_it {
    mm_bool preserved;
    mm_bool get_value;
    int entries;
    int callbacks;
    int err_internal;
    int err_ovr;
    mm_u32 codes[8];
};

/* HAL_ADC_IRQHandler for the regular group, software trigger, no DMA. */
static void hal_irq_handler(mm_u32 inst, struct hal_it *h)
{
    mm_u32 isr = rd(inst, ISR);
    mm_u32 ier = rd(inst, IER);
    h->entries++;
    if ((isr & ier & EOSMP) != 0u) {
        wr(inst, ISR, EOSMP);
    }
    if ((isr & ier & (EOC | EOS)) != 0u) {
        if ((rd(inst, CFGR) & CFGR_CONT) == 0u && (rd(inst, ISR) & EOS) != 0u) {
            if ((rd(inst, CR) & ADSTART) == 0u) {
                modify(inst, IER, EOC | EOS, 0u);
            } else {
                h->err_internal++;
            }
        }
        if (h->get_value && h->callbacks < 8) {
            h->codes[h->callbacks] = rd(inst, DR);
        }
        h->callbacks++;
        wr(inst, ISR, EOC | EOS);
    }
    if ((isr & ier & OVR) != 0u) {
        if (h->preserved || (rd(inst, CFGR) & CFGR_DMAEN) != 0u) {
            h->err_ovr++;
        }
        wr(inst, ISR, OVR);
    }
}

/* HAL_ADC_Start_IT; OVRMOD stands in for Init.Overrun. */
static int hal_start_it(mm_u32 inst, mm_bool eos_sel, mm_bool preserved, struct hal_it *h)
{
    memset(h, 0, sizeof(*h));
    h->preserved = preserved;
    h->get_value = MM_TRUE;
    if ((rd(inst, CR) & ADSTART) != 0u || hal_enable(inst)) return 1;
    modify(inst, CFGR, CFGR_OVRMOD, preserved ? 0u : CFGR_OVRMOD);
    wr(inst, ISR, EOC | EOS | OVR);
    modify(inst, IER, EOC | EOS | OVR, 0u);
    modify(inst, IER, 0u, eos_sel ? EOS : EOC);
    if (preserved) {
        modify(inst, IER, 0u, OVR);
    }
    ll_cr(inst, ADSTART);
    return 0;
}

/* The core takes the IRQ after each batch and tail-chains while it is pending. */
static void hal_run(mm_u32 inst, mm_u32 irq, struct hal_it *h, mm_u64 cycles)
{
    mm_u64 n;
    for (n = 0u; n < cycles; ++n) {
        tk(1);
        while (pend(irq) && h->entries < 64) {
            unpend(irq);
            mm_nvic_set_active(&g_nvic, irq, MM_TRUE);
            hal_irq_handler(inst, h);
            mm_nvic_set_active(&g_nvic, irq, MM_FALSE);
        }
    }
}

static int test_hal_irq_flow(void)
{
    struct hal_it h;
    mm_bool ccr = MM_FALSE;
    int k;

    setup();
    g_mv[0][0] = 1000;
    g_mv[0][1] = 2000;
    g_mv[1][1] = 3300;
    if (hal_init(0, 1, 0, &ccr) || hal_calibrate(0)) return 1;
    wr(0, SQR1, 2u | (5u << 6) | (0u << 12) | (1u << 18));

    if (hal_start_it(0, MM_FALSE, MM_TRUE, &h)) return 1;
    hal_run(0, 37, &h, 200);
    if (h.callbacks != 3 || h.entries != 3 || h.err_internal != 0 || h.err_ovr != 0) return 1;
    if (h.codes[0] != 4095u || h.codes[1] != 1241u || h.codes[2] != 2482u) return 1;
    if (rd(0, IER) != OVR || (rd(0, CR) & ADSTART) != 0u) return 1;

    /* EOS selection reads only the FIFO head; clearing EOC releases the rest. */
    if (hal_start_it(0, MM_TRUE, MM_TRUE, &h)) return 1;
    hal_run(0, 37, &h, 200);
    if (h.callbacks != 1 || h.entries != 1 || h.err_internal != 0 || h.err_ovr != 0) return 1;
    if (h.codes[0] != 4095u || rd(0, IER) != OVR || (rd(0, ISR) & EOC) != 0u) return 1;
    g_mv[1][1] = 2000;
    if (hal_start_it(0, MM_TRUE, MM_TRUE, &h)) return 1;
    hal_run(0, 37, &h, 200);
    if (h.callbacks != 1 || h.err_ovr != 0 || h.codes[0] != 2482u) return 1;
    if ((rd(0, ISR) & (EOC | OVR)) != 0u) return 1;

    wr(0, SQR1, 0u);
    modify(0, CFGR, 0u, CFGR_CONT);
    if (hal_start_it(0, MM_FALSE, MM_TRUE, &h)) return 1;
    hal_run(0, 37, &h, 95);
    if (h.callbacks != 6 || h.entries != 6 || h.err_internal != 0 || h.err_ovr != 0) return 1;
    for (k = 0; k < 6; ++k) {
        if (h.codes[k] != 1241u) return 1;
    }
    if (rd(0, IER) != (EOC | OVR) || (rd(0, CR) & ADSTART) == 0u) return 1;
    ll_cr(0, ADSTP);

    /* Preserved data and a callback that skips the read: the EOC clear
     * releases each result, so nothing overruns. */
    if (hal_start_it(0, MM_FALSE, MM_TRUE, &h)) return 1;
    h.get_value = MM_FALSE;
    hal_run(0, 37, &h, 65);
    if (h.callbacks != 4 || h.err_ovr != 0 || h.err_internal != 0 || (rd(0, ISR) & OVR) != 0u) return 1;
    ll_cr(0, ADSTP);
    if (rd(0, DR) != 1241u) return 1;

    /* Overwritten data without DMA: HAL clears OVR without an error. */
    if (hal_start_it(0, MM_FALSE, MM_FALSE, &h)) return 1;
    wr(0, IER, OVR);
    hal_run(0, 37, &h, 35);
    if (h.entries != 1 || h.err_ovr != 0 || (rd(0, ISR) & OVR) != 0u) return 1;
    ll_cr(0, ADSTP);
    return 0;
}

/* EOC cycles after the start in continuous mode, reading DR at each one. */
static int cont_eoc(mm_u32 ckmode, mm_u32 smpr1, mm_u32 sqr1, mm_u64 *at, mm_bool *eos, int count)
{
    mm_u64 n;
    int k = 0;
    setup();
    if (enabled(0, ckmode)) return 1;
    modify(0, CFGR, 0u, CFGR_CONT | CFGR_OVRMOD);
    wr(0, SMPR1, smpr1);
    wr(0, SQR1, sqr1);
    g_mv[0][0] = 1000;
    g_mv[0][1] = 2000;
    wr(0, ISR, EOSMP | EOC | EOS);
    ll_cr(0, ADSTART);
    tk(1);
    for (n = 1u; n <= 1000u && k < count; ++n) {
        tk(1);
        if ((rd(0, ISR) & EOC) != 0u) {
            at[k] = n;
            eos[k] = (rd(0, ISR) & EOS) != 0u;
            wr(0, ISR, EOS);
            (void)rd(0, DR);
            k++;
        }
        if ((rd(0, CR) & ADSTART) == 0u) return 1;
    }
    if (k != count || next_event() == STM32_ADC_NO_EVENT) return 1;
    return 0;
}

static int test_continuous_rate(void)
{
    static const struct {
        mm_u32 ckmode, smpr1, sqr1;
        mm_u64 at[4];
        mm_bool eos[4];
    } cases[] = {
        { 1, 0u, 0u, { 15, 30, 45, 60 }, { 1, 1, 1, 1 } },
        { 3, 0u, 0u, { 60, 120, 180, 240 }, { 1, 1, 1, 1 } },
        { 1, 1u, 0u, { 19, 38, 57, 76 }, { 1, 1, 1, 1 } },
        { 1, 1u << 3, 1u | (0u << 6) | (1u << 12), { 15, 34, 49, 68 }, { 0, 1, 0, 1 } },
    };
    size_t i;
    int k;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        mm_u64 at[4];
        mm_bool eos[4];
        if (cont_eoc(cases[i].ckmode, cases[i].smpr1, cases[i].sqr1, at, eos, 4)) return 1;
        for (k = 0; k < 4; ++k) {
            if (at[k] != cases[i].at[k] || (eos[k] != 0) != (cases[i].eos[k] != 0)) {
                printf("  case %u: EOC %d at %llu\n", (unsigned)i, k, (unsigned long long)at[k]);
                return 1;
            }
        }
    }
    return 0;
}

static int test_continuous_adstp(void)
{
    mm_u64 n;
    int eoc = 0;
    setup();
    if (enabled(0, 1)) return 1;
    modify(0, CFGR, 0u, CFGR_CONT | CFGR_OVRMOD);
    g_mv[0][0] = 1000;
    ll_cr(0, ADSTART);
    tk(1);
    for (n = 1u; n <= 50u; ++n) {
        tk(1);
        if ((rd(0, ISR) & EOC) != 0u) {
            if (rd(0, DR) != 1241u) return 1;
            eoc++;
        }
    }
    if (eoc != 3 || (rd(0, CR) & ADSTART) == 0u) return 1;
    wr(0, ISR, EOSMP | EOC | EOS | OVR);
    ll_cr(0, ADSTP);
    if ((rd(0, CR) & (ADSTART | ADSTP)) != 0u || next_event() != STM32_ADC_NO_EVENT) return 1;
    tk(1000);
    if ((rd(0, ISR) & (EOSMP | EOC | EOS)) != 0u) return 1;
    modify(0, CFGR, CFGR_CONT, 0u);
    if (convert(0) != 1241u || (rd(0, CR) & ADSTART) != 0u) return 1;
    if (next_event() != STM32_ADC_NO_EVENT) return 1;
    return 0;
}

/* Continuous, CKMODE = 1, 2.5 clocks: samples at 3, 18, 33, 48, 63 get
 * 500, 1000, 2000, 3300, 3300 mV; EOC 12 cycles later. */
static int start_cont(mm_u32 cfgr)
{
    setup();
    if (enabled(0, 1)) return 1;
    modify(0, CFGR, 0u, CFGR_CONT | cfgr);
    ll_cr(0, ADSTART);
    tk(1);
    return 0;
}

/* Continuous scan IN5, IN0, IN1 at 3300, 1000, 2000 mV; EOC every 15 cycles. */
static int start_cont_scan(mm_u32 cfgr)
{
    setup();
    if (enabled(0, 1)) return 1;
    g_mv[0][0] = 1000;
    g_mv[0][1] = 2000;
    g_mv[1][1] = 3300;
    wr(0, SQR1, 2u | (5u << 6) | (0u << 12) | (1u << 18));
    modify(0, CFGR, 0u, CFGR_CONT | cfgr);
    ll_cr(0, ADSTART);
    tk(1);
    return 0;
}

static void run_steps(mm_u64 from, mm_u64 to)
{
    mm_u64 n;
    for (n = from; n <= to; ++n) {
        g_mv[0][0] = n <= 3u ? 500 : n <= 18u ? 1000 : n <= 33u ? 2000 : 3300;
        tk(1);
    }
}

static int test_fifo_depth_then_ovr(void)
{
    if (start_cont(0u)) return 1;
    run_steps(1, 45);
    if ((rd(0, ISR) & (EOC | OVR)) != EOC) return 1;
    run_steps(46, 60);
    if ((rd(0, ISR) & OVR) == 0u) return 1;
    if (rd(0, DR) != 620u || (rd(0, ISR) & EOC) == 0u) return 1;
    if (rd(0, DR) != 1241u || (rd(0, ISR) & EOC) == 0u) return 1;
    if (rd(0, DR) != 2482u || (rd(0, ISR) & EOC) != 0u) return 1;
    if (rd(0, DR) != 2482u) return 1;
    wr(0, ISR, OVR);
    run_steps(61, 75);
    if ((rd(0, ISR) & (EOC | OVR)) != EOC || rd(0, DR) != 4095u) return 1;
    ll_cr(0, ADSTP);
    return 0;
}

static int test_ovrmod1_overwrite(void)
{
    if (start_cont(CFGR_OVRMOD)) return 1;
    run_steps(1, 29);
    if ((rd(0, ISR) & OVR) != 0u) return 1;
    run_steps(30, 30);
    if ((rd(0, ISR) & OVR) == 0u) return 1;
    run_steps(31, 60);
    if (rd(0, DR) != 4095u || (rd(0, ISR) & EOC) != 0u || rd(0, DR) != 4095u) return 1;
    /* OVR follows EOC: a result after EOC was cleared without a read is no overrun. */
    wr(0, ISR, OVR);
    run_steps(61, 75);
    if ((rd(0, ISR) & (EOC | OVR)) != EOC) return 1;
    wr(0, ISR, EOC);
    run_steps(76, 90);
    if ((rd(0, ISR) & (EOC | OVR)) != EOC) return 1;
    ll_cr(0, ADSTP);
    return 0;
}

/* One 75-cycle tick crosses five results of the IN5, IN0, IN1 scan: the 4th
 * and 5th overrun, and the next sample is 3 cycles away. */
static int test_one_tick_many_results(void)
{
    if (start_cont_scan(0u)) return 1;
    tk(75);
    if ((rd(0, ISR) & (EOC | EOS | OVR)) != (EOC | EOS | OVR) || next_event() != 3u) return 1;
    if ((rd(0, CR) & ADSTART) == 0u) return 1;
    if (rd(0, DR) != 4095u || (rd(0, ISR) & EOC) == 0u) return 1;
    if (rd(0, DR) != 1241u || (rd(0, ISR) & EOC) == 0u) return 1;
    if (rd(0, DR) != 2482u || (rd(0, ISR) & EOC) != 0u) return 1;
    ll_cr(0, ADSTP);

    if (start_cont_scan(CFGR_OVRMOD)) return 1;
    tk(75);
    if ((rd(0, ISR) & (EOC | OVR)) != (EOC | OVR) || next_event() != 3u) return 1;
    if (rd(0, DR) != 1241u || (rd(0, ISR) & EOC) != 0u) return 1;
    ll_cr(0, ADSTP);
    return 0;
}

static int test_peek_does_not_pop(void)
{
    setup();
    if (enabled(0, 1)) return 1;
    run_conv(0, 500);
    run_conv(0, 1000);
    if (peek_dr() != 620u || peek_dr() != 620u || (rd(0, ISR) & EOC) == 0u) return 1;
    if (rd(0, DR) != 620u || peek_dr() != 1241u || (rd(0, ISR) & EOC) == 0u) return 1;
    if (rd(0, DR) != 1241u || (rd(0, ISR) & EOC) != 0u) return 1;
    return 0;
}

/* ADSTP keeps queued results; ADDIS, an EOC clear and reset drop them. */
static int test_stop_disable_fifo(void)
{
    setup();
    if (enabled(0, 1)) return 1;
    run_conv(0, 500);
    run_conv(0, 1000);
    g_mv[0][0] = 2000;
    ll_cr(0, ADSTART);
    tk(1);
    tk(5);
    ll_cr(0, ADSTP);
    tk(100);
    if (rd(0, DR) != 620u || rd(0, DR) != 1241u || (rd(0, ISR) & EOC) != 0u) return 1;
    if (rd(0, DR) != 1241u) return 1;

    /* HAL_ADC_PollForConversion clears EOC before HAL_ADC_GetValue. */
    run_conv(0, 500);
    run_conv(0, 1000);
    wr(0, ISR, EOC | EOS);
    if ((rd(0, ISR) & EOC) != 0u || peek_dr() != 620u || rd(0, DR) != 620u) return 1;
    run_conv(0, 2000);
    if (rd(0, DR) != 2482u || (rd(0, ISR) & EOC) != 0u) return 1;

    run_conv(0, 500);
    run_conv(0, 1000);
    ll_cr(0, ADDIS);
    if ((rd(0, CR) & ADEN) != 0u) return 1;
    if ((rd(0, ISR) & EOC) != 0u || rd(0, DR) != 620u || rd(0, DR) != 620u) return 1;

    if (enabled(0, 1)) return 1;
    run_conv(0, 500);
    run_conv(0, 1000);
    stm32_adc_reset(&g_adc);
    if (rd(0, DR) != 0u || enabled(0, 1) || conv_mv(2000, 2482u)) return 1;
    return 0;
}

/* ADSTP while rank 2 converts keeps rank 1's result; the next start is rank 1. */
static int test_stop_mid_scan(void)
{
    static const mm_u32 expected[3] = { 4095u, 1241u, 2482u };
    int eoc = 0;
    int n;

    setup();
    if (enabled(0, 1)) return 1;
    g_mv[0][0] = 1000;
    g_mv[0][1] = 2000;
    g_mv[1][1] = 3300;
    wr(0, SQR1, 2u | (5u << 6) | (0u << 12) | (1u << 18));
    ll_cr(0, ADSTART);
    tk(1);
    tk(20);
    if ((rd(0, ISR) & (EOC | EOS)) != EOC) return 1;
    ll_cr(0, ADSTP);
    tk(100);
    if ((rd(0, ISR) & (EOC | EOS | OVR)) != EOC || rd(0, DR) != 4095u) return 1;
    if ((rd(0, ISR) & EOC) != 0u) return 1;

    wr(0, ISR, EOSMP);
    ll_cr(0, ADSTART);
    tk(1);
    for (n = 0; n < 100 && eoc < 3; ++n) {
        tk(1);
        if ((rd(0, ISR) & EOC) != 0u) {
            if (((rd(0, ISR) & EOS) != 0u) != (eoc == 2)) return 1;
            if (rd(0, DR) != expected[eoc++]) return 1;
        }
    }
    if (eoc != 3 || (rd(0, CR) & ADSTART) != 0u) return 1;
    return 0;
}

static int test_scan_eoc_eos_fifo(void)
{
    mm_u64 n;
    setup();
    if (enabled(0, 1)) return 1;
    g_mv[0][0] = 1000;
    g_mv[0][1] = 2000;
    g_mv[1][1] = 3300;
    wr(0, SQR1, 2u | (5u << 6) | (0u << 12) | (1u << 18));
    ll_cr(0, ADSTART);
    tk(1);
    for (n = 1u; n <= 45u; ++n) {
        tk(1);
        if (n == 14u && (rd(0, ISR) & EOC) != 0u) return 1;
        if (n == 15u && (rd(0, ISR) & (EOC | EOS)) != EOC) return 1;
        if (n == 44u && (rd(0, ISR) & EOS) != 0u) return 1;
    }
    if ((rd(0, ISR) & (EOC | EOS | OVR)) != (EOC | EOS) || (rd(0, CR) & ADSTART) != 0u) return 1;
    if (rd(0, DR) != 4095u || (rd(0, ISR) & EOC) == 0u) return 1;
    if (rd(0, DR) != 1241u || (rd(0, ISR) & EOC) == 0u) return 1;
    if (rd(0, DR) != 2482u || (rd(0, ISR) & EOC) != 0u) return 1;
    return 0;
}

#define NEG600 (OFR_EN | 0x600u)
#define SAT600 (OFR_EN | OFR_SATEN | 0x600u)
#define POS100 (OFR_EN | OFR_POS | 0x100u)

/* 1000 mV converts to 1241 / 310 / 77 / 19 at 12 / 10 / 8 / 6 bits; the
 * bit patterns follow RM0517 Figures 270-273. */
static int test_offset_alignment(void)
{
    static const struct {
        mm_u32 res, align, ofr1, ofr2;
        mm_i32 mv;
        mm_u32 code;
    } cases[] = {
        { 0, 0, 0, 0, 1000, 0x04D9u }, { 0, CFGR_ALIGN, 0, 0, 1000, 0x4D90u },
        { 1, 0, 0, 0, 1000, 0x0136u }, { 1, CFGR_ALIGN, 0, 0, 1000, 0x4D80u },
        { 2, 0, 0, 0, 1000, 0x004Du }, { 2, CFGR_ALIGN, 0, 0, 1000, 0x4D00u },
        { 3, 0, 0, 0, 1000, 0x0013u }, { 3, CFGR_ALIGN, 0, 0, 1000, 0x004Cu },
        { 0, 0, NEG600, 0, 1000, 0xFED9u }, { 0, CFGR_ALIGN, NEG600, 0, 1000, 0xF6C8u },
        { 1, 0, NEG600, 0, 1000, 0xFFB6u }, { 1, CFGR_ALIGN, NEG600, 0, 1000, 0xF6C0u },
        { 2, 0, NEG600, 0, 1000, 0xFFEDu }, { 2, CFGR_ALIGN, NEG600, 0, 1000, 0xF680u },
        { 3, 0, NEG600, 0, 1000, 0xFFFBu }, { 3, CFGR_ALIGN, NEG600, 0, 1000, 0xFFF6u },
        { 0, 0, SAT600, 0, 1000, 0x0000u }, { 0, CFGR_ALIGN, SAT600, 0, 1000, 0x0000u },
        { 3, 0, SAT600, 0, 1000, 0x0000u }, { 3, CFGR_ALIGN, SAT600, 0, 1000, 0x0000u },
        { 0, 0, POS100, 0, 1000, 0x05D9u }, { 0, CFGR_ALIGN, POS100, 0, 1000, 0x2EC8u },
        { 0, 0, POS100 | OFR_SATEN, 0, 1000, 0x05D9u },
        { 0, CFGR_ALIGN, POS100 | OFR_SATEN, 0, 1000, 0x5D90u },
        { 0, 0, POS100 | OFR_SATEN, 0, 3300, 0x0FFFu },
        { 0, CFGR_ALIGN, POS100 | OFR_SATEN, 0, 3300, 0xFFF0u },
        { 2, 0, POS100 | OFR_SATEN, 0, 3300, 0x00FFu },
        { 2, CFGR_ALIGN, POS100 | OFR_SATEN, 0, 3300, 0xFF00u },
        { 0, 0, OFR_EN | 0x100u, NEG600, 1000, 0x03D9u },
        { 0, 0, OFR_EN | OFR_CH(1) | 0x600u, 0, 1000, 0x04D9u },
        { 0, 0, 0x600u, 0, 1000, 0x04D9u },
    };
    size_t i;

    setup();
    if (enabled(0, 1)) return 1;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        mm_u32 code;
        modify(0, CFGR, CFGR_RES | CFGR_ALIGN, (cases[i].res << 3) | cases[i].align);
        wr(0, OFR1, cases[i].ofr1);
        wr(0, OFR1 + 4u, cases[i].ofr2);
        g_mv[0][0] = cases[i].mv;
        code = convert(0);
        if (code != cases[i].code) {
            printf("  case %u: 0x%04x\n", (unsigned)i, (unsigned)code);
            return 1;
        }
    }

    modify(0, CFGR, CFGR_RES, CFGR_ALIGN);
    wr(0, OFR1, NEG600);
    wr(0, OFR1 + 4u, 0u);
    modify(0, CFGR2, 0u, CFGR2_ROVSE);
    if (conv_mv(1000, 0x04D9u)) return 1;
    modify(0, CFGR2, CFGR2_ROVSE, 0u);

    /* ALIGN and OFRy are latched when the slot starts. */
    modify(0, CFGR, CFGR_ALIGN, 0u);
    wr(0, OFR1, 0u);
    g_mv[0][0] = 1000;
    wr(0, ISR, EOSMP | EOC | EOS);
    ll_cr(0, ADSTART);
    tk(1);
    tk(1);
    modify(0, CFGR, 0u, CFGR_ALIGN);
    wr(0, OFR1, NEG600);
    tk(100);
    if ((rd(0, ISR) & EOC) == 0u || rd(0, DR) != 0x04D9u) return 1;
    if (convert(0) != 0xF6C8u) return 1;
    return 0;
}

static mm_bool g_kernel_hclk;

static mm_bool fake_kernel_hclk(void *ctx)
{
    (void)ctx;
    return g_kernel_hclk;
}

/* Starts a calibration with CKMODE = 00 and PRESC code presc. */
static void cal_presc(mm_u32 presc, mm_bool kernel_hclk, mm_u64 hclk)
{
    setup();
    g_soc.kernel_is_hclk = fake_kernel_hclk;
    g_kernel_hclk = kernel_hclk;
    g_hclk = hclk;
    bring_up(0, 0);
    wr_common(CCR, rd_common(CCR) | (presc << 18));
    ll_cal(0);
}

/* 0 if ADCAL clears exactly `cycles` after the first tick. */
static int cal_clears_after(mm_u64 cycles)
{
    tk(1);
    tk(cycles - 1u);
    if ((rd(0, CR) & ADCAL) == 0u) return 1;
    tk(1);
    return (rd(0, CR) & ADCAL) != 0u ? 1 : 0;
}

static int test_ckmode0_presc(void)
{
    const mm_u32 warn = STM32_ADC_WARN_CKMODE0 | STM32_ADC_WARN_FREQ;
    cal_presc(2u, MM_TRUE, 64000000u);
    if ((g_adc.warned & warn) != 0u || cal_clears_after(5340u)) return 1;
    cal_presc(3u, MM_TRUE, 64000000u);
    if ((g_adc.warned & warn) != 0u || cal_clears_after(8010u)) return 1;
    cal_presc(11u, MM_TRUE, 64000000u);
    if ((g_adc.warned & warn) != STM32_ADC_WARN_FREQ) return 1;
    cal_presc(9u, MM_TRUE, 64000000u);
    if ((g_adc.warned & warn) != STM32_ADC_WARN_FREQ) return 1;
    cal_presc(1u, MM_TRUE, 250000000u);
    if ((g_adc.warned & warn) != STM32_ADC_WARN_FREQ) return 1;
    cal_presc(2u, MM_TRUE, 250000000u);
    if ((g_adc.warned & warn) != 0u) return 1;
    cal_presc(2u, MM_FALSE, 64000000u);
    if ((g_adc.warned & warn) != STM32_ADC_WARN_CKMODE0 || cal_clears_after(1335u)) return 1;
    cal_presc(12u, MM_TRUE, 64000000u);
    if ((g_adc.warned & warn) != STM32_ADC_WARN_CKMODE0 || cal_clears_after(1335u)) return 1;
    return 0;
}

#define WARN_UNMODELLED (STM32_ADC_WARN_EXTEN | STM32_ADC_WARN_DMA | STM32_ADC_WARN_INJECTED | \
                         STM32_ADC_WARN_AUTDLY | STM32_ADC_WARN_DISCEN)

static int test_unmodelled_warnings(void)
{
    static const struct {
        mm_u32 cfgr, warn;
    } cases[] = {
        { 1u << 10, STM32_ADC_WARN_EXTEN },
        { 2u << 10, STM32_ADC_WARN_EXTEN },
        { CFGR_DMAEN, STM32_ADC_WARN_DMA },
        { 1u << 14, STM32_ADC_WARN_AUTDLY },
        { 1u << 16, STM32_ADC_WARN_DISCEN },
    };
    mm_bool ccr = MM_FALSE;
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        setup();
        if (enabled(0, 1)) return 1;
        modify(0, CFGR, 0u, cases[i].cfgr);
        if ((g_adc.warned & WARN_UNMODELLED) != 0u) return 1;
        if (conv_mv(1000, 1241u)) return 1;
        if ((g_adc.warned & WARN_UNMODELLED) != cases[i].warn) {
            printf("  case %u: warned 0x%x\n", (unsigned)i, (unsigned)g_adc.warned);
            return 1;
        }
    }

    setup();
    if (enabled(0, 1)) return 1;
    modify(0, CFGR, 0u, 1u << 10);
    ll_cr(0, ADSTART);
    if ((rd(0, CR) & ADSTART) == 0u || next_event() != 0u) return 1;

    setup();
    bring_up(0, 1);
    ll_cr(0, JADSTART);
    if ((g_adc.warned & WARN_UNMODELLED) != 0u) return 1;
    if (enabled(0, 1)) return 1;
    ll_cr(0, JADSTART);
    if ((g_adc.warned & WARN_UNMODELLED) != STM32_ADC_WARN_INJECTED) return 1;
    if ((rd(0, CR) & JADSTART) != 0u || next_event() != STM32_ADC_NO_EVENT) return 1;

    setup();
    g_mv[0][0] = 1000;
    if (hal_init(0, 1, 0, &ccr) || hal_calibrate(0)) return 1;
    hal_config_channel(0, 1, 0, 0);
    if (hal_start(0) || hal_poll(0) || rd(0, DR) != 1241u || hal_stop(0)) return 1;
    if ((g_adc.warned & WARN_UNMODELLED) != 0u) return 1;
    return 0;
}

/* Exception entry takes the ADC's pend; a pend software raises in the handler survives
 * the handler clearing the flags, as does one raised before the flags. */
static int test_irq_keeps_software_pend(void)
{
    setup();
    if (enabled(0, 1)) return 1;
    wr(0, IER, EOC);
    run_conv(0, 1000);
    if (!pend(37)) return 1;
    unpend(37);
    mm_nvic_set_active(&g_nvic, 37, MM_TRUE);
    mm_nvic_set_pending(&g_nvic, 37, MM_TRUE);
    wr(0, ISR, EOC | EOS);
    if (!pend(37)) return 1;

    setup();
    if (enabled(0, 1)) return 1;
    mm_nvic_set_active(&g_nvic, 37, MM_TRUE);
    mm_nvic_set_pending(&g_nvic, 37, MM_TRUE);
    wr(0, IER, EOC);
    run_conv(0, 1000);
    if ((rd(0, ISR) & EOC) == 0u) return 1;
    wr(0, ISR, EOC | EOS);
    if (!pend(37)) return 1;

    /* Re-asserted in the handler, tail-chained, then pended by software. */
    setup();
    if (enabled(0, 1)) return 1;
    wr(0, IER, EOC);
    run_conv(0, 1000);
    unpend(37);
    mm_nvic_set_active(&g_nvic, 37, MM_TRUE);
    wr(0, IER, EOC);
    mm_nvic_set_active(&g_nvic, 37, MM_FALSE);
    tk(1);
    if (!pend(37)) return 1;
    unpend(37);
    mm_nvic_set_active(&g_nvic, 37, MM_TRUE);
    mm_nvic_set_pending(&g_nvic, 37, MM_TRUE);
    wr(0, ISR, EOC | EOS);
    tk(1);
    if (!pend(37)) return 1;
    return 0;
}

static int test_snapshot_roundtrip(void)
{
    static mm_u8 buf[16384];
    struct stm32_adc_state ref;
    struct mm_snapshot_writer w;
    struct mm_snapshot_reader r;
    mm_u64 ev;
    mm_u32 isr;
    int n;

    setup();
    if (enabled(0, 1)) return 1;
    g_mv[0][0] = 1000;
    g_mv[0][1] = 2000;
    wr(0, SQR1, 1u | (0u << 6) | (1u << 12));
    modify(0, CFGR, 0u, CFGR_CONT);
    wr(0, IER, EOC);
    wr(0, ISR, EOSMP | EOC | EOS);
    ll_cr(0, ADSTART);
    tk(1);
    for (n = 0; n < 100 && g_adc.inst[0].fifo_count == 0u; ++n) tk(1);
    ev = next_event();
    isr = rd(0, ISR);
    if (g_adc.inst[0].fifo_count != 1u || ev == STM32_ADC_NO_EVENT || (isr & EOC) == 0u) return 1;

    mm_snapshot_writer_init(&w, buf, (mm_u32)sizeof(buf));
    if (!mmio_bus_save(&g_bus, &w)) return 1;
    ref = g_adc;

    tk(5000);
    (void)rd(0, DR);
    wr(0, ISR, EOSMP | EOC | EOS | OVR);
    ll_cr(0, ADSTP);
    if (memcmp(g_adc.inst, ref.inst, sizeof(ref.inst)) == 0) return 1;

    mm_snapshot_reader_init(&r, buf, mm_snapshot_bytes_used(&w));
    if (!mmio_bus_load(&g_bus, &r)) return 1;
    if (memcmp(g_adc.inst, ref.inst, sizeof(ref.inst)) != 0) return 1;
    if (g_adc.now != ref.now || g_adc.next_deadline != ref.next_deadline ||
        g_adc.ccr != ref.ccr || g_adc.rng != ref.rng) return 1;
    if (rd(0, ISR) != isr || next_event() != ev) return 1;

    if (rd(0, DR) != 1241u) return 1;
    for (n = 0; n < 100 && (rd(0, ISR) & EOC) == 0u; ++n) tk(1);
    if (rd(0, DR) != 2482u) return 1;
    return 0;
}

int main(void)
{
    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        { "reset_values", test_reset_values },
        { "reset_clears_dirty_state", test_reset_clears_dirty_state },
        { "storage_and_plain_cr_bits", test_storage_and_plain_cr_bits },
        { "calibration_timing", test_calibration_timing },
        { "adrdy_delay", test_adrdy_delay },
        { "refused_writes", test_refused_writes },
        { "single_conversion", test_single_conversion },
        { "timing_matrix", test_timing_matrix },
        { "quantizer", test_quantizer },
        { "noise_is_seeded", test_noise_is_seeded },
        { "dr_peek_and_isr", test_dr_peek_and_isr },
        { "pin_path", test_pin_path },
        { "adstp_aborts", test_adstp_aborts },
        { "addis_and_recalibrate", test_addis_and_recalibrate },
        { "rmw_adstp_does_not_restart", test_rmw_adstp_does_not_restart },
        { "rmw_addis_does_not_reenable", test_rmw_addis_does_not_reenable },
        { "clock_gating", test_clock_gating },
        { "access_denied_raz_wi", test_access_denied_raz_wi },
        { "next_event", test_next_event },
        { "clock_warnings", test_clock_warnings },
        { "regulator_warning", test_regulator_warning },
        { "oversampling_warning", test_oversampling_warning },
        { "adstart_before_adrdy_waits", test_adstart_before_adrdy_waits },
        { "two_rank_scan", test_two_rank_scan },
        { "six_rank_scan", test_six_rank_scan },
        { "clock_gating_mid_conversion", test_clock_gating_mid_conversion },
        { "init_reads_settings", test_init_reads_settings },
        { "reboot_replays_noise", test_reboot_replays_noise },
        { "ckmode_locked_while_enabled", test_ckmode_locked_while_enabled },
        { "one_tick_crosses_deadlines", test_one_tick_crosses_deadlines },
        { "hal_flow", test_hal_flow },
        { "hal_enable_retry_after_calibration", test_hal_enable_retry_after_calibration },
        { "hal_init_skips_ccr_when_other_enabled", test_hal_init_skips_ccr_when_other_enabled },
        { "common_block", test_common_block },
        { "real_signal_path", test_real_signal_path },
        { "irq_level_ier_write", test_irq_level_ier_write },
        { "irq_each_flag", test_irq_each_flag },
        { "irq_repend_fifo", test_irq_repend_fifo },
        { "irq_held_while_active", test_irq_held_while_active },
        { "hal_irq_flow", test_hal_irq_flow },
        { "continuous_rate", test_continuous_rate },
        { "continuous_adstp", test_continuous_adstp },
        { "fifo_depth_then_ovr", test_fifo_depth_then_ovr },
        { "ovrmod1_overwrite", test_ovrmod1_overwrite },
        { "one_tick_many_results", test_one_tick_many_results },
        { "stop_mid_scan", test_stop_mid_scan },
        { "peek_does_not_pop", test_peek_does_not_pop },
        { "stop_disable_fifo", test_stop_disable_fifo },
        { "scan_eoc_eos_fifo", test_scan_eoc_eos_fifo },
        { "offset_alignment", test_offset_alignment },
        { "ckmode0_presc", test_ckmode0_presc },
        { "unmodelled_warnings", test_unmodelled_warnings },
        { "irq_keeps_software_pend", test_irq_keeps_software_pend },
        { "snapshot_roundtrip", test_snapshot_roundtrip },
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
