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

#ifndef M33MU_CPU_DB_H
#define M33MU_CPU_DB_H

#include "m33mu/target.h"

/* Look up a CPU by name (e.g., "stm32h563"). Returns MM_TRUE on success. */
mm_bool mm_cpu_lookup(const char *name, struct mm_target_cfg *cfg_out);

/* Get the default CPU name compiled in. */
const char *mm_cpu_default_name(void);

#endif /* M33MU_CPU_DB_H */
