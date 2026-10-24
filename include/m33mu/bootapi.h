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


#ifndef M33MU_BOOTAPI_H
#define M33MU_BOOTAPI_H

#include "m33mu/cpu.h"
#include "m33mu/memmap.h"

/* Vendor boot ROM / ROM API calls that m33mu implements natively (RP2350
 * bootrom, LPC55S69/MCXN947/RW612/i.MX RT700 ROM API). Returns MM_TRUE when
 * the PC was in a handled ROM entry and the call completed. */
mm_bool mm_target_bootapi_handle(struct mm_cpu *cpu, struct mm_memmap *map);
/* Clear the process-global "this target is active" flags of the boot ROM
 * handlers and RP2350 core extensions; the selected target sets its own
 * again when it registers its MMIO. */
void mm_target_deactivate_boot_state(void);

#endif /* M33MU_BOOTAPI_H */
