/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2025  Daniele Lacamera <root@danielinux.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 */

#ifndef M33MU_SCHEDULER_H
#define M33MU_SCHEDULER_H

#include "types.h"

/*
 * Cycle-count-based scheduler for deferred events.
 * Events are keyed by absolute cycle counts.
 */

struct mm_sched_event;

typedef void (*mm_sched_cb)(void *opaque, mm_u64 now_cycles);

struct mm_sched_event {
    mm_u64 due_cycle;
    mm_sched_cb cb;
    void *opaque;
    struct mm_sched_event *next;
};

struct mm_scheduler {
    struct mm_sched_event *head;
};

void mm_scheduler_init(struct mm_scheduler *sched);

/* Inserts event into the time-ordered list. Returns MM_FALSE on invalid input. */
mm_bool mm_scheduler_schedule(struct mm_scheduler *sched, struct mm_sched_event *ev);

/* Returns next due cycle; if no events, returns (mm_u64)-1. */
mm_u64 mm_scheduler_next_due(const struct mm_scheduler *sched);

/* Dispatches all events whose due_cycle <= now_cycles. */
void mm_scheduler_run_due(struct mm_scheduler *sched, mm_u64 now_cycles);

#endif /* M33MU_SCHEDULER_H */
