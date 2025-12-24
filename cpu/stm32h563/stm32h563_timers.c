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

#include <string.h>
#include "stm32h563/stm32h563_timers.h"
#include "stm32h563/stm32h563_mmio.h"

#define TIM_CR1  0x00u
#define TIM_DIER 0x0Cu
#define TIM_SR   0x10u
#define TIM_EGR  0x14u
#define TIM_CNT  0x24u
#define TIM_PSC  0x28u
#define TIM_ARR  0x2Cu

#define CR1_CEN  (1u << 0)
#define CR1_UDIS (1u << 1)
#define CR1_OPM  (1u << 3)
#define CR1_DIR  (1u << 4)

#define DIER_UIE (1u << 0)
#define SR_UIF   (1u << 0)
#define EGR_UG   (1u << 0)

struct tim_inst {
    mm_u32 base;
    mm_u32 cr1;
    mm_u32 dier;
    mm_u32 sr;
    mm_u32 cnt;
    mm_u32 psc;
    mm_u32 arr;
    mm_u32 arr_mask;
    mm_u64 psc_accum;
    int irq;
    mm_u32 *rcc_regs;
    mm_u32 *sec_reg;
    mm_u32 sec_bitmask;
    mm_bool secure_only;
    enum mm_sec_state current_sec;
};

static struct tim_inst timers[4];
static struct mm_nvic *g_nvic = 0;

static mm_u32 read_slice(mm_u32 reg, mm_u32 offset_in_reg, mm_u32 size_bytes)
{
    mm_u32 shift = offset_in_reg * 8u;
    mm_u32 mask = (size_bytes == 4u) ? 0xFFFFFFFFu : ((1u << (size_bytes * 8u)) - 1u);
    return (reg >> shift) & mask;
}

static mm_u32 apply_write(mm_u32 cur, mm_u32 offset_in_reg, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 shift = offset_in_reg * 8u;
    mm_u32 mask = (size_bytes == 4u) ? 0xFFFFFFFFu : ((1u << (size_bytes * 8u)) - 1u);
    mm_u32 shifted = (value & mask) << shift;
    return (cur & ~(mask << shift)) | shifted;
}

static mm_bool tim_clock_enabled(const struct tim_inst *t)
{
    mm_u32 apb1lenr;
    mm_u32 bit;
    if (t->rcc_regs == 0) return MM_TRUE;
    apb1lenr = t->rcc_regs[0x9c / 4];
    bit = (mm_u32)(t - timers);
    if (bit > 3u) return MM_TRUE;
    return ((apb1lenr >> bit) & 1u) != 0u;
}

static mm_bool tim_access_allowed(struct tim_inst *t)
{
    t->current_sec = mmio_active_sec();
    t->secure_only = (t->sec_reg != 0 && ((*(t->sec_reg)) & t->sec_bitmask) != 0u);
    if (t->secure_only && t->current_sec == MM_NONSECURE) {
        return MM_FALSE;
    }
    return MM_TRUE;
}

static void tim_raise_update(struct tim_inst *t)
{
    if ((t->cr1 & CR1_UDIS) != 0u) {
        return;
    }
    t->sr |= SR_UIF;
    if (g_nvic != 0 && (t->dier & DIER_UIE) != 0u) {
        mm_nvic_set_pending(g_nvic, (mm_u32)t->irq, MM_TRUE);
    }
    if ((t->cr1 & CR1_OPM) != 0u) {
        t->cr1 &= ~CR1_CEN;
    }
}

static void tim_apply_tick_up(struct tim_inst *t, mm_u64 ticks)
{
    mm_u64 cnt;
    mm_u64 arr;
    mm_u64 period;
    mm_u64 wraps = 0;
    mm_u64 to_overflow;
    mm_u64 remaining;
    if (ticks == 0u) return;
    arr = (mm_u64)(t->arr & t->arr_mask);
    cnt = (mm_u64)(t->cnt & t->arr_mask);
    period = arr + 1u;
    if (period == 0u) {
        return;
    }
    if (cnt > arr) {
        cnt = arr;
    }
    to_overflow = arr - cnt + 1u;
    if (ticks >= to_overflow) {
        wraps = 1u;
        remaining = ticks - to_overflow;
        if (remaining >= period) {
            wraps += remaining / period;
            remaining = remaining % period;
        }
        cnt = remaining;
    } else {
        cnt += ticks;
    }
    t->cnt = (mm_u32)(cnt & t->arr_mask);
    if (wraps != 0u) {
        tim_raise_update(t);
    }
}

static void tim_apply_tick_down(struct tim_inst *t, mm_u64 ticks)
{
    mm_u64 cnt;
    mm_u64 arr;
    mm_u64 period;
    mm_u64 wraps = 0;
    mm_u64 to_underflow;
    mm_u64 remaining;
    if (ticks == 0u) return;
    arr = (mm_u64)(t->arr & t->arr_mask);
    cnt = (mm_u64)(t->cnt & t->arr_mask);
    period = arr + 1u;
    if (period == 0u) {
        return;
    }
    if (cnt > arr) {
        cnt = arr;
    }
    to_underflow = cnt + 1u;
    if (ticks >= to_underflow) {
        wraps = 1u;
        remaining = ticks - to_underflow;
        if (remaining >= period) {
            wraps += remaining / period;
            remaining = remaining % period;
        }
        cnt = (remaining == 0u) ? arr : (arr - remaining);
    } else {
        cnt -= ticks;
    }
    t->cnt = (mm_u32)(cnt & t->arr_mask);
    if (wraps != 0u) {
        tim_raise_update(t);
    }
}

static void tim_tick(struct tim_inst *t, mm_u64 cycles)
{
    mm_u64 total;
    mm_u64 ticks;
    mm_u64 div;
    if ((t->cr1 & CR1_CEN) == 0u) return;
    if (!tim_clock_enabled(t)) return;
    div = (mm_u64)(t->psc + 1u);
    if (div == 0u) return;
    total = t->psc_accum + cycles;
    ticks = total / div;
    t->psc_accum = total % div;
    if ((t->cr1 & CR1_DIR) != 0u) {
        tim_apply_tick_down(t, ticks);
    } else {
        tim_apply_tick_up(t, ticks);
    }
}

static mm_bool tim_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct tim_inst *t = (struct tim_inst *)opaque;
    mm_u32 reg = 0;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (!tim_access_allowed(t)) {
        *value_out = 0u;
        return MM_TRUE;
    }
    if (offset >= 0x400u) return MM_FALSE;

    if ((offset + size_bytes) <= (TIM_CR1 + 4u)) {
        reg = t->cr1 & 0xFFFFu;
        *value_out = read_slice(reg, offset - TIM_CR1, size_bytes);
        return MM_TRUE;
    }
    if (offset >= TIM_DIER && (offset + size_bytes) <= (TIM_DIER + 4u)) {
        reg = t->dier & 0xFFFFu;
        *value_out = read_slice(reg, offset - TIM_DIER, size_bytes);
        return MM_TRUE;
    }
    if (offset >= TIM_SR && (offset + size_bytes) <= (TIM_SR + 4u)) {
        reg = t->sr & 0xFFFFu;
        *value_out = read_slice(reg, offset - TIM_SR, size_bytes);
        return MM_TRUE;
    }
    if (offset >= TIM_EGR && (offset + size_bytes) <= (TIM_EGR + 4u)) {
        *value_out = 0u;
        return MM_TRUE;
    }
    if (offset >= TIM_CNT && (offset + size_bytes) <= (TIM_CNT + 4u)) {
        reg = t->cnt & t->arr_mask;
        *value_out = read_slice(reg, offset - TIM_CNT, size_bytes);
        return MM_TRUE;
    }
    if (offset >= TIM_PSC && (offset + size_bytes) <= (TIM_PSC + 4u)) {
        reg = t->psc & 0xFFFFu;
        *value_out = read_slice(reg, offset - TIM_PSC, size_bytes);
        return MM_TRUE;
    }
    if (offset >= TIM_ARR && (offset + size_bytes) <= (TIM_ARR + 4u)) {
        reg = t->arr & t->arr_mask;
        *value_out = read_slice(reg, offset - TIM_ARR, size_bytes);
        return MM_TRUE;
    }

    *value_out = 0u;
    return MM_TRUE;
}

static mm_bool tim_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct tim_inst *t = (struct tim_inst *)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (!tim_access_allowed(t)) {
        return MM_TRUE;
    }
    if (offset >= 0x400u) return MM_FALSE;

    if ((offset + size_bytes) <= (TIM_CR1 + 4u)) {
        t->cr1 = apply_write(t->cr1, offset - TIM_CR1, size_bytes, value) & 0xFFFFu;
        return MM_TRUE;
    }
    if (offset >= TIM_DIER && (offset + size_bytes) <= (TIM_DIER + 4u)) {
        t->dier = apply_write(t->dier, offset - TIM_DIER, size_bytes, value) & 0xFFFFu;
        return MM_TRUE;
    }
    if (offset >= TIM_SR && (offset + size_bytes) <= (TIM_SR + 4u)) {
        mm_u32 shift = (offset - TIM_SR) * 8u;
        mm_u32 mask = (size_bytes == 4u) ? 0xFFFFFFFFu : ((1u << (size_bytes * 8u)) - 1u);
        mm_u32 write_mask = mask << shift;
        mm_u32 value_shifted = (value & mask) << shift;
        mm_u32 clear_bits = write_mask & ~value_shifted;
        t->sr &= ~clear_bits;
        return MM_TRUE;
    }
    if (offset >= TIM_EGR && (offset + size_bytes) <= (TIM_EGR + 4u)) {
        mm_u32 egr = apply_write(0u, offset - TIM_EGR, size_bytes, value);
        if ((egr & EGR_UG) != 0u) {
            t->cnt = 0u;
            t->psc_accum = 0u;
            tim_raise_update(t);
        }
        return MM_TRUE;
    }
    if (offset >= TIM_CNT && (offset + size_bytes) <= (TIM_CNT + 4u)) {
        t->cnt = apply_write(t->cnt, offset - TIM_CNT, size_bytes, value) & t->arr_mask;
        return MM_TRUE;
    }
    if (offset >= TIM_PSC && (offset + size_bytes) <= (TIM_PSC + 4u)) {
        t->psc = apply_write(t->psc, offset - TIM_PSC, size_bytes, value) & 0xFFFFu;
        return MM_TRUE;
    }
    if (offset >= TIM_ARR && (offset + size_bytes) <= (TIM_ARR + 4u)) {
        t->arr = apply_write(t->arr, offset - TIM_ARR, size_bytes, value) & t->arr_mask;
        return MM_TRUE;
    }

    return MM_TRUE;
}

void mm_stm32h563_timers_tick(mm_u64 cycles)
{
    size_t i;
    for (i = 0; i < sizeof(timers) / sizeof(timers[0]); ++i) {
        tim_tick(&timers[i], cycles);
    }
}

void mm_stm32h563_timers_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    static const mm_u32 bases[] = {
        0x40000000u, 0x40000400u, 0x40000800u, 0x40000C00u
    };
    static const int irq_map[] = { 45, 46, 47, 48 };
    mm_u32 *tz = mm_stm32h563_tzsc_regs();
    mm_u32 *tz1 = tz != 0 ? tz + (0x10u / 4u) : 0;
    size_t i;
    g_nvic = nvic;
    for (i = 0; i < sizeof(timers) / sizeof(timers[0]); ++i) {
        struct tim_inst *t = &timers[i];
        struct mmio_region reg;
        memset(t, 0, sizeof(*t));
        t->base = bases[i];
        t->irq = irq_map[i];
        t->arr_mask = (i == 0 || i == 3) ? 0xFFFFFFFFu : 0xFFFFu;
        t->arr = t->arr_mask;
        t->cnt = 0u;
        t->psc = 0u;
        t->psc_accum = 0u;
        t->rcc_regs = mm_stm32h563_rcc_regs();
        t->sec_reg = tz1;
        t->sec_bitmask = (1u << i);
        t->current_sec = MM_SECURE;

        reg.base = bases[i];
        reg.size = 0x400u;
        reg.opaque = t;
        reg.read = tim_read;
        reg.write = tim_write;
        mmio_bus_register_region(bus, &reg);

        reg.base = bases[i] + 0x10000000u;
        mmio_bus_register_region(bus, &reg);
    }
}

void mm_stm32h563_timers_reset(void)
{
    size_t i;
    for (i = 0; i < sizeof(timers) / sizeof(timers[0]); ++i) {
        memset(&timers[i], 0, sizeof(timers[i]));
    }
    g_nvic = 0;
}
