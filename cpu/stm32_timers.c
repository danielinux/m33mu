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
#include "stm32_timers.h"

#define TIM_CR1   0x00u
#define TIM_SMCR  0x08u
#define TIM_DIER  0x0Cu
#define TIM_SR    0x10u
#define TIM_EGR   0x14u
#define TIM_CCMR1 0x18u
#define TIM_CCMR2 0x1Cu
#define TIM_CCER  0x20u
#define TIM_CNT   0x24u
#define TIM_PSC   0x28u
#define TIM_ARR   0x2Cu
#define TIM_CCR1  0x34u
#define TIM_CCR4  0x40u

#define CR1_CEN  (1u << 0)
#define CR1_UDIS (1u << 1)
#define CR1_URS  (1u << 2)
#define CR1_OPM  (1u << 3)
#define CR1_DIR  (1u << 4)

#define DIER_UIE (1u << 0)
#define DIER_TIE (1u << 6)
#define SR_UIF   (1u << 0)
#define SR_TIF   (1u << 6)
#define EGR_UG   (1u << 0)
#define EGR_TG   (1u << 6)

#define CC_BIT(c)  (1u << (c))        /* CCxIE, CCxIF, CCxG for c = 1..4 */
#define CC_OF(c)   (1u << ((c) + 8u)) /* CCxOF */

#define CCS_OUTPUT   0u
#define CCS_DIRECT   1u
#define CCS_INDIRECT 2u

#define SMS_RESET    4u
#define TS_TI1FP1    5u
#define TS_TI2FP2    6u

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

static mm_bool tim_clock_enabled(const struct stm32_timer_inst *t)
{
    mm_u32 apb1lenr;
    if (t->rcc_regs == 0 || t->owner == 0 || t->owner->soc == 0) return MM_TRUE;
    apb1lenr = t->rcc_regs[t->owner->soc->apb1enr_offset / 4u];
    if (t->index > 3u) return MM_TRUE;
    return ((apb1lenr >> t->index) & 1u) != 0u;
}

static mm_bool tim_access_allowed(struct stm32_timer_inst *t)
{
    t->current_sec = mmio_active_sec();
    t->secure_only = (t->sec_reg != 0 && ((*(t->sec_reg)) & t->sec_bitmask) != 0u);
    if (t->secure_only && t->current_sec == MM_NONSECURE) {
        return MM_FALSE;
    }
    return MM_TRUE;
}

static void tim_raise_update(struct stm32_timer_inst *t)
{
    if ((t->cr1 & CR1_UDIS) != 0u) {
        return;
    }
    t->sr |= SR_UIF;
    if (t->owner != 0 && t->owner->nvic != 0 && (t->dier & DIER_UIE) != 0u) {
        mm_nvic_set_pending(t->owner->nvic, (mm_u32)t->irq, MM_TRUE);
    }
    if ((t->cr1 & CR1_OPM) != 0u) {
        t->cr1 &= ~CR1_CEN;
    }
}

static void tim_apply_tick_up(struct stm32_timer_inst *t, mm_u64 ticks)
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

static void tim_apply_tick_down(struct stm32_timer_inst *t, mm_u64 ticks)
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

static void tim_tick(struct stm32_timer_inst *t, mm_u64 cycles)
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

static void tim_flag(struct stm32_timer_inst *t, mm_u32 sr_bits, mm_u32 dier_bit)
{
    t->sr |= sr_bits;
    if (t->owner != 0 && t->owner->nvic != 0 && (t->dier & dier_bit) != 0u) {
        mm_nvic_set_pending(t->owner->nvic, (mm_u32)t->irq, MM_TRUE);
    }
}

static mm_u32 tim_ccs(const struct stm32_timer_inst *t, int c)
{
    return (t->ccmr[(c - 1) / 2] >> (8 * ((c - 1) % 2))) & 0x3u;
}

static mm_u32 tim_icpsc(const struct stm32_timer_inst *t, int c)
{
    return (t->ccmr[(c - 1) / 2] >> (8 * ((c - 1) % 2) + 2)) & 0x3u;
}

/* The TIx input a capture channel listens to: its own, or its pair's. */
static int tim_capture_source(const struct stm32_timer_inst *t, int c)
{
    mm_u32 ccs = tim_ccs(t, c);
    if (ccs == CCS_DIRECT) {
        return c;
    }
    if (ccs == CCS_INDIRECT) {
        return ((c - 1) ^ 1) + 1;
    }
    return 0;
}

/* CCxP/CCxNP: 00 rising, 01 falling, 11 both edges. */
static mm_bool tim_edge_selected(const struct stm32_timer_inst *t, int c, mm_bool rising)
{
    mm_u32 p = (t->ccer >> (4 * (c - 1) + 1)) & 1u;
    mm_u32 np = (t->ccer >> (4 * (c - 1) + 3)) & 1u;
    if (p != 0u && np != 0u) {
        return MM_TRUE;
    }
    return (p != 0u) ? !rising : rising;
}

static mm_u64 tim_period(const struct stm32_timer_inst *t)
{
    return (mm_u64)(t->arr & t->arr_mask) + 1u;
}

/* Counter ticks in the last late_cycles; the last tick was psc_accum cycles ago. */
static mm_u64 tim_ticks_since(const struct stm32_timer_inst *t, mm_u64 late_cycles)
{
    if ((t->cr1 & CR1_CEN) == 0u || !tim_clock_enabled(t) || late_cycles <= t->psc_accum) {
        return 0u;
    }
    return 1u + (late_cycles - t->psc_accum - 1u) / ((mm_u64)t->psc + 1u);
}

static mm_u32 tim_count_at(const struct stm32_timer_inst *t, mm_u64 late_cycles)
{
    mm_u64 period = tim_period(t);
    mm_u64 cnt = (mm_u64)(t->cnt & t->arr_mask);
    mm_u64 back = tim_ticks_since(t, late_cycles) % period;
    if ((t->cr1 & CR1_DIR) != 0u) {
        return (mm_u32)((cnt + back) % period);
    }
    return (mm_u32)((cnt + period - back) % period);
}

static void tim_capture(struct stm32_timer_inst *t, int c, mm_u32 value)
{
    t->ccr[c - 1] = value & t->arr_mask;
    if ((t->sr & CC_BIT(c)) != 0u) {
        t->sr |= CC_OF(c);
    }
    tim_flag(t, CC_BIT(c), CC_BIT(c));
}

/* Slave reset mode: the trigger edge restarts counter and prescaler. */
static void tim_slave_reset(struct stm32_timer_inst *t, mm_u64 late_cycles)
{
    mm_u64 div = (mm_u64)t->psc + 1u;
    mm_u64 period = tim_period(t);
    mm_u64 ticks = 0u;
    mm_u64 arr = (mm_u64)(t->arr & t->arr_mask);

    t->psc_accum = 0u;
    if ((t->cr1 & CR1_CEN) != 0u && tim_clock_enabled(t)) {
        ticks = (late_cycles / div) % period;
        t->psc_accum = late_cycles % div;
    }
    t->cnt = (mm_u32)(((t->cr1 & CR1_DIR) != 0u) ? (arr + period - ticks) % period : ticks);
    if ((t->cr1 & (CR1_URS | CR1_UDIS)) == 0u) {
        tim_flag(t, SR_UIF, DIER_UIE);
    }
    tim_flag(t, SR_TIF, DIER_TIE);
}

void stm32_timers_input_edge(struct stm32_timers_state *state, mm_u32 tim, int ti,
                             mm_bool rising, mm_u64 late_cycles)
{
    struct stm32_timer_inst *t;
    mm_u32 sms;
    mm_u32 ts;
    int c;

    if (state == 0 || tim >= sizeof(state->timers) / sizeof(state->timers[0]) ||
        ti < 1 || ti > 4) {
        return;
    }
    t = &state->timers[tim];
    if (!tim_clock_enabled(t)) {
        return;
    }
    for (c = 1; c <= 4; ++c) {
        mm_u32 div;
        if (tim_capture_source(t, c) != ti || (t->ccer & (1u << (4 * (c - 1)))) == 0u ||
            !tim_edge_selected(t, c, rising)) {
            continue;
        }
        div = 1u << tim_icpsc(t, c);
        if (++t->icpsc_count[c - 1] < div) {
            continue;
        }
        t->icpsc_count[c - 1] = 0u;
        tim_capture(t, c, tim_count_at(t, late_cycles));
    }
    sms = (t->smcr & 0x7u) | (((t->smcr >> 16) & 0x1u) << 3);
    ts = ((t->smcr >> 4) & 0x7u) | (((t->smcr >> 20) & 0x3u) << 3);
    if (sms == SMS_RESET && ((ts == TS_TI1FP1 && ti == 1) || (ts == TS_TI2FP2 && ti == 2)) &&
        tim_edge_selected(t, ti, rising)) {
        tim_slave_reset(t, late_cycles);
    }
}

static mm_bool tim_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct stm32_timer_inst *t = (struct stm32_timer_inst *)opaque;
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
    if (offset >= TIM_SMCR && (offset + size_bytes) <= (TIM_SMCR + 4u)) {
        *value_out = read_slice(t->smcr, offset - TIM_SMCR, size_bytes);
        return MM_TRUE;
    }
    if (offset >= TIM_CCMR1 && (offset + size_bytes) <= (TIM_CCMR2 + 4u)) {
        mm_u32 i = (offset - TIM_CCMR1) / 4u;
        *value_out = read_slice(t->ccmr[i], offset - TIM_CCMR1 - i * 4u, size_bytes);
        return MM_TRUE;
    }
    if (offset >= TIM_CCER && (offset + size_bytes) <= (TIM_CCER + 4u)) {
        *value_out = read_slice(t->ccer, offset - TIM_CCER, size_bytes);
        return MM_TRUE;
    }
    if (offset >= TIM_CCR1 && (offset + size_bytes) <= (TIM_CCR4 + 4u)) {
        int c = (int)((offset - TIM_CCR1) / 4u) + 1;
        *value_out = read_slice(t->ccr[c - 1] & t->arr_mask,
                                offset - TIM_CCR1 - (mm_u32)(c - 1) * 4u, size_bytes);
        /* Reading the captured value acknowledges the capture. */
        if (tim_ccs(t, c) != CCS_OUTPUT && !mmio_peek_mode()) {
            t->sr &= ~CC_BIT(c);
        }
        return MM_TRUE;
    }

    *value_out = 0u;
    return MM_TRUE;
}

static mm_bool tim_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct stm32_timer_inst *t = (struct stm32_timer_inst *)opaque;
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
        int c;
        for (c = 1; c <= 4; ++c) {
            if ((egr & CC_BIT(c)) == 0u) {
                continue;
            }
            if (tim_ccs(t, c) != CCS_OUTPUT) {
                tim_capture(t, c, t->cnt);
            } else {
                tim_flag(t, CC_BIT(c), CC_BIT(c));
            }
        }
        if ((egr & EGR_TG) != 0u) {
            tim_flag(t, SR_TIF, DIER_TIE);
        }
        if ((egr & EGR_UG) != 0u) {
            if ((t->cr1 & CR1_DIR) != 0u) {
                t->cnt = t->arr & t->arr_mask;
            } else {
                t->cnt = 0u;
            }
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
    if (offset >= TIM_SMCR && (offset + size_bytes) <= (TIM_SMCR + 4u)) {
        t->smcr = apply_write(t->smcr, offset - TIM_SMCR, size_bytes, value);
        return MM_TRUE;
    }
    if (offset >= TIM_CCMR1 && (offset + size_bytes) <= (TIM_CCMR2 + 4u)) {
        mm_u32 i = (offset - TIM_CCMR1) / 4u;
        t->ccmr[i] = apply_write(t->ccmr[i], offset - TIM_CCMR1 - i * 4u, size_bytes, value);
        return MM_TRUE;
    }
    if (offset >= TIM_CCER && (offset + size_bytes) <= (TIM_CCER + 4u)) {
        t->ccer = apply_write(t->ccer, offset - TIM_CCER, size_bytes, value) & 0xFFFFu;
        return MM_TRUE;
    }
    if (offset >= TIM_CCR1 && (offset + size_bytes) <= (TIM_CCR4 + 4u)) {
        int c = (int)((offset - TIM_CCR1) / 4u) + 1;
        /* CCRx is read-only while the channel captures. */
        if (tim_ccs(t, c) == CCS_OUTPUT) {
            t->ccr[c - 1] = apply_write(t->ccr[c - 1], offset - TIM_CCR1 - (mm_u32)(c - 1) * 4u,
                                        size_bytes, value) & t->arr_mask;
        }
        return MM_TRUE;
    }

    return MM_TRUE;
}

void stm32_timers_tick(struct stm32_timers_state *state, mm_u64 cycles)
{
    size_t i;
    if (state == 0) {
        return;
    }
    for (i = 0; i < sizeof(state->timers) / sizeof(state->timers[0]); ++i) {
        tim_tick(&state->timers[i], cycles);
    }
    if (state->soc != 0 && state->soc->watchdog_tick != 0) {
        state->soc->watchdog_tick(cycles);
    }
}

void stm32_timers_init(struct stm32_timers_state *state,
                       const struct stm32_timer_soc *soc,
                       struct mmio_bus *bus,
                       struct mm_nvic *nvic)
{
    static const mm_u32 bases[] = {
        0x40000000u, 0x40000400u, 0x40000800u, 0x40000C00u
    };
    static const int irq_map[] = { 45, 46, 47, 48 };
    mm_u32 *tz;
    mm_u32 *tz1;
    size_t i;

    if (state == 0) {
        return;
    }

    state->soc = soc;
    state->nvic = nvic;
    tz = (soc != 0 && soc->tzsc_regs != 0) ? soc->tzsc_regs() : 0;
    tz1 = tz != 0 ? tz + (0x10u / 4u) : 0;
    if (soc != 0 && soc->exti_set_nvic != 0) {
        soc->exti_set_nvic(nvic);
    }

    for (i = 0; i < sizeof(state->timers) / sizeof(state->timers[0]); ++i) {
        struct stm32_timer_inst *t = &state->timers[i];
        struct mmio_region reg;
        memset(t, 0, sizeof(*t));
        t->owner = state;
        t->index = (mm_u32)i;
        t->base = bases[i];
        t->irq = irq_map[i];
        t->arr_mask = (i == 0u || i == 3u) ? 0xFFFFFFFFu : 0xFFFFu;
        t->arr = t->arr_mask;
        t->rcc_regs = (soc != 0 && soc->rcc_regs != 0) ? soc->rcc_regs() : 0;
        t->sec_reg = tz1;
        t->sec_bitmask = (1u << i);
        t->current_sec = MM_SECURE;

        memset(&reg, 0, sizeof(reg));
        reg.base = bases[i];
        reg.size = 0x400u;
        reg.opaque = t;
        reg.read = tim_read;
        reg.write = tim_write;
        (void)mmio_bus_register_region(bus, &reg);

        reg.base = bases[i] + 0x10000000u;
        (void)mmio_bus_register_region(bus, &reg);
    }
}

void stm32_timers_reset(struct stm32_timers_state *state)
{
    size_t i;
    if (state == 0) {
        return;
    }
    for (i = 0; i < sizeof(state->timers) / sizeof(state->timers[0]); ++i) {
        struct stm32_timer_inst *t = &state->timers[i];
        t->cr1 = 0u;
        t->dier = 0u;
        t->sr = 0u;
        t->cnt = 0u;
        t->psc = 0u;
        t->arr = t->arr_mask;
        t->smcr = 0u;
        memset(t->ccmr, 0, sizeof(t->ccmr));
        t->ccer = 0u;
        memset(t->ccr, 0, sizeof(t->ccr));
        memset(t->icpsc_count, 0, sizeof(t->icpsc_count));
        t->psc_accum = 0u;
        t->secure_only = MM_FALSE;
        t->current_sec = MM_SECURE;
    }
}
