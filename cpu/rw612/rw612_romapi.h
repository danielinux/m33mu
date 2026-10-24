/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_RW612_ROMAPI_H
#define M33MU_RW612_ROMAPI_H

#include "m33mu/types.h"
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/cpu.h"

mm_bool mm_rw612_romapi_register_mmio(struct mmio_bus *bus);
void    mm_rw612_romapi_reset(void);
mm_bool mm_rw612_romapi_handle(struct mm_cpu *cpu, struct mm_memmap *map);

void mm_rw612_romapi_deactivate(void);

#endif /* M33MU_RW612_ROMAPI_H */
