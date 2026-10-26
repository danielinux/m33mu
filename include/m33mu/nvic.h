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

/* Architectural maximum for ARMv8-M (ICTR.INTLINESNUM = 14). A target
 * implements fewer lines with mm_nvic_set_lines(). */
#define MM_MAX_IRQ 480

struct mm_scs;

struct mm_nvic {
    mm_u32 enable_mask[(MM_MAX_IRQ + 31) / 32];
    mm_u32 pending_mask[(MM_MAX_IRQ + 31) / 32];
    mm_u32 active_mask[(MM_MAX_IRQ + 31) / 32];
    /* ITNS: 1 => targets Non-secure state, 0 => targets Secure state. */
    mm_u32 itns_mask[(MM_MAX_IRQ + 31) / 32];
    mm_u8 priority[MM_MAX_IRQ];
    /* Implemented interrupt lines (0 = MM_MAX_IRQ); lines at or above it
     * are RAZ/WI. */
    mm_u32 lines;
};

typedef void (*mm_nvic_enable_hook_t)(mm_u32 irq, mm_bool enable, void *opaque);

void mm_nvic_init(struct mm_nvic *nvic);
void mm_nvic_set_lines(struct mm_nvic *nvic, mm_u32 lines);
mm_u32 mm_nvic_lines(const struct mm_nvic *nvic);
/* Bits of 32-line register word `word` that map to implemented lines. */
mm_u32 mm_nvic_word_mask(const struct mm_nvic *nvic, mm_u32 word);
void mm_nvic_set_enable(struct mm_nvic *nvic, mm_u32 irq, mm_bool enable);
void mm_nvic_set_pending(struct mm_nvic *nvic, mm_u32 irq, mm_bool pending);
mm_bool mm_nvic_is_pending(const struct mm_nvic *nvic, mm_u32 irq);
void mm_nvic_set_itns(struct mm_nvic *nvic, mm_u32 irq, mm_bool target_nonsecure);
enum mm_sec_state mm_nvic_irq_target_sec(const struct mm_nvic *nvic, mm_u32 irq);
void mm_nvic_set_active(struct mm_nvic *nvic, mm_u32 irq, mm_bool active);
mm_bool mm_nvic_any_pending_enabled(const struct mm_nvic *nvic);
mm_bool mm_nvic_any_pending(const struct mm_nvic *nvic);
void mm_nvic_set_enable_hook(mm_nvic_enable_hook_t hook, void *opaque);
void mm_nvic_notify_enable_mask(mm_u32 idx, mm_u32 old_mask, mm_u32 new_mask);

/* Simplified: select highest-priority pending enabled IRQ; returns -1 if none. */
int mm_nvic_select(const struct mm_nvic *nvic, const struct mm_cpu *cpu);
int mm_nvic_select_ex(const struct mm_nvic *nvic, const struct mm_cpu *cpu, const struct mm_scs *scs);

/* Select IRQ and report its target security state using ITNS. */
int mm_nvic_select_routed(const struct mm_nvic *nvic, const struct mm_cpu *cpu, enum mm_sec_state *target_sec_out);
int mm_nvic_select_routed_ex(const struct mm_nvic *nvic,
                             const struct mm_cpu *cpu,
                             const struct mm_scs *scs,
                             enum mm_sec_state *target_sec_out);

#endif /* M33MU_NVIC_H */
