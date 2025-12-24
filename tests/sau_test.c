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
#include "m33mu/sau.h"

static int test_default_secure(void)
{
    struct mm_scs scs;
    mm_scs_init(&scs, 0);
    if (mm_sau_attr_for_addr(&scs, 0x08000000u) != MM_SAU_SECURE) return 1;
    return 0;
}

static int test_allns_default(void)
{
    struct mm_scs scs;
    mm_scs_init(&scs, 0);
    scs.sau_ctrl = 0x3u; /* ENABLE|ALLNS */
    if (mm_sau_attr_for_addr(&scs, 0x08000000u) != MM_SAU_NONSECURE) return 1;
    return 0;
}

static int test_region_priority_and_nsc(void)
{
    struct mm_scs scs;
    enum mm_sau_attr a;
    mm_scs_init(&scs, 0);
    scs.sau_ctrl = 0x1u; /* ENABLE */

    /* Region 0: mark 0x1000..0x1FFF as NonSecure. */
    scs.sau_rbar[0] = 0x00001000u;
    scs.sau_rlar[0] = 0x00001FE0u | 0x1u; /* ENABLE, limit ~0x1FFF */

    /* Region 3 overlaps and should win: mark 0x1800..0x18FF as NSC. */
    scs.sau_rbar[3] = 0x00001800u;
    scs.sau_rlar[3] = 0x000018E0u | 0x3u; /* ENABLE|NSC */

    a = mm_sau_attr_for_addr(&scs, 0x00001004u);
    if (a != MM_SAU_NONSECURE) return 1;
    a = mm_sau_attr_for_addr(&scs, 0x00001810u);
    if (a != MM_SAU_NSC) return 1;
    a = mm_sau_attr_for_addr(&scs, 0x00002000u);
    if (a != MM_SAU_SECURE) return 1;

    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "default_secure", test_default_secure },
        { "allns_default", test_allns_default },
        { "region_priority_and_nsc", test_region_priority_and_nsc },
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
        printf("sau_test: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}

