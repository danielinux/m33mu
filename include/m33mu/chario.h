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

#ifndef M33MU_CHARIO_H
#define M33MU_CHARIO_H

#include "types.h"

/*
 * Character backend used by UART/ITM-style devices.
 * write returns MM_TRUE on success; backends should be non-blocking.
 */

typedef mm_bool (*mm_char_write_fn)(void *opaque, mm_u8 byte);
typedef void (*mm_char_flush_fn)(void *opaque);

struct mm_char_backend {
    mm_char_write_fn write;
    mm_char_flush_fn flush;
    void *opaque;
};

void mm_char_backend_init(struct mm_char_backend *backend, mm_char_write_fn write_fn, mm_char_flush_fn flush_fn, void *opaque);
mm_bool mm_char_putc(struct mm_char_backend *backend, mm_u8 byte);
void mm_char_flush(struct mm_char_backend *backend);

#endif /* M33MU_CHARIO_H */
