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

#include "m33mu/nvic.h"
#include "m33mu/execution_irq.h"
#include "m33mu/mmio.h"
#include "m33mu/scs.h"
#include "m33mu/vector.h"
#include <stdlib.h>
#include <stdio.h>

static mm_nvic_enable_hook_t g_enable_hook = 0;
static void *g_enable_hook_opaque = 0;
static int nvic_trace_level(void)
{
    static mm_bool init = MM_FALSE;
    static int level = 0;
    const char *env;
    if (init) {
        return level;
    }
    env = getenv("M33MU_NVIC_TRACE");
    if (env != 0 && env[0] != '\0') {
        level = 1;
    }
    init = MM_TRUE;
    return level;
}

static void clear_masks(struct mm_nvic *n)
{
    size_t i;
    for (i = 0; i < (MM_MAX_IRQ + 31) / 32; ++i) {
        n->enable_mask[i] = 0;
        n->pending_mask[i] = 0;
        n->active_mask[i] = 0;
        n->itns_mask[i] = 0;
    }
}

void mm_nvic_init(struct mm_nvic *nvic)
{
    size_t i;
    clear_masks(nvic);
    for (i = 0; i < MM_MAX_IRQ; ++i) {
        nvic->priority[i] = 0xffu;
    }
    nvic->lines = MM_MAX_IRQ;
}

void mm_nvic_set_lines(struct mm_nvic *nvic, mm_u32 lines)
{
    if (nvic == 0) return;
    if (lines == 0u || lines > MM_MAX_IRQ) lines = MM_MAX_IRQ;
    nvic->lines = lines;
}

mm_u32 mm_nvic_lines(const struct mm_nvic *nvic)
{
    if (nvic == 0 || nvic->lines == 0u || nvic->lines > MM_MAX_IRQ) return MM_MAX_IRQ;
    return nvic->lines;
}

mm_u32 mm_nvic_word_mask(const struct mm_nvic *nvic, mm_u32 word)
{
    mm_u32 first = word * 32u;
    mm_u32 lines = mm_nvic_lines(nvic);
    if (nvic == 0 || first >= lines) return 0u;
    if (lines - first >= 32u) return 0xffffffffu;
    return (1u << (lines - first)) - 1u;
}

static mm_bool bitop(mm_u32 *arr, mm_u32 idx, mm_bool set)
{
    mm_u32 word = idx / 32u;
    mm_u32 bit = idx % 32u;
    mm_u32 mask = 1u << bit;
    if (word >= (MM_MAX_IRQ + 31u) / 32u) {
        return MM_FALSE;
    }
    if (set) {
        arr[word] |= mask;
    } else {
        arr[word] &= ~mask;
    }
    return MM_TRUE;
}

void mm_nvic_set_enable(struct mm_nvic *nvic, mm_u32 irq, mm_bool enable)
{
    if (irq >= mm_nvic_lines(nvic)) return;
    bitop(nvic->enable_mask, irq, enable);
    if (g_enable_hook != 0) {
        g_enable_hook(irq, enable, g_enable_hook_opaque);
    }
}

void mm_nvic_set_pending(struct mm_nvic *nvic, mm_u32 irq, mm_bool pending)
{
    mm_u32 word;
    mm_u32 mask;
    if (nvic == 0) return;
    word = irq / 32u;
    mask = 1u << (irq % 32u);
    if (irq >= mm_nvic_lines(nvic)) return;
    if (pending) {
        nvic->pending_mask[word] |= mask;
    } else {
        nvic->pending_mask[word] &= ~mask;
    }
    if (nvic_trace_level() >= 1) {
        printf("[NVIC_PENDING_SET] irq=%lu pending=%u mask=0x%08lx\n",
               (unsigned long)irq,
               pending ? 1u : 0u,
               (unsigned long)nvic->pending_mask[word]);
    }
}

void mm_nvic_set_itns(struct mm_nvic *nvic, mm_u32 irq, mm_bool target_nonsecure)
{
    if (irq >= mm_nvic_lines(nvic)) return;
    bitop(nvic->itns_mask, irq, target_nonsecure);
}

enum mm_sec_state mm_nvic_irq_target_sec(const struct mm_nvic *nvic, mm_u32 irq)
{
    mm_u32 word = irq / 32u;
    mm_u32 bit = irq % 32u;
    mm_u32 mask = 1u << bit;
    if (nvic == 0) {
        return MM_SECURE;
    }
    if (word >= (MM_MAX_IRQ + 31u) / 32u) {
        return MM_SECURE;
    }
    return ((nvic->itns_mask[word] & mask) != 0u) ? MM_NONSECURE : MM_SECURE;
}

void mm_nvic_set_active(struct mm_nvic *nvic, mm_u32 irq, mm_bool active)
{
    mm_u32 word = irq / 32u;
    mm_u32 bit = irq % 32u;
    mm_u32 mask = 1u << bit;
    if (nvic == 0) return;
    if (irq >= mm_nvic_lines(nvic)) return;
    if (active) nvic->active_mask[word] |= mask;
    else nvic->active_mask[word] &= ~mask;
}

mm_bool mm_nvic_any_pending_enabled(const struct mm_nvic *nvic)
{
    size_t i;
    if (nvic == 0) return MM_FALSE;
    for (i = 0; i < (MM_MAX_IRQ + 31u) / 32u; ++i) {
        if ((nvic->pending_mask[i] & nvic->enable_mask[i]) != 0u) {
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

mm_bool mm_nvic_any_pending(const struct mm_nvic *nvic)
{
    size_t i;
    if (nvic == 0) return MM_FALSE;
    for (i = 0; i < (MM_MAX_IRQ + 31u) / 32u; ++i) {
        if (nvic->pending_mask[i] != 0u) {
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

void mm_nvic_set_enable_hook(mm_nvic_enable_hook_t hook, void *opaque)
{
    g_enable_hook = hook;
    g_enable_hook_opaque = opaque;
}

void mm_nvic_notify_enable_mask(mm_u32 idx, mm_u32 old_mask, mm_u32 new_mask)
{
    mm_u32 changed;
    mm_u32 bit;
    if (g_enable_hook == 0) return;
    changed = old_mask ^ new_mask;
    if (changed == 0u) return;
    for (bit = 0; bit < 32u; ++bit) {
        mm_u32 mask = 1u << bit;
        if ((changed & mask) != 0u) {
            mm_u32 irq = idx * 32u + bit;
            mm_bool enabled = (new_mask & mask) != 0u ? MM_TRUE : MM_FALSE;
            g_enable_hook(irq, enabled, g_enable_hook_opaque);
        }
    }
}

mm_bool mm_nvic_is_pending(const struct mm_nvic *nvic, mm_u32 irq)
{
    mm_u32 word = irq / 32u;
    mm_u32 bit = irq % 32u;
    mm_u32 mask = 1u << bit;
    if (word >= (MM_MAX_IRQ + 31u) / 32u) {
        return MM_FALSE;
    }
    return (nvic->pending_mask[word] & mask) != 0u;
}

static mm_u32 ctz32(mm_u32 v)
{
#if defined(__GNUC__) || defined(__clang__)
    return (mm_u32)__builtin_ctz(v);
#else
    static const mm_u32 debruijn = 0x077CB531u;
    static const mm_u32 tbl[32] = {
         0,  1, 28,  2, 29, 14, 24,  3, 30, 22, 20, 15, 25, 17,  4,  8,
        31, 27, 13, 23, 21, 19, 16,  7, 26, 12, 18,  6, 11,  5, 10,  9
    };
    return tbl[((v & (0u - v)) * debruijn) >> 27];
#endif
}

static mm_bool usb_trace_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("M33MU_USB_TRACE");
        cached = (v && v[0] != '\0') ? 1 : 0;
    }
    return cached ? MM_TRUE : MM_FALSE;
}

static void usb_trace_irq14_mask(mm_bool masked, enum mm_sec_state tsec, const struct mm_cpu *cpu)
{
    static mm_bool last_ns = MM_FALSE;
    static mm_bool last_s = MM_FALSE;
    mm_bool *last = (tsec == MM_NONSECURE) ? &last_ns : &last_s;
    if (!usb_trace_enabled()) return;
    if (masked) {
        if (!*last) {
            if (tsec == MM_NONSECURE) {
                fprintf(stderr, "[USB_TRACE] irq14 masked by PRIMASK_NS=%u\n",
                        cpu ? (unsigned)cpu->primask_ns : 0u);
            } else {
                fprintf(stderr, "[USB_TRACE] irq14 masked by PRIMASK_S=%u\n",
                        cpu ? (unsigned)cpu->primask_s : 0u);
            }
        }
        *last = MM_TRUE;
        return;
    }
    if (*last) {
        if (tsec == MM_NONSECURE) {
            fprintf(stderr, "[USB_TRACE] irq14 unmasked PRIMASK_NS=%u\n",
                    cpu ? (unsigned)cpu->primask_ns : 0u);
        } else {
            fprintf(stderr, "[USB_TRACE] irq14 unmasked PRIMASK_S=%u\n",
                    cpu ? (unsigned)cpu->primask_s : 0u);
        }
    }
    *last = MM_FALSE;
}

int mm_nvic_select_routed_ex(const struct mm_nvic *nvic,
                             const struct mm_cpu *cpu,
                             const struct mm_scs *scs,
                             enum mm_sec_state *target_sec_out)
{
    mm_u32 best_irq = 0xffffffffu;
    mm_u8 best_prio = 0xffu;
    mm_u32 word;

    for (word = 0; word < (MM_MAX_IRQ + 31u) / 32u; ++word) {
        mm_u32 candidates = nvic->pending_mask[word]
                          & nvic->enable_mask[word]
                          & ~nvic->active_mask[word];
        while (candidates != 0u) {
            mm_u32 bit_idx = ctz32(candidates);
            mm_u32 mask = 1u << bit_idx;
            mm_u32 irq = word * 32u + bit_idx;
            enum mm_sec_state tsec;
            candidates &= ~mask;
            tsec = mm_nvic_irq_target_sec(nvic, irq);
            if (mm_execution_irq_primask_blocks_for_sec(cpu, tsec)) {
                if (irq == 14u) {
                    usb_trace_irq14_mask(MM_TRUE, tsec, cpu);
                }
                continue;
            }
            if (mm_execution_irq_faultmask_blocks_for_sec(cpu, tsec)) {
                continue;
            }
            if (mm_execution_irq_basepri_blocks_priority(cpu, scs, tsec,
                                                         nvic->priority[irq])) {
                continue;
            }
            if (!mm_execution_irq_priority_can_preempt_current(cpu, scs, nvic,
                                                               nvic->priority[irq])) {
                continue;
            }
            if (irq == 14u) {
                usb_trace_irq14_mask(MM_FALSE, tsec, cpu);
            }
            if (best_irq == 0xffffffffu || nvic->priority[irq] < best_prio) {
                best_prio = nvic->priority[irq];
                best_irq = irq;
            }
        }
    }
    if (best_irq == 0xffffffffu) {
        return -1;
    }
    if (target_sec_out) {
        *target_sec_out = mm_nvic_irq_target_sec(nvic, best_irq);
    }
    if (nvic_trace_level() >= 1) {
        printf("[NVIC_SELECT] irq=%lu target_sec=%d prio=0x%02x\n",
               (unsigned long)best_irq,
               (int)mm_nvic_irq_target_sec(nvic, best_irq),
               (unsigned)nvic->priority[best_irq]);
    }
    return (int)best_irq;
}

int mm_nvic_select_routed(const struct mm_nvic *nvic, const struct mm_cpu *cpu, enum mm_sec_state *target_sec_out)
{
    return mm_nvic_select_routed_ex(nvic, cpu, 0, target_sec_out);
}

int mm_nvic_select(const struct mm_nvic *nvic, const struct mm_cpu *cpu)
{
    return mm_nvic_select_ex(nvic, cpu, 0);
}

int mm_nvic_select_ex(const struct mm_nvic *nvic, const struct mm_cpu *cpu, const struct mm_scs *scs)
{
    return mm_nvic_select_routed_ex(nvic, cpu, scs, 0);
}

struct mm_nvic_mmio {
    struct mm_nvic *nvic;
};

static mm_bool nvic_mmio_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct mm_nvic_mmio *ctx = (struct mm_nvic_mmio *)opaque;
    if (ctx == 0 || ctx->nvic == 0 || value_out == 0) {
        return MM_FALSE;
    }
    /* Only support IPR byte/word reads; map 0xE000E400.. */
    if (offset >= 0x100u) {
        return MM_FALSE;
    }
    if (size_bytes == 1u) {
        mm_u32 idx = offset;
        if (idx >= MM_MAX_IRQ) {
            *value_out = 0xFFu;
            return MM_TRUE;
        }
        *value_out = ctx->nvic->priority[idx] & 0xFFu;
        return MM_TRUE;
    }
    if (size_bytes == 4u && (offset % 4u) == 0u) {
        mm_u32 i;
        mm_u32 val = 0;
        for (i = 0; i < 4u; ++i) {
            mm_u32 idx = offset + i;
            mm_u8 p = 0xFFu;
            if (idx < MM_MAX_IRQ) {
                p = ctx->nvic->priority[idx];
            }
            val |= ((mm_u32)p) << (i * 8u);
        }
        *value_out = val;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool nvic_mmio_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct mm_nvic_mmio *ctx = (struct mm_nvic_mmio *)opaque;
    if (ctx == 0 || ctx->nvic == 0) {
        return MM_FALSE;
    }
    if (offset >= 0x100u) {
        return MM_FALSE;
    }
    if (size_bytes == 1u) {
        mm_u32 idx = offset;
        if (idx < MM_MAX_IRQ) {
            ctx->nvic->priority[idx] = (mm_u8)(value & 0xFFu);
        }
        return MM_TRUE;
    }
    if (size_bytes == 4u && (offset % 4u) == 0u) {
        mm_u32 i;
        for (i = 0; i < 4u; ++i) {
            mm_u32 idx = offset + i;
            mm_u8 p = (mm_u8)((value >> (i * 8u)) & 0xFFu);
            if (idx < MM_MAX_IRQ) {
                ctx->nvic->priority[idx] = p;
            }
        }
        return MM_TRUE;
    }
    return MM_FALSE;
}

mm_bool mm_nvic_register_mmio(struct mm_nvic *nvic, struct mmio_bus *bus)
{
    static struct mm_nvic_mmio ctx;
    struct mmio_region reg;

    ctx.nvic = nvic;
    reg.base = 0xE000E400u;
    reg.size = 0x100u;
    reg.opaque = &ctx;
    reg.read = nvic_mmio_read;
    reg.write = nvic_mmio_write;
    return mmio_bus_register_region(bus, &reg);
}
