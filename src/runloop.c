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
#include "m33mu/decode.h"
#include "m33mu/exec_helpers.h"
#include "m33mu/mem.h"
#include "m33mu/cpu.h"
#include <stdio.h>

/* Minimal run loop: fetch + decode, execute only NOP/branch-like placeholders. */

enum mm_step_status {
    MM_STEP_OK = 0,
    MM_STEP_FAULT,
    MM_STEP_HALT
};

static enum mm_step_status execute_decoded(struct mm_cpu *cpu, const struct mm_decoded *dec, const struct mm_fetch_result *fetch)
{
    switch (dec->kind) {
    case MM_OP_NOP:
        return MM_STEP_OK;
    case MM_OP_B_UNCOND:
    case MM_OP_B_UNCOND_WIDE:
        if (fetch != 0) {
            cpu->r[15] = (fetch->pc_fetch + dec->imm) | 1u;
        }
        return MM_STEP_OK;
    default:
        /* Unimplemented instructions halt for now. */
        return MM_STEP_HALT;
    }
}

enum mm_step_status mm_step(struct mm_cpu *cpu, const struct mm_mem *mem, struct mm_fetch_result *out_fetch, struct mm_decoded *out_dec)
{
    struct mm_fetch_result fetch;
    struct mm_decoded dec;

    fetch = mm_fetch_t32(cpu, mem);
    if (out_fetch != 0) {
        *out_fetch = fetch;
    }
    if (fetch.fault) {
        return MM_STEP_FAULT;
    }

    dec = mm_decode_t32(&fetch);
    if (out_dec != 0) {
        *out_dec = dec;
    }
    if (dec.undefined) {
        return MM_STEP_HALT;
    }

    return execute_decoded(cpu, &dec, &fetch);
}
