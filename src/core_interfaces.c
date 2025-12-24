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

static enum mm_sec_state g_mmio_active_sec = MM_SECURE;

void mmio_set_active_sec(enum mm_sec_state sec)
{
    g_mmio_active_sec = sec;
}

enum mm_sec_state mmio_active_sec(void)
{
    return g_mmio_active_sec;
}

void mmio_bus_init(struct mmio_bus *bus, struct mmio_region *region_storage, size_t capacity)
{
    bus->regions = region_storage;
    bus->region_count = 0;
    bus->region_capacity = capacity;
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

    if (bus->region_count >= bus->region_capacity) {
        return MM_FALSE;
    }

    for (i = 0; i < bus->region_count; ++i) {
        const struct mmio_region *existing = &bus->regions[i];
        if (mmio_regions_overlap(existing->base, existing->size, region->base, region->size)) {
            return MM_FALSE;
        }
    }

    bus->regions[bus->region_count] = *region;
    bus->region_count += 1;
    return MM_TRUE;
}

static const struct mmio_region *mmio_bus_find(const struct mmio_bus *bus, mm_u32 addr)
{
    size_t i;
    for (i = 0; i < bus->region_count; ++i) {
        const struct mmio_region *region = &bus->regions[i];
        mm_u32 offset = addr - region->base;
        if (addr >= region->base && offset < region->size) {
            return region;
        }
    }
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
