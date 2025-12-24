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
#include "m33mu/nvic.h"
#include "m33mu/cpu.h"

static int test_pending_selection(void)
{
    struct mm_nvic nvic;
    struct mm_cpu cpu;
    int i;
    for (i = 0; i < 16; ++i) cpu.r[i] = 0;
    cpu.sec_state = MM_SECURE;
    cpu.primask_s = 0;
    cpu.primask_ns = 0;
    mm_nvic_init(&nvic);
    mm_nvic_set_enable(&nvic, 1, MM_TRUE);
    mm_nvic_set_enable(&nvic, 2, MM_TRUE);
    nvic.priority[1] = 0x10;
    nvic.priority[2] = 0x20;
    mm_nvic_set_pending(&nvic, 2, MM_TRUE);
    mm_nvic_set_pending(&nvic, 1, MM_TRUE);
    if (mm_nvic_select(&nvic, &cpu) != 1) return 1;
    return 0;
}

static int test_disable_blocks(void)
{
    struct mm_nvic nvic;
    struct mm_cpu cpu;
    int i;
    for (i = 0; i < 16; ++i) cpu.r[i] = 0;
    cpu.sec_state = MM_SECURE;
    cpu.primask_s = 0;
    cpu.primask_ns = 0;
    mm_nvic_init(&nvic);
    mm_nvic_set_enable(&nvic, 5, MM_TRUE);
    mm_nvic_set_pending(&nvic, 5, MM_TRUE);
    nvic.enable_mask[0] = 0; /* disable all */
    if (mm_nvic_select(&nvic, &cpu) != -1) return 1;
    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "pending_selection", test_pending_selection },
        { "disable_blocks", test_disable_blocks },
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
        printf("nvic_test: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
