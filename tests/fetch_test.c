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
#include "m33mu/fetch.h"
#include "m33mu/mem.h"
#include "m33mu/cpu.h"

static int test_16bit_fetch(void)
{
    mm_u8 buf[] = { 0x00, 0xbf }; /* NOP */
    struct mm_mem mem;
    struct mm_cpu cpu;
    struct mm_fetch_result res;
    int i;

    mem.buffer = buf;
    mem.length = sizeof(buf);
    mem.base = 0;
    for (i = 0; i < 16; ++i) {
        cpu.r[i] = 0;
    }
    cpu.xpsr = 0;
    cpu.r[15] = 0x00000001u;

    res = mm_fetch_t32(&cpu, &mem);
    if (res.fault || res.len != 2 || res.insn != 0xbf00u) {
        return 1;
    }
    if (cpu.r[15] != 0x00000003u) {
        return 1;
    }
    return 0;
}

static int test_32bit_fetch(void)
{
    mm_u8 buf[] = { 0x00, 0xf0, 0x00, 0xf8 }; /* 0xf000f800 (BL prefix style) */
    struct mm_mem mem;
    struct mm_cpu cpu;
    struct mm_fetch_result res;
    int i;

    mem.buffer = buf;
    mem.length = sizeof(buf);
    mem.base = 0;
    for (i = 0; i < 16; ++i) {
        cpu.r[i] = 0;
    }
    cpu.xpsr = 0;
    cpu.r[15] = 0x00000001u;

    res = mm_fetch_t32(&cpu, &mem);
    if (res.fault || res.len != 4 || res.insn != 0xf000f800u) {
        return 1;
    }
    if (cpu.r[15] != 0x00000005u) {
        return 1;
    }
    return 0;
}

static int test_fault_first_halfword(void)
{
    mm_u8 buf[] = { 0x00 }; /* incomplete halfword */
    struct mm_mem mem;
    struct mm_cpu cpu;
    struct mm_fetch_result res;
    int i;

    mem.buffer = buf;
    mem.length = sizeof(buf);
    mem.base = 0;
    for (i = 0; i < 16; ++i) {
        cpu.r[i] = 0;
    }
    cpu.xpsr = 0;
    cpu.r[15] = 0x00000001u;

    res = mm_fetch_t32(&cpu, &mem);
    if (!res.fault || res.fault_addr != 0x0u) {
        return 1;
    }
    if (cpu.r[15] != 0x00000001u) {
        return 1;
    }
    return 0;
}

static int test_fault_second_halfword(void)
{
    mm_u8 buf[] = { 0x34, 0xf8 }; /* prefix indicates 32-bit, but second halfword missing */
    struct mm_mem mem;
    struct mm_cpu cpu;
    struct mm_fetch_result res;
    int i;

    mem.buffer = buf;
    mem.length = sizeof(buf);
    mem.base = 0;
    for (i = 0; i < 16; ++i) {
        cpu.r[i] = 0;
    }
    cpu.xpsr = 0;
    cpu.r[15] = 0x00000001u;

    res = mm_fetch_t32(&cpu, &mem);
    if (!res.fault || res.fault_addr != 0x00000002u) {
        return 1;
    }
    if (cpu.r[15] != 0x00000001u) {
        return 1;
    }
    return 0;
}

int main(void)
{
    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        { "16-bit fetch", test_16bit_fetch },
        { "32-bit fetch", test_32bit_fetch },
        { "fault first halfword", test_fault_first_halfword },
        { "fault second halfword", test_fault_second_halfword },
    };
    const int test_count = (int)(sizeof(tests) / sizeof(tests[0]));
    int failures = 0;
    int i;

    for (i = 0; i < test_count; ++i) {
        if (tests[i].fn() != 0) {
            ++failures;
            printf("FAIL: %s\n", tests[i].name);
        } else {
            printf("PASS: %s\n", tests[i].name);
        }
    }

    if (failures != 0) {
        printf("fetch_test: %d failure(s)\n", failures);
        return 1;
    }

    return 0;
}
