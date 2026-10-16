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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m33mu/flash_ops.h"
#include "m33mu/optbytes.h"

/* A program writes a word or a quad-word; a ROM API program can write a
 * page, but never leaves it all 0xFF in a range this long while the array
 * held something else. Shorter all-0xFF ranges are programs of 0xFF. */
#define MIN_ERASE_LEN 32u

static struct {
    mm_bool enabled;
    mm_u8 *flash;
    mm_u8 *shadow;
    mm_u32 size;
    mm_u32 base;
    mm_u64 count;
    mm_u64 cut_index;
    mm_bool cut_on_erase;
    mm_u32 cut_erase_off;
    mm_bool cut_fired;
    struct mm_flash_cut cut;
    FILE *trace;
    const struct mm_flash_timing *timing;
    enum mm_flash_timing_level level;
    mm_u64 stall_us;
    mm_u64 latch_count[2];
    mm_u32 latch_first[2];
} ops = { MM_FALSE, 0, 0, 0u, 0u, 0u, MM_FLASH_OPS_NEVER, MM_FALSE, 0u, MM_FALSE,
          { 0u, MM_FLASH_OP_PROGRAM, 0u, 0u }, 0, 0, MM_FLASH_TIMING_NONE, 0u,
          { 0u, 0u }, { 0u, 0u } };

mm_bool mm_flash_ops_enable(mm_u8 *flash, mm_u32 size, mm_u32 base)
{
    mm_u8 *shadow;
    if (flash == 0 || size == 0u) return MM_FALSE;
    shadow = (mm_u8 *)malloc(size);
    if (shadow == 0) return MM_FALSE;
    mm_flash_ops_disable();
    memcpy(shadow, flash, size);
    ops.flash = flash;
    ops.shadow = shadow;
    ops.size = size;
    ops.base = base;
    ops.count = 0u;
    ops.enabled = MM_TRUE;
    mm_flash_ops_clear_cut();
    mm_flash_ops_clear_latches();
    return MM_TRUE;
}

void mm_flash_ops_disable(void)
{
    free(ops.shadow);
    ops.shadow = 0;
    ops.flash = 0;
    ops.size = 0u;
    ops.enabled = MM_FALSE;
}

mm_bool mm_flash_ops_enabled(void)
{
    return ops.enabled;
}

void mm_flash_ops_resync(void)
{
    if (!ops.enabled) return;
    memcpy(ops.shadow, ops.flash, ops.size);
}

mm_u32 mm_flash_ops_base(void)
{
    return ops.base;
}

mm_u32 mm_flash_ops_size(void)
{
    return ops.size;
}

mm_bool mm_flash_ops_load_file(const char *path)
{
    FILE *f;
    mm_u8 *buf;
    size_t n;
    int extra;
    if (!ops.enabled || path == 0) return MM_FALSE;
    f = fopen(path, "rb");
    if (f == 0) return MM_FALSE;
    buf = (mm_u8 *)malloc(ops.size);
    if (buf == 0) {
        fclose(f);
        return MM_FALSE;
    }
    memset(buf, 0xFF, ops.size);
    n = fread(buf, 1, ops.size, f);
    extra = fgetc(f);
    fclose(f);
    if (extra != EOF) {
        /* larger than the array: refuse rather than load part of it */
        free(buf);
        return MM_FALSE;
    }
    (void)n;
    memcpy(ops.flash, buf, ops.size);
    memcpy(ops.shadow, buf, ops.size);
    free(buf);
    return MM_TRUE;
}

mm_bool mm_flash_ops_save_file(const char *path)
{
    FILE *f;
    size_t n;
    if (!ops.enabled || path == 0) return MM_FALSE;
    f = fopen(path, "wb");
    if (f == 0) return MM_FALSE;
    n = fwrite(ops.flash, 1, ops.size, f);
    if (fclose(f) != 0 || n != ops.size) return MM_FALSE;
    return MM_TRUE;
}

static mm_bool all_erased(const mm_u8 *p, mm_u32 len)
{
    mm_u32 i;
    for (i = 0; i < len; ++i) {
        if (p[i] != 0xFFu) return MM_FALSE;
    }
    return MM_TRUE;
}

static enum mm_flash_op_kind classify(mm_u32 offset, mm_u32 len)
{
    mm_bool erased = all_erased(ops.flash + offset, len);
    if (offset == 0u && len == ops.size) {
        return erased ? MM_FLASH_OP_MASS_ERASE : MM_FLASH_OP_BULK;
    }
    if (erased && len >= MIN_ERASE_LEN) {
        return MM_FLASH_OP_ERASE;
    }
    return MM_FLASH_OP_PROGRAM;
}

static mm_u64 op_us(enum mm_flash_op_kind kind, mm_u32 len)
{
    const struct mm_flash_timing *t = ops.timing;
    int i;
    if (t == 0 || ops.level == MM_FLASH_TIMING_NONE) return 0u;
    i = (ops.level == MM_FLASH_TIMING_MAX) ? 1 : 0;
    switch (kind) {
    case MM_FLASH_OP_PROGRAM:
        if (t->program_unit == 0u) return 0u;
        return (mm_u64)((len + t->program_unit - 1u) / t->program_unit) * t->program_us[i];
    case MM_FLASH_OP_ERASE:
    case MM_FLASH_OP_MASS_ERASE:
        if (t->erase_unit == 0u) return 0u;
        return (mm_u64)((len + t->erase_unit - 1u) / t->erase_unit) * t->erase_us[i];
    case MM_FLASH_OP_BULK:
    case MM_FLASH_OP_OPTION:            /* no datasheet figure: taken as instant */
        break;
    }
    return 0u;
}

static void trace_op(enum mm_flash_op_kind kind, mm_u32 offset, mm_u32 len, mm_u64 us)
{
    mm_u32 addr = ops.base + offset;
    mm_u32 i;
    if (ops.trace == 0) return;
    switch (kind) {
    case MM_FLASH_OP_PROGRAM:
        fprintf(ops.trace, "P 0x%08lX 0x", (unsigned long)addr);
        for (i = len; i > 0u; --i) {
            fprintf(ops.trace, "%02X", ops.flash[offset + i - 1u]);
        }
        break;
    case MM_FLASH_OP_ERASE:
        fprintf(ops.trace, "E 0x%08lX #%lX", (unsigned long)addr, (unsigned long)len);
        break;
    case MM_FLASH_OP_MASS_ERASE:
        fprintf(ops.trace, "M 0x%08lX", (unsigned long)addr);
        break;
    case MM_FLASH_OP_BULK:
        fprintf(ops.trace, "B 0x%08lX #%lX", (unsigned long)addr, (unsigned long)len);
        break;
    case MM_FLASH_OP_OPTION:
        break;                          /* traced by mm_flash_ops_option_done() */
    }
    if (ops.timing != 0 && ops.level != MM_FLASH_TIMING_NONE) {
        fprintf(ops.trace, " @%llu", (unsigned long long)us);
    }
    fputc('\n', ops.trace);
    fflush(ops.trace);
}

static mm_bool should_cut(enum mm_flash_op_kind kind, mm_u32 offset, mm_u32 len)
{
    if (ops.cut_fired) return MM_FALSE;
    if (ops.count == ops.cut_index) return MM_TRUE;
    if (ops.cut_on_erase && (kind == MM_FLASH_OP_ERASE || kind == MM_FLASH_OP_MASS_ERASE) &&
        ops.cut_erase_off >= offset && ops.cut_erase_off - offset < len) {
        return MM_TRUE;
    }
    return MM_FALSE;
}

mm_bool mm_flash_ops_note(mm_u32 offset, mm_u32 len)
{
    enum mm_flash_op_kind kind;
    mm_u64 us;
    if (!ops.enabled || len == 0u || offset >= ops.size) return MM_TRUE;
    if (len > ops.size - offset) len = ops.size - offset;
    if (ops.cut_fired) {
        /* The power is off: nothing reaches the array any more. */
        memcpy(ops.flash + offset, ops.shadow + offset, len);
        return MM_FALSE;
    }
    kind = classify(offset, len);
    if (should_cut(kind, offset, len)) {
        /* Power fails just before this operation: it never happened. */
        memcpy(ops.flash + offset, ops.shadow + offset, len);
        ops.cut_fired = MM_TRUE;
        ops.cut.index = ops.count;
        ops.cut.kind = kind;
        ops.cut.addr = ops.base + offset;
        ops.cut.len = len;
        return MM_FALSE;
    }
    memcpy(ops.shadow + offset, ops.flash + offset, len);
    us = op_us(kind, len);
    ops.stall_us += us;
    trace_op(kind, offset, len, us);
    ops.count++;
    return MM_TRUE;
}

mm_u64 mm_flash_ops_count(void)
{
    return ops.count;
}

void mm_flash_ops_set_count(mm_u64 count)
{
    ops.count = count;
}

void mm_flash_ops_cut_at(mm_u64 index)
{
    ops.cut_index = index;
}

void mm_flash_ops_cut_on_erase(mm_u32 addr)
{
    ops.cut_on_erase = MM_TRUE;
    ops.cut_erase_off = addr - ops.base;
}

void mm_flash_ops_clear_cut(void)
{
    ops.cut_index = MM_FLASH_OPS_NEVER;
    ops.cut_on_erase = MM_FALSE;
    ops.cut_fired = MM_FALSE;
    ops.stall_us = 0u;
}

mm_bool mm_flash_ops_cut_fired(struct mm_flash_cut *out)
{
    if (!ops.cut_fired) return MM_FALSE;
    if (out != 0) *out = ops.cut;
    return MM_TRUE;
}

mm_bool mm_flash_ops_trace_open(const char *path)
{
    FILE *f;
    if (path == 0) return MM_FALSE;
    f = fopen(path, "a");
    if (f == 0) return MM_FALSE;
    mm_flash_ops_trace_close();
    ops.trace = f;
    return MM_TRUE;
}

void mm_flash_ops_trace_close(void)
{
    if (ops.trace != 0) {
        fclose(ops.trace);
        ops.trace = 0;
    }
}

const char *mm_flash_op_kind_name(enum mm_flash_op_kind kind)
{
    switch (kind) {
    case MM_FLASH_OP_PROGRAM: return "program";
    case MM_FLASH_OP_ERASE: return "erase";
    case MM_FLASH_OP_MASS_ERASE: return "mass erase";
    case MM_FLASH_OP_BULK: return "bulk rewrite";
    case MM_FLASH_OP_OPTION: return "option change";
    }
    return "?";
}

void mm_flash_ops_set_timing(const struct mm_flash_timing *timing,
                             enum mm_flash_timing_level level)
{
    ops.timing = timing;
    ops.level = level;
}

mm_u64 mm_flash_ops_take_stall_us(void)
{
    mm_u64 us = ops.stall_us;
    ops.stall_us = 0u;
    return us;
}

/* Shared by refused and protected attempts: MM_FALSE when the power is (or
 * now goes) off at this one. */
static mm_bool attempt_starts(enum mm_flash_op_kind kind, mm_u32 offset, mm_u32 len)
{
    if (!ops.enabled || offset >= ops.size) return MM_FALSE;
    if (ops.cut_fired) return MM_FALSE;
    if (should_cut(kind, offset, len)) {
        ops.cut_fired = MM_TRUE;
        ops.cut.index = ops.count;
        ops.cut.kind = kind;
        ops.cut.addr = ops.base + offset;
        ops.cut.len = len;
        return MM_FALSE;
    }
    return MM_TRUE;
}

static void latch(enum mm_flash_violation kind, mm_u32 offset)
{
    if (ops.latch_count[kind] == 0u) {
        ops.latch_first[kind] = ops.base + offset;
    }
    ops.latch_count[kind]++;
}

mm_bool mm_flash_ops_option_starts(void)
{
    if (!ops.enabled) return MM_TRUE;
    if (ops.cut_fired) return MM_FALSE;
    if (ops.count == ops.cut_index) {
        /* Power fails just before the change: the option bytes stay. */
        ops.cut_fired = MM_TRUE;
        ops.cut.index = ops.count;
        ops.cut.kind = MM_FLASH_OP_OPTION;
        ops.cut.addr = 0u;
        ops.cut.len = 0u;
        return MM_FALSE;
    }
    return MM_TRUE;
}

void mm_flash_ops_option_done(void)
{
    size_t i;
    mm_u32 value;
    if (!ops.enabled) return;
    if (ops.trace != 0) {
        fputc('O', ops.trace);
        for (i = 0; i < mm_optbytes_count(); ++i) {
            const char *name = mm_optbytes_name(i);
            if (name != 0 && mm_optbytes_get(name, &value)) {
                fprintf(ops.trace, " %s=0x%08lX", name, (unsigned long)value);
            }
        }
        fputc('\n', ops.trace);
        fflush(ops.trace);
    }
    ops.count++;
}

void mm_flash_ops_refused(mm_u32 offset, const mm_u8 *wanted, mm_u32 len)
{
    mm_u32 i;
    if (wanted == 0 || len == 0u) return;
    if (offset < ops.size && len > ops.size - offset) len = ops.size - offset;
    if (!attempt_starts(MM_FLASH_OP_PROGRAM, offset, len)) return;
    if (ops.trace != 0) {
        fprintf(ops.trace, "X 0x%08lX 0x", (unsigned long)(ops.base + offset));
        for (i = len; i > 0u; --i) fprintf(ops.trace, "%02X", wanted[i - 1u]);
        fprintf(ops.trace, " 0x");
        for (i = len; i > 0u; --i) fprintf(ops.trace, "%02X", ops.flash[offset + i - 1u]);
        fputc('\n', ops.trace);
        fflush(ops.trace);
    }
    latch(MM_FLASH_REFUSED, offset);
    ops.count++;
}

void mm_flash_ops_protected(mm_u32 offset, mm_bool erase)
{
    if (!attempt_starts(erase ? MM_FLASH_OP_ERASE : MM_FLASH_OP_PROGRAM, offset, 1u)) return;
    if (ops.trace != 0) {
        fprintf(ops.trace, "W 0x%08lX\n", (unsigned long)(ops.base + offset));
        fflush(ops.trace);
    }
    latch(MM_FLASH_PROTECTED, offset);
    ops.count++;
}

mm_u64 mm_flash_ops_latch(enum mm_flash_violation kind, mm_u32 *first_addr)
{
    if (first_addr != 0) *first_addr = ops.latch_first[kind];
    return ops.latch_count[kind];
}

void mm_flash_ops_clear_latches(void)
{
    ops.latch_count[0] = ops.latch_count[1] = 0u;
    ops.latch_first[0] = ops.latch_first[1] = 0u;
}
