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

/*
 * Simple memory abstraction that supports read 8/16/32 bit accesses.
 * The implementation is intentionally minimal so it can be used by the
 * fetch logic (and any future MMIO bus implementation).
 */

#include "m33mu/mem.h"

/* Read an unsigned 16-bit value from memory.
 * `fault` is set to MM_TRUE if the address is out of range.
 */
mm_bool mem_read16(const struct mm_mem *m, mm_u32 addr, mm_u32 *value_out) {
    if (m == 0 || value_out == 0) {
        return MM_FALSE;
    }
    if (addr < m->base) {
        return MM_FALSE;
    }
    if ((size_t)(addr - m->base) + 2u > m->length) {
        return MM_FALSE;
    }
    *value_out = ((mm_u32)m->buffer[addr - m->base] << 0) |
                 ((mm_u32)m->buffer[addr - m->base + 1] << 8);
    return MM_TRUE;
}

/* Read a 32-bit value. */
mm_bool mem_read32(const struct mm_mem *m, mm_u32 addr, mm_u32 *value_out) {
    if (m == 0 || value_out == 0) {
        return MM_FALSE;
    }
    if (addr < m->base) {
        return MM_FALSE;
    }
    if ((size_t)(addr - m->base) + 4u > m->length) {
        return MM_FALSE;
    }
    *value_out = ((mm_u32)m->buffer[addr - m->base] << 0) |
                 ((mm_u32)m->buffer[addr - m->base + 1] << 8) |
                 ((mm_u32)m->buffer[addr - m->base + 2] << 16) |
                 ((mm_u32)m->buffer[addr - m->base + 3] << 24);
    return MM_TRUE;
}

mm_bool mem_read(const struct mm_mem *m, mm_u32 addr, mm_u8 *dst, size_t len)
{
    if (m == 0 || dst == 0) {
        return MM_FALSE;
    }
    if (addr < m->base) {
        return MM_FALSE;
    }
    if ((size_t)(addr - m->base) + len > m->length) {
        return MM_FALSE;
    }
    while (len--) {
        *dst++ = m->buffer[addr - m->base];
        addr++;
    }
    return MM_TRUE;
}
