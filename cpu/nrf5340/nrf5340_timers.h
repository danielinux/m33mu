#ifndef M33MU_NRF5340_TIMERS_H
#define M33MU_NRF5340_TIMERS_H

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"

void mm_nrf5340_timers_init(struct mmio_bus *bus, struct mm_nvic *nvic);
void mm_nrf5340_timers_reset(void);
void mm_nrf5340_timers_tick(mm_u64 cycles);

#endif /* M33MU_NRF5340_TIMERS_H */
