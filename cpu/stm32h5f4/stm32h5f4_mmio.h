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

#ifndef M33MU_STM32H5F4_MMIO_H
#define M33MU_STM32H5F4_MMIO_H

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "m33mu/sau.h"

struct mm_memmap;
struct mm_flash_persist;

mm_bool mm_stm32h5f4_register_mmio(struct mmio_bus *bus);
void mm_stm32h5f4_flash_bind(struct mm_memmap *map,
                             mm_u8 *flash,
                             mm_u32 flash_size,
                             const struct mm_flash_persist *persist,
                             mm_u32 flags);
void mm_stm32h5f4_otp_init(const char *target_name);
mm_u64 mm_stm32h5f4_cpu_hz(void);
mm_u32 *mm_stm32h5f4_rcc_regs(void);
mm_u32 *mm_stm32h5f4_rcc_secure_regs(void);
mm_u32 *mm_stm32h5f4_tzsc_regs(void);
mm_bool mm_stm32h5f4_tz_attr_for_addr(mm_u32 addr,
                                      enum mm_sau_attr *attr_out,
                                      mm_u32 *region_out);
void mm_stm32h5f4_rng_set_nvic(struct mm_nvic *nvic);
void mm_stm32h5f4_exti_set_nvic(struct mm_nvic *nvic);
mm_bool mm_stm32h5f4_eth_register_mmio(struct mmio_bus *bus);
void mm_stm32h5f4_eth_set_nvic(struct mm_nvic *nvic);
void mm_stm32h5f4_eth_reset(void);
void mm_stm32h5f4_eth_poll(void);
void mm_stm32h5f4_watchdog_tick(mm_u64 cycles);
mm_bool mm_stm32h5f4_mpcbb_block_secure(int bank, mm_u32 block_index);
void mm_stm32h5f4_mmio_reset(void);
void mm_stm32h5f4_gpdma_set_nvic(struct mm_nvic *nvic);
mm_u8 mm_stm32h5f4_gpio_get_af(int bank, int pin);
mm_u8 mm_stm32h5f4_gpio_get_mode(int bank, int pin);

#endif /* M33MU_STM32H5F4_MMIO_H */
