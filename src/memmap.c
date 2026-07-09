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

#include "m33mu/memmap.h"
#include "m33mu/trace.h"
#include "m33mu/code_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mm_bool g_memwatch_enabled = MM_FALSE;
static mm_u32 g_memwatch_addr = 0;
static mm_u32 g_memwatch_size = 0;
static mm_u32 g_memwatch_pc = 0;
static struct mm_memmap *g_current_map = 0;

#define MM_INTERCEPT_OK(map, type, sec, addr, size) \
    ((map)->interceptor == 0 || \
     (map)->interceptor((map)->interceptor_opaque, (type), (sec), (addr), (size)))

static mm_bool read_buf_le(const mm_u8 *buf, mm_u32 offset, mm_u32 size, mm_u32 *value_out)
{
    mm_u32 v;
    mm_u32 i;
    if (buf == 0 || value_out == 0) {
        return MM_FALSE;
    }
    if (size == 1u) {
        *value_out = (mm_u32)buf[offset];
        return MM_TRUE;
    }
    if (size == 2u) {
        v = (mm_u32)buf[offset];
        v |= ((mm_u32)buf[offset + 1u]) << 8;
        *value_out = v;
        return MM_TRUE;
    }
    if (size == 4u) {
        v = (mm_u32)buf[offset];
        v |= ((mm_u32)buf[offset + 1u]) << 8;
        v |= ((mm_u32)buf[offset + 2u]) << 16;
        v |= ((mm_u32)buf[offset + 3u]) << 24;
        *value_out = v;
        return MM_TRUE;
    }
    v = 0;
    if (size > 0u && size <= 4u) {
        for (i = 0; i < size; ++i) {
            v |= ((mm_u32)buf[offset + i]) << (i * 8u);
        }
        *value_out = v;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool ram_offset_for_addr(const struct mm_memmap *map, mm_u32 addr, mm_u32 size, mm_u32 *offset_out);

static const struct mm_memmap *s_last_hit_map;
static mm_u32 s_last_hit_idx;

static mm_bool read_buf_bytes(const mm_u8 *buf, mm_u32 offset, mm_u32 size, mm_u8 *out)
{
    if (buf == 0 || out == 0 || size == 0u) {
        return MM_FALSE;
    }
    memcpy(out, buf + offset, size);
    return MM_TRUE;
}

static mm_bool memmap_read_old_bytes(const struct mm_memmap *map, enum mm_sec_state sec,
                                     mm_u32 addr, mm_u32 size, mm_u8 *out, mm_u16 *flags_out)
{
    mm_u32 base;
    mm_u32 size_limit;
    mm_u32 offset;
    mm_u32 val;

    if (map == 0 || out == 0 || size == 0u) {
        return MM_FALSE;
    }
    if (flags_out != 0) {
        *flags_out = 0;
    }
    if (map->flash.buffer != 0) {
        base = map->flash_base_s;
        size_limit = map->flash_size_s;
        if (size_limit == 0u && map->flash.length > 0u) {
            base = map->flash.base;
            size_limit = (mm_u32)map->flash.length;
        }
        if (addr >= base && (addr - base) + size <= size_limit) {
            offset = addr - base;
            return read_buf_bytes(map->flash.buffer, offset, size, out);
        }
        base = map->flash_base_ns;
        size_limit = map->flash_size_ns;
        if (size_limit == 0u && map->flash.length > 0u) {
            base = map->flash.base;
            size_limit = (mm_u32)map->flash.length;
        }
        if (addr >= base && (addr - base) + size <= size_limit) {
            offset = addr - base;
            return read_buf_bytes(map->flash.buffer, offset, size, out);
        }
    }
    if (map->ram.buffer != 0) {
        if (ram_offset_for_addr(map, addr, size, &offset)) {
            return read_buf_bytes(map->ram.buffer, offset, size, out);
        }
    }
    mmio_set_active_sec(sec);
    if (mmio_bus_peek(&map->mmio, addr, size, out) == MMIO_PEEK_OK) {
        if (flags_out != 0) {
            *flags_out = MM_TRACE_MEM_MMIO;
        }
        return MM_TRUE;
    }
    if (mmio_bus_read(&map->mmio, addr, size, &val)) {
        if (size == 1u) {
            out[0] = (mm_u8)(val & 0xffu);
        } else if (size == 2u) {
            out[0] = (mm_u8)(val & 0xffu);
            out[1] = (mm_u8)((val >> 8) & 0xffu);
        } else if (size == 4u) {
            out[0] = (mm_u8)(val & 0xffu);
            out[1] = (mm_u8)((val >> 8) & 0xffu);
            out[2] = (mm_u8)((val >> 16) & 0xffu);
            out[3] = (mm_u8)((val >> 24) & 0xffu);
        }
        if (flags_out != 0) {
            *flags_out = MM_TRACE_MEM_MMIO;
        }
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool ram_region_offset_for_addr(const struct mm_memmap *map,
                                          mm_u32 region_index,
                                          mm_u32 addr,
                                          mm_u32 size,
                                          mm_u32 *offset_out)
{
    const struct mm_ram_region *r;
    mm_u32 end;

    if (map == 0 || offset_out == 0 || region_index >= map->ram_region_count) {
        return MM_FALSE;
    }
    r = &map->ram_regions[region_index];
    end = r->size;
    if (addr >= r->base_s && (addr - r->base_s) + size <= end) {
        *offset_out = map->ram_region_offsets[region_index] + (addr - r->base_s);
        return MM_TRUE;
    }
    if (addr >= r->base_ns && (addr - r->base_ns) + size <= end) {
        *offset_out = map->ram_region_offsets[region_index] + (addr - r->base_ns);
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool ram_offset_for_addr(const struct mm_memmap *map, mm_u32 addr, mm_u32 size, mm_u32 *offset_out)
{
    mm_u32 i;
    mm_u32 last_hit;
    if (map == 0 || offset_out == 0) {
        return MM_FALSE;
    }
    if (map->ram_region_count > 0u) {
        last_hit = (s_last_hit_map == map) ? s_last_hit_idx : 0u;
        if (last_hit < map->ram_region_count &&
            ram_region_offset_for_addr(map, last_hit, addr, size, offset_out)) {
            return MM_TRUE;
        }
        for (i = 0; i < map->ram_region_count; ++i) {
            if (i == last_hit) {
                continue;
            }
            if (ram_region_offset_for_addr(map, i, addr, size, offset_out)) {
                s_last_hit_map = map;
                s_last_hit_idx = i;
                return MM_TRUE;
            }
        }
    } else {
        mm_u32 base = map->ram_base_s;
        mm_u32 size_limit = map->ram_size_s;
        if (addr >= base && (addr - base) + size <= size_limit) {
            *offset_out = addr - base;
            return MM_TRUE;
        }
        base = map->ram_base_ns;
        size_limit = map->ram_size_ns;
        if (addr >= base && (addr - base) + size <= size_limit) {
            *offset_out = addr - base;
            return MM_TRUE;
        }
    }
    if (map->ram_total_size > 0u &&
        map->ram_base_s == 0u &&
        map->ram_base_ns == 0u &&
        addr + size <= map->ram_total_size) {
        *offset_out = addr;
        return MM_TRUE;
    }
    return MM_FALSE;
}

void mm_memmap_init(struct mm_memmap *map, struct mmio_region *regions, size_t region_capacity)
{
    mmio_bus_init(&map->mmio, regions, region_capacity);
    map->flash.buffer = 0;
    map->flash.length = 0;
    map->flash.base = 0;
    map->ram.buffer = 0;
    map->ram.length = 0;
    map->ram.base = 0;
    map->ram_region_count = 0;
    map->ram_region_last_hit = ~0u;
    map->ram_total_size = 0;
    map->ram_region_offsets[0] = 0;
    map->ram_region_offsets[1] = 0;
    map->ram_region_offsets[2] = 0;
    map->ram_region_offsets[3] = 0;
    map->ram_region_offsets[4] = 0;
    map->ram_region_offsets[5] = 0;
    map->ram_region_offsets[6] = 0;
    map->ram_region_offsets[7] = 0;
    map->interceptor = 0;
    map->interceptor_opaque = 0;
    map->flash_write = 0;
    map->flash_write_opaque = 0;
    map->flash_ecc_check = 0;
    map->flash_ecc_check_opaque = 0;
    map->code_cache = 0;
    map->flash_base_s = map->flash_base_ns = 0;
    map->flash_size_s = map->flash_size_ns = 0;
    map->ram_base_s = map->ram_base_ns = 0;
    map->ram_size_s = map->ram_size_ns = 0;
    g_current_map = map;
}

struct mm_memmap *mm_memmap_current(void)
{
    return g_current_map;
}

void mm_memmap_set_interceptor(struct mm_memmap *map, mm_access_interceptor fn, void *opaque)
{
    map->interceptor = fn;
    map->interceptor_opaque = opaque;
}

void mm_memmap_set_flash_writer(struct mm_memmap *map, mm_flash_write_cb fn, void *opaque)
{
    map->flash_write = fn;
    map->flash_write_opaque = opaque;
}

void mm_memmap_set_flash_sector_secure(struct mm_memmap *map, mm_flash_sector_secure_cb fn, void *opaque)
{
    if (map == 0) return;
    map->flash_sector_secure = fn;
    map->flash_sector_secure_opaque = opaque;
}

void mm_memmap_set_flash_ecc_check(struct mm_memmap *map, mm_flash_ecc_check_cb fn, void *opaque)
{
    map->flash_ecc_check = fn;
    map->flash_ecc_check_opaque = opaque;
}

void mm_memmap_set_code_cache(struct mm_memmap *map, struct mm_code_cache *cc)
{
    if (map == 0) {
        return;
    }
    map->code_cache = cc;
}

mm_bool mm_memmap_configure_flash(struct mm_memmap *map, const struct mm_target_cfg *cfg, const mm_u8 *backing, mm_bool secure_view)
{
    if (map == 0 || cfg == 0 || backing == 0) {
        return MM_FALSE;
    }
    map->flash.buffer = backing;
    map->flash_base_s = cfg->flash_base_s;
    map->flash_base_ns = cfg->flash_base_ns;
    map->flash_size_s = cfg->flash_size_s;
    map->flash_size_ns = cfg->flash_size_ns;
    if (secure_view) {
        map->flash.length = cfg->flash_size_s;
        map->flash.base = cfg->flash_base_s;
    } else {
        map->flash.length = cfg->flash_size_ns;
        map->flash.base = cfg->flash_base_ns;
    }
    return MM_TRUE;
}

mm_bool mm_memmap_configure_ram(struct mm_memmap *map, const struct mm_target_cfg *cfg, mm_u8 *backing, mm_bool secure_view)
{
    if (map == 0 || cfg == 0 || backing == 0) {
        return MM_FALSE;
    }
    map->ram.buffer = backing;
    map->ram_base_s = cfg->ram_base_s;
    map->ram_base_ns = cfg->ram_base_ns;
    map->ram_size_s = cfg->ram_size_s;
    map->ram_size_ns = cfg->ram_size_ns;
    map->ram_region_last_hit = ~0u;
    if (cfg->ram_regions != 0 && cfg->ram_region_count > 0u) {
        mm_u32 total = 0;
        mm_u32 i;
        map->ram_region_count = cfg->ram_region_count;
        if (map->ram_region_count > (sizeof(map->ram_regions) / sizeof(map->ram_regions[0]))) {
            map->ram_region_count = (mm_u32)(sizeof(map->ram_regions) / sizeof(map->ram_regions[0]));
        }
        for (i = 0; i < map->ram_region_count; ++i) {
            map->ram_regions[i] = cfg->ram_regions[i];
            map->ram_region_offsets[i] = total;
            total += cfg->ram_regions[i].size;
        }
        map->ram_total_size = total;
    } else {
        map->ram_region_count = 0;
        map->ram_total_size = cfg->ram_size_s;
    }
    if (secure_view) {
        map->ram.length = (map->ram_total_size != 0u) ? map->ram_total_size : cfg->ram_size_s;
        map->ram.base = cfg->ram_base_s;
    } else {
        map->ram.length = (map->ram_total_size != 0u) ? map->ram_total_size : cfg->ram_size_ns;
        map->ram.base = cfg->ram_base_ns;
    }
    return MM_TRUE;
}

mm_bool mm_memmap_read(const struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr, mm_u32 size, mm_u32 *value_out)
{
    mm_u32 base;
    mm_u32 size_limit;
    mm_u32 offset;
    mm_u32 tmp;

    if (map == 0) {
        return MM_FALSE;
    }
    if (!MM_INTERCEPT_OK(map, MM_ACCESS_READ, sec, addr, size)) {
        return MM_FALSE;
    }
    /* Flash */
    if (map->flash.buffer != 0) {
        /* Try secure flash window. */
        base = map->flash_base_s;
        size_limit = map->flash_size_s;
        if (size_limit == 0u && map->flash.length > 0u) {
            base = map->flash.base;
            size_limit = (mm_u32)map->flash.length;
        }
        if (addr >= base && (addr - base) + size <= size_limit) {
            offset = addr - base;
            if (map->flash_ecc_check != 0 && !map->flash_ecc_check(map->flash_ecc_check_opaque, offset)) {
                return MM_FALSE;
            }
            if (read_buf_le(map->flash.buffer, offset, size, &tmp)) { *value_out = tmp; return MM_TRUE; }
        }
        /* Try non-secure flash window. */
        base = map->flash_base_ns;
        size_limit = map->flash_size_ns;
        if (size_limit == 0u && map->flash.length > 0u) {
            base = map->flash.base;
            size_limit = (mm_u32)map->flash.length;
        }
        if (addr >= base && (addr - base) + size <= size_limit) {
            offset = addr - base;
            /* The NS alias issues a non-secure bus transaction whatever the
             * CPU security state: the flash TZ filter reads it as zero when
             * the target sector is attributed secure (RM0481). */
            if (map->flash_sector_secure != 0 &&
                    map->flash_sector_secure(map->flash_sector_secure_opaque, offset)) {
                *value_out = 0u;
                return MM_TRUE;
            }
            if (map->flash_ecc_check != 0 && !map->flash_ecc_check(map->flash_ecc_check_opaque, offset)) {
                return MM_FALSE;
            }
            if (read_buf_le(map->flash.buffer, offset, size, &tmp)) { *value_out = tmp; return MM_TRUE; }
        }
    }
    /* RAM */
    if (map->ram.buffer != 0) {
        if (ram_offset_for_addr(map, addr, size, &offset)) {
            if (read_buf_le(map->ram.buffer, offset, size, &tmp)) { *value_out = tmp; return MM_TRUE; }
        }
    }
    /* MMIO */
    mmio_set_active_sec(sec);
    if (mmio_bus_read(&map->mmio, addr, size, value_out)) {
        return MM_TRUE;
    }
    if (addr >= 0xE000E000u && addr < 0xE0010000u) {
        *value_out = 0u;
        return MM_TRUE;
    }
    return MM_FALSE;
}

void mm_memmap_set_watch(mm_u32 addr, mm_u32 size)
{
    g_memwatch_enabled = MM_TRUE;
    g_memwatch_addr = addr;
    g_memwatch_size = (size == 0u) ? 1u : size;
}

void mm_memmap_clear_watch(void)
{
    g_memwatch_enabled = MM_FALSE;
    g_memwatch_addr = 0;
    g_memwatch_size = 0;
}

void mm_memmap_set_last_pc(mm_u32 pc)
{
    g_memwatch_pc = pc;
}

mm_bool mm_memmap_write(struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr, mm_u32 size, mm_u32 value)
{
    mm_u32 base;
    mm_u32 size_limit;
    mm_u32 offset;
    mm_u8 *buf;
    mm_u8 old_bytes[4];
    mm_u16 trace_flags = 0;

    if (map == 0) {
        return MM_FALSE;
    }
    if (!MM_INTERCEPT_OK(map, MM_ACCESS_WRITE, sec, addr, size)) {
        return MM_FALSE;
    }
    if (g_memwatch_enabled) {
        mm_u32 end = addr + size;
        mm_u32 w_end = g_memwatch_addr + g_memwatch_size;
        if (!(end <= g_memwatch_addr || addr >= w_end)) {
            printf("[MEMWATCH] pc=0x%08lx addr=0x%08lx size=%lu value=0x%08lx\n",
                   (unsigned long)g_memwatch_pc,
                   (unsigned long)addr,
                   (unsigned long)size,
                   (unsigned long)value);
        }
    }
    if (map->flash.buffer != 0 && map->flash_write != 0) {
        if (mm_trace_step_active() && size <= 4u &&
            memmap_read_old_bytes(map, sec, addr, size, old_bytes, &trace_flags)) {
            mm_trace_log_mem_write(addr, size, old_bytes, trace_flags);
        }
        base = map->flash_base_s;
        size_limit = map->flash_size_s;
        if (size_limit == 0u && map->flash.length > 0u) {
            base = map->flash.base;
            size_limit = (mm_u32)map->flash.length;
        }
        if (addr >= base && (addr - base) + size <= size_limit) {
            if (map->flash_write(map->flash_write_opaque, sec, addr, size, value)) {
                if (map->code_cache != 0) {
                    mm_code_cache_note_write(map->code_cache, addr, size);
                }
                return MM_TRUE;
            }
            return MM_FALSE;
        }
        base = map->flash_base_ns;
        size_limit = map->flash_size_ns;
        if (size_limit == 0u && map->flash.length > 0u) {
            base = map->flash.base;
            size_limit = (mm_u32)map->flash.length;
        }
        if (addr >= base && (addr - base) + size <= size_limit) {
            if (map->flash_write(map->flash_write_opaque, sec, addr, size, value)) {
                if (map->code_cache != 0) {
                    mm_code_cache_note_write(map->code_cache, addr, size);
                }
                return MM_TRUE;
            }
            return MM_FALSE;
        }
    }
    /* RAM only writable region for now */
    if (map->ram.buffer != 0) {
        if (ram_offset_for_addr(map, addr, size, &offset)) {
            if (mm_trace_step_active() && size <= 4u &&
                memmap_read_old_bytes(map, sec, addr, size, old_bytes, &trace_flags)) {
                mm_trace_log_mem_write(addr, size, old_bytes, trace_flags);
            }
            buf = (mm_u8 *)map->ram.buffer;
            if (size == 4u) {
                buf[offset] = (mm_u8)(value & 0xffu);
                buf[offset + 1u] = (mm_u8)((value >> 8) & 0xffu);
                buf[offset + 2u] = (mm_u8)((value >> 16) & 0xffu);
                buf[offset + 3u] = (mm_u8)((value >> 24) & 0xffu);
                if (map->code_cache != 0) {
                    mm_code_cache_note_write(map->code_cache, addr, size);
                }
                return MM_TRUE;
            } else if (size == 2u) {
                buf[offset] = (mm_u8)(value & 0xffu);
                buf[offset + 1u] = (mm_u8)((value >> 8) & 0xffu);
                if (map->code_cache != 0) {
                    mm_code_cache_note_write(map->code_cache, addr, size);
                }
                return MM_TRUE;
            } else if (size == 1u) {
                buf[offset] = (mm_u8)(value & 0xffu);
                if (map->code_cache != 0) {
                    mm_code_cache_note_write(map->code_cache, addr, size);
                }
                return MM_TRUE;
            }
        }
    }
    mmio_set_active_sec(sec);
    if (mm_trace_step_active() && size <= 4u &&
        memmap_read_old_bytes(map, sec, addr, size, old_bytes, &trace_flags)) {
        mm_trace_log_mem_write(addr, size, old_bytes, trace_flags);
    }
    if (mmio_bus_write(&map->mmio, addr, size, value)) {
        return MM_TRUE;
    }
    /* RAZ/WI fallback for unhandled System Control Space */
    if (addr >= 0xE000E000u && addr < 0xE0010000u) {
        return MM_TRUE;
    }
    return MM_FALSE;
}

mm_bool mm_memmap_fetch_read16(const struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr, mm_u32 *value_out)
{
    mm_u32 base;
    mm_u32 size_limit;
    mm_u32 offset;
    mm_u32 tmp;
    if (map == 0) {
        return MM_FALSE;
    }
    if (!MM_INTERCEPT_OK(map, MM_ACCESS_EXEC, sec, addr, 2u)) {
        return MM_FALSE;
    }
    /* Execute from flash only for now. */
    if (map->flash.buffer != 0) {
        (void)sec;

        base = map->flash_base_s;
        size_limit = map->flash_size_s;
        if (size_limit == 0u && map->flash.length > 0u) {
            base = map->flash.base;
            size_limit = (mm_u32)map->flash.length;
        }
        if (addr >= base && (addr - base) + 2u <= size_limit) {
            offset = addr - base;
            if (map->flash_ecc_check != 0 && !map->flash_ecc_check(map->flash_ecc_check_opaque, offset)) {
                return MM_FALSE;
            }
            if (read_buf_le(map->flash.buffer, offset, 2u, &tmp)) { *value_out = tmp; return MM_TRUE; }
            return MM_FALSE;
        }

        base = map->flash_base_ns;
        size_limit = map->flash_size_ns;
        if (size_limit == 0u && map->flash.length > 0u) {
            base = map->flash.base;
            size_limit = (mm_u32)map->flash.length;
        }
        if (addr >= base && (addr - base) + 2u <= size_limit) {
            offset = addr - base;
            if (map->flash_ecc_check != 0 && !map->flash_ecc_check(map->flash_ecc_check_opaque, offset)) {
                return MM_FALSE;
            }
            if (read_buf_le(map->flash.buffer, offset, 2u, &tmp)) { *value_out = tmp; return MM_TRUE; }
            return MM_FALSE;
        }
    }
    /* Execute from RAM if mapped. */
    if (map->ram.buffer != 0) {
        if (ram_offset_for_addr(map, addr, 2u, &offset)) {
            if (read_buf_le(map->ram.buffer, offset, 2u, &tmp)) { *value_out = tmp; return MM_TRUE; }
            return MM_FALSE;
        }
    }
    return MM_FALSE;
}

mm_bool mm_memmap_read8(const struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr, mm_u8 *value_out)
{
    mm_u32 base;
    mm_u32 size_limit;
    if (map == 0 || value_out == 0) {
        return MM_FALSE;
    }
    if (!MM_INTERCEPT_OK(map, MM_ACCESS_READ, sec, addr, 1u)) {
        return MM_FALSE;
    }
    if (map->flash.buffer != 0) {
        base = map->flash_base_s;
        size_limit = map->flash_size_s;
        if (size_limit == 0u && map->flash.length > 0u) {
            base = map->flash.base;
            size_limit = (mm_u32)map->flash.length;
        }
        if (addr >= base && (addr - base) < size_limit) {
            mm_u32 flash_off = addr - base;
            if (map->flash_ecc_check != 0 && !map->flash_ecc_check(map->flash_ecc_check_opaque, flash_off)) {
                return MM_FALSE;
            }
            *value_out = map->flash.buffer[flash_off];
            return MM_TRUE;
        }
        base = map->flash_base_ns;
        size_limit = map->flash_size_ns;
        if (size_limit == 0u && map->flash.length > 0u) {
            base = map->flash.base;
            size_limit = (mm_u32)map->flash.length;
        }
        if (addr >= base && (addr - base) < size_limit) {
            mm_u32 flash_off = addr - base;
            /* See mm_memmap_read: NS-alias reads of secure sectors are RAZ. */
            if (map->flash_sector_secure != 0 &&
                    map->flash_sector_secure(map->flash_sector_secure_opaque, flash_off)) {
                *value_out = 0u;
                return MM_TRUE;
            }
            if (map->flash_ecc_check != 0 && !map->flash_ecc_check(map->flash_ecc_check_opaque, flash_off)) {
                return MM_FALSE;
            }
            *value_out = map->flash.buffer[flash_off];
            return MM_TRUE;
        }
    }
    if (map->ram.buffer != 0) {
        mm_u32 offset = 0;
        if (ram_offset_for_addr(map, addr, 1u, &offset)) {
            *value_out = map->ram.buffer[offset];
            return MM_TRUE;
        }
    }
    {
        mm_u32 tmp;
        if (mmio_bus_read(&map->mmio, addr, 1u, &tmp)) {
            *value_out = (mm_u8)(tmp & 0xffu);
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

mm_bool mm_memmap_write8(struct mm_memmap *map, enum mm_sec_state sec, mm_u32 addr, mm_u8 value)
{
    mm_u8 *buf;
    mm_u8 old_bytes[1];
    mm_u16 trace_flags = 0;

    if (map == 0) {
        return MM_FALSE;
    }
    if (!MM_INTERCEPT_OK(map, MM_ACCESS_WRITE, sec, addr, 1u)) {
        return MM_FALSE;
    }
    if (map->ram.buffer != 0) {
        mm_u32 offset = 0;
        if (ram_offset_for_addr(map, addr, 1u, &offset)) {
            if (mm_trace_step_active() &&
                memmap_read_old_bytes(map, sec, addr, 1u, old_bytes, &trace_flags)) {
                mm_trace_log_mem_write(addr, 1u, old_bytes, trace_flags);
            }
            buf = (mm_u8 *)map->ram.buffer;
            buf[offset] = value;
            if (map->code_cache != 0) {
                mm_code_cache_note_write(map->code_cache, addr, 1u);
            }
            return MM_TRUE;
        }
    }
    if (mm_trace_step_active() &&
        memmap_read_old_bytes(map, sec, addr, 1u, old_bytes, &trace_flags)) {
        mm_trace_log_mem_write(addr, 1u, old_bytes, trace_flags);
    }
    if (mmio_bus_write(&map->mmio, addr, 1u, (mm_u32)value)) {
        return MM_TRUE;
    }
    return MM_FALSE;
}

mm_bool mm_memmap_write_ram_raw(struct mm_memmap *map, mm_u32 addr, mm_u32 size, mm_u32 value)
{
    mm_u32 offset;
    mm_u8 *buf;

    if (map == 0 || map->ram.buffer == 0) {
        return MM_FALSE;
    }
    if (!ram_offset_for_addr(map, addr, size, &offset)) {
        return MM_FALSE;
    }
    buf = (mm_u8 *)map->ram.buffer;
    if (size == 4u) {
        buf[offset] = (mm_u8)(value & 0xffu);
        buf[offset + 1u] = (mm_u8)((value >> 8) & 0xffu);
        buf[offset + 2u] = (mm_u8)((value >> 16) & 0xffu);
        buf[offset + 3u] = (mm_u8)((value >> 24) & 0xffu);
    } else if (size == 2u) {
        buf[offset] = (mm_u8)(value & 0xffu);
        buf[offset + 1u] = (mm_u8)((value >> 8) & 0xffu);
    } else if (size == 1u) {
        buf[offset] = (mm_u8)(value & 0xffu);
    } else {
        return MM_FALSE;
    }
    if (map->code_cache != 0) {
        mm_code_cache_note_write(map->code_cache, addr, size);
    }
    return MM_TRUE;
}
