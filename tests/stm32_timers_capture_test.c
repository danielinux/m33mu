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
#include "stm32_timers.h"

#define TIM2      0x40000000u
#define TIM2_IRQ  45u
#define CR1       0x00u
#define SMCR      0x08u
#define DIER      0x0Cu
#define SR        0x10u
#define EGR       0x14u
#define CCMR1     0x18u
#define CCER      0x20u
#define CNT       0x24u
#define PSC       0x28u
#define ARR       0x2Cu
#define CCR1      0x34u
#define CCR2      0x38u

#define CEN    (1u << 0)
#define DIR    (1u << 4)
#define CC1IF  (1u << 1)
#define CC2IF  (1u << 2)
#define TIF    (1u << 6)
#define UIF    (1u << 0)
#define CC1OF  (1u << 9)

static struct mmio_region g_regions[16];
static struct mmio_bus g_bus;
static struct mm_nvic g_nvic;
static struct stm32_timers_state g_tim;

static void setup(void)
{
    memset(&g_tim, 0, sizeof(g_tim));
    mmio_bus_init(&g_bus, g_regions, sizeof(g_regions) / sizeof(g_regions[0]));
    mm_nvic_init(&g_nvic);
    stm32_timers_init(&g_tim, 0, &g_bus, &g_nvic);
}

static void wr(mm_u32 off, mm_u32 v)
{
    (void)mmio_bus_write(&g_bus, TIM2 + off, 4, v);
}

static mm_u32 rd(mm_u32 off)
{
    mm_u32 v = 0;
    (void)mmio_bus_read(&g_bus, TIM2 + off, 4, &v);
    return v;
}

static void edge(int ti, mm_bool rising, mm_u64 late)
{
    stm32_timers_input_edge(&g_tim, 0, ti, rising, late);
}

/* Free-running TIM2 at one tick per cycle, CNT = 1000. */
static void start_counter(void)
{
    setup();
    wr(CR1, CEN);
    stm32_timers_tick(&g_tim, 1000u);
}

static int test_direct_rising_capture_is_backdated(void)
{
    start_counter();
    wr(CCMR1, 0x01u);
    wr(CCER, 0x1u);
    edge(1, MM_FALSE, 0u);
    if ((rd(SR) & CC1IF) != 0u) return 1;
    edge(1, MM_TRUE, 100u);
    if ((rd(SR) & CC1IF) == 0u) return 1;
    if (rd(CCR1) != 900u) return 1;
    if ((rd(SR) & CC1IF) != 0u) return 1;
    edge(2, MM_TRUE, 0u);
    if ((rd(SR) & CC1IF) != 0u) return 1;
    return 0;
}

static int test_polarity(void)
{
    start_counter();
    wr(CCMR1, 0x01u);
    wr(CCER, 0x3u);
    edge(1, MM_TRUE, 0u);
    if ((rd(SR) & CC1IF) != 0u) return 1;
    edge(1, MM_FALSE, 0u);
    if ((rd(SR) & CC1IF) == 0u) return 1;
    wr(SR, 0u);
    wr(CCER, 0xBu);
    edge(1, MM_TRUE, 0u);
    if ((rd(SR) & CC1IF) == 0u) return 1;
    wr(SR, 0u);
    edge(1, MM_FALSE, 0u);
    if ((rd(SR) & CC1IF) == 0u) return 1;
    return 0;
}

static int test_indirect_channel_listens_to_pair(void)
{
    start_counter();
    wr(CCMR1, 0x0200u);
    wr(CCER, 0x10u | 0x20u);
    edge(2, MM_FALSE, 0u);
    if ((rd(SR) & CC2IF) != 0u) return 1;
    edge(1, MM_FALSE, 10u);
    if ((rd(SR) & CC2IF) == 0u) return 1;
    if (rd(CCR2) != 990u) return 1;
    return 0;
}

static int test_input_prescaler(void)
{
    start_counter();
    wr(CCMR1, 0x01u | (1u << 2));
    wr(CCER, 0x1u);
    edge(1, MM_TRUE, 0u);
    if ((rd(SR) & CC1IF) != 0u) return 1;
    edge(1, MM_TRUE, 0u);
    if ((rd(SR) & CC1IF) == 0u) return 1;
    return 0;
}

static int test_overcapture_and_irq(void)
{
    start_counter();
    wr(CCMR1, 0x01u);
    wr(CCER, 0x1u);
    edge(1, MM_TRUE, 0u);
    if (mm_nvic_is_pending(&g_nvic, TIM2_IRQ)) return 1;
    edge(1, MM_TRUE, 0u);
    if ((rd(SR) & CC1OF) == 0u) return 1;
    wr(DIER, 1u << 1);
    edge(1, MM_TRUE, 0u);
    if (!mm_nvic_is_pending(&g_nvic, TIM2_IRQ)) return 1;
    return 0;
}

static int test_software_capture(void)
{
    start_counter();
    wr(CCMR1, 0x01u);
    wr(EGR, 1u << 1);
    if ((rd(SR) & CC1IF) == 0u) return 1;
    if (rd(CCR1) != 1000u) return 1;
    return 0;
}

static int test_ccr_writable_only_in_output_mode(void)
{
    setup();
    wr(CCR1, 123u);
    if (rd(CCR1) != 123u) return 1;
    wr(CCMR1, 0x01u);
    wr(CCR1, 456u);
    if (rd(CCR1) != 123u) return 1;
    return 0;
}

static int test_peek_read_keeps_flag(void)
{
    mm_u32 v = 0;
    start_counter();
    wr(CCMR1, 0x01u);
    wr(CCER, 0x1u);
    edge(1, MM_TRUE, 0u);
    mmio_set_peek_mode(MM_TRUE);
    (void)mmio_bus_read(&g_bus, TIM2 + CCR1, 4, &v);
    mmio_set_peek_mode(MM_FALSE);
    if ((rd(SR) & CC1IF) == 0u) return 1;
    return 0;
}

static int test_backdating_wraps_through_arr(void)
{
    setup();
    wr(ARR, 99u);
    wr(CR1, CEN);
    stm32_timers_tick(&g_tim, 105u);
    if (rd(CNT) != 5u) return 1;
    wr(CCMR1, 0x01u);
    wr(CCER, 0x1u);
    edge(1, MM_TRUE, 10u);
    if (rd(CCR1) != 95u) return 1;
    return 0;
}

static int test_backdating_down_counter(void)
{
    setup();
    wr(ARR, 99u);
    wr(CR1, CEN | DIR);
    wr(CNT, 50u);
    stm32_timers_tick(&g_tim, 10u);
    if (rd(CNT) != 40u) return 1;
    wr(CCMR1, 0x01u);
    wr(CCER, 0x1u);
    edge(1, MM_TRUE, 5u);
    if (rd(CCR1) != 45u) return 1;
    return 0;
}

static int test_backdating_with_prescaler(void)
{
    setup();
    wr(PSC, 3u);
    wr(CR1, CEN);
    stm32_timers_tick(&g_tim, 400u);
    if (rd(CNT) != 100u) return 1;
    wr(CCMR1, 0x01u);
    wr(CCER, 0x1u);
    edge(1, MM_TRUE, 10u);
    if (rd(CCR1) != 97u) return 1;
    return 0;
}

/* PWM input: CH1 captures the period on TI1 rising, CH2 the pulse on TI1
 * falling, and the TI1FP1 trigger restarts the counter. */
static int test_pwm_input_slave_reset(void)
{
    setup();
    wr(PSC, 3u);
    wr(CCMR1, 0x01u | 0x0200u);
    wr(CCER, 0x1u | 0x10u | 0x20u);
    wr(SMCR, 0x4u | (0x5u << 4));
    wr(CR1, CEN);
    stm32_timers_tick(&g_tim, 400u);
    edge(1, MM_TRUE, 10u);
    if (rd(CCR1) != 97u) return 1;
    if (rd(CNT) != 2u) return 1;
    if ((rd(SR) & (TIF | UIF)) != (TIF | UIF)) return 1;
    stm32_timers_tick(&g_tim, 2u);
    if (rd(CNT) != 3u) return 1;
    stm32_timers_tick(&g_tim, 120u);
    edge(1, MM_FALSE, 0u);
    if (rd(CCR2) != 33u) return 1;
    stm32_timers_tick(&g_tim, 80u);
    edge(1, MM_TRUE, 0u);
    if (rd(CCR1) != 53u) return 1;
    if (rd(CNT) != 0u) return 1;
    return 0;
}

static int test_slave_reset_respects_urs(void)
{
    setup();
    wr(CCER, 0x0u);
    wr(SMCR, 0x4u | (0x5u << 4));
    wr(CR1, CEN | (1u << 2));
    stm32_timers_tick(&g_tim, 50u);
    edge(1, MM_TRUE, 0u);
    if (rd(CNT) != 0u) return 1;
    if ((rd(SR) & UIF) != 0u || (rd(SR) & TIF) == 0u) return 1;
    return 0;
}

int main(void)
{
    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        { "direct_rising_capture_is_backdated", test_direct_rising_capture_is_backdated },
        { "polarity", test_polarity },
        { "indirect_channel_listens_to_pair", test_indirect_channel_listens_to_pair },
        { "input_prescaler", test_input_prescaler },
        { "overcapture_and_irq", test_overcapture_and_irq },
        { "software_capture", test_software_capture },
        { "ccr_writable_only_in_output_mode", test_ccr_writable_only_in_output_mode },
        { "peek_read_keeps_flag", test_peek_read_keeps_flag },
        { "backdating_wraps_through_arr", test_backdating_wraps_through_arr },
        { "backdating_down_counter", test_backdating_down_counter },
        { "backdating_with_prescaler", test_backdating_with_prescaler },
        { "pwm_input_slave_reset", test_pwm_input_slave_reset },
        { "slave_reset_respects_urs", test_slave_reset_respects_urs },
    };
    size_t i;
    int failures = 0;

    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        int rc = tests[i].fn();
        printf("%-45s %s\n", tests[i].name, rc == 0 ? "PASS" : "FAIL");
        if (rc != 0) failures++;
    }
    return (failures == 0) ? 0 : 1;
}
