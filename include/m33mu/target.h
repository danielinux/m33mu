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

#ifndef M33MU_TARGET_H
#define M33MU_TARGET_H

#include "m33mu/types.h"
#include "m33mu/flash_persist.h"

struct mmio_bus;
struct mm_nvic;
struct mm_memmap;

struct mm_target_cfg {
    mm_u32 flash_base_s;
    mm_u32 flash_size_s;
    mm_u32 flash_base_ns;
    mm_u32 flash_size_ns;

    mm_u32 ram_base_s;
    mm_u32 ram_size_s;
    mm_u32 ram_base_ns;
    mm_u32 ram_size_ns;

    mm_u32 flags;

    void (*soc_reset)(void);
    mm_bool (*soc_register_mmio)(struct mmio_bus *bus);
    void (*flash_bind)(struct mm_memmap *map,
                       mm_u8 *flash,
                       mm_u32 flash_size,
                       const struct mm_flash_persist *persist,
                       mm_u32 flags);
    mm_u64 (*clock_get_hz)(void);
    void (*usart_init)(struct mmio_bus *bus, struct mm_nvic *nvic);
    void (*usart_reset)(void);
    void (*usart_poll)(void);

    void (*timer_init)(struct mmio_bus *bus, struct mm_nvic *nvic);
    void (*timer_reset)(void);
    void (*timer_tick)(mm_u64 cycles);
};

#define MM_TARGET_FLAG_NVM_WRITEONCE (1u << 0)

#endif /* M33MU_TARGET_H */
