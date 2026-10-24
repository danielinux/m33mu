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
#include "m33mu/snapshot.h"
#include "m33mu/undo.h"

/*
 * MMIO bus interface.
 * Regions are registered with a base/size and callbacks. Offsets passed to
 * callbacks are relative to the region base.
 */

typedef mm_bool (*mmio_read_fn)(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out);
typedef mm_bool (*mmio_write_fn)(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value);
typedef enum {
    MMIO_PEEK_OK = 0,
    MMIO_PEEK_UNSUPPORTED = 1
} mmio_peek_result_t;
typedef mmio_peek_result_t (*mmio_peek_fn)(void *opaque, mm_u32 offset, mm_u32 size_bytes, void *dst);
typedef mm_bool (*mmio_save_fn)(void *opaque, struct mm_snapshot_writer *w);
typedef mm_bool (*mmio_load_fn)(void *opaque, struct mm_snapshot_reader *r);
typedef void (*mmio_begin_step_fn)(void *opaque);
typedef void (*mmio_end_step_fn)(void *opaque, const struct mm_undo_sink *sink);

#define MMIO_REGION_MAGIC 0x4d4d494fu
#define MMIO_REGION_F_EXT (1u << 0)

struct mmio_region {
    mm_u32 magic;
    mm_u32 flags;
    mm_u32 base;
    mm_u32 size;
    void *opaque;
    mmio_read_fn read;
    mmio_write_fn write;
    const char *name;
    mm_u32 version;
    mmio_peek_fn peek;
    mmio_save_fn save;
    mmio_load_fn load;
    mmio_begin_step_fn begin_step;
    mmio_end_step_fn end_step;
};

struct mmio_bus {
    struct mmio_region *regions;
    size_t region_count;
    size_t region_capacity;
    mm_bool has_step_hooks;
    size_t last_hit_index;
    mm_bool last_hit_valid;
};

void mmio_bus_init(struct mmio_bus *bus, struct mmio_region *region_storage, size_t capacity);
void mmio_bus_enable_step_hooks(struct mmio_bus *bus);
void mmio_set_peek_mode(mm_bool enabled);
mm_bool mmio_peek_mode(void);

/* Returns MM_TRUE on success; MM_FALSE if storage is full or overlaps occur. */
mm_bool mmio_bus_register_region(struct mmio_bus *bus, const struct mmio_region *region);
/* MM_TRUE if a registered region covers addr. Register-once models use it to
 * register again after the bus was rebuilt (a web session reload). */
mm_bool mmio_bus_maps(const struct mmio_bus *bus, mm_u32 addr);

/* Returns MM_TRUE on handled access, MM_FALSE on unmapped/fault. */
mm_bool mmio_bus_read(const struct mmio_bus *bus, mm_u32 addr, mm_u32 size_bytes, mm_u32 *value_out);
mm_bool mmio_bus_write(const struct mmio_bus *bus, mm_u32 addr, mm_u32 size_bytes, mm_u32 value);
mmio_peek_result_t mmio_bus_peek(const struct mmio_bus *bus, mm_u32 addr, mm_u32 size_bytes, void *dst);

mm_bool mmio_bus_save(const struct mmio_bus *bus, struct mm_snapshot_writer *w);
mm_bool mmio_bus_load(const struct mmio_bus *bus, struct mm_snapshot_reader *r);
void mmio_bus_begin_step(const struct mmio_bus *bus);
void mmio_bus_end_step(const struct mmio_bus *bus, const struct mm_undo_sink *sink);

/* Current security state of the in-flight MMIO access (set by memmap.c). */
void mmio_set_active_sec(enum mm_sec_state sec);
enum mm_sec_state mmio_active_sec(void);

#endif /* M33MU_MMIO_H */
