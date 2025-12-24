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

#include "m33mu/exception.h"

mm_bool mm_exception_read_handler(const struct mm_memmap *map,
                                  const struct mm_scs *scs,
                                  enum mm_sec_state sec,
                                  enum mm_vector_index index,
                                  mm_u32 *handler_out)
{
    mm_u32 vtor;

    if (map == 0 || scs == 0 || handler_out == 0) {
        return MM_FALSE;
    }

    vtor = (sec == MM_NONSECURE) ? scs->vtor_ns : scs->vtor_s;
    if (mm_vector_read(map, sec, vtor, (mm_u32)index, handler_out)) {
        return MM_TRUE;
    }

    /* Fallback: read directly from the configured flash backing if available. */
    if (map->flash.buffer != 0 &&
        vtor >= map->flash.base &&
        (vtor - map->flash.base) + (((mm_u32)index + 1u) * 4u) <= map->flash.length) {
        const mm_u32 *p = (const mm_u32 *)(map->flash.buffer + (vtor - map->flash.base));
        *handler_out = p[(mm_u32)index];
        return MM_TRUE;
    }

    return MM_FALSE;
}

