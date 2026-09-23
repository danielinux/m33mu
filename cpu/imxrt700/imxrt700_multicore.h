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

#ifndef M33MU_IMXRT700_MULTICORE_H
#define M33MU_IMXRT700_MULTICORE_H

#include "m33mu/types.h"
#include "m33mu/target.h"

/*
 * CPU1 (sense domain Cortex-M33) run control and the CPU0<->CPU1 messaging
 * unit (MU1).
 *
 * CPU1 starts the way the SDK's multicore manager starts it: CPU0 unlocks
 * GLIKEY4, programs SYSCON3 CPU1_SVTOR/CPU1_NSVTOR (address >> 7), enables the
 * CPU1 clock (CLKCTL3 PSCCTL0_COMP bit 0), releases its reset (RSTCTL3
 * PRSTCTL0 bit 31) and clears SYSCON3 CPU_STATUS.CPU_WAIT.  CPU1 then boots
 * secure from the vector table at CPU1_SVTOR << 7.
 */
extern const struct mm_target_mc_ops mm_imxrt700_mc_ops;

void mm_imxrt700_mc_attach(void);
void mm_imxrt700_mc_reset(void);
void mm_imxrt700_mc_poll(void);

#endif /* M33MU_IMXRT700_MULTICORE_H */
