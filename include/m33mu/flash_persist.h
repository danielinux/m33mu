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

#ifndef M33MU_FLASH_PERSIST_H
#define M33MU_FLASH_PERSIST_H

#include "m33mu/types.h"

struct mm_flash_persist_range {
    const char *path;
    mm_u32 offset;
    mm_u32 length;
};

struct mm_flash_persist {
    mm_bool enabled;
    mm_u8 *flash;
    mm_u32 flash_size;
    int count;
    struct mm_flash_persist_range ranges[16];
};

void mm_flash_persist_build(struct mm_flash_persist *persist,
                            mm_u8 *flash,
                            mm_u32 flash_size,
                            const char **paths,
                            const mm_u32 *offsets,
                            int count);
void mm_flash_persist_flush(struct mm_flash_persist *persist, mm_u32 addr, mm_u32 size);

#endif /* M33MU_FLASH_PERSIST_H */
