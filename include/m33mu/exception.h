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

#ifndef M33MU_EXCEPTION_H
#define M33MU_EXCEPTION_H

#include "m33mu/types.h"
#include "m33mu/memmap.h"
#include "m33mu/scs.h"
#include "m33mu/vector.h"

/* Read an exception handler address using the current VTOR for the security state.
 * Uses SCS VTOR banking (VTOR_S / VTOR_NS).
 */
mm_bool mm_exception_read_handler(const struct mm_memmap *map,
                                  const struct mm_scs *scs,
                                  enum mm_sec_state sec,
                                  enum mm_vector_index index,
                                  mm_u32 *handler_out);

#endif /* M33MU_EXCEPTION_H */

