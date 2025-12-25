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

#ifndef M33MU_GPIO_H
#define M33MU_GPIO_H

#include "types.h"

/*
 * GPIO line abstraction for level changes and notifications.
 */

typedef void (*mm_gpio_listener_fn)(void *opaque, mm_u8 level);
typedef mm_u32 (*mm_gpio_bank_read_fn)(void *opaque, int bank);
typedef mm_u32 (*mm_gpio_bank_read_moder_fn)(void *opaque, int bank);
typedef mm_bool (*mm_gpio_bank_clock_fn)(void *opaque, int bank);
typedef mm_u32 (*mm_gpio_bank_read_seccfgr_fn)(void *opaque, int bank);

struct mm_gpio_line {
    mm_gpio_listener_fn listener;
    void *opaque;
    mm_u8 level; /* 0 = low, non-zero = high */
};

void mm_gpio_line_init(struct mm_gpio_line *line, mm_gpio_listener_fn listener, void *opaque);
void mm_gpio_set_level(struct mm_gpio_line *line, mm_u8 level);
mm_u8 mm_gpio_get_level(const struct mm_gpio_line *line);
void mm_gpio_bank_set_reader(mm_gpio_bank_read_fn reader, void *opaque);
void mm_gpio_bank_set_moder_reader(mm_gpio_bank_read_moder_fn reader, void *opaque);
void mm_gpio_bank_set_clock_reader(mm_gpio_bank_clock_fn reader, void *opaque);
void mm_gpio_bank_set_seccfgr_reader(mm_gpio_bank_read_seccfgr_fn reader, void *opaque);
mm_u32 mm_gpio_bank_read(int bank);
mm_u32 mm_gpio_bank_read_moder(int bank);
mm_bool mm_gpio_bank_clock_enabled(int bank);
mm_u32 mm_gpio_bank_read_seccfgr(int bank);
mm_bool mm_gpio_bank_reader_present(void);

#endif /* M33MU_GPIO_H */
