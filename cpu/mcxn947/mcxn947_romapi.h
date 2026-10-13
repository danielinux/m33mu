#ifndef M33MU_MCXN947_ROMAPI_H
#define M33MU_MCXN947_ROMAPI_H

#include "m33mu/types.h"
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/cpu.h"
#include "m33mu/flash_persist.h"

mm_bool mm_mcxn947_romapi_register_mmio(struct mmio_bus *bus);
void mm_mcxn947_romapi_reset(void);
mm_bool mm_mcxn947_romapi_handle(struct mm_cpu *cpu, struct mm_memmap *map);
void mm_mcxn947_romapi_set_persist(const struct mm_flash_persist *persist);
void mm_mcxn947_romapi_set_tz_disabled(mm_bool disabled);

#endif /* M33MU_MCXN947_ROMAPI_H */
