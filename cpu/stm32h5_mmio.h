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

#ifndef M33MU_STM32H5_MMIO_H
#define M33MU_STM32H5_MMIO_H

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "m33mu/sau.h"
#include "m33mu/types.h"

struct mm_memmap;
struct mm_flash_persist;

/* Upper bounds for the statically allocated per-instance state. Raise these
 * when a variant needs more; stm32h5_mmio.c only ever touches the first
 * mpcbb_count / gpio_count entries. */
#define STM32H5_MPCBB_MAX 5
#define STM32H5_GPIO_MAX  11

/* Everything that differs between the STM32H5 parts sharing stm32h5_mmio.c.
 * Anything not listed here is identical across them and lives in the
 * implementation as a plain constant.
 *
 * The per-part descriptors initialise this positionally, so when adding
 * a field here, add it in the same position to every per-part descriptor
 * under cpu/stm32h533, cpu/stm32h563 and cpu/stm32h5f4. */
struct stm32h5_mmio_variant {
    /* Embedded flash geometry. sector_count is over the whole flash, both
     * banks; snb_mask and secwm_strt_mask are the unshifted field widths,
     * which widen with the sector count. */
    mm_u32 flash_sector_count;
    mm_u32 flash_snb_mask;
    mm_u32 flash_secwm_strt_mask;
    mm_u32 flash_secbb_nregs;

    /* GTZC MPCBB: one controller per SRAM bank, at 0x40032c00 + n * 0x400.
     * mpcbb_words[n] is the number of SECCFGR words that bank populates,
     * one word per 32 blocks. */
    mm_u32 mpcbb_count;
    const mm_u32 *mpcbb_words;

    /* GPIO banks present, counting from GPIOA. */
    int gpio_count;

    /* I2C instances present, counting from I2C1. */
    mm_u32 i2c_count;

    /* MM_TRUE when the secure RCC alias is just a second view of the one
     * register file, as the SVD describes it, rather than a bank of its
     * own. H533 models it that way; H563 and H5F4 keep two banks. */
    mm_bool rcc_secure_is_alias;

    /* MM_TRUE when the part reports SAU attribution for TZSC-filtered
     * peripherals through stm32h5_tz_attr_for_addr(). */
    mm_bool has_tz_attr;

    /* GPDMA1/GPDMA2 channel count, and optional explicit per-channel IRQ
     * tables for parts whose channel vectors are not contiguous. Null maps
     * mean irq_base + channel index. */
    mm_u8 gpdma_channels;
    const mm_u16 *gpdma1_irq_map;
    const mm_u16 *gpdma2_irq_map;

    /* Reset values that differ between parts. */
    mm_u32 rng_cr_reset;
    mm_bool rng_has_htcr123; /* RNG_HTCR1..3 present at 0x14..0x1c */
    mm_u32 ahb2enr_reset;

    /* USB full-speed block, present on some parts only. Null when the
     * variant does not model one. */
    mm_bool (*usb_register_mmio)(struct mmio_bus *bus);
    void (*usb_reset)(void);
};

/* Entry points. The four that a target can reach first each latch the
 * variant, so they may be called in any order. */
mm_bool stm32h5_register_mmio(const struct stm32h5_mmio_variant *v, struct mmio_bus *bus);
void stm32h5_mmio_reset(const struct stm32h5_mmio_variant *v);
void stm32h5_flash_bind(const struct stm32h5_mmio_variant *v,
                        struct mm_memmap *map,
                        mm_u8 *flash,
                        mm_u32 flash_size,
                        const struct mm_flash_persist *persist,
                        mm_u32 flags);
void stm32h5_otp_init(const struct stm32h5_mmio_variant *v, const char *target_name);

mm_u64 stm32h5_cpu_hz(void);
mm_u32 *stm32h5_rcc_regs(void);
mm_u32 *stm32h5_rcc_secure_regs(void);
mm_u32 *stm32h5_tzsc_regs(void);
mm_bool stm32h5_tz_attr_for_addr(mm_u32 addr,
                                 enum mm_sau_attr *attr_out,
                                 mm_u32 *region_out);
void stm32h5_rng_set_nvic(struct mm_nvic *nvic);
void stm32h5_exti_set_nvic(struct mm_nvic *nvic);
void stm32h5_gpdma_set_nvic(struct mm_nvic *nvic);
void stm32h5_watchdog_tick(mm_u64 cycles);
mm_bool stm32h5_mpcbb_block_secure(int bank, mm_u32 block_index);
mm_u8 stm32h5_gpio_get_af(int bank, int pin);
mm_u8 stm32h5_gpio_get_mode(int bank, int pin);

/* Provision a bank's secure watermark option byte, the way a board is
 * provisioned before a secure-boot image is flashed onto it. `bank` is 0 for
 * SECWM1 and 1 for SECWM2; `strt` and `end` are sector indices within the
 * bank, and strt > end means the bank carries no secure sector. Without this
 * the bank keeps its TZEN=1 reset value: bank 1 all secure, bank 2 all
 * non-secure. */
void stm32h5_flash_set_secwm(int bank, mm_u32 strt, mm_u32 end);

#endif /* M33MU_STM32H5_MMIO_H */
