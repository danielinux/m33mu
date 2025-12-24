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

#pragma once
#include <stdint.h>

#define FLASH_S_BASE     (0x0C000000u)
#define FLASH_NSC_BASE   (0x0C000400u)
#define FLASH_NSC_END    (0x0C0007FFu)

#define FLASH_NS_BASE    (0x08000000u)
#define FLASH_NS_VTOR    (0x08002000u)

/* Non-secure SRAM lives in the 0x2000_0000 alias range for this test. */
#define SRAM_NS_BASE     (0x20000000u)
#define SRAM_NS_END      (0x2001FFFFu)

#define SRAM_S_BASE      (0x30020000u)
