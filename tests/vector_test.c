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
#include "m33mu/vector.h"
#include "m33mu/cpu.h"
#include "m33mu/memmap.h"

static int test_reset_vector_load(void)
{
    struct mm_memmap map;
    struct mmio_region regions[2];
    struct mm_cpu cpu;
    mm_u8 flash[8];

    flash[0] = 0x00; flash[1] = 0x10; flash[2] = 0x00; flash[3] = 0x00; /* SP = 0x00001000 */
    flash[4] = 0x08; flash[5] = 0x00; flash[6] = 0x00; flash[7] = 0x00; /* PC = 0x00000008 */

    mm_memmap_init(&map, regions, 2);
    map.flash.buffer = flash;
    map.flash.length = sizeof(flash);

    cpu.vtor_s = 0;
    cpu.vtor_ns = 0;
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.control_s = cpu.control_ns = 0;
    cpu.msp_s = cpu.msp_ns = 0;
    cpu.psp_s = cpu.psp_ns = 0;

    if (!mm_vector_apply_reset(&cpu, &map, MM_SECURE)) return 1;
    if (mm_cpu_get_active_sp(&cpu) != 0x00001000u) return 1;
    if (cpu.r[15] != (0x8u | 1u)) return 1;
    return 0;
}

static int test_vector_read(void)
{
    struct mm_memmap map;
    struct mmio_region regions[2];
    mm_u8 flash[8];
    mm_u32 val;

    flash[0] = 0x34; flash[1] = 0x12; flash[2] = 0; flash[3] = 0;
    flash[4] = 0x78; flash[5] = 0x56; flash[6] = 0; flash[7] = 0;

    mm_memmap_init(&map, regions, 2);
    map.flash.buffer = flash;
    map.flash.length = sizeof(flash);

    if (!mm_vector_read(&map, MM_SECURE, 0, 0, &val)) return 1;
    if (val != 0x1234u) return 1;
    if (!mm_vector_read(&map, MM_SECURE, 0, 1, &val)) return 1;
    if (val != 0x5678u) return 1;
    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "reset_vector_load", test_reset_vector_load },
        { "vector_read", test_vector_read },
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
        printf("vector_test: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
