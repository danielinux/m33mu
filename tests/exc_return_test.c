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
#include "m33mu/exc_return.h"

static int test_basic_decode(void)
{
    struct mm_exc_return_info info;
    /* Secure, return to Thread using PSP, basic frame (0xFFFFFFFD). */
    info = mm_exc_return_decode(0xFFFFFFFDu);
    if (!info.valid) return 1;
    if (!info.use_psp) return 1;
    if (!info.basic_frame) return 1;
    if (!info.to_thread) return 1;
    if (info.target_sec != MM_SECURE) return 1;
    return 0;
}

static int test_nonsecure_target(void)
{
    struct mm_exc_return_info info;
    /* Non-secure, return to Handler using MSP, basic frame (0xFFFFFFB1). */
    info = mm_exc_return_decode(0xFFFFFFB1u);
    if (!info.valid) return 1;
    if (info.use_psp) return 1;
    if (info.to_thread) return 1;
    if (info.target_sec != MM_NONSECURE) return 1;
    return 0;
}

static int test_invalid(void)
{
    struct mm_exc_return_info info;
    info = mm_exc_return_decode(0x0u);
    if (info.valid) return 1;
    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "basic_decode", test_basic_decode },
        { "nonsecure_target", test_nonsecure_target },
        { "invalid", test_invalid },
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
        printf("exc_return_test: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
