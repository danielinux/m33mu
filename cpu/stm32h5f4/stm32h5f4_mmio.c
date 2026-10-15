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

/* STM32H5F4: 4 MB dual-bank flash, 1.5 MB SRAM in five banks, GPIOA..K.
 * Register behaviour lives in cpu/stm32h5_mmio.c; this file is the part
 * description plus the naming shim the CPU table binds to. Values are
 * from RM0517.
 *
 * No USB block is modelled here yet, although the part has one. */

#include "stm32h5f4/stm32h5f4_mmio.h"
#include "stm32h5f4/stm32h5f4_timers.h"
#include "stm32h5_mmio.h"

/* SECCFGR words per MPCBB bank, one word per 32 blocks of 512 bytes, so
 * one word per 16 KB: SRAM1..5 are 256/128/384/384/384 KB (RM0517 Table 6). */
static const mm_u32 g_mpcbb_words[] = { 16u, 8u, 24u, 24u, 24u };

/* GPDMA channel interrupts are not contiguous here: channels 0..7 follow
 * each controller's base vector, channels 8..11 are grouped far above. */
static const mm_u16 g_gpdma1_irq_map[12] = {
    27u, 28u, 29u, 30u, 31u, 32u, 33u, 34u, 136u, 137u, 138u, 139u
};
static const mm_u16 g_gpdma2_irq_map[12] = {
    90u, 91u, 92u, 93u, 94u, 95u, 96u, 97u, 140u, 141u, 142u, 143u
};

/* TIM2..TIM5 channel pins, DS14972 Rev 1 Table 15 (alternate functions AF0 to AF7). */
static const struct stm32h5_tim_pin g_tim_pins[] = {
    { 0, 0, 1, 0, 1 }, /* PA0 TIM2_CH1 */
    { 0, 0, 2, 3, 1 }, /* PA0 TIM5_CH1 */
    { 0, 1, 1, 0, 2 }, /* PA1 TIM2_CH2 */
    { 0, 1, 2, 3, 2 }, /* PA1 TIM5_CH2 */
    { 0, 2, 1, 0, 3 }, /* PA2 TIM2_CH3 */
    { 0, 2, 2, 3, 3 }, /* PA2 TIM5_CH3 */
    { 0, 3, 1, 0, 4 }, /* PA3 TIM2_CH4 */
    { 0, 3, 2, 3, 4 }, /* PA3 TIM5_CH4 */
    { 0, 5, 1, 0, 1 }, /* PA5 TIM2_CH1 */
    { 0, 6, 2, 1, 1 }, /* PA6 TIM3_CH1 */
    { 0, 7, 2, 1, 2 }, /* PA7 TIM3_CH2 */
    { 0, 15, 1, 0, 1 }, /* PA15 TIM2_CH1 */
    { 1, 0, 2, 1, 3 }, /* PB0 TIM3_CH3 */
    { 1, 1, 2, 1, 4 }, /* PB1 TIM3_CH4 */
    { 1, 3, 1, 0, 2 }, /* PB3 TIM2_CH2 */
    { 1, 4, 2, 1, 1 }, /* PB4 TIM3_CH1 */
    { 1, 5, 2, 1, 2 }, /* PB5 TIM3_CH2 */
    { 1, 6, 2, 2, 1 }, /* PB6 TIM4_CH1 */
    { 1, 7, 2, 2, 2 }, /* PB7 TIM4_CH2 */
    { 1, 8, 2, 2, 3 }, /* PB8 TIM4_CH3 */
    { 1, 9, 2, 2, 4 }, /* PB9 TIM4_CH4 */
    { 1, 10, 1, 0, 3 }, /* PB10 TIM2_CH3 */
    { 1, 11, 1, 0, 4 }, /* PB11 TIM2_CH4 */
    { 2, 2, 2, 2, 4 }, /* PC2 TIM4_CH4 */
    { 2, 4, 1, 0, 4 }, /* PC4 TIM2_CH4 */
    { 2, 6, 2, 1, 1 }, /* PC6 TIM3_CH1 */
    { 2, 7, 2, 1, 2 }, /* PC7 TIM3_CH2 */
    { 2, 8, 2, 1, 3 }, /* PC8 TIM3_CH3 */
    { 2, 9, 2, 1, 4 }, /* PC9 TIM3_CH4 */
    { 3, 12, 2, 2, 1 }, /* PD12 TIM4_CH1 */
    { 3, 13, 2, 2, 2 }, /* PD13 TIM4_CH2 */
    { 3, 14, 2, 2, 3 }, /* PD14 TIM4_CH3 */
    { 3, 15, 2, 2, 4 }, /* PD15 TIM4_CH4 */
    { 5, 0, 1, 0, 1 }, /* PF0 TIM2_CH1 */
    { 5, 1, 1, 0, 2 }, /* PF1 TIM2_CH2 */
    { 5, 3, 1, 0, 3 }, /* PF3 TIM2_CH3 */
    { 5, 4, 1, 0, 4 }, /* PF4 TIM2_CH4 */
    { 5, 11, 1, 0, 3 }, /* PF11 TIM2_CH3 */
    { 5, 12, 1, 0, 4 }, /* PF12 TIM2_CH4 */
    { 6, 0, 2, 2, 1 }, /* PG0 TIM4_CH1 */
    { 6, 1, 2, 2, 2 }, /* PG1 TIM4_CH2 */
    { 6, 2, 2, 2, 3 }, /* PG2 TIM4_CH3 */
    { 6, 3, 2, 2, 4 }, /* PG3 TIM4_CH4 */
    { 7, 10, 2, 3, 1 }, /* PH10 TIM5_CH1 */
    { 7, 11, 2, 3, 2 }, /* PH11 TIM5_CH2 */
    { 7, 12, 2, 3, 3 }, /* PH12 TIM5_CH3 */
    { 8, 0, 2, 3, 4 }, /* PI0 TIM5_CH4 */
    { 9, 3, 2, 2, 1 }, /* PJ3 TIM4_CH1 */
    { 9, 4, 2, 2, 2 }, /* PJ4 TIM4_CH2 */
    { 9, 5, 2, 2, 3 }, /* PJ5 TIM4_CH3 */
    { 9, 6, 2, 2, 4 }, /* PJ6 TIM4_CH4 */
    { 9, 7, 2, 3, 1 }, /* PJ7 TIM5_CH1 */
    { 9, 8, 2, 3, 2 }, /* PJ8 TIM5_CH2 */
    { 9, 9, 2, 3, 3 }, /* PJ9 TIM5_CH3 */
    { 9, 10, 2, 3, 4 }, /* PJ10 TIM5_CH4 */
    { 10, 11, 2, 1, 1 }, /* PK11 TIM3_CH1 */
    { 10, 12, 2, 1, 2 }, /* PK12 TIM3_CH2 */
    { 10, 13, 2, 1, 3 }, /* PK13 TIM3_CH3 */
    { 10, 14, 2, 1, 4 }, /* PK14 TIM3_CH4 */
};

static const struct stm32h5_mmio_variant g_variant = {
    /* 512 sectors of 8 KB over two banks; the wider geometry needs 8-bit
     * sector number fields where the H563 uses 7. */
    512u,        /* flash_sector_count */
    0xffu,       /* flash_snb_mask */
    0xffu,       /* flash_secwm_strt_mask */
    8u,          /* flash_secbb_nregs */

    5u,          /* mpcbb_count: SRAM1..5 */
    g_mpcbb_words,

    11,          /* gpio_count: GPIOA..K */
    4u,          /* i2c_count */

    MM_FALSE,    /* rcc_secure_is_alias: separate secure bank */
    MM_TRUE,     /* has_tz_attr */

    12u,         /* gpdma_channels: GPDMA1/2 CH0..CH11 */
    g_gpdma1_irq_map,
    g_gpdma2_irq_map,

    0x00200f00u, /* rng_cr_reset */
    MM_TRUE,     /* rng_has_htcr123 */
    0xF0000000u, /* ahb2enr_reset: SRAM2..5 clocks on */

    0,           /* no USB block modelled */
    0,

    0x1001647Au,  /* dbgmcu_idcode: DEV_ID 0x47A, revision Z (ES0639 Table 2) */

    g_tim_pins,
    sizeof(g_tim_pins) / sizeof(g_tim_pins[0]),
    mm_stm32h5f4_timers_input_edge
};

mm_bool mm_stm32h5f4_register_mmio(struct mmio_bus *bus)
{
    return stm32h5_register_mmio(&g_variant, bus);
}

void mm_stm32h5f4_mmio_reset(void)
{
    stm32h5_mmio_reset(&g_variant);
}

void mm_stm32h5f4_flash_bind(struct mm_memmap *map,
                             mm_u8 *flash,
                             mm_u32 flash_size,
                             const struct mm_flash_persist *persist,
                             mm_u32 flags)
{
    stm32h5_flash_bind(&g_variant, map, flash, flash_size, persist, flags);
}

void mm_stm32h5f4_otp_init(const char *target_name)
{
    stm32h5_otp_init(&g_variant, target_name);
}

mm_u64 mm_stm32h5f4_cpu_hz(void)
{
    return stm32h5_cpu_hz();
}

mm_u32 *mm_stm32h5f4_rcc_regs(void)
{
    return stm32h5_rcc_regs();
}

mm_u32 *mm_stm32h5f4_rcc_secure_regs(void)
{
    return stm32h5_rcc_secure_regs();
}

mm_u32 *mm_stm32h5f4_tzsc_regs(void)
{
    return stm32h5_tzsc_regs();
}

mm_bool mm_stm32h5f4_tz_attr_for_addr(mm_u32 addr,
                                      enum mm_sau_attr *attr_out,
                                      mm_u32 *region_out)
{
    return stm32h5_tz_attr_for_addr(addr, attr_out, region_out);
}

void mm_stm32h5f4_rng_set_nvic(struct mm_nvic *nvic)
{
    stm32h5_rng_set_nvic(nvic);
}

void mm_stm32h5f4_exti_set_nvic(struct mm_nvic *nvic)
{
    stm32h5_exti_set_nvic(nvic);
}

void mm_stm32h5f4_gpdma_set_nvic(struct mm_nvic *nvic)
{
    stm32h5_gpdma_set_nvic(nvic);
}

void mm_stm32h5f4_watchdog_tick(mm_u64 cycles)
{
    stm32h5_watchdog_tick(cycles);
}

mm_bool mm_stm32h5f4_mpcbb_block_secure(int bank, mm_u32 block_index)
{
    return stm32h5_mpcbb_block_secure(bank, block_index);
}

mm_u8 mm_stm32h5f4_gpio_get_af(int bank, int pin)
{
    return stm32h5_gpio_get_af(bank, pin);
}

mm_u8 mm_stm32h5f4_gpio_get_mode(int bank, int pin)
{
    return stm32h5_gpio_get_mode(bank, pin);
}
