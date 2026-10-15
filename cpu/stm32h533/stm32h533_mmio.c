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

/* STM32H533: 512 KB dual-bank flash, 272 KB SRAM in three banks, GPIOA..H,
 * three I2C, USB full-speed. Register behaviour lives in cpu/stm32h5_mmio.c;
 * this file is the part description plus the naming shim the CPU table binds
 * to. Values are from RM0493 and cpu/stm32h533/STM32H533.svd, except the
 * GTZC and MPCBB addresses, where the SVD is wrong and ST's CMSIS device
 * header stm32h533xx.h is followed instead (see the commit that fixed the
 * TZSC base). */

#include "stm32h533/stm32h533_mmio.h"
#include "stm32h533/stm32h533_usb.h"
#include "stm32h533/stm32h533_timers.h"
#include "stm32h5_mmio.h"

/* SECCFGR words per MPCBB bank, one word per 32 blocks of 512 bytes. */
static const mm_u32 g_mpcbb_words[] = { 32u, 32u, 32u };

/* TIM2..TIM5 channel pins, DS14539 Rev 3 Table 15 (alternate functions AF0 to AF7). */
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
};

static const struct stm32h5_mmio_variant g_variant = {
    /* 64 sectors of 8 KB over two banks; 7-bit sector number fields */
    64u,         /* flash_sector_count */
    0x7fu,       /* flash_snb_mask */
    0x7fu,       /* flash_secwm_strt_mask */
    4u,          /* flash_secbb_nregs */

    3u,          /* mpcbb_count: SRAM1..3 */
    g_mpcbb_words,

    8,           /* gpio_count: GPIOA..H */
    3u,          /* i2c_count: no I2C4 on this part */

    MM_TRUE,     /* rcc_secure_is_alias: one bank behind both aliases */
    MM_FALSE,    /* has_tz_attr: this part reports no SAU attribution */

    8u,          /* gpdma_channels: GPDMA1/2 CH0..CH7 */
    0,           /* channel IRQs contiguous from irq_base 27 */
    0,           /* channel IRQs contiguous from irq_base 90 */

    0x00871f00u, /* rng_cr_reset */
    MM_FALSE,    /* rng_has_htcr123 */
    0xC0000000u, /* ahb2enr_reset */

    mm_stm32h533_usb_register_mmio,
    mm_stm32h533_usb_reset,

    0x10006478u,  /* dbgmcu_idcode: DEV_ID 0x478, revision A (ES0621 Table 2) */

    g_tim_pins,
    sizeof(g_tim_pins) / sizeof(g_tim_pins[0]),
    mm_stm32h533_timers_input_edge
};

mm_bool mm_stm32h533_register_mmio(struct mmio_bus *bus)
{
    return stm32h5_register_mmio(&g_variant, bus);
}

void mm_stm32h533_mmio_reset(void)
{
    stm32h5_mmio_reset(&g_variant);
}

void mm_stm32h533_flash_bind(struct mm_memmap *map,
                             mm_u8 *flash,
                             mm_u32 flash_size,
                             const struct mm_flash_persist *persist,
                             mm_u32 flags)
{
    stm32h5_flash_bind(&g_variant, map, flash, flash_size, persist, flags);
}

void mm_stm32h533_otp_init(const char *target_name)
{
    stm32h5_otp_init(&g_variant, target_name);
}

mm_u64 mm_stm32h533_cpu_hz(void)
{
    return stm32h5_cpu_hz();
}

mm_u32 *mm_stm32h533_rcc_regs(void)
{
    return stm32h5_rcc_regs();
}

mm_u32 *mm_stm32h533_tzsc_regs(void)
{
    return stm32h5_tzsc_regs();
}

void mm_stm32h533_rng_set_nvic(struct mm_nvic *nvic)
{
    stm32h5_rng_set_nvic(nvic);
}

void mm_stm32h533_exti_set_nvic(struct mm_nvic *nvic)
{
    stm32h5_exti_set_nvic(nvic);
}

void mm_stm32h533_watchdog_tick(mm_u64 cycles)
{
    stm32h5_watchdog_tick(cycles);
}

mm_bool mm_stm32h533_mpcbb_block_secure(int bank, mm_u32 block_index)
{
    return stm32h5_mpcbb_block_secure(bank, block_index);
}
