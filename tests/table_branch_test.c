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
#include "m33mu/memmap.h"
#include "m33mu/target.h"
#include "m33mu/table_branch.h"

static void setup_ram_map(struct mm_memmap *map, mm_u8 *ram, size_t ram_len)
{
    struct mmio_region regions[1];
    struct mm_target_cfg cfg;

    memset(regions, 0, sizeof(regions));
    mm_memmap_init(map, regions, 1u);

    memset(&cfg, 0, sizeof(cfg));
    cfg.ram_base_s = 0x20000000u;
    cfg.ram_size_s = (mm_u32)ram_len;
    cfg.ram_base_ns = 0x20000000u;
    cfg.ram_size_ns = (mm_u32)ram_len;

    (void)mm_memmap_configure_ram(map, &cfg, ram, MM_FALSE);
}

static int test_tbb_target(void)
{
    struct mm_memmap map;
    mm_u8 ram[256];
    mm_u32 target = 0;
    mm_u32 fault = 0;
    mm_u32 pc_fetch = 0x1000u;

    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));

    /* table[3] = 5 -> offset 10 bytes */
    ram[3] = 5u;

    if (!mm_table_branch_target(&map, MM_NONSECURE, pc_fetch,
                                0x20000000u, 3u, MM_FALSE,
                                &target, &fault)) {
        return 1;
    }
    if (target != 0x100fu) { /* (0x1000+4)+10 = 0x100E, then Thumb bit => 0x100F */
        return 1;
    }
    return 0;
}

static int test_tbh_target(void)
{
    struct mm_memmap map;
    mm_u8 ram[256];
    mm_u32 target = 0;
    mm_u32 fault = 0;
    mm_u32 pc_fetch = 0x2000u;

    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));

    /* Index is Rm<<1. With Rm=2, read halfword at offset 4: entry=7 -> offset 14 bytes. */
    ram[4] = 7u;
    ram[5] = 0u;

    if (!mm_table_branch_target(&map, MM_NONSECURE, pc_fetch,
                                0x20000000u, 2u, MM_TRUE,
                                &target, &fault)) {
        return 1;
    }
    if (target != 0x2013u) { /* (0x2000+4)+14 = 0x2012, then Thumb bit => 0x2013 */
        return 1;
    }
    return 0;
}

static int test_fault_addr(void)
{
    struct mm_memmap map;
    mm_u8 ram[8];
    mm_u32 target = 0;
    mm_u32 fault = 0;

    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));

    /* Out of range for our 8-byte RAM mapping. */
    if (mm_table_branch_target(&map, MM_NONSECURE, 0x0u,
                              0x20000000u, 100u, MM_FALSE,
                              &target, &fault)) {
        return 1;
    }
    if (fault != 0x20000064u) {
        return 1;
    }
    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "tbb_target", test_tbb_target },
        { "tbh_target", test_tbh_target },
        { "fault_addr", test_fault_addr },
    };
    const int count = (int)(sizeof(tests) / sizeof(tests[0]));
    int failures = 0;
    int i;

    for (i = 0; i < count; ++i) {
        if (tests[i].fn() != 0) {
            ++failures;
            printf("FAIL: %s\n", tests[i].name);
        } else {
            printf("PASS: %s\n", tests[i].name);
        }
    }
    if (failures != 0) {
        printf("table_branch_test: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}

