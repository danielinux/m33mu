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

#include "stm32h5_adc.h"
#include "stm32h5_mmio.h"

#define RCC_AHB2ENR        0x8Cu
#define RCC_AHB2ENR_ADCEN  (1u << 10)
#define RCC_AHB2ENR_ADC3EN (1u << 24)
#define RCC_CCIPR5         0xE8u
#define RCC_CCIPR5_ADCDACSEL 0x7u
#define TZSC_SECCFGR3      0x18u
#define TZSC_ADC12SEC      (1u << 14)
#define TZSC_ADC3SEC       (1u << 6)
#define GPIO_MODE_ANALOG   3u
#define ADC3_SEED_SALT     0xAD3ull

#define IN(b, p) { STM32_ADC_IN_PIN, (b), (p) }
#define IN_INT   { STM32_ADC_IN_INTERNAL, 0u, 0u }
#define NO_PIN   { STM32_ADC_IN_NONE, 0u, 0u }

/* Positive inputs, largest package: DS14972 (H5F4), STM32_open_pin_data (H563, H533);
 * internal channels from RM0517 Table 290 and RM0481 Table 282. ADC1/2 are the
 * same on all three parts. ADC3 IN10 is OPAMP1, IN18/IN19 the DAC1 outputs. */
static const struct stm32_adc_input g_adc1_in[STM32_ADC_NUM_CHANNELS] = {
    IN(0, 0), IN(0, 1), IN(5, 11), IN(0, 6), IN(2, 4), IN(1, 1), IN(5, 12), IN(0, 7),
    IN(2, 5), IN(1, 0), IN(2, 0), IN(2, 1), IN(2, 2), IN(2, 3), IN(0, 2), IN(0, 3),
    IN_INT, IN_INT, IN(0, 4), IN(0, 5)
};

static const struct stm32_adc_input g_adc2_in[STM32_ADC_NUM_CHANNELS] = {
    IN(0, 0), IN(0, 1), IN(5, 13), IN(0, 6), IN(2, 4), IN(1, 1), IN(5, 14), IN(0, 7),
    IN(2, 5), IN(1, 0), IN(2, 0), IN(2, 1), IN(2, 2), IN(2, 3), IN(0, 2), IN(0, 3),
    IN_INT, IN_INT, IN(0, 4), IN(0, 5)
};

static const struct stm32_adc_input g_adc3_in[STM32_ADC_NUM_CHANNELS] = {
    IN(1, 2), IN(4, 7), IN(4, 8), IN(4, 9), IN(4, 10), IN(4, 11), IN(4, 12), IN(4, 13),
    IN(4, 14), IN(4, 15), IN_INT, NO_PIN, NO_PIN, NO_PIN, IN_INT, IN_INT,
    IN_INT, IN_INT, IN_INT, IN_INT
};

/* RCC enable and TZSC secure bit of one ADC block. */
struct adc_gate {
    mm_u32 clk_en;
    mm_u32 tz_sec;
};

static const struct adc_gate g_gate12 = { RCC_AHB2ENR_ADCEN, TZSC_ADC12SEC };
static const struct adc_gate g_gate3 = { RCC_AHB2ENR_ADC3EN, TZSC_ADC3SEC };

static struct stm32_adc_state g_adc;
static struct stm32_adc_state g_adc3;
static struct stm32_adc_state *const g_blocks[2] = { &g_adc, &g_adc3 };
static mm_u32 g_block_count;

/* ADC1/2 share ADCEN, ADC3 has ADC3EN; either RCC bank enables it, as for GPIO. */
static mm_bool adc_clock_enabled(void *ctx, mm_u32 inst)
{
    mm_u32 ahb2enr = stm32h5_rcc_regs()[RCC_AHB2ENR / 4u] |
                     stm32h5_rcc_secure_regs()[RCC_AHB2ENR / 4u];
    (void)inst;
    return (ahb2enr & ((const struct adc_gate *)ctx)->clk_en) != 0u ? MM_TRUE : MM_FALSE;
}

static mm_bool adc_pin_is_analog(void *ctx, int bank, int pin)
{
    (void)ctx;
    return stm32h5_gpio_get_mode(bank, pin) == GPIO_MODE_ANALOG ? MM_TRUE : MM_FALSE;
}

static mm_u64 adc_hclk_hz(void *ctx)
{
    (void)ctx;
    return stm32h5_cpu_hz();
}

static mm_bool adc_access_allowed(void *ctx)
{
    const mm_u32 *tz = stm32h5_tzsc_regs();
    mm_u32 sec = ((const struct adc_gate *)ctx)->tz_sec;
    if (tz != 0 && (tz[TZSC_SECCFGR3 / 4u] & sec) != 0u &&
        mmio_active_sec() == MM_NONSECURE) {
        return MM_FALSE;
    }
    return MM_TRUE;
}

/* CKMODE = 00 is modelled only for ADCDACSEL = 000 (rcc_hclk, RM0517/RM0481 RCC_CCIPR5).
 * The bank that firmware did not program reads 000, so OR-ing them yields the selection. */
static mm_bool adc_kernel_is_hclk(void *ctx)
{
    mm_u32 ccipr5 = stm32h5_rcc_regs()[RCC_CCIPR5 / 4u] |
                    stm32h5_rcc_secure_regs()[RCC_CCIPR5 / 4u];
    (void)ctx;
    return (ccipr5 & RCC_CCIPR5_ADCDACSEL) == 0u ? MM_TRUE : MM_FALSE;
}

static const struct stm32_adc_soc g_soc = {
    0x42028000u,
    2u,
    { 37, 69 },
    { g_adc1_in, g_adc2_in },
    (void *)&g_gate12,
    adc_clock_enabled,
    adc_pin_is_analog,
    adc_hclk_hz,
    0,
    adc_access_allowed,
    adc_kernel_is_hclk,
    "ADC12"
};

/* ADC3 is a single-instance block with its own common registers (RM0517 30.1). */
static const struct stm32_adc_soc g_soc3 = {
    0x4202D800u,
    1u,
    { 149, -1 },
    { g_adc3_in, 0 },
    (void *)&g_gate3,
    adc_clock_enabled,
    adc_pin_is_analog,
    adc_hclk_hz,
    0,
    adc_access_allowed,
    adc_kernel_is_hclk,
    "ADC3"
};

static void adc_init_blocks(struct mmio_bus *bus, struct mm_nvic *nvic, mm_u32 count)
{
    stm32_adc_init(&g_adc, &g_soc, bus, nvic);
    if (count > 1u) {
        stm32_adc_init(&g_adc3, &g_soc3, bus, nvic);
        stm32_adc_set_noise(&g_adc3, g_adc3.noise_lsb, g_adc3.noise_seed ^ ADC3_SEED_SALT);
    }
    g_block_count = count;
}

void stm32h5_adc_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    adc_init_blocks(bus, nvic, 1u);
}

void stm32h5_adc_init_with_adc3(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    adc_init_blocks(bus, nvic, 2u);
}

/* Before a re-init this walks the previous part's blocks; reset touches no soc or nvic. */
void stm32h5_adc_reset(void)
{
    mm_u32 i;
    for (i = 0; i < g_block_count; ++i) {
        stm32_adc_reset(g_blocks[i]);
    }
}

void stm32h5_adc_tick(mm_u64 cycles)
{
    mm_u32 i;
    for (i = 0; i < g_block_count; ++i) {
        stm32_adc_tick(g_blocks[i], cycles);
    }
}

mm_u64 stm32h5_adc_cycles_until_event(void)
{
    mm_u64 best = STM32_ADC_NO_EVENT;
    mm_u32 i;
    for (i = 0; i < g_block_count; ++i) {
        mm_u64 t = stm32_adc_cycles_until_event(g_blocks[i]);
        if (t < best) {
            best = t;
        }
    }
    return best;
}

const struct stm32_adc_state *stm32h5_adc_state(void)
{
    return &g_adc;
}

const struct stm32_adc_state *stm32h5_adc3_state(void)
{
    return &g_adc3;
}
