/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_LPC55S69_ROMAPI_H
#define M33MU_LPC55S69_ROMAPI_H

#include "m33mu/types.h"
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/cpu.h"

mm_bool mm_lpc55s69_romapi_register_mmio(struct mmio_bus *bus);
void    mm_lpc55s69_romapi_reset(void);
mm_bool mm_lpc55s69_romapi_handle(struct mm_cpu *cpu, struct mm_memmap *map);

void mm_lpc55s69_romapi_deactivate(void);

#endif /* M33MU_LPC55S69_ROMAPI_H */
