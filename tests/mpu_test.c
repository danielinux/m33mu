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
#include "m33mu/mpu.h"

static int test_disabled_by_default(void)
{
    struct mm_scs scs;
    mm_u32 rbar;
    mm_u32 rlar;
    mm_scs_init(&scs, 0);

    if (mm_mpu_enabled(&scs, MM_SECURE) != MM_FALSE) return 1;
    if (mm_mpu_region_lookup(&scs, MM_SECURE, 0x1000u, &rbar, &rlar) != MM_FALSE) return 1;
    if (mm_mpu_is_xn_exec(&scs, MM_SECURE, 0x1000u) != MM_FALSE) return 1;

    return 0;
}

static int test_basic_xn_match(void)
{
    struct mm_scs scs;
    mm_scs_init(&scs, 0);

    scs.mpu_ctrl_s = 0x1u; /* ENABLE */
    scs.mpu_rbar_s[0] = 0x00001000u | 0x1u; /* base 0x1000, XN */
    scs.mpu_rlar_s[0] = 0x00001FE0u | 0x1u; /* limit ~0x1FFF, ENABLE */

    if (mm_mpu_is_xn_exec(&scs, MM_SECURE, 0x00001000u) != MM_TRUE) return 1;
    if (mm_mpu_is_xn_exec(&scs, MM_SECURE, 0x00001FFFu) != MM_TRUE) return 1;
    if (mm_mpu_is_xn_exec(&scs, MM_SECURE, 0x00002000u) != MM_FALSE) return 1;

    return 0;
}

static int test_highest_region_wins(void)
{
    struct mm_scs scs;
    mm_scs_init(&scs, 0);

    scs.mpu_ctrl_s = 0x1u; /* ENABLE */

    /* Region 2: allow exec (XN=0) for 0x1000..0x1FFF. */
    scs.mpu_rbar_s[2] = 0x00001000u; /* XN=0 */
    scs.mpu_rlar_s[2] = 0x00001FE0u | 0x1u;

    /* Region 6 overlaps and should win: XN=1 for 0x1800..0x18FF. */
    scs.mpu_rbar_s[6] = 0x00001800u | 0x1u;
    scs.mpu_rlar_s[6] = 0x000018E0u | 0x1u;

    if (mm_mpu_is_xn_exec(&scs, MM_SECURE, 0x00001010u) != MM_FALSE) return 1;
    if (mm_mpu_is_xn_exec(&scs, MM_SECURE, 0x00001810u) != MM_TRUE) return 1;

    return 0;
}

static int test_banked_secure_vs_nonsecure(void)
{
    struct mm_scs scs;
    mm_scs_init(&scs, 0);

    scs.mpu_ctrl_s = 0x1u;
    scs.mpu_ctrl_ns = 0x1u;
    scs.mpu_rbar_s[0] = 0x00001000u | 0x1u;
    scs.mpu_rlar_s[0] = 0x00001FE0u | 0x1u;
    scs.mpu_rbar_ns[0] = 0x00001000u; /* XN=0 */
    scs.mpu_rlar_ns[0] = 0x00001FE0u | 0x1u;

    if (mm_mpu_is_xn_exec(&scs, MM_SECURE, 0x00001010u) != MM_TRUE) return 1;
    if (mm_mpu_is_xn_exec(&scs, MM_NONSECURE, 0x00001010u) != MM_FALSE) return 1;

    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "disabled_by_default", test_disabled_by_default },
        { "basic_xn_match", test_basic_xn_match },
        { "highest_region_wins", test_highest_region_wins },
        { "banked_secure_vs_nonsecure", test_banked_secure_vs_nonsecure },
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
        printf("mpu_test: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}

