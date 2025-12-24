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

#ifndef M33MU_CAPSTONE_H
#define M33MU_CAPSTONE_H

#include "m33mu/types.h"

struct mm_fetch_result;
struct mm_decoded;

/* Capstone integration (optional). */
mm_bool capstone_available(void);
mm_bool capstone_init(void);
void capstone_shutdown(void);
mm_bool capstone_set_enabled(mm_bool enabled);
mm_bool capstone_is_enabled(void);
void capstone_log(const struct mm_fetch_result *fetch);
mm_bool capstone_cross_check(const struct mm_fetch_result *fetch, const struct mm_decoded *dec);
mm_bool capstone_it_check_pre(const struct mm_fetch_result *fetch, const struct mm_decoded *dec,
                              mm_u8 it_pattern, mm_u8 it_remaining, mm_u8 it_cond);
mm_bool capstone_it_check_post(const struct mm_fetch_result *fetch, const struct mm_decoded *dec,
                               mm_u8 it_pattern, mm_u8 it_remaining, mm_u8 it_cond);

#endif /* M33MU_CAPSTONE_H */
