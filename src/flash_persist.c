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
#include <string.h>
#include "m33mu/flash_persist.h"

static void sort_indices(mm_u32 *idx, const mm_u32 *offsets, int count)
{
    int i;
    int j;
    for (i = 0; i < count; ++i) {
        idx[i] = (mm_u32)i;
    }
    for (i = 0; i < count; ++i) {
        for (j = i + 1; j < count; ++j) {
            if (offsets[idx[j]] < offsets[idx[i]]) {
                mm_u32 tmp = idx[i];
                idx[i] = idx[j];
                idx[j] = tmp;
            }
        }
    }
}

void mm_flash_persist_build(struct mm_flash_persist *persist,
                            mm_u8 *flash,
                            mm_u32 flash_size,
                            const char **paths,
                            const mm_u32 *offsets,
                            int count)
{
    int i;
    mm_u32 idx[16];
    if (persist == 0) {
        return;
    }
    memset(persist, 0, sizeof(*persist));
    if (flash == 0 || paths == 0 || offsets == 0 || count <= 0) {
        return;
    }
    if (count > (int)(sizeof(persist->ranges) / sizeof(persist->ranges[0]))) {
        count = (int)(sizeof(persist->ranges) / sizeof(persist->ranges[0]));
    }
    sort_indices(idx, offsets, count);
    persist->enabled = MM_TRUE;
    persist->flash = flash;
    persist->flash_size = flash_size;
    persist->count = count;
    for (i = 0; i < count; ++i) {
        int cur = (int)idx[i];
        int next = (i + 1 < count) ? (int)idx[i + 1] : -1;
        mm_u32 start = offsets[cur];
        mm_u32 end = (next >= 0) ? offsets[next] : flash_size;
        if (end < start) {
            end = start;
        }
        if (end > flash_size) {
            end = flash_size;
        }
        persist->ranges[i].path = paths[cur];
        persist->ranges[i].offset = start;
        persist->ranges[i].length = end - start;
    }
}

void mm_flash_persist_flush(struct mm_flash_persist *persist, mm_u32 addr, mm_u32 size)
{
    int i;
    if (persist == 0 || !persist->enabled || persist->flash == 0) {
        return;
    }
    if (size == 0u) {
        return;
    }
    for (i = 0; i < persist->count; ++i) {
        mm_u32 start = persist->ranges[i].offset;
        mm_u32 end = start + persist->ranges[i].length;
        mm_u32 a0 = addr;
        mm_u32 a1 = addr + size;
        if (a1 <= start || a0 >= end) {
            continue;
        }
        if (persist->ranges[i].path != 0) {
            FILE *f = fopen(persist->ranges[i].path, "wb");
            if (f == 0) {
                fprintf(stderr, "persist: failed to open %s\n", persist->ranges[i].path);
                continue;
            }
            if (persist->ranges[i].length > 0) {
                size_t n = fwrite(persist->flash + start, 1u, (size_t)persist->ranges[i].length, f);
                if (n != (size_t)persist->ranges[i].length) {
                    fprintf(stderr, "persist: short write for %s\n", persist->ranges[i].path);
                }
            }
            fclose(f);
        }
    }
}
