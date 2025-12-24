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

#ifndef M33MU_SAU_H
#define M33MU_SAU_H

#include "m33mu/types.h"
#include "m33mu/scs.h"

enum mm_sau_attr {
    MM_SAU_SECURE = 0,
    MM_SAU_NONSECURE = 1,
    MM_SAU_NSC = 2
};

/* Compute SAU security attribution for an address.
 *
 * Rules:
 * - If SAU is disabled: Secure.
 * - If SAU is enabled: highest-numbered enabled matching region wins.
 * - If no region matches: Secure, unless SAU_CTRL.ALLNS is set.
 */
enum mm_sau_attr mm_sau_attr_for_addr(const struct mm_scs *scs, mm_u32 addr);

#endif /* M33MU_SAU_H */

