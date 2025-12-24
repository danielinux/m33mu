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

#ifndef M33MU_MEM_H
#define M33MU_MEM_H

#include "m33mu/types.h"

/* Minimal memory buffer abstraction used by the fetch logic. */
struct mm_mem {
    const mm_u8 *buffer; /* immutable data; adjust to mm_u8* if writes are needed */
    size_t length;       /* byte length */
    mm_u32 base;         /* base address */
};

/* Read an unsigned 16-bit value starting at `addr`. Returns MM_TRUE on success. */
mm_bool mem_read16(const struct mm_mem *m, mm_u32 addr, mm_u32 *value_out);

/* Read a 32-bit value. Same semantics as mem_read16. */
mm_bool mem_read32(const struct mm_mem *m, mm_u32 addr, mm_u32 *value_out);

/* Read raw bytes into buffer. */
mm_bool mem_read(const struct mm_mem *m, mm_u32 addr, mm_u8 *dst, size_t len);

#endif /* M33MU_MEM_H */
