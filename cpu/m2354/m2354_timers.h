/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_M2354_TIMERS_H
#define M33MU_M2354_TIMERS_H

#include "m33mu/types.h"

struct mmio_bus;
struct mm_nvic;

void mm_m2354_timers_init(struct mmio_bus *bus, struct mm_nvic *nvic);
void mm_m2354_timers_reset(void);
void mm_m2354_timers_tick(mm_u64 cycles);

#endif /* M33MU_M2354_TIMERS_H */
