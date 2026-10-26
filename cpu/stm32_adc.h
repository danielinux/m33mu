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

#ifndef M33MU_STM32_ADC_H
#define M33MU_STM32_ADC_H

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "m33mu/types.h"

#define STM32_ADC_MAX_INST     2u
#define STM32_ADC_NUM_CHANNELS 20u
#define STM32_ADC_INST_REGS    64u
#define STM32_ADC_NO_EVENT     ((mm_u64)-1)
#define STM32_ADC_FIFO_DEPTH   3u

#define STM32_ADC_WARN_CKMODE0      (1u << 0)
#define STM32_ADC_WARN_FREQ         (1u << 1)
#define STM32_ADC_WARN_INTERNAL     (1u << 2)
#define STM32_ADC_WARN_DIFSEL       (1u << 3)
#define STM32_ADC_WARN_OVERSAMPLING (1u << 4)
#define STM32_ADC_WARN_REGULATOR    (1u << 5)
#define STM32_ADC_WARN_EXTEN        (1u << 6)
#define STM32_ADC_WARN_DMA          (1u << 7)
#define STM32_ADC_WARN_INJECTED     (1u << 8)
#define STM32_ADC_WARN_AUTDLY       (1u << 9)
#define STM32_ADC_WARN_DISCEN       (1u << 10)

/* Each inputs[] table has STM32_ADC_NUM_CHANNELS entries; a zero entry is
 * "no pin" and converts 0 mV. */
enum stm32_adc_input_kind {
    STM32_ADC_IN_NONE = 0,
    STM32_ADC_IN_PIN = 1,
    STM32_ADC_IN_INTERNAL = 2
};

struct stm32_adc_input {
    mm_u8 kind;
    mm_u8 bank;
    mm_u8 pin;
};

/* Instances at base + 0x100 * n, common at base + 0x300, secure alias at
 * base + 0x10000000. */
struct stm32_adc_soc {
    mm_u32 base;
    mm_u32 inst_count;
    int irq[STM32_ADC_MAX_INST];
    const struct stm32_adc_input *inputs[STM32_ADC_MAX_INST];
    void *ctx;
    mm_bool (*clock_enabled)(void *ctx, mm_u32 inst);        /* NULL: always on */
    mm_bool (*pin_is_analog)(void *ctx, int bank, int pin);  /* NULL: always analog */
    mm_u64 (*hclk_hz)(void *ctx);                            /* NULL: no f_ADC check */
    mm_bool (*sample_mv)(void *ctx, int bank, int pin, mm_u64 cycles_ahead,
                         mm_i32 *mv_out);                     /* NULL: mm_signal_sample_mv() */
    mm_bool (*access_allowed)(void *ctx);                    /* NULL: always; false: RAZ/WI */
    mm_bool (*kernel_is_hclk)(void *ctx);                    /* NULL: CKMODE=00 unsupported */
    const char *name;                                        /* snapshot name, NULL: "ADC" */
};

struct stm32_adc_inst {
    mm_u32 regs[STM32_ADC_INST_REGS];
    mm_u8 cal_ev;
    mm_u8 rdy_ev;
    mm_u8 conv_ev;
    mm_bool ready;
    mm_bool sampled;
    mm_bool cont;
    mm_bool align;
    mm_bool irq_held;
    mm_u32 fifo_count;
    mm_u16 fifo[STM32_ADC_FIFO_DEPTH];
    mm_u32 ofr;
    mm_u32 ckdiv;
    mm_u32 rank;
    mm_u32 chan;
    mm_u32 smp_half;
    mm_u32 res;
    mm_u32 res_half;
    mm_u32 code;
    mm_u64 cal_at;
    mm_u64 cal_guard_end;
    mm_u64 rdy_at;
    mm_u64 conv_at;
    mm_u64 seq_start;
    mm_u64 slot_half;
};

struct stm32_adc_state {
    struct stm32_adc_inst inst[STM32_ADC_MAX_INST];
    mm_u32 inst_count;
    mm_u32 ccr;
    const struct stm32_adc_soc *soc;
    struct mm_nvic *nvic;
    mm_u64 now;
    mm_u64 next_deadline;
    mm_i32 vref_mv;
    mm_u32 noise_lsb;
    mm_u64 noise_seed;
    mm_u64 rng;
    mm_u32 warned;
};

/* Takes vref and noise from the mm_signal_adc_* settings; reset re-seeds the PRNG. */
void stm32_adc_init(struct stm32_adc_state *s, const struct stm32_adc_soc *soc,
                    struct mmio_bus *bus, struct mm_nvic *nvic);
void stm32_adc_reset(struct stm32_adc_state *s);

/* Call once per executed batch, before mm_signal_advance() for the same cycles. */
void stm32_adc_tick(struct stm32_adc_state *s, mm_u64 cycles);

/* Cycles until the next ADC flag change, or STM32_ADC_NO_EVENT. */
mm_u64 stm32_adc_cycles_until_event(const struct stm32_adc_state *s);

/* Full scale, default 3300 mV; values <= 0 are ignored. */
void stm32_adc_set_vref_mv(struct stm32_adc_state *s, mm_i32 mv);

/* Uniform +/- lsb noise before the clamp, from a PRNG seeded at each reset. */
void stm32_adc_set_noise(struct stm32_adc_state *s, mm_u32 lsb, mm_u64 seed);

#endif /* M33MU_STM32_ADC_H */
