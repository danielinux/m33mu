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

#ifndef M33MU_NVIC_H
#define M33MU_NVIC_H

#include "m33mu/types.h"
#include "m33mu/cpu.h"
#include "m33mu/mmio.h"

#define MM_MAX_IRQ 64

struct mm_nvic {
    mm_u32 enable_mask[(MM_MAX_IRQ + 31) / 32];
    mm_u32 pending_mask[(MM_MAX_IRQ + 31) / 32];
    mm_u32 active_mask[(MM_MAX_IRQ + 31) / 32];
    /* ITNS: 1 => targets Non-secure state, 0 => targets Secure state. */
    mm_u32 itns_mask[(MM_MAX_IRQ + 31) / 32];
    mm_u8 priority[MM_MAX_IRQ];
};

void mm_nvic_init(struct mm_nvic *nvic);
void mm_nvic_set_enable(struct mm_nvic *nvic, mm_u32 irq, mm_bool enable);
void mm_nvic_set_pending(struct mm_nvic *nvic, mm_u32 irq, mm_bool pending);
mm_bool mm_nvic_is_pending(const struct mm_nvic *nvic, mm_u32 irq);
void mm_nvic_set_itns(struct mm_nvic *nvic, mm_u32 irq, mm_bool target_nonsecure);
enum mm_sec_state mm_nvic_irq_target_sec(const struct mm_nvic *nvic, mm_u32 irq);

/* Simplified: select highest-priority pending enabled IRQ; returns -1 if none. */
int mm_nvic_select(const struct mm_nvic *nvic, const struct mm_cpu *cpu);

/* Select IRQ and report its target security state using ITNS. */
int mm_nvic_select_routed(const struct mm_nvic *nvic, const struct mm_cpu *cpu, enum mm_sec_state *target_sec_out);

#endif /* M33MU_NVIC_H */
