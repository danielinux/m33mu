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
#include "m33mu/cpu.h"

static int test_clrex_cancels_exclusive_pair(void)
{
    struct mm_cpu cpu;
    mm_bool ok;

    memset(&cpu, 0, sizeof(cpu));

    mm_cpu_excl_set(&cpu, MM_NONSECURE, 0x20000000u, 4u);
    mm_cpu_excl_clear(&cpu); /* CLREX */

    ok = mm_cpu_excl_check_and_clear(&cpu, MM_NONSECURE, 0x20000000u, 4u);
    if (ok) return 1; /* STREX must fail after CLREX */
    return 0;
}

static int test_strex_consumes_exclusive_state(void)
{
    struct mm_cpu cpu;
    mm_bool ok1;
    mm_bool ok2;

    memset(&cpu, 0, sizeof(cpu));

    mm_cpu_excl_set(&cpu, MM_NONSECURE, 0x20000010u, 4u);
    ok1 = mm_cpu_excl_check_and_clear(&cpu, MM_NONSECURE, 0x20000010u, 4u);
    ok2 = mm_cpu_excl_check_and_clear(&cpu, MM_NONSECURE, 0x20000010u, 4u);

    if (!ok1) return 1;
    if (ok2) return 1; /* must be cleared after first STREX attempt */
    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "clrex_cancels_pair", test_clrex_cancels_exclusive_pair },
        { "strex_consumes_state", test_strex_consumes_exclusive_state },
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
        printf("exclusive_monitor_test: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}

