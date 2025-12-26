#ifndef M33MU_MCXW71C_TIMERS_H
#define M33MU_MCXW71C_TIMERS_H

#include "m33mu/types.h"

struct mmio_bus;
struct mm_nvic;

void mm_mcxw71c_timers_init(struct mmio_bus *bus, struct mm_nvic *nvic);
void mm_mcxw71c_timers_reset(void);
void mm_mcxw71c_timers_tick(mm_u64 cycles);

#endif /* M33MU_MCXW71C_TIMERS_H */
