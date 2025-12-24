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
#include "m33mu/scs.h"
#include "m33mu/mmio.h"

static int test_vtor_banked(void)
{
    struct mm_scs scs;
    struct mmio_bus bus;
    struct mmio_region regions[1];
    mm_u32 val = 0;

    mm_scs_init(&scs, 0x0u);
    mmio_bus_init(&bus, regions, 1);
    if (!mm_scs_register_regions(&scs, &bus, 0xE000ED00u, 0xE000ED00u, 0)) return 1;

    /* Write VTOR_S */
    if (!mmio_bus_write(&bus, 0xE000ED08u, 4u, 0x1000u)) return 1;
    if (!mmio_bus_read(&bus, 0xE000ED08u, 4u, &val)) return 1;
    if (val != 0x1000u) return 1;

    return 0;
}

static int test_cpuid_read(void)
{
    struct mm_scs scs;
    struct mmio_bus bus;
    struct mmio_region regions[2];
    mm_u32 val = 0;

    mm_scs_init(&scs, 0x410fc241u);
    mmio_bus_init(&bus, regions, 2);
    if (!mm_scs_register_regions(&scs, &bus, 0xE000ED00u, 0xE002ED00u, 0)) {
        printf("register regions failed\n");
        return 1;
    }
    if (!mmio_bus_read(&bus, 0xE000ED00u, 4u, &val)) {
        printf("cpuid read secure failed\n");
        return 1;
    }
    if (val != 0x410fc241u) {
        printf("cpuid secure got 0x%lx\n", (unsigned long)val);
        return 1;
    }
    if (!mmio_bus_read(&bus, 0xE002ED00u, 4u, &val)) {
        printf("cpuid ns read failed\n");
        return 1;
    }
    if (val != 0x410fc241u) {
        printf("cpuid ns got 0x%lx\n", (unsigned long)val);
        return 1;
    }
    return 0;
}

static int test_shcsr_basic(void)
{
    struct mm_scs scs;
    struct mmio_bus bus;
    struct mmio_region regions[1];
    mm_u32 val = 0;

    mm_scs_init(&scs, 0x0u);
    mmio_bus_init(&bus, regions, 1);
    if (!mm_scs_register_regions(&scs, &bus, 0xE000ED00u, 0xE000ED00u, 0)) return 1;

    if (!mmio_bus_write(&bus, 0xE000ED24u, 4u, 0x1u)) return 1;
    if (!mmio_bus_read(&bus, 0xE000ED24u, 4u, &val)) return 1;
    if (val != 0x1u) return 1;
    return 0;
}

static int test_mpu_bank(void)
{
    struct mm_scs scs;
    struct mmio_bus bus;
    struct mmio_region regions[1];
    mm_u32 val = 0;

    mm_scs_init(&scs, 0x0u);
    mmio_bus_init(&bus, regions, 1);
    if (!mm_scs_register_regions(&scs, &bus, 0xE000ED00u, 0xE000ED00u, 0)) return 1;

    if (!mmio_bus_read(&bus, 0xE000ED90u, 4u, &val)) return 1;
    if (val != 0x00000800u) return 1;

    if (!mmio_bus_write(&bus, 0xE000ED94u, 4u, 0x5u)) return 1; /* secure ctrl */

    if (!mmio_bus_read(&bus, 0xE000ED94u, 4u, &val)) return 1;
    if (val != 0x5u) return 1;
    return 0;
}

static int test_sau_secure_only(void)
{
    struct mm_scs scs;
    struct mmio_bus bus;
    struct mmio_region regions[1];
    mm_u32 val = 0;

    mm_scs_init(&scs, 0x0u);
    mmio_bus_init(&bus, regions, 1);
    if (!mm_scs_register_regions(&scs, &bus, 0xE000ED00u, 0xE000ED00u, 0)) return 1;

    /* SAU lives at SCB+0xCC and is secure-only in this model.
     * Non-secure access is RAZ/WI, so we only read it from secure here.
     */
    if (!mmio_bus_read(&bus, 0xE000EDCCu, 4u, &val)) return 1;
    if (val != 0x7u) return 1;

    /* Only secure writes are honoured; non-secure reads see zeros. */
    if (!mmio_bus_write(&bus, 0xE000EDD0u, 4u, 0x3u)) return 1; /* SAU_CTRL */
    if (!mmio_bus_read(&bus, 0xE000EDD0u, 4u, &val)) return 1;
    if (val != 0x3u) return 1;

    scs.last_access_sec = MM_NONSECURE;
    if (!mmio_bus_write(&bus, 0xE000EDD0u, 4u, 0x1u)) return 1;
    if (!mmio_bus_read(&bus, 0xE000EDD0u, 4u, &val)) return 1;
    if (val != 0x0u) return 1;
    if (!mmio_bus_read(&bus, 0xE000EDCCu, 4u, &val)) return 1;
    if (val != 0x0u) return 1;
    scs.last_access_sec = MM_SECURE;
    if (!mmio_bus_read(&bus, 0xE000EDD0u, 4u, &val)) return 1;
    if (val != 0x3u) return 1;
    return 0;
}

static int test_sau_region_bank(void)
{
    struct mm_scs scs;
    struct mmio_bus bus;
    struct mmio_region regions[1];
    mm_u32 val = 0;

    mm_scs_init(&scs, 0x0u);
    mmio_bus_init(&bus, regions, 1);
    if (!mm_scs_register_regions(&scs, &bus, 0xE000ED00u, 0xE000ED00u, 0)) return 1;

    /* Program region 2 and region 3 using the new SAU layout (CTRL@0xD0, RNR@0xD4,
     * RBAR@0xD8, RLAR@0xDC) and read back through the bank.
     */
    if (!mmio_bus_write(&bus, 0xE000EDD4u, 4u, 2u)) return 1; /* SAU_RNR (new) */
    if (!mmio_bus_write(&bus, 0xE000EDD8u, 4u, 0x11111000u)) return 1; /* SAU_RBAR */
    if (!mmio_bus_write(&bus, 0xE000EDDCu, 4u, 0x22222001u)) return 1; /* SAU_RLAR (EN=1) */

    if (!mmio_bus_write(&bus, 0xE000EDD4u, 4u, 3u)) return 1; /* SAU_RNR (new) */
    if (!mmio_bus_write(&bus, 0xE000EDD8u, 4u, 0x33333000u)) return 1; /* SAU_RBAR */
    if (!mmio_bus_write(&bus, 0xE000EDDCu, 4u, 0x44444001u)) return 1; /* SAU_RLAR (EN=1) */

    if (!mmio_bus_write(&bus, 0xE000EDD4u, 4u, 2u)) return 1;
    if (!mmio_bus_read(&bus, 0xE000EDD8u, 4u, &val)) return 1;
    if (val != 0x11111000u) return 1;
    if (!mmio_bus_read(&bus, 0xE000EDDCu, 4u, &val)) return 1;
    if (val != 0x22222001u) return 1;

    if (!mmio_bus_write(&bus, 0xE000EDD4u, 4u, 3u)) return 1;
    if (!mmio_bus_read(&bus, 0xE000EDD8u, 4u, &val)) return 1;
    if (val != 0x33333000u) return 1;
    if (!mmio_bus_read(&bus, 0xE000EDDCu, 4u, &val)) return 1;
    if (val != 0x44444001u) return 1;

    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "vtor_banked", test_vtor_banked },
        { "cpuid_read", test_cpuid_read },
        { "shcsr_basic", test_shcsr_basic },
        { "mpu_bank", test_mpu_bank },
        { "sau_secure_only", test_sau_secure_only },
        { "sau_region_bank", test_sau_region_bank },
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
        printf("scs_test: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
