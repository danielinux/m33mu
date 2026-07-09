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

#ifndef M33MU_MEMMAP_H
#define M33MU_MEMMAP_H

#include "m33mu/types.h"
#include "m33mu/target.h"
#include "m33mu/mem.h"
#include "m33mu/mmio.h"
#include "m33mu/cpu.h"

struct mm_code_cache;

enum mm_access_type {
    MM_ACCESS_READ = 0,
    MM_ACCESS_WRITE = 1,
    MM_ACCESS_EXEC = 2
};

typedef mm_bool (*mm_access_interceptor)(void *opaque,
                                         enum mm_access_type type,
                                         enum mm_sec_state sec,
                                         mm_u32 addr,
                                         mm_u32 size_bytes);

typedef mm_bool (*mm_flash_write_cb)(void *opaque,
                                     enum mm_sec_state sec,
                                     mm_u32 addr,
                                     mm_u32 size_bytes,
                                     mm_u32 value);

/*
 * ECC check callback: called before every flash read/fetch with the byte
 * offset within the flash buffer.  Return MM_TRUE to allow the read, or
 * MM_FALSE to signal a bus fault (e.g. erased ECC word on LPC55S69).
 */
typedef mm_bool (*mm_flash_ecc_check_cb)(void *opaque, mm_u32 byte_offset);

/* Returns MM_TRUE if the flash sector containing byte_offset is currently
 * attributed SECURE (watermark and/or block-based attribution). Used to
 * filter accesses through the non-secure flash alias: the alias carries a
 * non-secure bus transaction whatever the CPU security state, and the
 * flash TZ filter reads it as zero when the target sector is secure. */
typedef mm_bool (*mm_flash_sector_secure_cb)(void *opaque, mm_u32 byte_offset);

struct mm_memmap {
    struct mm_mem flash;
    struct mm_mem ram;
    struct mm_ram_region ram_regions[8];
    mm_u32 ram_region_count;
    mm_u32 ram_region_last_hit;
    mm_u32 ram_total_size;
    mm_u32 ram_region_offsets[8];
    mm_u32 flash_base_s;
    mm_u32 flash_size_s;
    mm_u32 flash_base_ns;
    mm_u32 flash_size_ns;
    mm_u32 ram_base_s;
    mm_u32 ram_size_s;
    mm_u32 ram_base_ns;
    mm_u32 ram_size_ns;
    struct mmio_bus mmio;
    struct mm_code_cache *code_cache;
    mm_access_interceptor interceptor;
    void *interceptor_opaque;
    mm_flash_write_cb flash_write;
    void *flash_write_opaque;
    mm_flash_ecc_check_cb flash_ecc_check;
    void *flash_ecc_check_opaque;
    mm_flash_sector_secure_cb flash_sector_secure;
    void *flash_sector_secure_opaque;
};

void mm_memmap_init(struct mm_memmap *map, struct mmio_region *regions, size_t region_capacity);
struct mm_memmap *mm_memmap_current(void);
void mm_memmap_set_interceptor(struct mm_memmap *map, mm_access_interceptor fn, void *opaque);
void mm_memmap_set_flash_writer(struct mm_memmap *map, mm_flash_write_cb fn, void *opaque);
void mm_memmap_set_flash_ecc_check(struct mm_memmap *map, mm_flash_ecc_check_cb fn, void *opaque);
void mm_memmap_set_flash_sector_secure(struct mm_memmap *map, mm_flash_sector_secure_cb fn, void *opaque);
void mm_memmap_set_code_cache(struct mm_memmap *map, struct mm_code_cache *cc);
mm_bool mm_memmap_configure_flash(struct mm_memmap *map, const struct mm_target_cfg *cfg, const mm_u8 *backing, mm_bool secure_view);
mm_bool mm_memmap_configure_ram(struct mm_memmap *map, const struct mm_target_cfg *cfg, mm_u8 *backing, mm_bool secure_view);

/* Accessors that go through interceptors and fall back to MMIO for unmapped regions. */
mm_bool mm_memmap_read(const struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr, mm_u32 size, mm_u32 *value_out);
mm_bool mm_memmap_write(struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr, mm_u32 size, mm_u32 value);
mm_bool mm_memmap_write_ram_raw(struct mm_memmap *map, mm_u32 addr, mm_u32 size, mm_u32 value);
void mm_memmap_set_watch(mm_u32 addr, mm_u32 size);
void mm_memmap_clear_watch(void);
void mm_memmap_set_last_pc(mm_u32 pc);
mm_bool mm_memmap_fetch_read16(const struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr, mm_u32 *value_out);
mm_bool mm_memmap_read8(const struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr, mm_u8 *value_out);
mm_bool mm_memmap_write8(struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr, mm_u8 value);

#endif /* M33MU_MEMMAP_H */
