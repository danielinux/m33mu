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

#ifndef M33MU_IMXRT700_ROMAPI_H
#define M33MU_IMXRT700_ROMAPI_H

#include "m33mu/types.h"
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/cpu.h"

/*
 * Boot ROM (0x13000000 secure / 0x03000000 non-secure, 256 KB) with the
 * bootloader API tree at 0x1303FC00 (SDK fsl_romapi layout): version,
 * copyright, runBootloader and the OTP driver (fuse read/program).  The
 * nboot and IAP pointers are NULL.
 */
mm_bool mm_imxrt700_romapi_register_mmio(struct mmio_bus *bus);
void mm_imxrt700_romapi_reset(void);
mm_bool mm_imxrt700_romapi_handle(struct mm_cpu *cpu, struct mm_memmap *map);

#endif /* M33MU_IMXRT700_ROMAPI_H */
