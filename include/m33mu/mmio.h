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

#ifndef M33MU_MMIO_H
#define M33MU_MMIO_H

#include "types.h"
#include "m33mu/cpu.h"

/*
 * MMIO bus interface.
 * Regions are registered with a base/size and callbacks. Offsets passed to
 * callbacks are relative to the region base.
 */

typedef mm_bool (*mmio_read_fn)(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out);
typedef mm_bool (*mmio_write_fn)(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value);

struct mmio_region {
    mm_u32 base;
    mm_u32 size;
    void *opaque;
    mmio_read_fn read;
    mmio_write_fn write;
};

struct mmio_bus {
    struct mmio_region *regions;
    size_t region_count;
    size_t region_capacity;
};

void mmio_bus_init(struct mmio_bus *bus, struct mmio_region *region_storage, size_t capacity);

/* Returns MM_TRUE on success; MM_FALSE if storage is full or overlaps occur. */
mm_bool mmio_bus_register_region(struct mmio_bus *bus, const struct mmio_region *region);

/* Returns MM_TRUE on handled access, MM_FALSE on unmapped/fault. */
mm_bool mmio_bus_read(const struct mmio_bus *bus, mm_u32 addr, mm_u32 size_bytes, mm_u32 *value_out);
mm_bool mmio_bus_write(const struct mmio_bus *bus, mm_u32 addr, mm_u32 size_bytes, mm_u32 value);

/* Current security state of the in-flight MMIO access (set by memmap.c). */
void mmio_set_active_sec(enum mm_sec_state sec);
enum mm_sec_state mmio_active_sec(void);

#endif /* M33MU_MMIO_H */
