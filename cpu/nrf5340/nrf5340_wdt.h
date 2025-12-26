#ifndef M33MU_NRF5340_WDT_H
#define M33MU_NRF5340_WDT_H

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"

mm_bool mm_nrf5340_wdt_register(struct mmio_bus *bus);
void mm_nrf5340_wdt_reset(void);
void mm_nrf5340_wdt_set_nvic(struct mm_nvic *nvic);
void mm_nrf5340_wdt_tick(mm_u64 cycles);

#endif /* M33MU_NRF5340_WDT_H */
