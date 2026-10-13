/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_M2354_MMIO_H
#define M33MU_M2354_MMIO_H

#include "m33mu/types.h"
#include "m33mu/sau.h"

struct mmio_bus;
struct mm_memmap;
struct mm_flash_persist;

mm_bool mm_m2354_register_mmio(struct mmio_bus *bus);
void mm_m2354_flash_bind(struct mm_memmap *map,
                         mm_u8 *flash, mm_u32 flash_size,
                         const struct mm_flash_persist *persist,
                         mm_u32 flags);
mm_u64 mm_m2354_cpu_hz(void);
void mm_m2354_mmio_reset(void);
mm_bool mm_m2354_tz_attr_for_addr(mm_u32 addr,
                                  enum mm_sau_attr *attr_out,
                                  mm_u32 *region_out);

/* Registers one MMIO region at both the secure base and the non-secure
 * alias (base + 0x10000000).  Shared with the UART and timer models. */
mm_bool mm_m2354_register_aliased(struct mmio_bus *bus, mm_u32 base,
                                  mm_u32 size, void *opaque,
                                  mm_bool (*read)(void *, mm_u32, mm_u32, mm_u32 *),
                                  mm_bool (*write)(void *, mm_u32, mm_u32, mm_u32));

/* Non-zero when the APBCLK0/APBCLK1 clock-enable bit for a peripheral is set.
 * reg is 0 for APBCLK0 and 1 for APBCLK1. */
mm_bool mm_m2354_apb_periph_active(mm_u32 reg, mm_u32 bit);

#endif /* M33MU_M2354_MMIO_H */
