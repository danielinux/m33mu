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

/* Output compare and PWM on TIM2: each output edge carries the cycle it
 * happens on inside a ticked batch. */

#include <stdio.h>
#include <string.h>
#include "stm32_timers.h"
#include "m33mu/gpio.h"

#define TIM2   0x40000000u
#define CR1    0x00u
#define DIER   0x0Cu
#define SR     0x10u
#define EGR    0x14u
#define CCMR1  0x18u
#define CCER   0x20u
#define CNT    0x24u
#define PSC    0x28u
#define ARR    0x2Cu
#define CCR1   0x34u
#define CCR2   0x38u

#define CEN    (1u << 0)
#define DIR    (1u << 4)
#define CC1IF  (1u << 1)

#define OCM(m) ((mm_u32)(m) << 4)
#define PWM1   6u
#define PWM2   7u

struct edge { int ch; mm_u8 level; mm_u64 cycle; };

static struct mmio_region g_regions[16];
static struct mmio_bus g_bus;
static struct mm_nvic g_nvic;
static struct stm32_timers_state g_tim;
static struct edge g_edges[256];
static int g_edge_count;
static mm_u64 g_now;

static void on_output(mm_u32 tim, int ch, mm_u8 level, mm_u64 cycles_ahead)
{
    if (tim != 0u || g_edge_count >= (int)(sizeof(g_edges) / sizeof(g_edges[0]))) return;
    g_edges[g_edge_count].ch = ch;
    g_edges[g_edge_count].level = level;
    g_edges[g_edge_count].cycle = g_now + cycles_ahead;
    g_edge_count++;
}

static const struct stm32_timer_soc g_soc = { 0u, 0, 0, 0, 0, on_output };

static void setup(void)
{
    memset(&g_tim, 0, sizeof(g_tim));
    mmio_bus_init(&g_bus, g_regions, sizeof(g_regions) / sizeof(g_regions[0]));
    mm_nvic_init(&g_nvic);
    stm32_timers_init(&g_tim, &g_soc, &g_bus, &g_nvic);
    g_edge_count = 0;
    g_now = 0u;
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

static void tick(mm_u64 cycles, mm_u64 batch)
{
    while (cycles > 0u) {
        mm_u64 n = cycles < batch ? cycles : batch;
        stm32_timers_tick(&g_tim, n);
        g_now += n;
        cycles -= n;
    }
}

static int expect(const struct edge *want, int n)
{
    int i;
    if (g_edge_count != n) {
        printf("  %d edges, want %d\n", g_edge_count, n);
        for (i = 0; i < g_edge_count; ++i) {
            printf("  ch%d %u @%llu\n", g_edges[i].ch, g_edges[i].level, (unsigned long long)g_edges[i].cycle);
        }
        return 1;
    }
    for (i = 0; i < n; ++i) {
        if (g_edges[i].ch != want[i].ch || g_edges[i].level != want[i].level ||
            g_edges[i].cycle != want[i].cycle) {
            printf("  edge %d: ch%d %u @%llu, want ch%d %u @%llu\n", i, g_edges[i].ch, g_edges[i].level,
                   (unsigned long long)g_edges[i].cycle, want[i].ch, want[i].level,
                   (unsigned long long)want[i].cycle);
            return 1;
        }
    }
    return 0;
}

/* 25 % duty, period 100 ticks: high at the update, low at the match. */
static int run_pwm1(mm_u64 batch)
{
    static const struct edge want[] = {
        { 1, 1, 0 }, { 1, 0, 25 }, { 1, 1, 100 }, { 1, 0, 125 }, { 1, 1, 200 }, { 1, 0, 225 },
    };
    setup();
    wr(ARR, 99u);
    wr(CCR1, 25u);
    wr(CCMR1, OCM(PWM1));
    wr(CCER, 0x1u);
    wr(CR1, CEN);
    tick(250u, batch);
    return expect(want, 6);
}

static int test_pwm1_edges_in_one_batch(void)
{
    return run_pwm1(1000u);
}

static int test_pwm1_edges_independent_of_batching(void)
{
    return run_pwm1(1u) || run_pwm1(7u) || run_pwm1(64u);
}

/* PSC = 3: tick k lands on cycle 4k counted from the counter start. */
static int test_prescaler_timing(void)
{
    static const struct edge want[] = { { 1, 1, 0 }, { 1, 0, 40 }, { 1, 1, 80 } };
    setup();
    wr(PSC, 3u);
    wr(ARR, 19u);
    wr(CCR1, 10u);
    wr(CCMR1, OCM(PWM1));
    wr(CCER, 0x1u);
    wr(CR1, CEN);
    tick(81u, 13u);
    return expect(want, 3);
}

/* PWM2 with active-low polarity is the same waveform as PWM1. */
static int test_pwm2_inverted_polarity(void)
{
    static const struct edge want[] = { { 1, 1, 0 }, { 1, 0, 25 }, { 1, 1, 100 } };
    setup();
    wr(ARR, 99u);
    wr(CCR1, 25u);
    wr(CCMR1, OCM(PWM2));
    wr(CCER, 0x1u | 0x2u);
    wr(CR1, CEN);
    tick(101u, 1000u);
    return expect(want, 3);
}

static int test_toggle_and_flag(void)
{
    static const struct edge want[] = { { 2, 0, 0 }, { 2, 1, 10 }, { 2, 0, 60 }, { 2, 1, 110 } };
    setup();
    wr(ARR, 49u);
    wr(CCR2, 10u);
    wr(CCMR1, OCM(3u) << 8);
    wr(CCER, 0x10u);
    wr(CR1, CEN);
    tick(120u, 1000u);
    if (expect(want, 4)) return 1;
    if ((rd(SR) & (1u << 2)) == 0u) return 1;
    return 0;
}

/* Force modes need no counter; clearing CCxE releases the pad. */
static int test_force_and_disable(void)
{
    static const struct edge want[] = { { 1, 1, 0 }, { 1, 0, 0 }, { 1, MM_GPIO_LEVEL_Z, 0 } };
    setup();
    wr(CCMR1, OCM(5u));
    wr(CCER, 0x1u);
    wr(CCMR1, OCM(4u));
    wr(CCER, 0x0u);
    return expect(want, 3);
}

/* 0 % and 100 % duty: CCR = 0 never goes high, CCR > ARR never goes low. */
static int test_duty_extremes(void)
{
    static const struct edge low[] = { { 1, 0, 0 } };
    static const struct edge high[] = { { 1, 1, 0 } };
    setup();
    wr(ARR, 99u);
    wr(CCR1, 0u);
    wr(CCMR1, OCM(PWM1));
    wr(CCER, 0x1u);
    wr(CR1, CEN);
    tick(500u, 33u);
    if (expect(low, 1)) return 1;
    setup();
    wr(ARR, 99u);
    wr(CCR1, 100u);
    wr(CCMR1, OCM(PWM1));
    wr(CCER, 0x1u);
    wr(CR1, CEN);
    tick(500u, 33u);
    return expect(high, 1);
}

/* Counting down, PWM1 is active while CNT <= CCR. */
static int test_downcount_pwm1(void)
{
    static const struct edge want[] = { { 1, 0, 0 }, { 1, 1, 74 }, { 1, 0, 100 }, { 1, 1, 174 } };
    setup();
    wr(ARR, 99u);
    wr(CR1, DIR);
    wr(EGR, 1u);
    wr(CCR1, 25u);
    wr(CCMR1, OCM(PWM1));
    wr(CCER, 0x1u);
    wr(CR1, CEN | DIR);
    tick(180u, 1000u);
    return expect(want, 4);
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "pwm1_edges_in_one_batch", test_pwm1_edges_in_one_batch },
        { "pwm1_edges_independent_of_batching", test_pwm1_edges_independent_of_batching },
        { "prescaler_timing", test_prescaler_timing },
        { "pwm2_inverted_polarity", test_pwm2_inverted_polarity },
        { "toggle_and_flag", test_toggle_and_flag },
        { "force_and_disable", test_force_and_disable },
        { "duty_extremes", test_duty_extremes },
        { "downcount_pwm1", test_downcount_pwm1 },
    };
    int failed = 0;
    size_t i;
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        int r = tests[i].fn();
        printf("%s %s\n", r ? "FAIL" : "ok", tests[i].name);
        failed |= r;
    }
    (void)CC1IF;
    (void)CNT;
    (void)DIER;
    return failed;
}
