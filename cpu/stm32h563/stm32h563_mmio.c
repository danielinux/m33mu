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

/* STM32H563: 2 MB dual-bank flash, 640 KB SRAM in three banks, GPIOA..I,
 * USB full-speed. Register behaviour lives in cpu/stm32h5_mmio.c; this file
 * is the part description plus the naming shim the CPU table binds to.
 * Values are from RM0481 and cpu/stm32h563/STM32H563.svd. */

#include "stm32h563/stm32h563_mmio.h"
#include "stm32h563/stm32h563_usb.h"
#include "stm32h5_mmio.h"

/* SECCFGR words per MPCBB bank, one word per 32 blocks of 512 bytes. */
static const mm_u32 g_mpcbb_words[] = { 32u, 32u, 32u };

static const struct stm32h5_mmio_variant g_variant = {
    /* 256 sectors of 8 KB over two banks; 7-bit sector number fields */
    256u,        /* flash_sector_count */
    0x7fu,       /* flash_snb_mask */
    0x7fu,       /* flash_secwm_strt_mask */
    4u,          /* flash_secbb_nregs */

    3u,          /* mpcbb_count: SRAM1..3 */
    g_mpcbb_words,

    9,           /* gpio_count: GPIOA..I */
    4u,          /* i2c_count */

    8u,          /* gpdma_channels: GPDMA1/2 CH0..CH7 */
    0,           /* channel IRQs contiguous from irq_base 27 */
    0,           /* channel IRQs contiguous from irq_base 90 */

    0x00871f00u, /* rng_cr_reset */
    MM_FALSE,    /* rng_has_htcr123 */
    0xC0000000u, /* ahb2enr_reset */

    mm_stm32h563_usb_register_mmio,
    mm_stm32h563_usb_reset
};

mm_bool mm_stm32h563_register_mmio(struct mmio_bus *bus)
{
    return stm32h5_register_mmio(&g_variant, bus);
}

void mm_stm32h563_mmio_reset(void)
{
    stm32h5_mmio_reset(&g_variant);
}

void mm_stm32h563_flash_bind(struct mm_memmap *map,
                             mm_u8 *flash,
                             mm_u32 flash_size,
                             const struct mm_flash_persist *persist,
                             mm_u32 flags)
{
    stm32h5_flash_bind(&g_variant, map, flash, flash_size, persist, flags);
}

void mm_stm32h563_otp_init(const char *target_name)
{
    stm32h5_otp_init(&g_variant, target_name);
}

mm_u64 mm_stm32h563_cpu_hz(void)
{
    return stm32h5_cpu_hz();
}

mm_u32 *mm_stm32h563_rcc_regs(void)
{
    return stm32h5_rcc_regs();
}

mm_u32 *mm_stm32h563_rcc_secure_regs(void)
{
    return stm32h5_rcc_secure_regs();
}

mm_u32 *mm_stm32h563_tzsc_regs(void)
{
    return stm32h5_tzsc_regs();
}

mm_bool mm_stm32h563_tz_attr_for_addr(mm_u32 addr,
                                      enum mm_sau_attr *attr_out,
                                      mm_u32 *region_out)
{
    return stm32h5_tz_attr_for_addr(addr, attr_out, region_out);
}

void mm_stm32h563_rng_set_nvic(struct mm_nvic *nvic)
{
    stm32h5_rng_set_nvic(nvic);
}

void mm_stm32h563_exti_set_nvic(struct mm_nvic *nvic)
{
    stm32h5_exti_set_nvic(nvic);
}

void mm_stm32h563_gpdma_set_nvic(struct mm_nvic *nvic)
{
    stm32h5_gpdma_set_nvic(nvic);
}

void mm_stm32h563_watchdog_tick(mm_u64 cycles)
{
    stm32h5_watchdog_tick(cycles);
}

mm_bool mm_stm32h563_mpcbb_block_secure(int bank, mm_u32 block_index)
{
    return stm32h5_mpcbb_block_secure(bank, block_index);
}

mm_u8 mm_stm32h563_gpio_get_af(int bank, int pin)
{
    return stm32h5_gpio_get_af(bank, pin);
}

mm_u8 mm_stm32h563_gpio_get_mode(int bank, int pin)
{
    return stm32h5_gpio_get_mode(bank, pin);
}
