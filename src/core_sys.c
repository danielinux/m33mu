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

#include "m33mu/core_sys.h"

struct mm_core_stub {
    int unused;
};

static mm_bool stub_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    (void)opaque;
    (void)offset;
    if (value_out == 0) {
        return MM_FALSE;
    }
    if (size_bytes != 1u && size_bytes != 2u && size_bytes != 4u) {
        return MM_FALSE;
    }
    *value_out = 0;
    return MM_TRUE;
}

static mm_bool stub_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    (void)opaque;
    (void)offset;
    (void)value;
    if (size_bytes != 1u && size_bytes != 2u && size_bytes != 4u) {
        return MM_FALSE;
    }
    return MM_TRUE;
}

mm_bool mm_core_sys_register(struct mmio_bus *bus)
{
    static struct mm_core_stub stub;
    struct mmio_region regs[3];

    /* ITM */
    regs[0].base = 0xE0000000u;
    regs[0].size = 0x1000u;
    /* DWT */
    regs[1].base = 0xE0001000u;
    regs[1].size = 0x1000u;
    /* FPB */
    regs[2].base = 0xE0002000u;
    regs[2].size = 0x1000u;

    {
        int i;
        for (i = 0; i < 3; ++i) {
            regs[i].opaque = &stub;
            regs[i].read = stub_read;
            regs[i].write = stub_write;
            if (!mmio_bus_register_region(bus, &regs[i])) {
                return MM_FALSE;
            }
        }
    }
    return MM_TRUE;
}
