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

#ifndef M33MU_STM32N657_MMIO_H
#define M33MU_STM32N657_MMIO_H

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "m33mu/sau.h"

struct mm_memmap;

mm_bool mm_stm32n657_register_mmio(struct mmio_bus *bus);
void mm_stm32n657_mmio_reset(void);
mm_u64 mm_stm32n657_cpu_hz(void);
mm_u32 *mm_stm32n657_rcc_regs(void);
mm_bool mm_stm32n657_tz_attr_for_addr(mm_u32 addr,
                                      enum mm_sau_attr *attr_out,
                                      mm_u32 *region_out);

#endif /* M33MU_STM32N657_MMIO_H */
