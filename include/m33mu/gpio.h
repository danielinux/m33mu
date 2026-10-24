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
typedef mm_bool (*mm_gpio_set_external_input_fn)(void *opaque, int bank, int pin, mm_bool level,
                                                 mm_u64 late_cycles);

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

/* Drive a pin as an external signal would; MM_FALSE if rejected. The level
 * changed late_cycles before the call, which timer captures back-date to. */
void mm_gpio_set_external_input_writer(mm_gpio_set_external_input_fn writer, void *opaque);
mm_bool mm_gpio_set_external_input(int bank, int pin, mm_bool level, mm_u64 late_cycles);
mm_bool mm_gpio_external_input_writer_present(void);

/* Level the firmware drives on a pad: 0, 1 or MM_GPIO_LEVEL_Z (not driven). */
#define MM_GPIO_LEVEL_Z 2u
typedef void (*mm_gpio_pad_listener_fn)(void *opaque, int bank, int pin, mm_u8 level);
void mm_gpio_set_pad_listener(mm_gpio_pad_listener_fn listener, void *opaque);
mm_bool mm_gpio_pad_listener_present(void);
void mm_gpio_notify_pad(int bank, int pin, mm_u8 level);
/* The same for a change that a peripheral (a timer output) makes
 * cycles_ahead cycles after the current time, inside the batch of cycles
 * being ticked. Listeners read the offset with mm_gpio_pad_event_ahead_cycles(). */
void mm_gpio_notify_pad_ahead(int bank, int pin, mm_u8 level, mm_u64 cycles_ahead);
mm_u64 mm_gpio_pad_event_ahead_cycles(void);

/* Electrical pad configuration as firmware programmed it. A target reports
 * changes through the config listener; the drive level still arrives through
 * the pad listener above. */
#define MM_GPIO_MODE_INPUT  0u
#define MM_GPIO_MODE_OUTPUT 1u
#define MM_GPIO_MODE_AF     2u
#define MM_GPIO_MODE_ANALOG 3u
#define MM_GPIO_PULL_NONE 0u
#define MM_GPIO_PULL_UP   1u
#define MM_GPIO_PULL_DOWN 2u
struct mm_gpio_pad_config {
    mm_u8 mode;
    mm_u8 open_drain;
    mm_u8 pull;
    mm_u8 af;           /* alternate-function number, meaningful in AF mode */
};
typedef void (*mm_gpio_pad_config_listener_fn)(void *opaque, int bank, int pin,
                                               const struct mm_gpio_pad_config *config);
void mm_gpio_set_pad_config_listener(mm_gpio_pad_config_listener_fn listener, void *opaque);
mm_bool mm_gpio_pad_config_listener_present(void);
void mm_gpio_notify_pad_config(int bank, int pin, const struct mm_gpio_pad_config *config);

#endif /* M33MU_GPIO_H */
