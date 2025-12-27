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

#ifndef M33MU_STM32H563_MMIO_H
#define M33MU_STM32H563_MMIO_H

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"

struct mm_memmap;
struct mm_flash_persist;

mm_bool mm_stm32h563_register_mmio(struct mmio_bus *bus);
void mm_stm32h563_flash_bind(struct mm_memmap *map,
                             mm_u8 *flash,
                             mm_u32 flash_size,
                             const struct mm_flash_persist *persist,
                             mm_u32 flags);
mm_u64 mm_stm32h563_cpu_hz(void);
mm_u32 *mm_stm32h563_rcc_regs(void);
mm_u32 *mm_stm32h563_tzsc_regs(void);
void mm_stm32h563_rng_set_nvic(struct mm_nvic *nvic);
void mm_stm32h563_exti_set_nvic(struct mm_nvic *nvic);
mm_bool mm_stm32h563_usb_register_mmio(struct mmio_bus *bus);
void mm_stm32h563_usb_set_nvic(struct mm_nvic *nvic);
void mm_stm32h563_usb_reset(void);
mm_bool mm_stm32h563_eth_register_mmio(struct mmio_bus *bus);
void mm_stm32h563_eth_set_nvic(struct mm_nvic *nvic);
void mm_stm32h563_eth_reset(void);
void mm_stm32h563_eth_poll(void);
void mm_stm32h563_watchdog_tick(mm_u64 cycles);
mm_bool mm_stm32h563_mpcbb_block_secure(int bank, mm_u32 block_index);
void mm_stm32h563_mmio_reset(void);

#endif /* M33MU_STM32H563_MMIO_H */
