/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <string.h>

#include "m33mu/execution_clock.h"
#include "m33mu/signal.h"
#include "m33mu/timer.h"

void mm_execution_clock_reset(struct mm_execution_clock *clock)
{
    if (clock != 0) {
        memset(clock, 0, sizeof(*clock));
    }
}

void mm_execution_clock_advance(struct mm_scs *scs,
                                const struct mm_target_cfg *cfg,
                                mm_u64 cycles,
                                mm_bool check_signal_trigger,
                                mm_u32 trigger_pc,
                                struct mm_execution_clock *clock)
{
    if (clock == 0) {
        return;
    }
    mm_execution_advance_cycles(scs, cfg, cycles, check_signal_trigger,
                                trigger_pc, &clock->cycle_total,
                                &clock->virtual_cycles,
                                &clock->cycles_since_poll);
}

void mm_execution_advance_cycles(struct mm_scs *scs,
                                 const struct mm_target_cfg *cfg,
                                 mm_u64 cycles,
                                 mm_bool check_signal_trigger,
                                 mm_u32 trigger_pc,
                                 mm_u64 *cycle_total,
                                 mm_u64 *virtual_cycles,
                                 mm_u64 *cycles_since_poll)
{
    if (cycles == 0u) {
        return;
    }
    if (cycle_total != 0) {
        *cycle_total += cycles;
    }
    if (virtual_cycles != 0) {
        *virtual_cycles += cycles;
    }
    if (cycles_since_poll != 0) {
        *cycles_since_poll += cycles;
    }
    mm_scs_systick_advance(scs, cycles);
    mm_timer_tick(cfg, cycles);
    if (check_signal_trigger) {
        mm_signal_check_trigger(trigger_pc);
    }
    mm_signal_advance(cycles);
}
