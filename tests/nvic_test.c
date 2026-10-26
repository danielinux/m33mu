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

#include <stdio.h>
#include <string.h>
#include "m33mu/nvic.h"
#include "m33mu/cpu.h"
#include "m33mu/scs.h"

static int test_pending_selection(void)
{
    struct mm_nvic nvic;
    struct mm_cpu cpu;
    int i;
    memset(&cpu, 0, sizeof(cpu));
    for (i = 0; i < 16; ++i) cpu.r[i] = 0;
    cpu.sec_state = MM_SECURE;
    cpu.primask_s = 0;
    cpu.primask_ns = 0;
    mm_nvic_init(&nvic);
    mm_nvic_set_enable(&nvic, 1, MM_TRUE);
    mm_nvic_set_enable(&nvic, 2, MM_TRUE);
    nvic.priority[1] = 0x10;
    nvic.priority[2] = 0x20;
    mm_nvic_set_pending(&nvic, 2, MM_TRUE);
    mm_nvic_set_pending(&nvic, 1, MM_TRUE);
    if (mm_nvic_select(&nvic, &cpu) != 1) return 1;
    return 0;
}

static int test_disable_blocks(void)
{
    struct mm_nvic nvic;
    struct mm_cpu cpu;
    int i;
    memset(&cpu, 0, sizeof(cpu));
    for (i = 0; i < 16; ++i) cpu.r[i] = 0;
    cpu.sec_state = MM_SECURE;
    cpu.primask_s = 0;
    cpu.primask_ns = 0;
    mm_nvic_init(&nvic);
    mm_nvic_set_enable(&nvic, 5, MM_TRUE);
    mm_nvic_set_pending(&nvic, 5, MM_TRUE);
    nvic.enable_mask[0] = 0; /* disable all */
    if (mm_nvic_select(&nvic, &cpu) != -1) return 1;
    return 0;
}

static int test_faultmask_blocks(void)
{
    struct mm_nvic nvic;
    struct mm_cpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.sec_state = MM_SECURE;
    cpu.faultmask_s = 1u;
    mm_nvic_init(&nvic);
    mm_nvic_set_enable(&nvic, 5u, MM_TRUE);
    mm_nvic_set_pending(&nvic, 5u, MM_TRUE);
    nvic.priority[5] = 0x20u;
    if (mm_nvic_select(&nvic, &cpu) != -1) return 1;
    return 0;
}

static int test_prigroup_masks_subpriority(void)
{
    struct mm_nvic nvic;
    struct mm_cpu cpu;
    struct mm_scs scs;
    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    cpu.sec_state = MM_SECURE;
    cpu.basepri_s = 0x20u;
    scs.aircr_s = (1u << 8); /* PRIGROUP=1 */
    mm_nvic_init(&nvic);
    mm_nvic_set_enable(&nvic, 9u, MM_TRUE);
    mm_nvic_set_pending(&nvic, 9u, MM_TRUE);
    nvic.priority[9] = 0x23u;
    if (mm_nvic_select_ex(&nvic, &cpu, &scs) != -1) return 1;
    return 0;
}

static int test_lower_priority_pending_irq_cannot_preempt_active_irq(void)
{
    struct mm_nvic nvic;
    struct mm_cpu cpu;
    struct mm_scs scs;
    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_HANDLER;
    cpu.xpsr = 17u; /* IRQ1 active */
    mm_nvic_init(&nvic);
    mm_nvic_set_enable(&nvic, 1u, MM_TRUE);
    mm_nvic_set_enable(&nvic, 2u, MM_TRUE);
    mm_nvic_set_active(&nvic, 1u, MM_TRUE);
    mm_nvic_set_pending(&nvic, 2u, MM_TRUE);
    nvic.priority[1] = 0x40u;
    nvic.priority[2] = 0xC0u;
    if (mm_nvic_select_ex(&nvic, &cpu, &scs) != -1) return 1;
    return 0;
}

static int test_higher_priority_pending_irq_can_preempt_active_irq(void)
{
    struct mm_nvic nvic;
    struct mm_cpu cpu;
    struct mm_scs scs;
    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_HANDLER;
    cpu.xpsr = 17u; /* IRQ1 active */
    mm_nvic_init(&nvic);
    mm_nvic_set_enable(&nvic, 1u, MM_TRUE);
    mm_nvic_set_enable(&nvic, 2u, MM_TRUE);
    mm_nvic_set_active(&nvic, 1u, MM_TRUE);
    mm_nvic_set_pending(&nvic, 2u, MM_TRUE);
    nvic.priority[1] = 0xC0u;
    nvic.priority[2] = 0x40u;
    if (mm_nvic_select_ex(&nvic, &cpu, &scs) != 2) return 1;
    return 0;
}

static int test_equal_group_priority_subpriority_cannot_preempt(void)
{
    struct mm_nvic nvic;
    struct mm_cpu cpu;
    struct mm_scs scs;
    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_HANDLER;
    cpu.xpsr = 17u; /* IRQ1 active */
    scs.aircr_s = (1u << 8); /* PRIGROUP=1 so subpriority bits exist */
    mm_nvic_init(&nvic);
    mm_nvic_set_enable(&nvic, 1u, MM_TRUE);
    mm_nvic_set_enable(&nvic, 2u, MM_TRUE);
    mm_nvic_set_active(&nvic, 1u, MM_TRUE);
    mm_nvic_set_pending(&nvic, 2u, MM_TRUE);
    nvic.priority[1] = 0x22u;
    nvic.priority[2] = 0x20u;
    if (mm_nvic_select_ex(&nvic, &cpu, &scs) != -1) return 1;
    return 0;
}

static int test_high_irq_state(void)
{
    static const mm_u32 irqs[] = { 143u, 149u, 161u, 180u, MM_MAX_IRQ - 1u };
    struct mm_nvic nvic;
    struct mm_cpu cpu;
    size_t i;
    memset(&cpu, 0, sizeof(cpu));
    cpu.sec_state = MM_SECURE;
    for (i = 0; i < sizeof(irqs) / sizeof(irqs[0]); ++i) {
        mm_u32 irq = irqs[i];
        mm_u32 word = irq / 32u;
        mm_u32 bit = 1u << (irq % 32u);
        mm_u32 w;
        mm_nvic_init(&nvic);
        mm_nvic_set_enable(&nvic, irq, MM_TRUE);
        mm_nvic_set_pending(&nvic, irq, MM_TRUE);
        nvic.priority[irq] = 0x40u;
        if (nvic.enable_mask[word] != bit || nvic.pending_mask[word] != bit) return 1;
        if (!mm_nvic_is_pending(&nvic, irq)) return 1;
        if (!mm_nvic_any_pending_enabled(&nvic)) return 1;
        if (mm_nvic_select(&nvic, &cpu) != (int)irq) return 1;
        mm_nvic_set_active(&nvic, irq, MM_TRUE);
        if (nvic.active_mask[word] != bit) return 1;
        if (mm_nvic_select(&nvic, &cpu) != -1) return 1;
        mm_nvic_set_active(&nvic, irq, MM_FALSE);
        mm_nvic_set_pending(&nvic, irq, MM_FALSE);
        mm_nvic_set_enable(&nvic, irq, MM_FALSE);
        for (w = 0; w < (MM_MAX_IRQ + 31u) / 32u; ++w) {
            if (nvic.enable_mask[w] != 0u || nvic.pending_mask[w] != 0u ||
                nvic.active_mask[w] != 0u) return 1;
        }
    }
    return 0;
}

static int test_high_irq_itns(void)
{
    struct {
        struct mm_nvic nvic;
        mm_u32 sentinel;
    } wrap;
    struct mm_nvic before;
    mm_nvic_init(&wrap.nvic);
    wrap.sentinel = 0xA5A5A5A5u;
    mm_nvic_set_itns(&wrap.nvic, 149u, MM_TRUE);
    if (mm_nvic_irq_target_sec(&wrap.nvic, 149u) != MM_NONSECURE) return 1;
    mm_nvic_set_itns(&wrap.nvic, 149u, MM_FALSE);
    if (mm_nvic_irq_target_sec(&wrap.nvic, 149u) != MM_SECURE) return 1;

    memcpy(&before, &wrap.nvic, sizeof(before));
    mm_nvic_set_enable(&wrap.nvic, MM_MAX_IRQ, MM_TRUE);
    mm_nvic_set_pending(&wrap.nvic, MM_MAX_IRQ, MM_TRUE);
    mm_nvic_set_itns(&wrap.nvic, MM_MAX_IRQ, MM_TRUE);
    mm_nvic_set_active(&wrap.nvic, MM_MAX_IRQ, MM_TRUE);
    if (mm_nvic_is_pending(&wrap.nvic, MM_MAX_IRQ)) return 1;
    if (mm_nvic_irq_target_sec(&wrap.nvic, MM_MAX_IRQ) != MM_SECURE) return 1;
    if (memcmp(&before, &wrap.nvic, sizeof(before)) != 0) return 1;
    if (wrap.sentinel != 0xA5A5A5A5u) return 1;
    return 0;
}

static int test_high_irq_priority_order(void)
{
    struct mm_nvic nvic;
    struct mm_cpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.sec_state = MM_SECURE;
    mm_nvic_init(&nvic);
    mm_nvic_set_enable(&nvic, 149u, MM_TRUE);
    mm_nvic_set_enable(&nvic, 161u, MM_TRUE);
    nvic.priority[149] = 0x80u;
    nvic.priority[161] = 0x20u;
    mm_nvic_set_pending(&nvic, 149u, MM_TRUE);
    mm_nvic_set_pending(&nvic, 161u, MM_TRUE);
    if (mm_nvic_select(&nvic, &cpu) != 161) return 1;
    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "pending_selection", test_pending_selection },
        { "disable_blocks", test_disable_blocks },
        { "faultmask_blocks", test_faultmask_blocks },
        { "prigroup_masks_subpriority", test_prigroup_masks_subpriority },
        { "lower_priority_pending_irq_cannot_preempt_active_irq", test_lower_priority_pending_irq_cannot_preempt_active_irq },
        { "higher_priority_pending_irq_can_preempt_active_irq", test_higher_priority_pending_irq_can_preempt_active_irq },
        { "equal_group_priority_subpriority_cannot_preempt", test_equal_group_priority_subpriority_cannot_preempt },
        { "high_irq_state", test_high_irq_state },
        { "high_irq_itns", test_high_irq_itns },
        { "high_irq_priority_order", test_high_irq_priority_order },
    };
    int failures = 0;
    int i;
    const int count = (int)(sizeof(tests) / sizeof(tests[0]));
    for (i = 0; i < count; ++i) {
        if (tests[i].fn() != 0) {
            ++failures;
            printf("FAIL: %s\n", tests[i].name);
        } else {
            printf("PASS: %s\n", tests[i].name);
        }
    }
    if (failures != 0) {
        printf("nvic_test: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
