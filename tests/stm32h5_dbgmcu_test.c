/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdio.h>
#include "m33mu/mmio.h"
#include "stm32h533/stm32h533_mmio.h"
#include "stm32h563/stm32h563_mmio.h"
#include "stm32h5f4/stm32h5f4_mmio.h"

#define DBGMCU_IDCODE 0x44024000u
#define DBGMCU_CR     0x44024004u

static struct mmio_region g_regions[256];
static struct mmio_bus g_bus;

static int test_stm32h563_idcode_reads_rev_w(void)
{
    mm_u32 value = 0u;

    mmio_bus_init(&g_bus, g_regions, 256);
    mm_stm32h563_mmio_reset();
    if (!mm_stm32h563_register_mmio(&g_bus)) return 1;
    if (!mmio_bus_read(&g_bus, DBGMCU_IDCODE, 4u, &value)) return 1;
    if (value != 0x100F6484u) return 1;
    value = 0u;
    if (!mmio_bus_read(&g_bus, DBGMCU_IDCODE, 2u, &value)) return 1;
    if (value != 0x6484u) return 1;
    value = 0u;
    if (!mmio_bus_read(&g_bus, DBGMCU_IDCODE + 2u, 2u, &value)) return 1;
    if (value != 0x100Fu) return 1;
    return 0;
}

static int test_stm32h563_idcode_ignores_writes(void)
{
    mm_u32 value = 0u;

    mmio_bus_init(&g_bus, g_regions, 256);
    mm_stm32h563_mmio_reset();
    if (!mm_stm32h563_register_mmio(&g_bus)) return 1;
    if (!mmio_bus_write(&g_bus, DBGMCU_IDCODE, 4u, 0u)) return 1;
    if (!mmio_bus_read(&g_bus, DBGMCU_IDCODE, 4u, &value)) return 1;
    if (value != 0x100F6484u) return 1;
    if (!mmio_bus_write(&g_bus, DBGMCU_IDCODE + 2u, 2u, 0xFFFFu)) return 1;
    if (!mmio_bus_write(&g_bus, DBGMCU_IDCODE, 1u, 0xFFu)) return 1;
    if (!mmio_bus_read(&g_bus, DBGMCU_IDCODE, 4u, &value)) return 1;
    if (value != 0x100F6484u) return 1;
    return 0;
}

static int test_stm32h563_dbgmcu_cr_stays_writable(void)
{
    mm_u32 value = 0u;

    mmio_bus_init(&g_bus, g_regions, 256);
    mm_stm32h563_mmio_reset();
    if (!mm_stm32h563_register_mmio(&g_bus)) return 1;
    if (!mmio_bus_write(&g_bus, DBGMCU_CR, 4u, 0x7u)) return 1;
    if (!mmio_bus_read(&g_bus, DBGMCU_CR, 4u, &value)) return 1;
    if (value != 0x7u) return 1;
    if (!mmio_bus_read(&g_bus, DBGMCU_IDCODE, 4u, &value)) return 1;
    if (value != 0x100F6484u) return 1;
    return 0;
}

static int test_stm32h533_idcode_reads_rev_a(void)
{
    mm_u32 value = 0u;

    mmio_bus_init(&g_bus, g_regions, 256);
    mm_stm32h533_mmio_reset();
    if (!mm_stm32h533_register_mmio(&g_bus)) return 1;
    if (!mmio_bus_read(&g_bus, DBGMCU_IDCODE, 4u, &value)) return 1;
    if (value != 0x10006478u) return 1;
    return 0;
}

static int test_stm32h5f4_idcode_reads_rev_z(void)
{
    mm_u32 value = 0u;

    mmio_bus_init(&g_bus, g_regions, 256);
    mm_stm32h5f4_mmio_reset();
    if (!mm_stm32h5f4_register_mmio(&g_bus)) return 1;
    if (!mmio_bus_read(&g_bus, DBGMCU_IDCODE, 4u, &value)) return 1;
    if (value != 0x1001647Au) return 1;
    return 0;
}

static int test_stm32h563_idcode_without_reset(void)
{
    mm_u32 value = 0u;

    mmio_bus_init(&g_bus, g_regions, 256);
    if (!mm_stm32h563_register_mmio(&g_bus)) return 1;
    if (!mmio_bus_read(&g_bus, DBGMCU_IDCODE, 4u, &value)) return 1;
    if (value != 0x100F6484u) return 1;
    return 0;
}

static int test_stm32h563_idcode_survives_reset(void)
{
    mm_u32 value = 0u;

    mmio_bus_init(&g_bus, g_regions, 256);
    if (!mm_stm32h563_register_mmio(&g_bus)) return 1;
    mm_stm32h563_mmio_reset();
    if (!mmio_bus_read(&g_bus, DBGMCU_IDCODE, 4u, &value)) return 1;
    if (value != 0x100F6484u) return 1;
    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "stm32h563_idcode_reads_rev_w", test_stm32h563_idcode_reads_rev_w },
        { "stm32h563_idcode_ignores_writes", test_stm32h563_idcode_ignores_writes },
        { "stm32h563_dbgmcu_cr_stays_writable", test_stm32h563_dbgmcu_cr_stays_writable },
        { "stm32h533_idcode_reads_rev_a", test_stm32h533_idcode_reads_rev_a },
        { "stm32h5f4_idcode_reads_rev_z", test_stm32h5f4_idcode_reads_rev_z },
        { "stm32h563_idcode_without_reset", test_stm32h563_idcode_without_reset },
        { "stm32h563_idcode_survives_reset", test_stm32h563_idcode_survives_reset },
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
        printf("stm32h5_dbgmcu_test: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
