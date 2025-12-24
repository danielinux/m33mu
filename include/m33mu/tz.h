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

#ifndef M33MU_TZ_H
#define M33MU_TZ_H

#include "m33mu/types.h"
#include "m33mu/cpu.h"

/* Minimal TrustZone state-changing instruction semantics used by CMSE toolchains. */

void mm_tz_exec_sg(struct mm_cpu *cpu);
void mm_tz_exec_bxns(struct mm_cpu *cpu, mm_u32 target);
void mm_tz_exec_blxns(struct mm_cpu *cpu, mm_u32 target, mm_u32 return_addr);

#endif /* M33MU_TZ_H */

