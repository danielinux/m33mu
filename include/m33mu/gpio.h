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
typedef mm_bool (*mm_rcc_clock_list_fn)(void *opaque, int line, char *out, size_t out_len);
typedef mm_bool (*mm_gpio_bank_info_fn)(void *opaque, int bank, char *name_out, size_t name_len, int *pins_out);
typedef mm_bool (*mm_gpio_set_external_input_fn)(void *opaque, int bank, int pin, mm_bool level);

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

void mm_rcc_set_clock_list_reader(mm_rcc_clock_list_fn reader, void *opaque);
mm_bool mm_rcc_clock_list_present(void);
mm_bool mm_rcc_clock_list_line(int line, char *out, size_t out_len);

void mm_gpio_set_bank_info_reader(mm_gpio_bank_info_fn reader, void *opaque);
mm_bool mm_gpio_bank_info(int bank, char *name_out, size_t name_len, int *pins_out);

/*
 * External-input injection: lets a peripheral-agnostic source (e.g. the
 * signal-injection module, src/signal.c) drive a GPIO pin's IDR bit as if
 * a physical signal were applied, without needing direct access to the
 * per-target static gpio_ctx_data[] table. Mirrors the read-side "reader"
 * registration pattern above. bank is 0=A, 1=B, ... matching the same
 * bank indexing used by mm_gpio_bank_read()/mm_gpio_bank_info(); pin is
 * 0-15. Returns MM_FALSE if no writer is registered for the current
 * target, or if the target rejects the write (e.g. pin not currently in
 * input mode).
 */
void mm_gpio_set_external_input_writer(mm_gpio_set_external_input_fn writer, void *opaque);
mm_bool mm_gpio_set_external_input(int bank, int pin, mm_bool level);
mm_bool mm_gpio_external_input_writer_present(void);

#endif /* M33MU_GPIO_H */
