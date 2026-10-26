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
#include "m33mu/signal.h"
#include "stm32_adc.h"

#define ADC_ISR     0x00u
#define ADC_IER     0x04u
#define ADC_CR      0x08u
#define ADC_CFGR    0x0Cu
#define ADC_CFGR2   0x10u
#define ADC_SMPR1   0x14u
#define ADC_SMPR2   0x18u
#define ADC_TR1     0x20u
#define ADC_TR2     0x24u
#define ADC_TR3     0x28u
#define ADC_SQR1    0x30u
#define ADC_SQR2    0x34u
#define ADC_DR      0x40u
#define ADC_OFR1    0x60u
#define ADC_JDR1    0x80u
#define ADC_JDR4    0x8Cu
#define ADC_DIFSEL  0xB0u

#define ADC_COMMON  3u
#define ADC_CSR     0x00u
#define ADC_CCR     0x08u

#define CR_ADEN     (1u << 0)
#define CR_ADDIS    (1u << 1)
#define CR_ADSTART  (1u << 2)
#define CR_JADSTART (1u << 3)
#define CR_ADSTP    (1u << 4)
#define CR_ADVREGEN (1u << 28)
#define CR_DEEPPWD  (1u << 29)
#define CR_ADCALDIF (1u << 30)
#define CR_ADCAL    (1u << 31)
#define CR_STORED_RS (CR_ADEN | CR_ADSTART | CR_ADCAL)
#define CR_PLAIN    (CR_ADVREGEN | CR_DEEPPWD | CR_ADCALDIF)

#define ISR_ADRDY   (1u << 0)
#define ISR_EOSMP   (1u << 1)
#define ISR_EOC     (1u << 2)
#define ISR_EOS     (1u << 3)
#define ISR_OVR     (1u << 4)
#define ISR_MASK    0x7FFu

#define CFGR_DMAEN     (1u << 0)
#define CFGR_RES_SHIFT 3u
#define CFGR_EXTEN     (3u << 10)
#define CFGR_OVRMOD    (1u << 12)
#define CFGR_CONT      (1u << 13)
#define CFGR_AUTDLY    (1u << 14)
#define CFGR_ALIGN     (1u << 15)
#define CFGR_DISCEN    (1u << 16)
#define OFR_EN         (1u << 31)
#define OFR_CH_SHIFT   26u
#define OFR_SATEN      (1u << 25)
#define OFR_POS        (1u << 24)
#define OFR_OFFSET     0xFFFu
#define CFGR2_ROVSE    (1u << 0)
#define SMPR1_SMPPLUS  (1u << 31)
#define CCR_CKMODE_SHIFT 16u
#define CCR_CKMODE_MASK  (0x3u << CCR_CKMODE_SHIFT)
#define CCR_PRESC_SHIFT  18u
#define CCR_PRESC_MASK   (0xFu << CCR_PRESC_SHIFT)

/* Delays in half ADC clocks (DS14972 Table 97, RM0517 Table 293). */
#define HALF_CAL     2670u
#define HALF_STAB    30u
#define HALF_CAL_GUARD 8u

#define EV_OFF          0u
#define EV_REL          1u
#define EV_ABS          2u
#define EV_WAIT_RDY_REL 3u
#define EV_WAIT_RDY     4u

static const mm_u32 g_smp_half[8] = { 5u, 13u, 25u, 49u, 95u, 185u, 495u, 1281u };
static const mm_u32 g_res_half[4] = { 25u, 21u, 17u, 13u };
static const mm_u32 g_res_bits[4] = { 12u, 10u, 8u, 6u };
static const mm_u32 g_presc_div[12] = { 1u, 2u, 4u, 6u, 8u, 10u, 12u, 16u, 32u, 64u, 128u, 256u };

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

static mm_u32 *reg(struct stm32_adc_inst *in, mm_u32 off)
{
    return &in->regs[off / 4u];
}

/* Level-like line: pend while ISR & IER is non-zero. While the handler is
 * active the request is held and pends at the first tick after it returns. */
static void adc_irq_eval(struct stm32_adc_state *s, struct stm32_adc_inst *in)
{
    mm_u32 idx = (mm_u32)(in - s->inst);
    mm_u32 line;
    int irq;

    if (s->nvic == 0 || s->soc == 0 || idx >= STM32_ADC_MAX_INST) {
        return;
    }
    irq = s->soc->irq[idx];
    if (irq < 0 || irq >= MM_MAX_IRQ) {
        return;
    }
    line = (mm_u32)irq;
    in->irq_held = MM_FALSE;
    if ((*reg(in, ADC_ISR) & *reg(in, ADC_IER) & ISR_MASK) == 0u) {
        return;
    }
    if (mm_nvic_is_active(s->nvic, line)) {
        in->irq_held = MM_TRUE;
    } else if (!mm_nvic_is_pending(s->nvic, line)) {
        mm_nvic_set_pending(s->nvic, line, MM_TRUE);
    }
}

static void adc_isr_update(struct stm32_adc_state *s, struct stm32_adc_inst *in, mm_u32 set,
                           mm_u32 clr)
{
    *reg(in, ADC_ISR) = (*reg(in, ADC_ISR) & ~clr) | set;
    adc_irq_eval(s, in);
}

/* OVRMOD = 0: three-stage FIFO, a result arriving when full is lost.
 * OVRMOD = 1: DR is overwritten, OVR when EOC is still set. */
static void adc_push(struct stm32_adc_state *s, struct stm32_adc_inst *in, mm_u32 set)
{
    mm_bool ovr;

    if ((*reg(in, ADC_CFGR) & CFGR_OVRMOD) != 0u) {
        ovr = (*reg(in, ADC_ISR) & ISR_EOC) != 0u;
        in->fifo[0] = (mm_u16)in->code;
        in->fifo_count = 1u;
    } else {
        ovr = in->fifo_count >= STM32_ADC_FIFO_DEPTH;
        if (!ovr) {
            in->fifo[in->fifo_count++] = (mm_u16)in->code;
        }
    }
    *reg(in, ADC_DR) = in->fifo[0];
    adc_isr_update(s, in, set | ISR_EOC | (ovr ? ISR_OVR : 0u), 0u);
}

/* DR keeps the last value once the FIFO is empty; EOC follows the FIFO. */
static void adc_pop(struct stm32_adc_state *s, struct stm32_adc_inst *in)
{
    mm_u32 i;

    if (in->fifo_count != 0u) {
        for (i = 1u; i < in->fifo_count; ++i) {
            in->fifo[i - 1u] = in->fifo[i];
        }
        in->fifo_count--;
    }
    if (in->fifo_count != 0u) {
        *reg(in, ADC_DR) = in->fifo[0];
        adc_isr_update(s, in, ISR_EOC, 0u);
    } else {
        adc_isr_update(s, in, 0u, ISR_EOC);
    }
}

static void adc_warn(struct stm32_adc_state *s, mm_u32 bit, const char *msg)
{
    if ((s->warned & bit) != 0u) {
        return;
    }
    s->warned |= bit;
    fprintf(stderr, "[ADC] warning: %s\n", msg);
}

static mm_bool adc_clock_on(const struct stm32_adc_state *s, mm_u32 idx)
{
    if (s->soc == 0 || s->soc->clock_enabled == 0) {
        return MM_TRUE;
    }
    return s->soc->clock_enabled(s->soc->ctx, idx) ? MM_TRUE : MM_FALSE;
}

static mm_u64 adc_cyc(const struct stm32_adc_inst *in, mm_u64 half)
{
    return (half * in->ckdiv + 1u) / 2u;
}

/* CKMODE = 01/10/11: HCLK/1, /2, /4. CKMODE = 00: kernel clock / PRESC, modelled
 * only when the kernel clock is HCLK. */
static mm_u32 adc_latch_clock(struct stm32_adc_state *s)
{
    mm_u32 mode = (s->ccr >> CCR_CKMODE_SHIFT) & 0x3u;
    mm_u32 presc = (s->ccr & CCR_PRESC_MASK) >> CCR_PRESC_SHIFT;
    mm_u32 div = 1u;
    if (mode != 0u) {
        div = 1u << (mode - 1u);
    } else if (presc < sizeof(g_presc_div) / sizeof(g_presc_div[0]) && s->soc != 0 &&
               s->soc->kernel_is_hclk != 0 && s->soc->kernel_is_hclk(s->soc->ctx)) {
        div = g_presc_div[presc];
    } else {
        adc_warn(s, STM32_ADC_WARN_CKMODE0,
                 "CKMODE=00 with a kernel clock other than HCLK or a reserved PRESC, "
                 "running at HCLK/1");
    }
    if (s->soc != 0 && s->soc->hclk_hz != 0) {
        mm_u64 f = s->soc->hclk_hz(s->soc->ctx) / div;
        if ((f < 1500000u || f > 75000000u) && (s->warned & STM32_ADC_WARN_FREQ) == 0u) {
            s->warned |= STM32_ADC_WARN_FREQ;
            fprintf(stderr, "[ADC] warning: f_ADC %llu Hz outside 1.5-75 MHz\n",
                    (unsigned long long)f);
        }
    }
    return div;
}

static mm_u64 adc_inst_deadline(const struct stm32_adc_state *s, const struct stm32_adc_inst *in)
{
    mm_u64 t = STM32_ADC_NO_EVENT;
    if (in->cal_ev == EV_REL || in->rdy_ev == EV_REL || in->conv_ev == EV_REL ||
        in->conv_ev == EV_WAIT_RDY_REL) {
        return s->now;
    }
    if (in->cal_ev == EV_ABS && in->cal_at < t) t = in->cal_at;
    if (in->rdy_ev == EV_ABS && in->rdy_at < t) t = in->rdy_at;
    if (in->conv_ev == EV_ABS && in->conv_at < t) t = in->conv_at;
    return t;
}

static void adc_update_deadline(struct stm32_adc_state *s)
{
    mm_u32 i;
    s->next_deadline = STM32_ADC_NO_EVENT;
    for (i = 0; i < s->inst_count; ++i) {
        mm_u64 t = adc_inst_deadline(s, &s->inst[i]);
        if (t < s->next_deadline) {
            s->next_deadline = t;
        }
    }
}

static mm_u32 adc_rank_channel(struct stm32_adc_inst *in, mm_u32 rank)
{
    if (rank < 4u) {
        return (*reg(in, ADC_SQR1) >> (6u * (rank + 1u))) & 0x1Fu;
    }
    rank -= 4u;
    return (*reg(in, ADC_SQR2 + (rank / 5u) * 4u) >> (6u * (rank % 5u))) & 0x1Fu;
}

static mm_u32 adc_smp_half(struct stm32_adc_inst *in, mm_u32 chan)
{
    mm_u32 smpr1 = *reg(in, ADC_SMPR1);
    mm_u32 code;
    if (chan < 10u) {
        code = (smpr1 >> (3u * chan)) & 0x7u;
    } else if (chan < STM32_ADC_NUM_CHANNELS) {
        code = (*reg(in, ADC_SMPR2) >> (3u * (chan - 10u))) & 0x7u;
    } else {
        code = 0u;
    }
    if (code == 0u && (smpr1 & SMPR1_SMPPLUS) != 0u) {
        return 7u;
    }
    return g_smp_half[code];
}

/* The lowest y wins when several OFRy select the channel (RM0517 ADC_OFRy). */
static mm_u32 adc_match_ofr(struct stm32_adc_inst *in, mm_u32 chan)
{
    mm_u32 i;
    for (i = 0; i < 4u; ++i) {
        mm_u32 ofr = *reg(in, ADC_OFR1 + 4u * i);
        if ((ofr & OFR_EN) != 0u && ((ofr >> OFR_CH_SHIFT) & 0x1Fu) == chan) {
            return ofr;
        }
    }
    return 0u;
}

/* Oversampling ignores ALIGN and the offsets. */
static void adc_slot_begin(struct stm32_adc_inst *in)
{
    mm_u32 res = (*reg(in, ADC_CFGR) >> CFGR_RES_SHIFT) & 0x3u;
    mm_bool rovse = (*reg(in, ADC_CFGR2) & CFGR2_ROVSE) != 0u;
    in->chan = adc_rank_channel(in, in->rank);
    in->smp_half = adc_smp_half(in, in->chan);
    in->align = !rovse && (*reg(in, ADC_CFGR) & CFGR_ALIGN) != 0u;
    in->ofr = rovse ? 0u : adc_match_ofr(in, in->chan);
    in->res = res;
    in->res_half = g_res_half[res];
    in->sampled = MM_FALSE;
    in->conv_at = in->seq_start + adc_cyc(in, in->slot_half + in->smp_half);
    in->conv_ev = EV_ABS;
}

static void adc_seq_begin(struct stm32_adc_inst *in, mm_u64 t)
{
    in->seq_start = t;
    in->slot_half = 0u;
    in->rank = 0u;
    adc_slot_begin(in);
}

static mm_u64 adc_rng_next(struct stm32_adc_state *s)
{
    mm_u64 z;
    s->rng += 0x9E3779B97F4A7C15ull;
    z = s->rng;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static mm_u32 adc_quantize(struct stm32_adc_state *s, mm_i32 mv, mm_u32 bits)
{
    mm_i64 max = ((mm_i64)1 << bits) - 1;
    mm_i64 den = 2 * (mm_i64)s->vref_mv;
    mm_i64 num = 2 * (mm_i64)mv * max + s->vref_mv;
    mm_i64 q = num / den;
    if (num % den != 0 && num < 0) {
        q--;
    }
    if (s->noise_lsb != 0u) {
        mm_u64 span = 2u * (mm_u64)s->noise_lsb + 1u;
        q += (mm_i64)(adc_rng_next(s) % span) - (mm_i64)s->noise_lsb;
    }
    if (q < 0) q = 0;
    if (q > max) q = max;
    return (mm_u32)q;
}

/* Offset on the 12-bit scale (RM0517 Table 294), then Figures 270-273. */
static mm_u32 adc_format(const struct stm32_adc_inst *in, mm_u32 raw)
{
    mm_u32 bits = g_res_bits[in->res];
    mm_i32 v = (mm_i32)raw;
    mm_bool sgn = MM_FALSE;
    mm_u32 shift;

    if (in->ofr != 0u) {
        mm_i32 o = (mm_i32)((in->ofr & OFR_OFFSET) >> (12u - bits));
        v = ((in->ofr & OFR_POS) != 0u) ? v + o : v - o;
        if ((in->ofr & OFR_SATEN) != 0u) {
            mm_i32 max = (mm_i32)((1u << bits) - 1u);
            if (v < 0) v = 0;
            if (v > max) v = max;
        } else {
            sgn = MM_TRUE;
        }
    }
    if (!in->align) {
        return (mm_u32)v & 0xFFFFu;
    }
    shift = (bits == 6u ? 8u : 16u) - bits - (sgn ? 1u : 0u);
    return ((mm_u32)v << shift) & 0xFFFFu;
}

static mm_i32 adc_input_mv(struct stm32_adc_state *s, mm_u32 idx, mm_u32 chan, mm_u64 cycles_ahead)
{
    struct stm32_adc_inst *in = &s->inst[idx];
    const struct stm32_adc_input *e;
    mm_i32 mv = 0;
    mm_bool ok;

    if (chan < STM32_ADC_NUM_CHANNELS && ((*reg(in, ADC_DIFSEL) >> chan) & 1u) != 0u) {
        adc_warn(s, STM32_ADC_WARN_DIFSEL, "differential input not modelled, converting single-ended");
    }
    if (s->soc == 0 || chan >= STM32_ADC_NUM_CHANNELS || s->soc->inputs[idx] == 0) {
        return 0;
    }
    e = &s->soc->inputs[idx][chan];
    if (e->kind == STM32_ADC_IN_INTERNAL) {
        adc_warn(s, STM32_ADC_WARN_INTERNAL, "internal ADC channels convert as 0 mV");
        return 0;
    }
    if (e->kind != STM32_ADC_IN_PIN) {
        return 0;
    }
    if (s->soc->pin_is_analog != 0 && !s->soc->pin_is_analog(s->soc->ctx, e->bank, e->pin)) {
        return 0;
    }
    if (s->soc->sample_mv != 0) {
        ok = s->soc->sample_mv(s->soc->ctx, e->bank, e->pin, cycles_ahead, &mv);
    } else {
        ok = mm_signal_sample_mv(e->bank, e->pin, cycles_ahead, &mv);
    }
    return ok ? mv : 0;
}

static void adc_conv_event(struct stm32_adc_state *s, mm_u32 idx, mm_u64 t, mm_u64 batch_start)
{
    struct stm32_adc_inst *in = &s->inst[idx];
    mm_bool last;

    if (!in->sampled) {
        mm_i32 mv = adc_input_mv(s, idx, in->chan, t > batch_start ? t - batch_start : 0u);
        in->code = adc_format(in, adc_quantize(s, mv, g_res_bits[in->res]));
        adc_isr_update(s, in, ISR_EOSMP, 0u);
        in->sampled = MM_TRUE;
        in->conv_at = in->seq_start + adc_cyc(in, in->slot_half + in->smp_half + in->res_half);
        return;
    }
    /* Continuous mode keeps seq_start, so slots stay on one exact time base. */
    in->slot_half += in->smp_half + in->res_half;
    in->rank++;
    last = in->rank > (*reg(in, ADC_SQR1) & 0xFu);
    if (last && in->cont) {
        in->rank = 0u;
    } else if (last) {
        *reg(in, ADC_CR) &= ~CR_ADSTART;
        in->conv_ev = EV_OFF;
    }
    adc_push(s, in, last ? ISR_EOS : 0u);
    if (in->conv_ev != EV_OFF) {
        adc_slot_begin(in);
    }
}

static void adc_inst_shift(struct stm32_adc_inst *in, mm_u64 d)
{
    if (in->cal_ev == EV_ABS) in->cal_at += d;
    if (in->rdy_ev == EV_ABS) in->rdy_at += d;
    if (in->conv_ev == EV_ABS || in->conv_ev == EV_WAIT_RDY) {
        in->conv_at += d;
        in->seq_start += d;
    }
    in->cal_guard_end += d;
}

static void adc_inst_run(struct stm32_adc_state *s, mm_u32 idx, mm_u64 start, mm_u64 end)
{
    struct stm32_adc_inst *in = &s->inst[idx];

    if (in->cal_ev == EV_REL) { in->cal_at += end; in->cal_ev = EV_ABS; }
    if (in->rdy_ev == EV_REL) { in->rdy_at += end; in->rdy_ev = EV_ABS; }
    if (in->conv_ev == EV_REL) {
        adc_seq_begin(in, end);
    } else if (in->conv_ev == EV_WAIT_RDY_REL) {
        in->conv_at = end;
        in->conv_ev = EV_WAIT_RDY;
    }
    for (;;) {
        mm_u64 t = STM32_ADC_NO_EVENT;
        int which = 0;
        if (in->cal_ev == EV_ABS && in->cal_at < t) { t = in->cal_at; which = 1; }
        if (in->rdy_ev == EV_ABS && in->rdy_at < t) { t = in->rdy_at; which = 2; }
        if (in->conv_ev == EV_ABS && in->conv_at < t) { t = in->conv_at; which = 3; }
        if (which == 0 || t > end) {
            break;
        }
        if (which == 1) {
            in->cal_ev = EV_OFF;
            *reg(in, ADC_CR) &= ~CR_ADCAL;
            in->cal_guard_end = t + adc_cyc(in, HALF_CAL_GUARD);
        } else if (which == 2) {
            in->rdy_ev = EV_OFF;
            in->ready = MM_TRUE;
            adc_isr_update(s, in, ISR_ADRDY, 0u);
            if (in->conv_ev == EV_WAIT_RDY) {
                adc_seq_begin(in, in->conv_at > t ? in->conv_at : t);
            }
        } else {
            adc_conv_event(s, idx, t, start);
        }
    }
}

void stm32_adc_tick(struct stm32_adc_state *s, mm_u64 cycles)
{
    mm_u64 start;
    mm_u64 end;
    mm_u32 i;

    if (s == 0) {
        return;
    }
    for (i = 0; i < s->inst_count; ++i) {
        if (s->inst[i].irq_held) {
            adc_irq_eval(s, &s->inst[i]);
        }
    }
    if (s->next_deadline == STM32_ADC_NO_EVENT) {
        s->now += cycles;
        return;
    }
    start = s->now;
    end = start + cycles;
    for (i = 0; i < s->inst_count; ++i) {
        struct stm32_adc_inst *in = &s->inst[i];
        if (adc_inst_deadline(s, in) == STM32_ADC_NO_EVENT) {
            continue;
        }
        if (!adc_clock_on(s, i)) {
            adc_inst_shift(in, cycles);
            continue;
        }
        adc_inst_run(s, i, start, end);
    }
    s->now = end;
    adc_update_deadline(s);
}

mm_u64 stm32_adc_cycles_until_event(const struct stm32_adc_state *s)
{
    mm_u64 best = STM32_ADC_NO_EVENT;
    mm_u32 i;

    if (s == 0 || s->next_deadline == STM32_ADC_NO_EVENT) {
        return STM32_ADC_NO_EVENT;
    }
    for (i = 0; i < s->inst_count; ++i) {
        mm_u64 t = adc_inst_deadline(s, &s->inst[i]);
        if (t < best && adc_clock_on(s, i)) {
            best = t;
        }
    }
    if (best == STM32_ADC_NO_EVENT) {
        return STM32_ADC_NO_EVENT;
    }
    return best > s->now ? best - s->now : 0u;
}

static void adc_check_regulator(struct stm32_adc_state *s, const struct stm32_adc_inst *in)
{
    mm_u32 cr = in->regs[ADC_CR / 4u];
    if ((cr & CR_ADVREGEN) == 0u || (cr & CR_DEEPPWD) != 0u) {
        adc_warn(s, STM32_ADC_WARN_REGULATOR, "ADC enabled or calibrated with its regulator off");
    }
}

/* Set-only bits; rewriting a bit that is already set never restarts anything. */
static void adc_cr_write(struct stm32_adc_state *s, struct stm32_adc_inst *in, mm_u32 v)
{
    mm_u32 *cr = reg(in, ADC_CR);
    mm_u32 plain = v & CR_PLAIN;
    mm_u32 req = v & ~*cr & (CR_ADCAL | CR_ADEN | CR_ADSTART);

    if ((plain & CR_DEEPPWD) != 0u) {
        plain &= ~CR_ADVREGEN;
    }
    *cr = (*cr & CR_STORED_RS) | plain;

    if ((v & CR_ADSTP) != 0u && (*cr & CR_ADSTART) != 0u) {
        *cr &= ~CR_ADSTART;
        in->conv_ev = EV_OFF;
    }
    if ((v & CR_ADDIS) != 0u && (*cr & CR_ADEN) != 0u && (*cr & CR_ADSTART) == 0u) {
        *cr &= ~CR_ADEN;
        in->rdy_ev = EV_OFF;
        in->ready = MM_FALSE;
        in->fifo_count = 0u;
        adc_isr_update(s, in, 0u, ISR_EOC);
    }
    if ((req & CR_ADCAL) != 0u && (*cr & (CR_ADEN | CR_ADCAL)) == 0u) {
        adc_check_regulator(s, in);
        in->ckdiv = adc_latch_clock(s);
        *cr |= CR_ADCAL;
        in->cal_at = adc_cyc(in, HALF_CAL);
        in->cal_ev = EV_REL;
    }
    /* The write is timed at the start of the current batch. */
    if ((req & CR_ADEN) != 0u && (*cr & CR_STORED_RS) == 0u && s->now >= in->cal_guard_end) {
        adc_check_regulator(s, in);
        in->ckdiv = adc_latch_clock(s);
        *cr |= CR_ADEN;
        in->rdy_at = adc_cyc(in, HALF_STAB);
        in->rdy_ev = EV_REL;
        in->ready = MM_FALSE;
    }
    if ((v & CR_JADSTART) != 0u && (*cr & CR_ADEN) != 0u) {
        adc_warn(s, STM32_ADC_WARN_INJECTED, "injected group not modelled, JADSTART ignored");
    }
    if ((req & CR_ADSTART) != 0u && (*cr & CR_ADEN) != 0u && (*cr & CR_ADSTART) == 0u) {
        mm_u32 cfgr = *reg(in, ADC_CFGR);
        if ((*reg(in, ADC_CFGR2) & CFGR2_ROVSE) != 0u) {
            adc_warn(s, STM32_ADC_WARN_OVERSAMPLING, "oversampling not modelled, converting without it");
        }
        if ((cfgr & CFGR_EXTEN) != 0u) {
            adc_warn(s, STM32_ADC_WARN_EXTEN, "hardware trigger (EXTEN) not modelled, converting on ADSTART");
        }
        if ((cfgr & CFGR_DMAEN) != 0u) {
            adc_warn(s, STM32_ADC_WARN_DMA, "DMA requests not modelled");
        }
        if ((cfgr & CFGR_AUTDLY) != 0u) {
            adc_warn(s, STM32_ADC_WARN_AUTDLY, "delayed conversion (AUTDLY) not modelled");
        }
        if ((cfgr & CFGR_DISCEN) != 0u) {
            adc_warn(s, STM32_ADC_WARN_DISCEN, "discontinuous mode not modelled, converting the whole sequence");
        }
        in->cont = (*reg(in, ADC_CFGR) & CFGR_CONT) != 0u;
        *cr |= CR_ADSTART;
        in->conv_ev = in->ready ? EV_REL : EV_WAIT_RDY_REL;
    }
    adc_update_deadline(s);
}

static mm_u32 adc_common_read(const struct stm32_adc_state *s, mm_u32 off)
{
    if (off == ADC_CSR) {
        mm_u32 v = s->inst[0].regs[ADC_ISR / 4u] & ISR_MASK;
        if (s->inst_count > 1u) {
            v |= (s->inst[1].regs[ADC_ISR / 4u] & ISR_MASK) << 16;
        }
        return v;
    }
    if (off == ADC_CCR) {
        return s->ccr;
    }
    return 0u;
}

static mm_bool adc_read_impl(struct stm32_adc_state *s, mm_u32 offset, mm_u32 size_bytes,
                             mm_u32 *value_out, mm_bool side_effects)
{
    mm_u32 block = offset >> 8;
    mm_u32 off = offset & 0xFCu;
    mm_u32 byte = offset & 0x3u;
    mm_u32 v = 0u;

    if (value_out == 0 || size_bytes == 0u || size_bytes > 4u || offset >= 0x400u) {
        return MM_FALSE;
    }
    *value_out = 0u;
    if (byte + size_bytes > 4u) {
        return MM_TRUE;
    }
    if (block < s->inst_count) {
        struct stm32_adc_inst *in = &s->inst[block];
        if (!adc_clock_on(s, block)) {
            return MM_TRUE;
        }
        v = *reg(in, off);
        if (off == ADC_ISR) {
            v &= ISR_MASK;
        } else if (off == ADC_DR && side_effects) {
            adc_pop(s, in);
        }
    } else if (block == ADC_COMMON) {
        if (!adc_clock_on(s, 0u)) {
            return MM_TRUE;
        }
        v = adc_common_read(s, off);
    }
    *value_out = read_slice(v, byte, size_bytes);
    return MM_TRUE;
}

static mm_bool adc_access_allowed(const struct stm32_adc_state *s)
{
    if (s->soc == 0 || s->soc->access_allowed == 0) {
        return MM_TRUE;
    }
    return s->soc->access_allowed(s->soc->ctx) ? MM_TRUE : MM_FALSE;
}

static mm_bool adc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct stm32_adc_state *s = (struct stm32_adc_state *)opaque;
    mm_bool peek = mmio_peek_mode();

    if (!peek && value_out != 0 && !adc_access_allowed(s)) {
        *value_out = 0u;
        return MM_TRUE;
    }
    return adc_read_impl(s, offset, size_bytes, value_out, peek ? MM_FALSE : MM_TRUE);
}

static mmio_peek_result_t adc_peek(void *opaque, mm_u32 offset, mm_u32 size_bytes, void *dst)
{
    mm_u8 *d = (mm_u8 *)dst;
    mm_u32 v = 0u;
    mm_u32 i;
    if (dst == 0 || !adc_read_impl((struct stm32_adc_state *)opaque, offset, size_bytes, &v,
                                   MM_FALSE)) {
        return MMIO_PEEK_UNSUPPORTED;
    }
    for (i = 0; i < size_bytes; ++i) {
        d[i] = (mm_u8)(v >> (8u * i));
    }
    return MMIO_PEEK_OK;
}

/* Raw copy of the instances: bump the region version when stm32_adc_inst changes.
 * vref, noise and the one-time warnings are session settings and stay as they are. */
struct adc_snapshot {
    struct stm32_adc_inst inst[STM32_ADC_MAX_INST];
    mm_u64 now;
    mm_u64 rng;
    mm_u32 ccr;
};

static mm_bool adc_save(void *opaque, struct mm_snapshot_writer *w)
{
    struct stm32_adc_state *s = (struct stm32_adc_state *)opaque;
    struct adc_snapshot snap;
    if (s == 0 || w == 0) {
        return MM_FALSE;
    }
    memset(&snap, 0, sizeof(snap));
    memcpy(snap.inst, s->inst, sizeof(snap.inst));
    snap.now = s->now;
    snap.rng = s->rng;
    snap.ccr = s->ccr;
    return mm_snapshot_write(w, &snap, (mm_u32)sizeof(snap));
}

static mm_bool adc_snapshot_valid(const struct adc_snapshot *snap)
{
    mm_u32 i;
    for (i = 0; i < STM32_ADC_MAX_INST; ++i) {
        const struct stm32_adc_inst *in = &snap->inst[i];
        if (in->fifo_count > STM32_ADC_FIFO_DEPTH || in->rank > 16u || in->chan > 0x1Fu ||
            in->res > 3u || in->ckdiv == 0u || in->ckdiv > 256u || in->cal_ev > EV_WAIT_RDY ||
            in->rdy_ev > EV_WAIT_RDY || in->conv_ev > EV_WAIT_RDY) {
            return MM_FALSE;
        }
    }
    return MM_TRUE;
}

static mm_bool adc_load(void *opaque, struct mm_snapshot_reader *r)
{
    struct stm32_adc_state *s = (struct stm32_adc_state *)opaque;
    struct adc_snapshot snap;
    if (s == 0 || r == 0) {
        return MM_FALSE;
    }
    if ((r->size - r->offset) != (mm_u32)sizeof(snap)) {
        return MM_FALSE;
    }
    if (!mm_snapshot_read(r, &snap, (mm_u32)sizeof(snap)) || !adc_snapshot_valid(&snap)) {
        return MM_FALSE;
    }
    memcpy(s->inst, snap.inst, sizeof(s->inst));
    s->now = snap.now;
    s->rng = snap.rng;
    s->ccr = snap.ccr;
    adc_update_deadline(s);
    return MM_TRUE;
}

/* RM0517 allows CKMODE and PRESC changes only while every ADC is disabled. */
static mm_bool adc_any_enabled(const struct stm32_adc_state *s)
{
    mm_u32 i;
    for (i = 0; i < s->inst_count; ++i) {
        if ((s->inst[i].regs[ADC_CR / 4u] & CR_ADEN) != 0u) {
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

static mm_bool adc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct stm32_adc_state *s = (struct stm32_adc_state *)opaque;
    mm_u32 block = offset >> 8;
    mm_u32 off = offset & 0xFCu;
    mm_u32 byte = offset & 0x3u;
    struct stm32_adc_inst *in;

    if (size_bytes == 0u || size_bytes > 4u || offset >= 0x400u) {
        return MM_FALSE;
    }
    if (byte + size_bytes > 4u || !adc_access_allowed(s)) {
        return MM_TRUE;
    }
    if (block == ADC_COMMON) {
        if (adc_clock_on(s, 0u) && off == ADC_CCR) {
            mm_u32 ccr = apply_write(s->ccr, byte, size_bytes, value);
            if (adc_any_enabled(s)) {
                mm_u32 locked = CCR_CKMODE_MASK | CCR_PRESC_MASK;
                ccr = (ccr & ~locked) | (s->ccr & locked);
            }
            s->ccr = ccr;
        }
        return MM_TRUE;
    }
    if (block >= s->inst_count || !adc_clock_on(s, block)) {
        return MM_TRUE;
    }
    in = &s->inst[block];
    if (off == ADC_ISR) {
        mm_u32 clr = apply_write(0u, byte, size_bytes, value) & ISR_MASK;
        /* Clearing EOC releases the queued results; DR keeps its value. */
        if ((clr & ISR_EOC) != 0u) {
            in->fifo_count = 0u;
        }
        adc_isr_update(s, in, 0u, clr);
    } else if (off == ADC_CR) {
        adc_cr_write(s, in, apply_write(*reg(in, ADC_CR), byte, size_bytes, value));
    } else if (off != ADC_DR && !(off >= ADC_JDR1 && off <= ADC_JDR4)) {
        *reg(in, off) = apply_write(*reg(in, off), byte, size_bytes, value);
        if (off == ADC_IER) {
            adc_irq_eval(s, in);
        }
    }
    return MM_TRUE;
}

void stm32_adc_reset(struct stm32_adc_state *s)
{
    mm_u32 i;
    if (s == 0) {
        return;
    }
    for (i = 0; i < STM32_ADC_MAX_INST; ++i) {
        struct stm32_adc_inst *in = &s->inst[i];
        memset(in, 0, sizeof(*in));
        *reg(in, ADC_CR) = CR_DEEPPWD;
        *reg(in, ADC_CFGR) = 0x80000000u;
        *reg(in, ADC_TR1) = 0x0FFF0000u;
        *reg(in, ADC_TR2) = 0x00FF0000u;
        *reg(in, ADC_TR3) = 0x00FF0000u;
        in->ckdiv = 1u;
    }
    s->ccr = 0u;
    s->rng = s->noise_seed;
    s->next_deadline = STM32_ADC_NO_EVENT;
}

void stm32_adc_init(struct stm32_adc_state *s, const struct stm32_adc_soc *soc,
                    struct mmio_bus *bus, struct mm_nvic *nvic)
{
    struct mmio_region r;
    mm_i32 vref_mv = mm_signal_adc_vref_mv();

    if (s == 0) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->soc = soc;
    s->nvic = nvic;
    s->inst_count = (soc != 0 && soc->inst_count < STM32_ADC_MAX_INST) ? soc->inst_count
                                                                       : STM32_ADC_MAX_INST;
    if (s->inst_count == 0u) {
        s->inst_count = 1u;
    }
    s->vref_mv = (vref_mv > 0) ? vref_mv : 3300;
    s->noise_lsb = mm_signal_adc_noise_lsb();
    s->noise_seed = mm_signal_adc_seed();
    stm32_adc_reset(s);
    if (bus == 0 || soc == 0) {
        return;
    }
    memset(&r, 0, sizeof(r));
    r.magic = MMIO_REGION_MAGIC;
    r.flags = MMIO_REGION_F_EXT;
    r.base = soc->base;
    r.size = 0x400u;
    r.opaque = s;
    r.read = adc_read;
    r.write = adc_write;
    r.peek = adc_peek;
    r.name = soc->name != 0 ? soc->name : "ADC";
    r.version = 1u;
    r.save = adc_save;
    r.load = adc_load;
    (void)mmio_bus_register_region(bus, &r);
    r.base = soc->base + 0x10000000u;
    r.name = 0;
    r.save = 0;
    r.load = 0;
    (void)mmio_bus_register_region(bus, &r);
}

void stm32_adc_set_vref_mv(struct stm32_adc_state *s, mm_i32 mv)
{
    if (s != 0 && mv > 0) {
        s->vref_mv = mv;
    }
}

void stm32_adc_set_noise(struct stm32_adc_state *s, mm_u32 lsb, mm_u64 seed)
{
    if (s == 0) {
        return;
    }
    s->noise_lsb = lsb;
    s->noise_seed = seed;
    s->rng = seed;
}
