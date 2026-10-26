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
#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "m33mu/cpu_db.h"
#include "m33mu/signal.h"
#include "m33mu/timer.h"
#include "stm32h5_adc.h"
#include "stm32h5f4/stm32h5f4_mmio.h"

#define ADC1         0x42028000u
#define ADC2         0x42028100u
#define ADC_COMMON   0x42028300u
#define ADC3         0x4202D800u
#define ADC3_COMMON  0x4202DB00u
#define ADC_SEC      0x10000000u

#define ISR     0x00u
#define IER     0x04u
#define CR      0x08u
#define SMPR1   0x14u
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
#define ADCALDIF (1u << 30)
#define ADCAL    (1u << 31)
#define RS       (ADCAL | ADEN | ADDIS | JADSTART | JADSTP | ADSTART | ADSTP)

#define ADRDY    (1u << 0)
#define EOSMP    (1u << 1)
#define EOC      (1u << 2)
#define EOS      (1u << 3)

#define RCC_AHB2ENR    0x44020C8Cu
#define RCC_S_AHB2ENR  0x54020C8Cu
#define AHB2ENR_RESET  0xF0000000u
#define GPIOAEN        (1u << 0)
#define GPIOBEN        (1u << 1)
#define GPIOEEN        (1u << 4)
#define GPIOFEN        (1u << 5)
#define ADCEN          (1u << 10)
#define ADC3EN         (1u << 24)
#define TZSC_SECCFGR3  0x50032418u
#define ADC3SEC        (1u << 6)
#define ADC12SEC       (1u << 14)
#define GPIOA_MODER    0x42020000u
#define GPIOB_MODER    0x42020400u
#define GPIOE_MODER    0x42021000u
#define GPIOF_MODER    0x42021400u
#define RCC_CCIPR5     0x44020CE8u

#define NO_EVENT ((mm_u64)-1)

static const char *g_tmp_path = "/tmp/stm32h5f4_adc_test_fixture.sigc";

static struct mm_target_cfg g_cfg;
static struct mmio_region g_regions[512];
static struct mmio_bus g_bus;
static struct mm_nvic g_nvic;

static int setup(void)
{
    if (!mm_cpu_lookup("stm32h5f4", &g_cfg)) return 1;
    mmio_set_active_sec(MM_SECURE);
    mmio_bus_init(&g_bus, g_regions, sizeof(g_regions) / sizeof(g_regions[0]));
    mmio_bus_enable_step_hooks(&g_bus);
    mm_stm32h5f4_mmio_reset();
    if (!mm_stm32h5f4_register_mmio(&g_bus)) return 1;
    mm_nvic_init(&g_nvic);
    mm_timer_reset(&g_cfg);
    mm_timer_init(&g_cfg, &g_bus, &g_nvic);
    return 0;
}

static void teardown(void)
{
    mmio_set_active_sec(MM_SECURE);
}

static mm_u64 next(void)
{
    return mm_timer_adc_cycles_until_event(&g_cfg);
}

static void wr(mm_u32 addr, mm_u32 v)
{
    (void)mmio_bus_write(&g_bus, addr, 4, v);
}

static mm_u32 rd(mm_u32 addr)
{
    mm_u32 v = 0xDEADBEEFu;
    if (!mmio_bus_read(&g_bus, addr, 4, &v)) return 0xDEADBEEFu;
    return v;
}

static void ll_cr(mm_u32 adc, mm_u32 bit)
{
    wr(adc + CR, (rd(adc + CR) & ~RS) | bit);
}

static void tk(mm_u64 cycles)
{
    mm_timer_tick(&g_cfg, cycles);
}

/* CKMODE = HCLK/4, regulator on, ADEN, ADRDY. */
static int bring_up(mm_u32 adc, mm_u32 common)
{
    wr(adc + CR, rd(adc + CR) & ~(DEEPPWD | RS));
    ll_cr(adc, ADVREGEN);
    wr(common + CCR, (rd(common + CCR) & ~(0x3u << 16)) | (3u << 16));
    ll_cr(adc, ADEN);
    tk(1);
    tk(60);
    return (rd(adc + ISR) & ADRDY) != 0u ? 0 : 1;
}

static mm_u32 convert(mm_u32 adc, mm_u32 chan)
{
    wr(adc + SQR1, chan << 6);
    wr(adc + ISR, EOSMP | EOC | EOS);
    ll_cr(adc, ADSTART);
    tk(1);
    tk(400);
    if ((rd(adc + ISR) & EOC) == 0u) return 0xFFFFFFFFu;
    return rd(adc + DR);
}

static mm_u8 in_kind(const struct stm32_adc_state *s, mm_u32 inst, mm_u32 chan)
{
    return s->soc->inputs[inst][chan].kind;
}

static mm_bool is_internal(mm_u32 inst, mm_u32 chan)
{
    return in_kind(stm32h5_adc_state(), inst, chan) == STM32_ADC_IN_INTERNAL ? MM_TRUE : MM_FALSE;
}

static void put_le(FILE *f, mm_u64 v, int bytes)
{
    int i;
    for (i = 0; i < bytes; ++i) {
        fputc((int)((v >> (8 * i)) & 0xFFu), f);
    }
}

/* One flat trace, bound to (bank, pin); sample 0 holds until a trigger. */
static mm_bool bind_flat(mm_i16 mv, int bank, int pin)
{
    FILE *f;
    char name[32];

    mm_signal_reset();
    f = fopen(g_tmp_path, "wb");
    if (f == 0) return MM_FALSE;
    fwrite("SIGC", 1, 4, f);
    put_le(f, 1, 4);
    put_le(f, 1, 4);
    put_le(f, 1, 4);
    put_le(f, 1, 4);
    put_le(f, 1, 4);
    put_le(f, 1000u, 4);
    memset(name, 0, sizeof(name));
    snprintf(name, sizeof(name), "ain");
    put_le(f, 1, 4);
    fwrite(name, 1, sizeof(name), f);
    put_le(f, 1, 4);
    put_le(f, 3300, 4);
    put_le(f, (mm_u16)mv, 2);
    fclose(f);
    if (!mm_signal_load(g_tmp_path)) return MM_FALSE;
    return mm_signal_bind("ain", bank, pin, MM_SIGNAL_ROLE_GPIO) >= 0 ? MM_TRUE : MM_FALSE;
}

/* Runs first: the glue has no blocks before its init. A target without the ADC
 * hooks has no ADC event while the H5F4 ADC has one. */
static int test_no_source_no_event(void)
{
    struct mm_target_cfg u585;
    int rc = 1;
    if (stm32h5_adc_cycles_until_event() != NO_EVENT) return 1;
    if (!mm_cpu_lookup("stm32u585", &u585) || u585.adc_init != 0 || u585.adc_reset != 0 ||
        u585.adc_tick != 0 || u585.adc_cycles_until_event != 0) return 1;
    if (setup()) return 1;
    if (g_cfg.adc_init != stm32h5_adc_init_with_adc3) goto out;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN);
    wr(ADC1 + CR, 0u);
    ll_cr(ADC1, ADVREGEN);
    ll_cr(ADC1, ADEN);
    if (next() != 0u) goto out;
    if (mm_timer_adc_cycles_until_event(&u585) != NO_EVENT ||
        mm_timer_adc_cycles_until_event(0) != NO_EVENT) goto out;
    rc = 0;
out:
    teardown();
    return rc;
}

static int test_rcc_gating(void)
{
    int rc = 1;
    if (setup()) return 1;
    if (rd(RCC_AHB2ENR) != AHB2ENR_RESET) goto out;
    if (rd(ADC1 + CR) != 0u) goto out;
    wr(ADC1 + SMPR1, 0x7u);
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN);
    if (rd(ADC1 + CR) != DEEPPWD || rd(ADC1 + SMPR1) != 0u) goto out;
    if (rd(ADC1 + ADC_SEC + CR) != DEEPPWD) goto out;
    wr(RCC_AHB2ENR, AHB2ENR_RESET);
    if (rd(ADC1 + CR) != 0u || rd(ADC2 + CR) != 0u) goto out;
    wr(RCC_S_AHB2ENR, AHB2ENR_RESET | ADCEN);
    if (rd(ADC1 + CR) != DEEPPWD || rd(ADC2 + CR) != DEEPPWD) goto out;
    rc = 0;
out:
    teardown();
    return rc;
}

static int test_aliases(void)
{
    int rc = 1;
    if (setup()) return 1;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN);
    wr(ADC1 + SMPR1, 0x1FFu);
    if (rd(ADC1 + ADC_SEC + SMPR1) != 0x1FFu) goto out;
    if (rd(ADC2 + SMPR1) != 0u) goto out;
    wr(ADC2 + ADC_SEC + SMPR1, 0x24u);
    if (rd(ADC2 + SMPR1) != 0x24u || rd(ADC1 + SMPR1) != 0x1FFu) goto out;
    wr(ADC_COMMON + CCR, 2u << 16);
    if (rd(ADC_COMMON + ADC_SEC + CCR) != (2u << 16)) goto out;
    rc = 0;
out:
    teardown();
    return rc;
}

static int test_tzsc_secure_only(void)
{
    int rc = 1;
    mm_u8 buf[4];
    if (setup()) return 1;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN);
    wr(ADC1 + SMPR1, 0x5u);
    wr(TZSC_SECCFGR3, ADC12SEC);
    mmio_set_active_sec(MM_NONSECURE);
    if (rd(ADC1 + SMPR1) != 0u || rd(ADC1 + CR) != 0u) goto out;
    wr(ADC1 + SMPR1, 0x6u);
    wr(ADC_COMMON + CCR, 3u << 16);
    if (mmio_bus_peek(&g_bus, ADC1 + SMPR1, 4, buf) != MMIO_PEEK_OK || buf[0] != 0x5u) goto out;
    mmio_set_active_sec(MM_SECURE);
    if (rd(ADC1 + SMPR1) != 0x5u || rd(ADC_COMMON + CCR) != 0u) goto out;
    wr(TZSC_SECCFGR3, 0u);
    mmio_set_active_sec(MM_NONSECURE);
    if (rd(ADC1 + SMPR1) != 0x5u) goto out;
    rc = 0;
out:
    teardown();
    return rc;
}

static int test_channel_table_and_moder(void)
{
    int rc = 1;
    if (setup()) return 1;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN | GPIOAEN | GPIOFEN);
    if (bring_up(ADC1, ADC_COMMON) || bring_up(ADC2, ADC_COMMON)) goto out;

    if (!bind_flat(1000, 0, 0)) goto out;
    if (convert(ADC1, 0) != 1241u || convert(ADC2, 0) != 1241u) goto out;
    if (convert(ADC1, 1) != 0u) goto out;
    wr(GPIOA_MODER, rd(GPIOA_MODER) & ~0x3u);
    if (convert(ADC1, 0) != 0u) goto out;
    wr(GPIOA_MODER, rd(GPIOA_MODER) | 0x3u);
    if (convert(ADC1, 0) != 1241u) goto out;

    if (!bind_flat(1000, 5, 11)) goto out;
    if (convert(ADC1, 2) != 1241u || convert(ADC2, 2) != 0u) goto out;
    if (!bind_flat(1000, 5, 13)) goto out;
    if (convert(ADC1, 2) != 0u || convert(ADC2, 2) != 1241u) goto out;
    if (!bind_flat(1000, 0, 4)) goto out;
    if (convert(ADC1, 18) != 1241u || convert(ADC2, 18) != 1241u) goto out;
    if (!is_internal(0, 16) || !is_internal(0, 17) || !is_internal(1, 16) || !is_internal(1, 17)) goto out;
    if (is_internal(0, 15) || is_internal(1, 18)) goto out;
    if ((stm32h5_adc_state()->warned & STM32_ADC_WARN_INTERNAL) != 0u) goto out;
    if (convert(ADC1, 16) != 0u || convert(ADC1, 17) != 0u) goto out;
    if ((stm32h5_adc_state()->warned & STM32_ADC_WARN_INTERNAL) == 0u) goto out;
    if (convert(ADC2, 16) != 0u || convert(ADC2, 17) != 0u) goto out;
    rc = 0;
out:
    mm_signal_reset();
    remove(g_tmp_path);
    teardown();
    return rc;
}

static int test_vref_from_settings(void)
{
    int rc = 1;
    mm_signal_reset();
    mm_signal_set_vdd_mv(2800);
    if (setup()) goto out;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN | GPIOAEN);
    if (bring_up(ADC1, ADC_COMMON)) goto out;
    /* bind_flat() resets the settings; vref is already latched by the init in setup(). */
    if (!bind_flat(1000, 0, 0)) goto out;
    if (convert(ADC1, 0) != 1463u) goto out;
    rc = 0;
out:
    mm_signal_reset();
    remove(g_tmp_path);
    teardown();
    return rc;
}

static int test_next_event_while_pending(void)
{
    int rc = 1;
    if (setup()) return 1;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN);
    if (next() != NO_EVENT) goto out;
    wr(ADC1 + CR, rd(ADC1 + CR) & ~(DEEPPWD | RS));
    ll_cr(ADC1, ADVREGEN);
    wr(ADC_COMMON + CCR, 3u << 16);
    wr(ADC1 + CR, (rd(ADC1 + CR) & ~(RS | ADCALDIF)) | ADCAL);
    if (next() != 0u) goto out;
    tk(10);
    if (next() != 5340u) goto out;
    wr(RCC_AHB2ENR, AHB2ENR_RESET);
    if (next() != NO_EVENT) goto out;
    tk(100);
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN);
    if (next() != 5340u) goto out;
    tk(5339);
    if ((rd(ADC1 + CR) & ADCAL) == 0u) goto out;
    tk(1);
    if ((rd(ADC1 + CR) & ADCAL) != 0u) goto out;
    if (next() != NO_EVENT) goto out;

    ll_cr(ADC1, ADEN);
    if ((rd(ADC1 + CR) & ADEN) != 0u) goto out;
    tk(16);
    ll_cr(ADC1, ADEN);
    if ((rd(ADC1 + CR) & ADEN) == 0u) goto out;
    tk(1);
    if (next() != 60u) goto out;
    tk(60);
    if ((rd(ADC1 + ISR) & ADRDY) == 0u) goto out;

    wr(ADC1 + SMPR1, 3u);
    wr(ADC1 + SQR1, 0u);
    ll_cr(ADC1, ADSTART);
    if (next() != 0u) goto out;
    tk(1);
    if (next() != 98u) goto out;
    tk(98);
    if ((rd(ADC1 + ISR) & (EOSMP | EOC)) != EOSMP) goto out;
    if (next() != 50u) goto out;
    tk(50);
    if ((rd(ADC1 + ISR) & EOC) == 0u) goto out;
    if (next() != NO_EVENT) goto out;
    rc = 0;
out:
    teardown();
    return rc;
}

static int test_adc2_irq_line(void)
{
    int rc = 1;
    if (setup()) return 1;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN);
    if (bring_up(ADC2, ADC_COMMON)) goto out;
    wr(ADC2 + IER, EOC);
    wr(ADC2 + SQR1, 0u);
    ll_cr(ADC2, ADSTART);
    tk(1);
    tk(400);
    if (!mm_nvic_is_pending(&g_nvic, 69) || mm_nvic_is_pending(&g_nvic, 37)) goto out;
    rc = 0;
out:
    teardown();
    return rc;
}

static int test_adc3_aliases(void)
{
    int rc = 1;
    if (setup()) return 1;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN | ADC3EN);
    if (rd(ADC3 + CR) != DEEPPWD || rd(ADC3 + ADC_SEC + CR) != DEEPPWD) goto out;
    wr(ADC3 + SMPR1, 0x1FFu);
    if (rd(ADC3 + ADC_SEC + SMPR1) != 0x1FFu || rd(ADC1 + SMPR1) != 0u) goto out;
    wr(ADC3 + 0x100u + SMPR1, 0x24u);
    if (rd(ADC3 + 0x100u + SMPR1) != 0u || rd(ADC2 + SMPR1) != 0u) goto out;
    wr(ADC3_COMMON + CCR, 2u << 16);
    if (rd(ADC3_COMMON + ADC_SEC + CCR) != (2u << 16) || rd(ADC_COMMON + CCR) != 0u) goto out;
    rc = 0;
out:
    teardown();
    return rc;
}

static int test_adc3_clock_gating(void)
{
    int rc = 1;
    if (setup()) return 1;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN);
    wr(ADC3 + SMPR1, 0x7u);
    if (rd(ADC3 + CR) != 0u || rd(ADC1 + CR) != DEEPPWD) goto out;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADC3EN);
    if (rd(ADC3 + CR) != DEEPPWD || rd(ADC3 + SMPR1) != 0u || rd(ADC1 + CR) != 0u) goto out;
    wr(RCC_AHB2ENR, AHB2ENR_RESET);
    if (rd(ADC3 + CR) != 0u) goto out;
    wr(RCC_S_AHB2ENR, AHB2ENR_RESET | ADC3EN);
    if (rd(ADC3 + CR) != DEEPPWD || rd(ADC1 + CR) != 0u) goto out;
    rc = 0;
out:
    teardown();
    return rc;
}

static int test_adc3_tzsc(void)
{
    int rc = 1;
    mm_u8 buf[4];
    if (setup()) return 1;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN | ADC3EN);
    wr(ADC3 + SMPR1, 0x5u);
    wr(ADC1 + SMPR1, 0x3u);
    wr(TZSC_SECCFGR3, ADC3SEC);
    mmio_set_active_sec(MM_NONSECURE);
    if (rd(ADC3 + SMPR1) != 0u || rd(ADC3 + CR) != 0u) goto out;
    wr(ADC3 + SMPR1, 0x6u);
    if (mmio_bus_peek(&g_bus, ADC3 + SMPR1, 4, buf) != MMIO_PEEK_OK || buf[0] != 0x5u) goto out;
    if (rd(ADC1 + SMPR1) != 0x3u) goto out;
    mmio_set_active_sec(MM_SECURE);
    if (rd(ADC3 + SMPR1) != 0x5u) goto out;
    wr(TZSC_SECCFGR3, ADC12SEC);
    mmio_set_active_sec(MM_NONSECURE);
    if (rd(ADC3 + SMPR1) != 0x5u || rd(ADC1 + SMPR1) != 0u) goto out;
    rc = 0;
out:
    teardown();
    return rc;
}

static int test_adc3_channel_table(void)
{
    int rc = 1;
    const struct stm32_adc_state *s3 = stm32h5_adc3_state();
    mm_u32 c;
    if (setup()) return 1;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADC3EN | GPIOBEN | GPIOEEN);
    if (bring_up(ADC3, ADC3_COMMON)) goto out;

    if (!bind_flat(1000, 1, 2)) goto out;
    if (convert(ADC3, 0) != 1241u || convert(ADC3, 1) != 0u) goto out;
    wr(GPIOB_MODER, rd(GPIOB_MODER) & ~(0x3u << 4));
    if (convert(ADC3, 0) != 0u) goto out;
    wr(GPIOB_MODER, rd(GPIOB_MODER) | (0x3u << 4));
    if (convert(ADC3, 0) != 1241u) goto out;

    if (!bind_flat(1000, 4, 7)) goto out;
    if (convert(ADC3, 1) != 1241u) goto out;
    wr(GPIOE_MODER, rd(GPIOE_MODER) & ~(0x3u << 14));
    if (convert(ADC3, 1) != 0u) goto out;
    wr(GPIOE_MODER, rd(GPIOE_MODER) | (0x3u << 14));
    if (!bind_flat(1000, 4, 15)) goto out;
    if (convert(ADC3, 9) != 1241u) goto out;
    if (!bind_flat(1000, 0, 0)) goto out;
    if (convert(ADC3, 0) != 0u) goto out;

    for (c = 0; c < STM32_ADC_NUM_CHANNELS; ++c) {
        mm_u8 want = (c < 10u) ? STM32_ADC_IN_PIN
                     : (c == 10u || c >= 14u) ? STM32_ADC_IN_INTERNAL : STM32_ADC_IN_NONE;
        if (in_kind(s3, 0, c) != want) goto out;
    }
    if ((s3->warned & STM32_ADC_WARN_INTERNAL) != 0u) goto out;
    if (convert(ADC3, 14) != 0u) goto out;
    if ((s3->warned & STM32_ADC_WARN_INTERNAL) == 0u) goto out;
    if ((stm32h5_adc_state()->warned & STM32_ADC_WARN_INTERNAL) != 0u) goto out;
    rc = 0;
out:
    mm_signal_reset();
    remove(g_tmp_path);
    teardown();
    return rc;
}

static int test_adc3_irq_line(void)
{
    int rc = 1;
    if (setup()) return 1;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADC3EN);
    if (bring_up(ADC3, ADC3_COMMON)) goto out;
    wr(ADC3 + IER, EOC);
    wr(ADC3 + SQR1, 0u);
    ll_cr(ADC3, ADSTART);
    tk(1);
    tk(400);
    if (!mm_nvic_is_pending(&g_nvic, 149)) goto out;
    if (mm_nvic_is_pending(&g_nvic, 37) || mm_nvic_is_pending(&g_nvic, 69)) goto out;
    rc = 0;
out:
    teardown();
    return rc;
}

/* t_CAL is 1335 ADC clocks: 5340 cycles at HCLK/4 (ADC1), 2670 at HCLK/2 (ADC3). */
static int test_next_event_min(void)
{
    int rc = 1;
    if (setup()) return 1;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN | ADC3EN);
    wr(ADC1 + CR, rd(ADC1 + CR) & ~(DEEPPWD | RS));
    ll_cr(ADC1, ADVREGEN);
    wr(ADC_COMMON + CCR, 3u << 16);
    wr(ADC1 + CR, (rd(ADC1 + CR) & ~(RS | ADCALDIF)) | ADCAL);
    tk(10);
    if (next() != 5340u) goto out;
    wr(ADC3 + CR, rd(ADC3 + CR) & ~(DEEPPWD | RS));
    ll_cr(ADC3, ADVREGEN);
    wr(ADC3_COMMON + CCR, 2u << 16);
    wr(ADC3 + CR, (rd(ADC3 + CR) & ~(RS | ADCALDIF)) | ADCAL);
    if (next() != 0u) goto out;
    tk(10);
    if (next() != 2670u) goto out;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN);
    if (next() != 5330u) goto out;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN | ADC3EN);
    if (next() != 2670u) goto out;
    tk(2670);
    if ((rd(ADC3 + CR) & ADCAL) != 0u || (rd(ADC1 + CR) & ADCAL) == 0u) goto out;
    if (next() != 2660u) goto out;
    tk(2660);
    if ((rd(ADC1 + CR) & ADCAL) != 0u) goto out;
    if (next() != NO_EVENT) goto out;
    if ((stm32h5_adc3_state()->warned & STM32_ADC_WARN_REGULATOR) != 0u) goto out;
    rc = 0;
out:
    teardown();
    return rc;
}

static int test_adc3_reset_and_seed(void)
{
    int rc = 1;
    const struct stm32_adc_state *s1 = stm32h5_adc_state();
    const struct stm32_adc_state *s3 = stm32h5_adc3_state();
    mm_signal_set_adc_noise(2u, 0x1234u);
    if (setup()) goto out;
    if (s1->noise_seed != 0x1234u || s3->noise_seed != (0x1234u ^ 0xAD3u)) goto out;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADC3EN);
    wr(ADC3 + SMPR1, 0x5u);
    wr(ADC3_COMMON + CCR, 2u << 16);
    wr(ADC3 + CR, rd(ADC3 + CR) & ~(DEEPPWD | RS));
    ll_cr(ADC3, ADVREGEN);
    wr(ADC3 + CR, (rd(ADC3 + CR) & ~(RS | ADCALDIF)) | ADCAL);
    tk(10);
    if (next() == NO_EVENT) goto out;
    mm_timer_reset(&g_cfg);
    if (rd(ADC3 + CR) != DEEPPWD || rd(ADC3 + SMPR1) != 0u || rd(ADC3_COMMON + CCR) != 0u) goto out;
    if (next() != NO_EVENT) goto out;
    if (s3->noise_seed != (0x1234u ^ 0xAD3u) || s3->rng != s3->noise_seed) goto out;
    rc = 0;
out:
    mm_signal_reset();
    teardown();
    return rc;
}

static void start_cal(mm_u32 adc, mm_u32 common, mm_u32 ccr)
{
    wr(adc + CR, rd(adc + CR) & ~(DEEPPWD | RS));
    ll_cr(adc, ADVREGEN);
    wr(common + CCR, ccr);
    wr(adc + CR, (rd(adc + CR) & ~(RS | ADCALDIF)) | ADCAL);
}

/* CKMODE = 00, PRESC = /6: 8010 cycles of t_CAL with ADCDACSEL = rcc_hclk (reset value),
 * HCLK/1 and a warning with ADCDACSEL = sys_ck. ADC3 follows its own CCR. */
static int test_ckmode0_kernel(void)
{
    int rc = 1;
    if (setup()) return 1;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN | ADC3EN);
    start_cal(ADC1, ADC_COMMON, 3u << 18);
    tk(1);
    if (next() != 8010u || (stm32h5_adc_state()->warned & STM32_ADC_WARN_CKMODE0) != 0u) goto out;
    start_cal(ADC3, ADC3_COMMON, 4u << 18);
    tk(1);
    if (next() != 8009u) goto out;
    tk(8009);
    if ((rd(ADC1 + CR) & ADCAL) != 0u || (rd(ADC3 + CR) & ADCAL) == 0u || next() != 2671u) goto out;
    teardown();

    if (setup()) return 1;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN);
    wr(RCC_CCIPR5, 1u);
    start_cal(ADC1, ADC_COMMON, 3u << 18);
    tk(1);
    if (next() != 1335u || (stm32h5_adc_state()->warned & STM32_ADC_WARN_CKMODE0) == 0u) goto out;
    rc = 0;
out:
    teardown();
    return rc;
}

static int test_snapshot_two_blocks(void)
{
    static mm_u8 buf[262144];
    struct mm_snapshot_writer w;
    struct mm_snapshot_reader r;
    int rc = 1;
    if (setup()) return 1;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN | ADC3EN);
    wr(ADC1 + SMPR1, 0x11u);
    wr(ADC2 + SMPR1, 0x22u);
    wr(ADC3 + SMPR1, 0x33u);
    start_cal(ADC3, ADC3_COMMON, 2u << 16);
    tk(10);
    if (next() != 2670u) goto out;
    mm_snapshot_writer_init(&w, buf, (mm_u32)sizeof(buf));
    if (!mmio_bus_save(&g_bus, &w)) goto out;

    wr(ADC1 + SMPR1, 0u);
    wr(ADC2 + SMPR1, 0u);
    wr(ADC3 + SMPR1, 0u);
    tk(5000);
    if ((rd(ADC3 + CR) & ADCAL) != 0u || next() != NO_EVENT) goto out;

    mm_snapshot_reader_init(&r, buf, mm_snapshot_bytes_used(&w));
    if (!mmio_bus_load(&g_bus, &r)) goto out;
    if (rd(ADC1 + SMPR1) != 0x11u || rd(ADC2 + SMPR1) != 0x22u || rd(ADC3 + SMPR1) != 0x33u) goto out;
    if ((rd(ADC3 + CR) & ADCAL) == 0u || next() != 2670u) goto out;
    tk(2670);
    if ((rd(ADC3 + CR) & ADCAL) != 0u) goto out;
    rc = 0;
out:
    teardown();
    return rc;
}

int main(void)
{
    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        { "no_source_no_event", test_no_source_no_event },
        { "rcc_gating", test_rcc_gating },
        { "aliases", test_aliases },
        { "tzsc_secure_only", test_tzsc_secure_only },
        { "channel_table_and_moder", test_channel_table_and_moder },
        { "next_event_while_pending", test_next_event_while_pending },
        { "vref_from_settings", test_vref_from_settings },
        { "adc2_irq_line", test_adc2_irq_line },
        { "adc3_aliases", test_adc3_aliases },
        { "adc3_clock_gating", test_adc3_clock_gating },
        { "adc3_tzsc", test_adc3_tzsc },
        { "adc3_channel_table", test_adc3_channel_table },
        { "adc3_irq_line", test_adc3_irq_line },
        { "next_event_min", test_next_event_min },
        { "adc3_reset_and_seed", test_adc3_reset_and_seed },
        { "ckmode0_kernel", test_ckmode0_kernel },
        { "snapshot_two_blocks", test_snapshot_two_blocks },
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
