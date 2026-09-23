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

/* RAM regions declared with alias_of share one backing store (RT700 SRAM is
 * visible both at the code-bus alias 0x0/0x10000000 and the system-bus alias
 * 0x20000000/0x30000000).  Writes through one alias must be visible through
 * the other, and must invalidate code decoded through any alias. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m33mu/memmap.h"
#include "m33mu/code_cache.h"
#include "m33mu/target.h"

static const struct mm_ram_region regions_cfg[] = {
    { 0x30000000u, 0x20000000u, 0x2000u, -1, 0u },
    { 0x10000000u, 0x00000000u, 0x2000u, -1, 1u },
    { 0x30100000u, 0x20100000u, 0x1000u, -1, 0u },
};

int main(void)
{
    struct mm_memmap map;
    struct mmio_region regions[4];
    struct mm_target_cfg cfg;
    struct mm_code_cache cc;
    mm_u8 ram[0x3000];
    mm_u32 v = 0;
    mm_u32 page_code_s;
    mm_u32 page_code_ns;

    memset(&cfg, 0, sizeof(cfg));
    memset(ram, 0, sizeof(ram));
    cfg.ram_base_s = 0x30000000u;
    cfg.ram_size_s = 0x2000u;
    cfg.ram_base_ns = 0x20000000u;
    cfg.ram_size_ns = 0x2000u;
    cfg.ram_regions = regions_cfg;
    cfg.ram_region_count = 3u;

    mm_memmap_init(&map, regions, 4);
    if (!mm_memmap_configure_ram(&map, &cfg, ram, MM_TRUE)) {
        printf("configure failed\n");
        return 1;
    }
    if (map.ram_total_size != 0x3000u) {
        printf("total size 0x%lx, expected 0x3000\n", (unsigned long)map.ram_total_size);
        return 1;
    }
    if (map.ram_region_offsets[1] != 0u || map.ram_region_offsets[2] != 0x2000u) {
        printf("bad offsets\n");
        return 1;
    }

    if (!mm_memmap_write(&map, MM_SECURE, 0x20000100u, 4u, 0xCAFEF00Du)) return 1;
    if (!mm_memmap_read(&map, MM_SECURE, 0x00000100u, 4u, &v) || v != 0xCAFEF00Du) {
        printf("code alias NS read 0x%08lx\n", (unsigned long)v);
        return 1;
    }
    if (!mm_memmap_read(&map, MM_SECURE, 0x10000100u, 4u, &v) || v != 0xCAFEF00Du) return 1;
    if (!mm_memmap_write(&map, MM_SECURE, 0x10001FFCu, 4u, 0x12345678u)) return 1;
    if (!mm_memmap_read(&map, MM_SECURE, 0x30001FFCu, 4u, &v) || v != 0x12345678u) return 1;
    /* The third region has its own backing and must not alias region 1. */
    if (!mm_memmap_write(&map, MM_SECURE, 0x20100000u, 4u, 0xA5A5A5A5u)) return 1;
    if (!mm_memmap_read(&map, MM_SECURE, 0x20000000u, 4u, &v) || v != 0u) return 1;

    mm_code_cache_init(&cc, &map);
    free(cc.page_has_tb);
    cc.page_has_tb = 0; /* treat every page as holding translated code */
    mm_memmap_set_code_cache(&map, &cc);
    page_code_s = (0x10000100u - cc.page_base) >> M33MU_CODE_PAGE_SHIFT;
    page_code_ns = (0x00000100u - cc.page_base) >> M33MU_CODE_PAGE_SHIFT;
    if (!mm_memmap_write(&map, MM_SECURE, 0x30000100u, 4u, 0u)) return 1;
    if (cc.page_gen[page_code_s] == 0u || cc.page_gen[page_code_ns] == 0u) {
        printf("write via system alias did not invalidate code alias\n");
        return 1;
    }
    mm_code_cache_release(&cc);
    printf("memmap_ram_alias_test: PASS\n");
    return 0;
}
