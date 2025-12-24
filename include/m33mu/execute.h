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

#ifndef M33MU_EXECUTE_H
#define M33MU_EXECUTE_H

#include "m33mu/types.h"
#include "m33mu/cpu.h"
#include "m33mu/decode.h"
#include "m33mu/fetch.h"
#include "m33mu/memmap.h"
#include "m33mu/scs.h"
#include "m33mu/gdbstub.h"

enum mm_exec_status {
    MM_EXEC_OK = 0,
    MM_EXEC_CONTINUE = 1
};

struct mm_execute_ctx {
    struct mm_cpu *cpu;
    struct mm_memmap *map;
    struct mm_scs *scs;
    struct mm_gdb_stub *gdb;
    const struct mm_fetch_result *fetch;
    const struct mm_decoded *dec;
    mm_bool opt_dump;
    mm_bool opt_gdb;
    mm_u8 *it_pattern;
    mm_u8 *it_remaining;
    mm_u8 *it_cond;
    mm_bool *done;
    mm_bool (*handle_pc_write)(struct mm_cpu *cpu,
                               struct mm_memmap *map,
                               mm_u32 value,
                               mm_u8 *it_pattern,
                               mm_u8 *it_remaining,
                               mm_u8 *it_cond);
    mm_bool (*raise_mem_fault)(struct mm_cpu *cpu,
                               struct mm_memmap *map,
                               struct mm_scs *scs,
                               mm_u32 fault_pc,
                               mm_u32 fault_xpsr,
                               mm_u32 addr,
                               mm_bool is_exec);
    mm_bool (*raise_usage_fault)(struct mm_cpu *cpu,
                                 struct mm_memmap *map,
                                 struct mm_scs *scs,
                                 mm_u32 fault_pc,
                                 mm_u32 fault_xpsr,
                                 mm_u32 ufsr_bits);
    mm_bool (*exc_return_unstack)(struct mm_cpu *cpu,
                                  struct mm_memmap *map,
                                  mm_u32 exc_ret);
    mm_bool (*enter_exception)(struct mm_cpu *cpu,
                               struct mm_memmap *map,
                               struct mm_scs *scs,
                               mm_u32 exc_num,
                               mm_u32 return_pc,
                               mm_u32 xpsr_in);
};

mm_u8 itstate_get(mm_u32 xpsr);
mm_u32 itstate_set(mm_u32 xpsr, mm_u8 itstate);
mm_u8 itstate_advance(mm_u8 itstate);
void itstate_sync_from_xpsr(mm_u32 xpsr, mm_u8 *pattern_out, mm_u8 *remaining_out, mm_u8 *cond_out);

enum mm_exec_status mm_execute_decoded(struct mm_execute_ctx *ctx);

#endif /* M33MU_EXECUTE_H */
