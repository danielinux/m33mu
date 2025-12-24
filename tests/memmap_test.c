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
#include "m33mu/mem.h"

static mm_bool deny_write(void *opaque, enum mm_access_type type, enum mm_sec_state sec, mm_u32 addr, mm_u32 size)
{
    (void)opaque;
    (void)sec;
    (void)addr;
    (void)size;
    return type != MM_ACCESS_WRITE;
}

static int test_banked_flash_same_backing(void)
{
    struct mm_memmap map;
    struct mmio_region regions[4];
    struct mm_target_cfg cfg;
    mm_u8 flash[16];
    mm_u32 val = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.flash_base_s = 0;
    cfg.flash_size_s = sizeof(flash);
    cfg.flash_base_ns = 0;
    cfg.flash_size_ns = sizeof(flash);
    cfg.ram_base_s = cfg.ram_base_ns = 0;
    cfg.ram_size_s = cfg.ram_size_ns = 0;

    flash[0] = 0x12;
    flash[1] = 0x34;
    mm_memmap_init(&map, regions, 4);
    if (!mm_memmap_configure_flash(&map, &cfg, flash, MM_TRUE)) return 1;
    map.flash.base = 0;
    if (!mm_memmap_read(&map, MM_SECURE, 0, 4, &val)) return 1;
    if (val != 0x3412u) return 1;
    return 0;
}

static int test_ram_write_read(void)
{
    struct mm_memmap map;
    struct mmio_region regions[4];
    struct mm_target_cfg cfg;
    mm_u8 ram[16];
    mm_u32 val = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.flash_base_s = cfg.flash_base_ns = 0;
    cfg.flash_size_s = cfg.flash_size_ns = 0;
    cfg.ram_base_s = 0;
    cfg.ram_size_s = sizeof(ram);
    cfg.ram_base_ns = 0;
    cfg.ram_size_ns = sizeof(ram);

    mm_memmap_init(&map, regions, 4);
    if (!mm_memmap_configure_ram(&map, &cfg, ram, MM_TRUE)) return 1;
    map.ram.base = 0;
    if (!mm_memmap_write(&map, MM_SECURE, 0, 4, 0xdeadbeefu)) return 1;
    if (!mm_memmap_read(&map, MM_SECURE, 0, 4, &val)) return 1;
    if (val != 0xdeadbeefu) return 1;
    return 0;
}

static int test_interceptor_blocks_write(void)
{
    struct mm_memmap map;
    struct mmio_region regions[4];
    struct mm_target_cfg cfg;
    mm_u8 ram[8];

    memset(&cfg, 0, sizeof(cfg));
    cfg.flash_base_s = cfg.flash_base_ns = 0;
    cfg.flash_size_s = cfg.flash_size_ns = 0;
    cfg.ram_base_s = 0;
    cfg.ram_size_s = sizeof(ram);
    cfg.ram_base_ns = 0;
    cfg.ram_size_ns = sizeof(ram);

    mm_memmap_init(&map, regions, 4);
    mm_memmap_set_interceptor(&map, deny_write, 0);
    if (!mm_memmap_configure_ram(&map, &cfg, ram, MM_TRUE)) return 1;
    if (mm_memmap_write(&map, MM_SECURE, 0, 4, 0x1u)) return 1;
    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "banked_flash", test_banked_flash_same_backing },
        { "ram_write_read", test_ram_write_read },
        { "interceptor_blocks", test_interceptor_blocks_write },
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
        printf("memmap_test: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
