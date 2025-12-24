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
#include "m33mu/cpu.h"

static void init_cpu(struct mm_cpu *cpu)
{
    int i;
    for (i = 0; i < 16; ++i) {
        cpu->r[i] = 0;
    }
    cpu->xpsr = 0;
    cpu->sec_state = MM_SECURE;
    cpu->mode = MM_THREAD;
    cpu->msp_s = 0x1000u;
    cpu->psp_s = 0x2000u;
    cpu->msp_ns = 0x3000u;
    cpu->psp_ns = 0x4000u;
    cpu->msplim_s = 0;
    cpu->psplim_s = 0;
    cpu->msplim_ns = 0;
    cpu->psplim_ns = 0;
    cpu->control_s = 0;
    cpu->control_ns = 0;
    cpu->primask_s = cpu->primask_ns = 0;
    cpu->basepri_s = cpu->basepri_ns = 0;
    cpu->faultmask_s = cpu->faultmask_ns = 0;
    cpu->vtor_s = cpu->vtor_ns = 0;
}

static int test_thread_sp_sel(void)
{
    struct mm_cpu cpu;
    init_cpu(&cpu);
    cpu.control_s = 0; /* use MSP */
    if (mm_cpu_get_active_sp(&cpu) != cpu.msp_s) return 1;
    cpu.control_s = 0x2u; /* SPSEL -> PSP */
    if (mm_cpu_get_active_sp(&cpu) != cpu.psp_s) return 1;
    return 0;
}

static int test_handler_uses_msp(void)
{
    struct mm_cpu cpu;
    init_cpu(&cpu);
    cpu.control_s = 0x2u; /* would select PSP in Thread */
    cpu.mode = MM_HANDLER;
    if (mm_cpu_get_active_sp(&cpu) != cpu.msp_s) return 1;
    return 0;
}

static int test_ns_banks(void)
{
    struct mm_cpu cpu;
    init_cpu(&cpu);
    cpu.sec_state = MM_NONSECURE;
    cpu.control_ns = 0;
    if (mm_cpu_get_active_sp(&cpu) != cpu.msp_ns) return 1;
    cpu.control_ns = 0x2u;
    if (mm_cpu_get_active_sp(&cpu) != cpu.psp_ns) return 1;
    return 0;
}

static int test_privileged_flag(void)
{
    struct mm_cpu cpu;
    init_cpu(&cpu);
    cpu.sec_state = MM_SECURE;
    mm_cpu_set_privileged(&cpu, MM_TRUE);
    if (mm_cpu_get_privileged(&cpu)) return 1;
    mm_cpu_set_privileged(&cpu, MM_FALSE);
    if (!mm_cpu_get_privileged(&cpu)) return 1;
    cpu.sec_state = MM_NONSECURE;
    mm_cpu_set_privileged(&cpu, MM_TRUE);
    if (mm_cpu_get_privileged(&cpu)) return 1;
    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "thread_sp_sel", test_thread_sp_sel },
        { "handler_msp", test_handler_uses_msp },
        { "ns_banks", test_ns_banks },
        { "privileged_flag", test_privileged_flag },
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
        printf("cpu_banks_test: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
