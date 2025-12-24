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

#ifndef M33MU_RUNLOOP_H
#define M33MU_RUNLOOP_H

#include "m33mu/types.h"
#include "m33mu/fetch.h"
#include "m33mu/decode.h"
#include "m33mu/mem.h"
#include "m33mu/cpu.h"

enum mm_step_status {
    MM_STEP_OK = 0,
    MM_STEP_FAULT,
    MM_STEP_HALT
};

/* Single-step: fetch, decode, and execute minimal subset. */
enum mm_step_status mm_step(struct mm_cpu *cpu, const struct mm_mem *mem, struct mm_fetch_result *out_fetch, struct mm_decoded *out_dec);

#endif /* M33MU_RUNLOOP_H */
