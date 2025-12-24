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
#include "m33mu/exception.h"
#include "m33mu/memmap.h"
#include "m33mu/scs.h"

static void write32(mm_u8 *buf, mm_u32 off, mm_u32 v)
{
    buf[off + 0] = (mm_u8)(v & 0xffu);
    buf[off + 1] = (mm_u8)((v >> 8) & 0xffu);
    buf[off + 2] = (mm_u8)((v >> 16) & 0xffu);
    buf[off + 3] = (mm_u8)((v >> 24) & 0xffu);
}

static int test_vtor_banks_select_different_handlers(void)
{
    struct mm_memmap map;
    struct mmio_region regions[4];
    struct mm_target_cfg cfg;
    struct mm_scs scs;
    mm_u8 flash[0x300];
    mm_u32 handler = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.flash_base_s = 0;
    cfg.flash_size_s = sizeof(flash);
    cfg.flash_base_ns = 0;
    cfg.flash_size_ns = sizeof(flash);
    cfg.ram_base_s = cfg.ram_base_ns = 0;
    cfg.ram_size_s = cfg.ram_size_ns = 0;

    {
        size_t i;
        for (i = 0; i < sizeof(flash); ++i) flash[i] = 0;
    }

    /* Two vector tables in the same backing at different VTORs. */
    /* At vtor 0x000: SysTick handler = 0x11111111 */
    write32(flash, 0x000 + (15u * 4u), 0x11111111u);
    /* At vtor 0x100: SysTick handler = 0x22222222 */
    write32(flash, 0x100 + (15u * 4u), 0x22222222u);

    mm_memmap_init(&map, regions, 4);
    if (!mm_memmap_configure_flash(&map, &cfg, flash, MM_TRUE)) return 1;
    if (!mm_memmap_configure_flash(&map, &cfg, flash, MM_FALSE)) return 1;
    map.flash.base = 0;
    map.flash.length = sizeof(flash);

    mm_scs_init(&scs, 0);
    scs.vtor_s = 0x000u;
    scs.vtor_ns = 0x100u;

    if (!mm_exception_read_handler(&map, &scs, MM_SECURE, MM_VECT_SYSTICK, &handler)) return 1;
    if (handler != 0x11111111u) return 1;

    if (!mm_exception_read_handler(&map, &scs, MM_NONSECURE, MM_VECT_SYSTICK, &handler)) return 1;
    if (handler != 0x22222222u) return 1;

    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "vtor_banks_select_handlers", test_vtor_banks_select_different_handlers },
    };
    int failures = 0;
    int i;
    const int count = (int)(sizeof(tests) / sizeof(tests[0]));
    for (i = 0; i < count; ++i) {
        if (tests[i].fn() != 0) {
            ++failures;
            printf("FAIL: %s\n", tests[i].name);
        } else {
            printf("PASS: %s\n", tests[i].name);
        }
    }
    if (failures != 0) {
        printf("exception_vector_test: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
