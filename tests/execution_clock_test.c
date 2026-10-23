/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "m33mu/execution_clock.h"

static mm_u64 timer_cycles;

static void timer_tick(mm_u64 cycles)
{
    timer_cycles += cycles;
}

int main(void)
{
    struct mm_scs scs;
    struct mm_target_cfg cfg;
    mm_u64 total = 7u;
    mm_u64 virtual_cycles = 11u;
    mm_u64 since_poll = 3u;

    memset(&cfg, 0, sizeof(cfg));
    cfg.timer_tick = timer_tick;
    mm_scs_init(&scs, 0u);
    timer_cycles = 0u;

    mm_execution_advance_cycles(&scs, &cfg, 0u, MM_FALSE, 0u,
                                &total, &virtual_cycles, &since_poll);
    if (total != 7u || virtual_cycles != 11u || since_poll != 3u || timer_cycles != 0u) {
        printf("execution_clock_test: zero-cycle advance changed state\n");
        return 1;
    }

    mm_execution_advance_cycles(&scs, &cfg, 13u, MM_FALSE, 0u,
                                &total, &virtual_cycles, &since_poll);
    if (total != 20u || virtual_cycles != 24u || since_poll != 16u || timer_cycles != 13u) {
        printf("execution_clock_test: cycle accounting mismatch\n");
        return 1;
    }

    mm_execution_advance_cycles(&scs, &cfg, 5u, MM_FALSE, 0u, 0, 0, 0);
    if (timer_cycles != 18u) {
        printf("execution_clock_test: optional counters blocked peripheral time\n");
        return 1;
    }

    printf("execution_clock_test: counters and timer advance\n");
    return 0;
}
