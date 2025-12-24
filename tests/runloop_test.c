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
#include "m33mu/runloop.h"
#include "m33mu/cpu.h"
#include "m33mu/mem.h"

static int test_nop_run(void)
{
    mm_u8 bytes[] = { 0x00, 0xbf, 0x00, 0xbf };
    struct mm_mem mem;
    struct mm_cpu cpu;
    struct mm_fetch_result fetch;
    struct mm_decoded dec;
    enum mm_step_status st;
    int i;

    mem.buffer = bytes;
    mem.length = sizeof(bytes);
    mem.base = 0;
    for (i = 0; i < 16; ++i) cpu.r[i] = 0;
    cpu.r[15] = 1u;
    cpu.xpsr = 0;

    st = mm_step(&cpu, &mem, &fetch, &dec);
    if (st != MM_STEP_OK) return 1;
    if (cpu.r[15] != 3u) return 1;

    st = mm_step(&cpu, &mem, &fetch, &dec);
    if (st != MM_STEP_OK) return 1;
    if (cpu.r[15] != 5u) return 1;

    return 0;
}

static int test_branch(void)
{
    mm_u8 bytes[] = { 0x02, 0xe0, 0x00, 0xbf }; /* B +4; NOP */
    struct mm_mem mem;
    struct mm_cpu cpu;
    struct mm_fetch_result fetch;
    struct mm_decoded dec;
    enum mm_step_status st;
    int i;

    mem.buffer = bytes;
    mem.length = sizeof(bytes);
    mem.base = 0;
    for (i = 0; i < 16; ++i) cpu.r[i] = 0;
    cpu.r[15] = 1u;
    cpu.xpsr = 0;

    st = mm_step(&cpu, &mem, &fetch, &dec);
    if (st != MM_STEP_OK) return 1;
    if (cpu.r[15] != ((fetch.pc_fetch + dec.imm) | 1u)) return 1;

    return 0;
}

static int test_fault(void)
{
    mm_u8 bytes[] = { 0x00 }; /* incomplete insn */
    struct mm_mem mem;
    struct mm_cpu cpu;
    enum mm_step_status st;
    int i;

    mem.buffer = bytes;
    mem.length = sizeof(bytes);
    mem.base = 0;
    for (i = 0; i < 16; ++i) cpu.r[i] = 0;
    cpu.r[15] = 1u;
    cpu.xpsr = 0;

    st = mm_step(&cpu, &mem, 0, 0);
    if (st != MM_STEP_FAULT) return 1;
    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "nop_run", test_nop_run },
        { "branch", test_branch },
        { "fault", test_fault },
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
        printf("runloop_test: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
