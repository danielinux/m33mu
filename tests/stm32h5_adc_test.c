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

/* ADC1/2 on the RM0481 parts (STM32H563, STM32H533), which have no ADC3. */

#include <stdio.h>
#include <string.h>
#include "m33mu/cpu_db.h"
#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "m33mu/signal.h"
#include "m33mu/target.h"
#include "m33mu/timer.h"
#include "stm32h5_adc.h"
#include "stm32h563/stm32h563_mmio.h"
#include "stm32h533/stm32h533_mmio.h"
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
#define AHB2ENR_RESET  0xC0000000u
#define GPIOAEN        (1u << 0)
#define GPIOCEN        (1u << 2)
#define GPIOFEN        (1u << 5)
#define ADCEN          (1u << 10)
#define ADC3EN         (1u << 24)
#define TZSC_SECCFGR3  0x50032418u
#define ADC3SEC        (1u << 6)
#define ADC12SEC       (1u << 14)
#define GPIOA_MODER    0x42020000u
#define RCC_CCIPR5     0x44020CE8u

#define NO_EVENT ((mm_u64)-1)

struct part {
    const char *name;
    void (*mmio_reset)(void);
    mm_bool (*register_mmio)(struct mmio_bus *bus);
};

static const struct part g_parts[] = {
    { "stm32h563", mm_stm32h563_mmio_reset, mm_stm32h563_register_mmio },
    { "stm32h533", mm_stm32h533_mmio_reset, mm_stm32h533_register_mmio },
};

static const char *g_tmp_path = "/tmp/stm32h5_adc_test_fixture.sigc";

static struct mm_target_cfg g_cfg;
static struct mmio_region g_regions[512];
static struct mmio_bus g_bus;
static struct mm_nvic g_nvic;

/* Brings the part up the way mm_execution_state does: timer reset, then timer init. */
static int setup(const struct part *p)
{
    if (!mm_cpu_lookup(p->name, &g_cfg)) return 1;
    mmio_set_active_sec(MM_SECURE);
    mmio_bus_init(&g_bus, g_regions, sizeof(g_regions) / sizeof(g_regions[0]));
    mmio_bus_enable_step_hooks(&g_bus);
    p->mmio_reset();
    if (!p->register_mmio(&g_bus)) return 1;
    mm_nvic_init(&g_nvic);
    mm_nvic_set_lines(&g_nvic, g_cfg.irq_lines);
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

static mm_bool mapped(mm_u32 addr)
{
    mm_u32 v;
    return mmio_bus_read(&g_bus, addr, 4, &v);
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
static int bring_up(mm_u32 adc)
{
    wr(adc + CR, rd(adc + CR) & ~(DEEPPWD | RS));
    ll_cr(adc, ADVREGEN);
    wr(ADC_COMMON + CCR, (rd(ADC_COMMON + CCR) & ~(0x3u << 16)) | (3u << 16));
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

static mm_bool is_internal(mm_u32 inst, mm_u32 chan)
{
    return stm32h5_adc_state()->soc->inputs[inst][chan].kind == STM32_ADC_IN_INTERNAL
               ? MM_TRUE : MM_FALSE;
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

static int test_hooks(const struct part *p)
{
    int rc = 1;
    if (setup(p)) goto out;
    if (g_cfg.adc_init != stm32h5_adc_init || g_cfg.adc_reset != stm32h5_adc_reset) goto out;
    if (g_cfg.adc_tick != stm32h5_adc_tick ||
        g_cfg.adc_cycles_until_event != stm32h5_adc_cycles_until_event) goto out;
    rc = 0;
out:
    teardown();
    return rc;
}

static int test_no_source_no_event(const struct part *p)
{
    int rc = 1;
    if (setup(p)) return 1;
    if (next() != NO_EVENT) goto out;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN);
    wr(ADC1 + CR, 0u);
    ll_cr(ADC1, ADVREGEN);
    ll_cr(ADC1, ADEN);
    if (next() != 0u) goto out;
    rc = 0;
out:
    teardown();
    return rc;
}

/* CKMODE = 00: HCLK / PRESC while ADCDACSEL selects rcc_hclk (RCC_CCIPR5 reset value). */
static int test_ckmode0_kernel(const struct part *p)
{
    int rc = 1;
    mm_u32 sel;
    for (sel = 0u; sel < 2u; ++sel) {
        if (setup(p)) return 1;
        wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN);
        wr(RCC_CCIPR5, sel);
        wr(ADC1 + CR, rd(ADC1 + CR) & ~(DEEPPWD | RS));
        ll_cr(ADC1, ADVREGEN);
        wr(ADC_COMMON + CCR, 3u << 18);
        wr(ADC1 + CR, (rd(ADC1 + CR) & ~(RS | ADCALDIF)) | ADCAL);
        tk(1);
        if (next() != (sel == 0u ? 8010u : 1335u)) goto out;
        if (((stm32h5_adc_state()->warned & STM32_ADC_WARN_CKMODE0) != 0u) != (sel != 0u)) goto out;
        teardown();
    }
    rc = 0;
out:
    teardown();
    return rc;
}

/* H563 has a separate secure RCC bank, H533 one bank behind both aliases. */
static int test_rcc_gating(const struct part *p)
{
    int rc = 1;
    mm_bool alias = strcmp(p->name, "stm32h533") == 0 ? MM_TRUE : MM_FALSE;
    if (setup(p)) return 1;
    if (rd(RCC_AHB2ENR) != AHB2ENR_RESET) goto out;
    if (rd(ADC1 + CR) != 0u || rd(ADC2 + CR) != 0u) goto out;
    wr(ADC1 + SMPR1, 0x7u);
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN);
    if (rd(ADC1 + CR) != DEEPPWD || rd(ADC1 + SMPR1) != 0u) goto out;
    if (rd(ADC1 + ADC_SEC + CR) != DEEPPWD || rd(ADC2 + CR) != DEEPPWD) goto out;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADC3EN);
    if (rd(ADC1 + CR) != 0u || rd(ADC2 + CR) != 0u) goto out;
    wr(RCC_AHB2ENR, AHB2ENR_RESET);
    wr(RCC_S_AHB2ENR, AHB2ENR_RESET | ADCEN);
    if (rd(ADC1 + CR) != DEEPPWD || rd(ADC2 + CR) != DEEPPWD) goto out;
    if (((rd(RCC_AHB2ENR) & ADCEN) != 0u) != alias) goto out;
    wr(RCC_AHB2ENR, AHB2ENR_RESET);
    if (alias) {
        if (rd(ADC1 + CR) != 0u || rd(ADC2 + CR) != 0u) goto out;
    } else {
        if (rd(ADC1 + CR) != DEEPPWD || rd(ADC2 + CR) != DEEPPWD) goto out;
    }
    rc = 0;
out:
    teardown();
    return rc;
}

static int test_aliases(const struct part *p)
{
    int rc = 1;
    if (setup(p)) return 1;
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

/* RM0481 Table 282 note 1: ADC3 only on STM32H543/553. */
static int test_no_adc3(const struct part *p)
{
    int rc = 1;
    if (setup(p)) return 1;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN | ADC3EN);
    if (mapped(ADC3) || mapped(ADC3 + ADC_SEC) || mapped(ADC3_COMMON + CCR)) goto out;
    if (mapped(ADC3_COMMON + ADC_SEC + CCR)) goto out;
    rc = 0;
out:
    teardown();
    return rc;
}

static int test_tzsc(const struct part *p)
{
    int rc = 1;
    mm_u8 buf[4];
    if (setup(p)) return 1;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN);
    wr(ADC1 + SMPR1, 0x5u);
    wr(TZSC_SECCFGR3, ADC12SEC);
    mmio_set_active_sec(MM_NONSECURE);
    if (rd(ADC1 + SMPR1) != 0u || rd(ADC2 + CR) != 0u) goto out;
    wr(ADC1 + SMPR1, 0x6u);
    if (mmio_bus_peek(&g_bus, ADC1 + SMPR1, 4, buf) != MMIO_PEEK_OK || buf[0] != 0x5u) goto out;
    mmio_set_active_sec(MM_SECURE);
    if (rd(ADC1 + SMPR1) != 0x5u) goto out;
    wr(TZSC_SECCFGR3, ADC3SEC);
    mmio_set_active_sec(MM_NONSECURE);
    if (rd(ADC1 + SMPR1) != 0x5u || rd(ADC1 + CR) != DEEPPWD) goto out;
    rc = 0;
out:
    teardown();
    return rc;
}

static int test_channel_table(const struct part *p)
{
    int rc = 1;
    if (setup(p)) return 1;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN | GPIOAEN | GPIOCEN | GPIOFEN);
    if (bring_up(ADC1) || bring_up(ADC2)) goto out;

    if (!bind_flat(1000, 0, 0)) goto out;
    if (convert(ADC1, 0) != 1241u || convert(ADC2, 0) != 1241u) goto out;
    if (convert(ADC1, 1) != 0u) goto out;
    wr(GPIOA_MODER, rd(GPIOA_MODER) & ~0x3u);
    if (convert(ADC1, 0) != 0u) goto out;
    wr(GPIOA_MODER, rd(GPIOA_MODER) | 0x3u);
    if (convert(ADC1, 0) != 1241u) goto out;

    if (!bind_flat(1000, 0, 6)) goto out;
    if (convert(ADC1, 3) != 1241u || convert(ADC2, 3) != 1241u) goto out;
    if (!bind_flat(1000, 2, 0)) goto out;
    if (convert(ADC1, 10) != 1241u || convert(ADC2, 10) != 1241u) goto out;
    if (!bind_flat(1000, 5, 11)) goto out;
    if (convert(ADC1, 2) != 1241u || convert(ADC2, 2) != 0u) goto out;
    if (!bind_flat(1000, 5, 13)) goto out;
    if (convert(ADC1, 2) != 0u || convert(ADC2, 2) != 1241u) goto out;
    if (!bind_flat(1000, 5, 12)) goto out;
    if (convert(ADC1, 6) != 1241u || convert(ADC2, 6) != 0u) goto out;
    if (!bind_flat(1000, 5, 14)) goto out;
    if (convert(ADC1, 6) != 0u || convert(ADC2, 6) != 1241u) goto out;
    if (!bind_flat(1000, 0, 4)) goto out;
    if (convert(ADC1, 18) != 1241u || convert(ADC2, 18) != 1241u) goto out;
    if (!is_internal(0, 16) || !is_internal(0, 17) || !is_internal(1, 16) || !is_internal(1, 17)) goto out;
    if (is_internal(0, 15) || is_internal(1, 18)) goto out;
    rc = 0;
out:
    mm_signal_reset();
    remove(g_tmp_path);
    teardown();
    return rc;
}

static int test_vref_from_settings(const struct part *p)
{
    int rc = 1;
    mm_signal_reset();
    mm_signal_set_vdd_mv(2800);
    if (setup(p)) goto out;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN | GPIOAEN);
    if (bring_up(ADC1)) goto out;
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

static mm_bool eoc_irq(mm_u32 adc)
{
    if (bring_up(adc)) return MM_FALSE;
    wr(adc + IER, EOC);
    wr(adc + SQR1, 0u);
    ll_cr(adc, ADSTART);
    tk(1);
    tk(400);
    return MM_TRUE;
}

static int test_irq_lines(const struct part *p)
{
    int rc = 1;
    if (setup(p)) return 1;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN);
    if (!eoc_irq(ADC1)) goto out;
    if (!mm_nvic_is_pending(&g_nvic, 37) || mm_nvic_is_pending(&g_nvic, 69)) goto out;
    teardown();
    if (setup(p)) return 1;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN);
    if (!eoc_irq(ADC2)) goto out;
    if (!mm_nvic_is_pending(&g_nvic, 69) || mm_nvic_is_pending(&g_nvic, 37)) goto out;
    rc = 0;
out:
    teardown();
    return rc;
}

static int test_next_event_while_pending(const struct part *p)
{
    int rc = 1;
    if (setup(p)) return 1;
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

/* Runs before any init: the glue has no blocks yet. */
static int test_uninitialised_is_safe(void)
{
    stm32h5_adc_reset();
    stm32h5_adc_tick(10u);
    if (stm32h5_adc_cycles_until_event() != NO_EVENT) return 1;
    if (stm32h5_adc_state()->now != 0u || stm32h5_adc3_state()->now != 0u) return 1;
    return 0;
}

/* A reboot into another part resets the previous blocks, then init keeps only ADC1/2. */
static int test_part_switch(void)
{
    struct mm_target_cfg f4;
    mm_u64 now3;
    int rc = 1;
    if (!mm_cpu_lookup("stm32h5f4", &f4) || f4.adc_init != stm32h5_adc_init_with_adc3) return 1;
    mmio_set_active_sec(MM_SECURE);
    mmio_bus_init(&g_bus, g_regions, sizeof(g_regions) / sizeof(g_regions[0]));
    mm_stm32h5f4_mmio_reset();
    if (!mm_stm32h5f4_register_mmio(&g_bus)) return 1;
    mm_nvic_init(&g_nvic);
    mm_timer_reset(&f4);
    mm_timer_init(&f4, &g_bus, &g_nvic);
    wr(RCC_AHB2ENR, rd(RCC_AHB2ENR) | ADC3EN);
    wr(ADC3 + CR, rd(ADC3 + CR) & ~(DEEPPWD | RS));
    ll_cr(ADC3, ADVREGEN);
    wr(ADC3_COMMON + CCR, 2u << 16);
    wr(ADC3 + CR, (rd(ADC3 + CR) & ~(RS | ADCALDIF)) | ADCAL);
    mm_timer_tick(&f4, 10u);
    if (mm_timer_adc_cycles_until_event(&f4) != 2670u) goto out;

    if (setup(&g_parts[0])) goto out;
    now3 = stm32h5_adc3_state()->now;
    if ((stm32h5_adc3_state()->inst[0].regs[CR / 4u] & ADCAL) != 0u) goto out;
    wr(RCC_AHB2ENR, AHB2ENR_RESET | ADCEN);
    wr(ADC1 + CR, rd(ADC1 + CR) & ~(DEEPPWD | RS));
    ll_cr(ADC1, ADVREGEN);
    wr(ADC_COMMON + CCR, 3u << 16);
    wr(ADC1 + CR, (rd(ADC1 + CR) & ~(RS | ADCALDIF)) | ADCAL);
    tk(10);
    if (next() != 5340u || stm32h5_adc3_state()->now != now3) goto out;
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
        { "hooks", test_hooks },
        { "no_source_no_event", test_no_source_no_event },
        { "ckmode0_kernel", test_ckmode0_kernel },
        { "rcc_gating", test_rcc_gating },
        { "aliases", test_aliases },
        { "no_adc3", test_no_adc3 },
        { "tzsc", test_tzsc },
        { "channel_table", test_channel_table },
        { "vref_from_settings", test_vref_from_settings },
        { "irq_lines", test_irq_lines },
        { "next_event_while_pending", test_next_event_while_pending },
    };
    size_t i;
    size_t j;
    int failures = 0;
    int rc;

    rc = test_uninitialised_is_safe();
    printf("%-10s %-35s %s\n", "-", "uninitialised_is_safe", rc == 0 ? "PASS" : "FAIL");
    if (rc != 0) failures++;
    for (j = 0; j < sizeof(g_parts) / sizeof(g_parts[0]); ++j) {
        for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
            rc = tests[i].fn(&g_parts[j]);
            printf("%-10s %-35s %s\n", g_parts[j].name, tests[i].name, rc == 0 ? "PASS" : "FAIL");
            if (rc != 0) failures++;
        }
    }
    rc = test_part_switch();
    printf("%-10s %-35s %s\n", "-", "part_switch", rc == 0 ? "PASS" : "FAIL");
    if (rc != 0) failures++;
    return (failures == 0) ? 0 : 1;
}
