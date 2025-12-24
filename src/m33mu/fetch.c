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

#include "m33mu/fetch.h"
#include "m33mu/mem.h"
#include "m33mu/cpu.h"
#include "m33mu/memmap.h"

mm_bool t32_is_32bit_prefix(mm_u16 prefix)
{
    return (prefix & 0xF800u) >= 0xE800u;
}

struct mm_fetch_result mm_fetch_t32(struct mm_cpu *cpu, const struct mm_mem *mem)
{
    struct mm_fetch_result res;
    mm_u32 hw1;

    res.insn = 0;
    res.len = 0;
    res.fault = MM_TRUE;
    res.fault_addr = 0;
    res.pc_fetch = 0;

    if (cpu == 0 || mem == 0) {
        return res;
    }

    res.pc_fetch = cpu->r[15] & ~1u;

    if (!mem_read16(mem, res.pc_fetch, &hw1)) {
        res.fault_addr = res.pc_fetch;
        return res;
    }

    if (t32_is_32bit_prefix((mm_u16)hw1)) {
        mm_u32 hw2;
        if (!mem_read16(mem, res.pc_fetch + 2u, &hw2)) {
            res.fault_addr = res.pc_fetch + 2u;
            return res;
        }
        res.insn = ((mm_u32)hw1 << 16) | hw2;
        res.len = 4;
        res.fault = MM_FALSE;
        cpu->r[15] = (res.pc_fetch + 4u) | 1u;
        return res;
    }

    res.insn = hw1;
    res.len = 2;
    res.fault = MM_FALSE;
    cpu->r[15] = (res.pc_fetch + 2u) | 1u;
    return res;
}

struct mm_fetch_result mm_fetch_t32_memmap(struct mm_cpu *cpu, const struct mm_memmap *map, enum mm_sec_state sec)
{
    struct mm_fetch_result res;
    mm_u32 hw1;

    res.insn = 0;
    res.len = 0;
    res.fault = MM_TRUE;
    res.fault_addr = 0;
    res.pc_fetch = 0;

    if (cpu == 0 || map == 0) {
        return res;
    }

    res.pc_fetch = cpu->r[15] & ~1u;

    if (!mm_memmap_fetch_read16(map, sec, res.pc_fetch, &hw1)) {
        res.fault_addr = res.pc_fetch;
        return res;
    }

    if (t32_is_32bit_prefix((mm_u16)hw1)) {
        mm_u32 hw2;
        if (!mm_memmap_fetch_read16(map, sec, res.pc_fetch + 2u, &hw2)) {
            res.fault_addr = res.pc_fetch + 2u;
            return res;
        }
        res.insn = ((mm_u32)hw1 << 16) | hw2;
        res.len = 4;
        res.fault = MM_FALSE;
        cpu->r[15] = (res.pc_fetch + 4u) | 1u;
        return res;
    }

    res.insn = hw1;
    res.len = 2;
    res.fault = MM_FALSE;
    cpu->r[15] = (res.pc_fetch + 2u) | 1u;
    return res;
}
