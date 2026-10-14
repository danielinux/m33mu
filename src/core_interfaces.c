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

#include "m33mu/mmio.h"
#include "m33mu/irq.h"
#include "m33mu/scheduler.h"
#include "m33mu/chario.h"
#include "m33mu/gpio.h"
#include "m33mu/dma.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static enum mm_sec_state g_mmio_active_sec = MM_SECURE;
static mm_bool g_mmio_peek_mode = MM_FALSE;

#define MMIO_CACHE_PAGE_SHIFT 12u
#define MMIO_CACHE_PAGE_MASK  0xffu

struct mmio_lookup_cache_entry {
    const struct mmio_bus *bus;
    mm_u32 page_tag;
    size_t region_index;
    mm_bool valid;
};

static struct mmio_lookup_cache_entry g_mmio_lookup_cache[256];

static void mmio_cache_invalidate_bus(const struct mmio_bus *bus)
{
    size_t i;
    if (bus == 0) {
        return;
    }
    for (i = 0; i < (sizeof(g_mmio_lookup_cache) / sizeof(g_mmio_lookup_cache[0])); ++i) {
        if (g_mmio_lookup_cache[i].valid && g_mmio_lookup_cache[i].bus == bus) {
            g_mmio_lookup_cache[i].valid = MM_FALSE;
        }
    }
}

static size_t mmio_cache_slot_for_addr(mm_u32 addr)
{
    return (size_t)((addr >> MMIO_CACHE_PAGE_SHIFT) & MMIO_CACHE_PAGE_MASK);
}

static void mmio_cache_remember(const struct mmio_bus *bus, mm_u32 addr, size_t region_index)
{
    size_t slot;
    if (bus == 0) {
        return;
    }
    slot = mmio_cache_slot_for_addr(addr);
    g_mmio_lookup_cache[slot].bus = bus;
    g_mmio_lookup_cache[slot].page_tag = addr >> MMIO_CACHE_PAGE_SHIFT;
    g_mmio_lookup_cache[slot].region_index = region_index;
    g_mmio_lookup_cache[slot].valid = MM_TRUE;
}

static const struct mmio_region *mmio_cache_lookup(const struct mmio_bus *bus, mm_u32 addr, size_t *region_index_out)
{
    struct mmio_lookup_cache_entry *entry;
    const struct mmio_region *region;
    mm_u32 offset;
    size_t slot;

    if (bus == 0 || region_index_out == 0) {
        return 0;
    }
    slot = mmio_cache_slot_for_addr(addr);
    entry = &g_mmio_lookup_cache[slot];
    if (!entry->valid ||
        entry->bus != bus ||
        entry->page_tag != (addr >> MMIO_CACHE_PAGE_SHIFT) ||
        entry->region_index >= bus->region_count) {
        return 0;
    }
    region = &bus->regions[entry->region_index];
    offset = addr - region->base;
    if (addr >= region->base && offset < region->size) {
        *region_index_out = entry->region_index;
        return region;
    }
    entry->valid = MM_FALSE;
    return 0;
}

void mmio_set_active_sec(enum mm_sec_state sec)
{
    g_mmio_active_sec = sec;
}

enum mm_sec_state mmio_active_sec(void)
{
    return g_mmio_active_sec;
}

void mmio_set_peek_mode(mm_bool enabled)
{
    g_mmio_peek_mode = enabled ? MM_TRUE : MM_FALSE;
}

mm_bool mmio_peek_mode(void)
{
    return g_mmio_peek_mode;
}

void mmio_bus_init(struct mmio_bus *bus, struct mmio_region *region_storage, size_t capacity)
{
    bus->regions = region_storage;
    bus->region_count = 0;
    bus->region_capacity = capacity;
    bus->has_step_hooks = MM_FALSE;
    bus->last_hit_index = 0u;
    bus->last_hit_valid = MM_FALSE;
    mmio_cache_invalidate_bus(bus);
    if (region_storage != 0 && capacity > 0u) {
        memset(region_storage, 0, capacity * sizeof(*region_storage));
    }
}

void mmio_bus_enable_step_hooks(struct mmio_bus *bus)
{
    if (bus == 0) {
        return;
    }
    bus->has_step_hooks = MM_TRUE;
}

static mm_bool mmio_regions_overlap(mm_u32 abase, mm_u32 asize, mm_u32 bbase, mm_u32 bsize)
{
    mm_u32 aend;
    mm_u32 bend;
    aend = abase + asize;
    bend = bbase + bsize;
    return (abase < bend) && (bbase < aend);
}

mm_bool mmio_bus_register_region(struct mmio_bus *bus, const struct mmio_region *region)
{
    size_t i;
    struct mmio_region clean;

    if (bus->region_count >= bus->region_capacity) {
        return MM_FALSE;
    }

    for (i = 0; i < bus->region_count; ++i) {
        const struct mmio_region *existing = &bus->regions[i];
        if (mmio_regions_overlap(existing->base, existing->size, region->base, region->size)) {
            return MM_FALSE;
        }
    }

    clean = *region;
    if ((clean.flags & MMIO_REGION_F_EXT) == 0u || clean.magic != MMIO_REGION_MAGIC) {
        clean.magic = MMIO_REGION_MAGIC;
        clean.flags = 0;
        clean.name = 0;
        clean.version = 0;
        clean.peek = 0;
        clean.save = 0;
        clean.load = 0;
        clean.begin_step = 0;
        clean.end_step = 0;
    }
    bus->regions[bus->region_count] = clean;
    bus->region_count += 1;
    bus->last_hit_valid = MM_FALSE;
    mmio_cache_invalidate_bus(bus);
    return MM_TRUE;
}

static const struct mmio_region *mmio_bus_find(const struct mmio_bus *bus, mm_u32 addr)
{
    struct mmio_bus *mutable_bus;
    size_t i;
    size_t hit_index;

    if (bus == 0 || bus->regions == 0) {
        return 0;
    }

    mutable_bus = (struct mmio_bus *)bus;
    if (mutable_bus->last_hit_valid &&
        mutable_bus->last_hit_index < mutable_bus->region_count) {
        const struct mmio_region *last = &mutable_bus->regions[mutable_bus->last_hit_index];
        mm_u32 last_offset = addr - last->base;
        if (addr >= last->base && last_offset < last->size) {
            mmio_cache_remember(bus, addr, mutable_bus->last_hit_index);
            return last;
        }
    }

    if (mmio_cache_lookup(bus, addr, &hit_index) != 0) {
        mutable_bus->last_hit_index = hit_index;
        mutable_bus->last_hit_valid = MM_TRUE;
        return &mutable_bus->regions[hit_index];
    }

    for (i = 0; i < bus->region_count; ++i) {
        const struct mmio_region *region = &bus->regions[i];
        mm_u32 offset = addr - region->base;
        if (addr >= region->base && offset < region->size) {
            mutable_bus->last_hit_index = i;
            mutable_bus->last_hit_valid = MM_TRUE;
            mmio_cache_remember(bus, addr, i);
            return region;
        }
    }
    mutable_bus->last_hit_valid = MM_FALSE;
    return 0;
}

mm_bool mmio_bus_read(const struct mmio_bus *bus, mm_u32 addr, mm_u32 size_bytes, mm_u32 *value_out)
{
    const struct mmio_region *region;
    mm_u32 offset;

    region = mmio_bus_find(bus, addr);
    if (region == 0 || region->read == 0) {
        return MM_FALSE;
    }

    offset = addr - region->base;
    return region->read(region->opaque, offset, size_bytes, value_out);
}

mm_bool mmio_bus_write(const struct mmio_bus *bus, mm_u32 addr, mm_u32 size_bytes, mm_u32 value)
{
    const struct mmio_region *region;
    mm_u32 offset;

    region = mmio_bus_find(bus, addr);
    if (region == 0 || region->write == 0) {
        return MM_FALSE;
    }

    offset = addr - region->base;
    return region->write(region->opaque, offset, size_bytes, value);
}

mmio_peek_result_t mmio_bus_peek(const struct mmio_bus *bus, mm_u32 addr, mm_u32 size_bytes, void *dst)
{
    const struct mmio_region *region;
    mm_u32 offset;

    if (bus == 0 || bus->regions == 0 || bus->region_capacity == 0 ||
        bus->region_count > bus->region_capacity || !bus->has_step_hooks) {
        return MMIO_PEEK_UNSUPPORTED;
    }
    region = mmio_bus_find(bus, addr);
    if (region == 0) {
        return MMIO_PEEK_UNSUPPORTED;
    }
    offset = addr - region->base;
    if (region->magic == MMIO_REGION_MAGIC &&
        (region->flags & MMIO_REGION_F_EXT) != 0u &&
        region->peek != 0) {
        return region->peek(region->opaque, offset, size_bytes, dst);
    }
    if (region->read != 0 && dst != 0 && (size_bytes == 1u || size_bytes == 2u || size_bytes == 4u)) {
        mm_u32 val = 0;
        mm_bool prev = g_mmio_peek_mode;
        mmio_set_peek_mode(MM_TRUE);
        if (!region->read(region->opaque, offset, size_bytes, &val)) {
            mmio_set_peek_mode(prev);
            return MMIO_PEEK_UNSUPPORTED;
        }
        mmio_set_peek_mode(prev);
        if (size_bytes == 1u) {
            ((mm_u8 *)dst)[0] = (mm_u8)(val & 0xffu);
        } else if (size_bytes == 2u) {
            ((mm_u8 *)dst)[0] = (mm_u8)(val & 0xffu);
            ((mm_u8 *)dst)[1] = (mm_u8)((val >> 8) & 0xffu);
        } else {
            ((mm_u8 *)dst)[0] = (mm_u8)(val & 0xffu);
            ((mm_u8 *)dst)[1] = (mm_u8)((val >> 8) & 0xffu);
            ((mm_u8 *)dst)[2] = (mm_u8)((val >> 16) & 0xffu);
            ((mm_u8 *)dst)[3] = (mm_u8)((val >> 24) & 0xffu);
        }
        return MMIO_PEEK_OK;
    }
    return MMIO_PEEK_UNSUPPORTED;
}

mm_bool mmio_bus_save(const struct mmio_bus *bus, struct mm_snapshot_writer *w)
{
    size_t i;
    mm_u32 count = 0;
    if (bus == 0 || w == 0) {
        return MM_FALSE;
    }
    for (i = 0; i < bus->region_count; ++i) {
        const struct mmio_region *region = &bus->regions[i];
        if (region->magic == MMIO_REGION_MAGIC &&
            (region->flags & MMIO_REGION_F_EXT) != 0u &&
            region->save != 0 && region->name != 0) {
            count++;
        }
    }
    if (!mm_snapshot_write_u32(w, count)) {
        return MM_FALSE;
    }
    for (i = 0; i < bus->region_count; ++i) {
        const struct mmio_region *region = &bus->regions[i];
        mm_u32 name_len;
        mm_u32 size_offset;
        if (region->magic != MMIO_REGION_MAGIC ||
            (region->flags & MMIO_REGION_F_EXT) == 0u ||
            region->save == 0 || region->name == 0) {
            continue;
        }
        name_len = (mm_u32)strlen(region->name);
        if (!mm_snapshot_write_u32(w, name_len)) {
            return MM_FALSE;
        }
        if (name_len > 0u && !mm_snapshot_write(w, region->name, name_len)) {
            return MM_FALSE;
        }
        if (!mm_snapshot_write_u32(w, region->version)) {
            return MM_FALSE;
        }
        if (!mm_snapshot_writer_begin_section(w, &size_offset)) {
            return MM_FALSE;
        }
        if (!region->save(region->opaque, w)) {
            return MM_FALSE;
        }
        if (!mm_snapshot_writer_end_section(w, size_offset)) {
            return MM_FALSE;
        }
    }
    return MM_TRUE;
}

mm_bool mmio_bus_load(const struct mmio_bus *bus, struct mm_snapshot_reader *r)
{
    mm_u32 count = 0;
    mm_u32 i;
    if (bus == 0 || r == 0) {
        return MM_FALSE;
    }
    if (!mm_snapshot_read_u32(r, &count)) {
        return MM_FALSE;
    }
    for (i = 0; i < count; ++i) {
        mm_u32 name_len = 0;
        mm_u32 version = 0;
        char *name = 0;
        struct mm_snapshot_reader section;
        if (!mm_snapshot_read_u32(r, &name_len)) {
            return MM_FALSE;
        }
        if (name_len > 0u) {
            name = (char *)malloc(name_len + 1u);
            if (name == 0) {
                return MM_FALSE;
            }
            if (!mm_snapshot_read(r, name, name_len)) {
                free(name);
                return MM_FALSE;
            }
            name[name_len] = '\0';
        }
        if (!mm_snapshot_read_u32(r, &version)) {
            free(name);
            return MM_FALSE;
        }
        if (!mm_snapshot_reader_begin_section(r, &section)) {
            free(name);
            return MM_FALSE;
        }
        {
            size_t j;
            mm_bool loaded = MM_FALSE;
            for (j = 0; j < bus->region_count; ++j) {
                const struct mmio_region *region = &bus->regions[j];
                if (region->magic != MMIO_REGION_MAGIC ||
                    (region->flags & MMIO_REGION_F_EXT) == 0u ||
                    region->load == 0 || region->name == 0) {
                    continue;
                }
                if (strcmp(region->name, name ? name : "") == 0 && region->version == version) {
                    if (!region->load(region->opaque, &section)) {
                        free(name);
                        return MM_FALSE;
                    }
                    loaded = MM_TRUE;
                    break;
                }
            }
            (void)loaded;
        }
        free(name);
    }
    return MM_TRUE;
}

void mmio_bus_begin_step(const struct mmio_bus *bus)
{
    size_t i;
    static mm_bool warned = MM_FALSE;
    const size_t max_regions = 4096u;
    if (bus == 0 || bus->regions == 0 || !bus->has_step_hooks) {
        if (!warned) {
            fprintf(stderr, "[MMIO] begin_step skipped (bus or regions null)\n");
            warned = MM_TRUE;
        }
        return;
    }
    if (bus->region_capacity == 0 || bus->region_capacity > max_regions ||
        bus->region_count > bus->region_capacity) {
        if (!warned) {
            fprintf(stderr, "[MMIO] begin_step skipped (count=%lu cap=%lu regions=%p)\n",
                    (unsigned long)bus->region_count,
                    (unsigned long)bus->region_capacity,
                    (void *)bus->regions);
            warned = MM_TRUE;
        }
        return;
    }
    for (i = 0; i < bus->region_count; ++i) {
        const struct mmio_region *region = &bus->regions[i];
        if (region->magic == MMIO_REGION_MAGIC &&
            (region->flags & MMIO_REGION_F_EXT) != 0u &&
            region->begin_step != 0) {
            region->begin_step(region->opaque);
        }
    }
}

void mmio_bus_end_step(const struct mmio_bus *bus, const struct mm_undo_sink *sink)
{
    size_t i;
    static mm_bool warned = MM_FALSE;
    const size_t max_regions = 4096u;
    if (bus == 0 || sink == 0 || bus->regions == 0 || !bus->has_step_hooks) {
        if (!warned) {
            fprintf(stderr, "[MMIO] end_step skipped (bus/sink/regions null)\n");
            warned = MM_TRUE;
        }
        return;
    }
    if (bus->region_capacity == 0 || bus->region_capacity > max_regions ||
        bus->region_count > bus->region_capacity) {
        if (!warned) {
            fprintf(stderr, "[MMIO] end_step skipped (count=%lu cap=%lu regions=%p)\n",
                    (unsigned long)bus->region_count,
                    (unsigned long)bus->region_capacity,
                    (void *)bus->regions);
            warned = MM_TRUE;
        }
        return;
    }
    for (i = 0; i < bus->region_count; ++i) {
        const struct mmio_region *region = &bus->regions[i];
        if (region->magic == MMIO_REGION_MAGIC &&
            (region->flags & MMIO_REGION_F_EXT) != 0u &&
            region->end_step != 0) {
            region->end_step(region->opaque, sink);
        }
    }
}

void mm_irq_line_init(struct mm_irq_line *line, mm_irq_sink_fn sink, void *opaque)
{
    line->sink = sink;
    line->opaque = opaque;
    line->level = MM_FALSE;
}

static void mm_irq_apply(struct mm_irq_line *line, mm_bool level)
{
    if (line->level != level) {
        line->level = level;
        if (line->sink != 0) {
            line->sink(line->opaque, level);
        }
    }
}

void mm_irq_line_raise(struct mm_irq_line *line)
{
    mm_irq_apply(line, MM_TRUE);
}

void mm_irq_line_lower(struct mm_irq_line *line)
{
    mm_irq_apply(line, MM_FALSE);
}

mm_bool mm_irq_line_level(const struct mm_irq_line *line)
{
    return line->level;
}

void mm_scheduler_init(struct mm_scheduler *sched)
{
    sched->head = 0;
}

mm_bool mm_scheduler_schedule(struct mm_scheduler *sched, struct mm_sched_event *ev)
{
    struct mm_sched_event **link;
    struct mm_sched_event *cur;

    if (ev == 0 || ev->cb == 0) {
        return MM_FALSE;
    }

    ev->next = 0;
    link = &sched->head;
    cur = sched->head;

    while (cur != 0 && cur->due_cycle <= ev->due_cycle) {
        link = &cur->next;
        cur = cur->next;
    }

    ev->next = cur;
    *link = ev;
    return MM_TRUE;
}

mm_u64 mm_scheduler_next_due(const struct mm_scheduler *sched)
{
    if (sched->head == 0) {
        return (mm_u64)-1;
    }
    return sched->head->due_cycle;
}

void mm_scheduler_run_due(struct mm_scheduler *sched, mm_u64 now_cycles)
{
    while (sched->head != 0 && sched->head->due_cycle <= now_cycles) {
        struct mm_sched_event *ev;
        ev = sched->head;
        sched->head = ev->next;
        ev->next = 0;
        ev->cb(ev->opaque, now_cycles);
    }
}

void mm_char_backend_init(struct mm_char_backend *backend, mm_char_write_fn write_fn, mm_char_flush_fn flush_fn, void *opaque)
{
    backend->write = write_fn;
    backend->flush = flush_fn;
    backend->opaque = opaque;
}

mm_bool mm_char_putc(struct mm_char_backend *backend, mm_u8 byte)
{
    if (backend->write == 0) {
        return MM_FALSE;
    }
    return backend->write(backend->opaque, byte);
}

void mm_char_flush(struct mm_char_backend *backend)
{
    if (backend->flush != 0) {
        backend->flush(backend->opaque);
    }
}

void mm_gpio_line_init(struct mm_gpio_line *line, mm_gpio_listener_fn listener, void *opaque)
{
    line->listener = listener;
    line->opaque = opaque;
    line->level = 0;
}

void mm_gpio_set_level(struct mm_gpio_line *line, mm_u8 level)
{
    if (line->level != level) {
        line->level = level;
        if (line->listener != 0) {
            line->listener(line->opaque, level);
        }
    }
}

mm_u8 mm_gpio_get_level(const struct mm_gpio_line *line)
{
    return line->level;
}

static mm_gpio_bank_read_fn g_gpio_bank_reader = 0;
static void *g_gpio_bank_reader_opaque = 0;
static mm_gpio_bank_read_moder_fn g_gpio_bank_moder_reader = 0;
static void *g_gpio_bank_moder_opaque = 0;
static mm_gpio_bank_clock_fn g_gpio_bank_clock_reader = 0;
static void *g_gpio_bank_clock_opaque = 0;
static mm_gpio_bank_read_seccfgr_fn g_gpio_bank_seccfgr_reader = 0;
static void *g_gpio_bank_seccfgr_opaque = 0;
static mm_rcc_clock_list_fn g_rcc_clock_list_reader = 0;
static void *g_rcc_clock_list_opaque = 0;
static mm_gpio_bank_info_fn g_gpio_bank_info_reader = 0;
static void *g_gpio_bank_info_opaque = 0;

void mm_gpio_bank_set_reader(mm_gpio_bank_read_fn reader, void *opaque)
{
    g_gpio_bank_reader = reader;
    g_gpio_bank_reader_opaque = opaque;
}

void mm_gpio_bank_set_moder_reader(mm_gpio_bank_read_moder_fn reader, void *opaque)
{
    g_gpio_bank_moder_reader = reader;
    g_gpio_bank_moder_opaque = opaque;
}

void mm_gpio_bank_set_clock_reader(mm_gpio_bank_clock_fn reader, void *opaque)
{
    g_gpio_bank_clock_reader = reader;
    g_gpio_bank_clock_opaque = opaque;
}

void mm_gpio_bank_set_seccfgr_reader(mm_gpio_bank_read_seccfgr_fn reader, void *opaque)
{
    g_gpio_bank_seccfgr_reader = reader;
    g_gpio_bank_seccfgr_opaque = opaque;
}

void mm_rcc_set_clock_list_reader(mm_rcc_clock_list_fn reader, void *opaque)
{
    g_rcc_clock_list_reader = reader;
    g_rcc_clock_list_opaque = opaque;
}

void mm_gpio_set_bank_info_reader(mm_gpio_bank_info_fn reader, void *opaque)
{
    g_gpio_bank_info_reader = reader;
    g_gpio_bank_info_opaque = opaque;
}

mm_u32 mm_gpio_bank_read(int bank)
{
    if (g_gpio_bank_reader == 0) {
        return 0u;
    }
    return g_gpio_bank_reader(g_gpio_bank_reader_opaque, bank);
}

mm_u32 mm_gpio_bank_read_moder(int bank)
{
    if (g_gpio_bank_moder_reader == 0) {
        return 0u;
    }
    return g_gpio_bank_moder_reader(g_gpio_bank_moder_opaque, bank);
}

mm_bool mm_gpio_bank_clock_enabled(int bank)
{
    if (g_gpio_bank_clock_reader == 0) {
        return MM_TRUE;
    }
    return g_gpio_bank_clock_reader(g_gpio_bank_clock_opaque, bank);
}

mm_u32 mm_gpio_bank_read_seccfgr(int bank)
{
    if (g_gpio_bank_seccfgr_reader == 0) {
        return 0u;
    }
    return g_gpio_bank_seccfgr_reader(g_gpio_bank_seccfgr_opaque, bank);
}

mm_bool mm_gpio_bank_reader_present(void)
{
    return (g_gpio_bank_reader != 0) ? MM_TRUE : MM_FALSE;
}

static mm_gpio_set_external_input_fn g_gpio_external_input_writer = 0;
static void *g_gpio_external_input_opaque = 0;

void mm_gpio_set_external_input_writer(mm_gpio_set_external_input_fn writer, void *opaque)
{
    g_gpio_external_input_writer = writer;
    g_gpio_external_input_opaque = opaque;
}

mm_bool mm_gpio_set_external_input(int bank, int pin, mm_bool level)
{
    if (g_gpio_external_input_writer == 0) {
        return MM_FALSE;
    }
    return g_gpio_external_input_writer(g_gpio_external_input_opaque, bank, pin, level);
}

mm_bool mm_gpio_external_input_writer_present(void)
{
    return (g_gpio_external_input_writer != 0) ? MM_TRUE : MM_FALSE;
}

mm_bool mm_rcc_clock_list_present(void)
{
    return (g_rcc_clock_list_reader != 0) ? MM_TRUE : MM_FALSE;
}

mm_bool mm_rcc_clock_list_line(int line, char *out, size_t out_len)
{
    if (g_rcc_clock_list_reader == 0 || out == 0 || out_len == 0u) {
        return MM_FALSE;
    }
    return g_rcc_clock_list_reader(g_rcc_clock_list_opaque, line, out, out_len);
}

mm_bool mm_gpio_bank_info(int bank, char *name_out, size_t name_len, int *pins_out)
{
    if (g_gpio_bank_info_reader != 0) {
        return g_gpio_bank_info_reader(g_gpio_bank_info_opaque, bank, name_out, name_len, pins_out);
    }
    if (bank < 0 || bank >= 9) {
        return MM_FALSE;
    }
    if (name_out != 0 && name_len > 0u) {
        name_out[0] = (char)('A' + bank);
        if (name_len > 1u) {
            name_out[1] = '\0';
        }
    }
    if (pins_out != 0) {
        *pins_out = 16;
    }
    return MM_TRUE;
}

void mm_dma_master_init(struct mm_dma_master *dma, mm_dma_request_fn request_fn, void *opaque)
{
    dma->request = request_fn;
    dma->opaque = opaque;
}

mm_bool mm_dma_transfer(struct mm_dma_master *dma, mm_u32 addr, void *buffer, size_t length_bytes, mm_bool write_direction)
{
    if (dma->request == 0) {
        return MM_FALSE;
    }
    return dma->request(dma->opaque, addr, buffer, length_bytes, write_direction);
}
