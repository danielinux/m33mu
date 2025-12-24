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

#ifndef M33MU_MPU_H
#define M33MU_MPU_H

#include "m33mu/types.h"
#include "m33mu/scs.h"
#include "m33mu/cpu.h"

/*
 * Minimal ARMv8-M MPU helper.
 *
 * For the initial TrustZone/memory-protection tests we only need enough
 * behavior to model instruction fetch execute-never (XN) based on RBAR/RLAR.
 */

mm_bool mm_mpu_enabled(const struct mm_scs *scs, enum mm_sec_state sec);

/* Returns MM_TRUE if the MPU matches a region for addr and that region has XN set. */
mm_bool mm_mpu_is_xn_exec(const struct mm_scs *scs, enum mm_sec_state sec, mm_u32 addr);

/* Returns MM_TRUE if addr is covered by any enabled region (highest-numbered wins). */
mm_bool mm_mpu_region_lookup(const struct mm_scs *scs, enum mm_sec_state sec, mm_u32 addr, mm_u32 *rbar_out, mm_u32 *rlar_out);

#endif /* M33MU_MPU_H */
