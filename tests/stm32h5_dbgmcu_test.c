/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include "m33mu/mmio.h"
#include "stm32h533/stm32h533_mmio.h"
#include "stm32h563/stm32h563_mmio.h"
#include "stm32h5f4/stm32h5f4_mmio.h"

#define DBGMCU_IDCODE 0x44024000u
#define DBGMCU_CR     0x44024004u
#define IDCODE_ENV    "M33MU_STM32H5_IDCODE"

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

static int stm32h563_read_idcode(mm_u32 *value)
{
    *value = 0u;
    mmio_bus_init(&g_bus, g_regions, 256);
    mm_stm32h563_mmio_reset();
    if (!mm_stm32h563_register_mmio(&g_bus)) return 1;
    if (!mmio_bus_read(&g_bus, DBGMCU_IDCODE, 4u, value)) return 1;
    return 0;
}

static int test_stm32h563_idcode_unset_env_reads_stock(void)
{
    mm_u32 value;

    unsetenv(IDCODE_ENV);
    if (stm32h563_read_idcode(&value) != 0) return 1;
    if (value != 0x100F6484u) return 1;
    return 0;
}

static int test_stm32h563_idcode_env_hex(void)
{
    mm_u32 value;
    int ret;

    setenv(IDCODE_ENV, "0x10016484", 1);
    ret = stm32h563_read_idcode(&value);
    if (ret == 0 && value != 0x10016484u) ret = 1;
    value = 0u;
    if (ret == 0 && (!mmio_bus_read(&g_bus, DBGMCU_IDCODE + 2u, 2u, &value) || value != 0x1001u)) ret = 1;
    unsetenv(IDCODE_ENV);
    return ret;
}

static int test_stm32h563_idcode_env_decimal(void)
{
    mm_u32 value;
    int ret;

    setenv(IDCODE_ENV, "268526724", 1);
    ret = stm32h563_read_idcode(&value);
    if (ret == 0 && value != 0x10016484u) ret = 1;
    setenv(IDCODE_ENV, "4294967295", 1);
    if (ret == 0) ret = stm32h563_read_idcode(&value);
    if (ret == 0 && value != 0xFFFFFFFFu) ret = 1;
    unsetenv(IDCODE_ENV);
    return ret;
}

static int test_stm32h563_idcode_env_invalid_ignored(void)
{
    static const char *const bad[] = {
        "", "x10016484", "0x", "0x1001648g", "268526724abc", " 0x10016484",
        "-1", "+1", "0x-1", "0x 1", "0x100000000", "4294967296",
        "99999999999999999999999"
    };
    mm_u32 value;
    size_t i;
    int ret = 0;

    for (i = 0; ret == 0 && i < sizeof(bad) / sizeof(bad[0]); ++i) {
        setenv(IDCODE_ENV, bad[i], 1);
        ret = stm32h563_read_idcode(&value);
        if (ret == 0 && value != 0x100F6484u) ret = 1;
    }
    unsetenv(IDCODE_ENV);
    return ret;
}

static int test_stm32h563_idcode_env_survives_reset_and_writes(void)
{
    mm_u32 value = 0u;
    int ret = 0;

    setenv(IDCODE_ENV, "0x10016484", 1);
    mmio_bus_init(&g_bus, g_regions, 256);
    if (!mm_stm32h563_register_mmio(&g_bus)) ret = 1;
    if (ret == 0 && (!mmio_bus_read(&g_bus, DBGMCU_IDCODE, 4u, &value) || value != 0x10016484u)) ret = 1;
    if (ret == 0) mm_stm32h563_mmio_reset();
    if (ret == 0 && (!mmio_bus_read(&g_bus, DBGMCU_IDCODE, 4u, &value) || value != 0x10016484u)) ret = 1;
    if (ret == 0 && !mmio_bus_write(&g_bus, DBGMCU_IDCODE, 4u, 0x100F6484u)) ret = 1;
    if (ret == 0 && !mmio_bus_write(&g_bus, DBGMCU_IDCODE, 1u, 0xFFu)) ret = 1;
    if (ret == 0 && !mmio_bus_write(&g_bus, DBGMCU_CR, 4u, 0x7u)) ret = 1;
    if (ret == 0 && (!mmio_bus_read(&g_bus, DBGMCU_IDCODE, 4u, &value) || value != 0x10016484u)) ret = 1;
    if (ret == 0 && (!mmio_bus_read(&g_bus, DBGMCU_CR, 4u, &value) || value != 0x7u)) ret = 1;
    unsetenv(IDCODE_ENV);
    if (ret == 0) mm_stm32h563_mmio_reset();
    if (ret == 0 && (!mmio_bus_read(&g_bus, DBGMCU_IDCODE, 4u, &value) || value != 0x100F6484u)) ret = 1;
    return ret;
}

static int test_stm32h533_idcode_env_applies(void)
{
    mm_u32 value = 0u;
    int ret = 0;

    setenv(IDCODE_ENV, "0x20006478", 1);
    mmio_bus_init(&g_bus, g_regions, 256);
    mm_stm32h533_mmio_reset();
    if (!mm_stm32h533_register_mmio(&g_bus)) ret = 1;
    if (ret == 0 && (!mmio_bus_read(&g_bus, DBGMCU_IDCODE, 4u, &value) || value != 0x20006478u)) ret = 1;
    unsetenv(IDCODE_ENV);
    return ret;
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
        { "stm32h563_idcode_unset_env_reads_stock", test_stm32h563_idcode_unset_env_reads_stock },
        { "stm32h563_idcode_env_hex", test_stm32h563_idcode_env_hex },
        { "stm32h563_idcode_env_decimal", test_stm32h563_idcode_env_decimal },
        { "stm32h563_idcode_env_invalid_ignored", test_stm32h563_idcode_env_invalid_ignored },
        { "stm32h563_idcode_env_survives_reset_and_writes", test_stm32h563_idcode_env_survives_reset_and_writes },
        { "stm32h533_idcode_env_applies", test_stm32h533_idcode_env_applies },
    };
    int failures = 0;
    int i;
    const int count = (int)(sizeof(tests) / sizeof(tests[0]));
    unsetenv(IDCODE_ENV);
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
