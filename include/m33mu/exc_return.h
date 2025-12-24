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

#ifndef M33MU_EXC_RETURN_H
#define M33MU_EXC_RETURN_H

#include "m33mu/types.h"
#include "m33mu/cpu.h"

struct mm_exc_return_info {
    mm_bool valid;
    mm_bool use_psp;
    mm_bool basic_frame; /* true if no FP context */
    mm_bool to_thread;
    enum mm_sec_state target_sec;
};

struct mm_exc_return_info mm_exc_return_decode(mm_u32 value);
mm_u32 mm_exc_return_encode(enum mm_sec_state sec, mm_bool use_psp, mm_bool to_thread);

#endif /* M33MU_EXC_RETURN_H */
