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
#include "stm32h5_mmio.h"

/* SECCFGR words per MPCBB bank, one word per 32 blocks of 512 bytes. */
static const mm_u32 g_mpcbb_words[] = { 32u, 32u, 32u };

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
    mm_stm32h533_usb_reset
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
