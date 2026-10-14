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
#include "stm32_gpio.h"
#include "m33mu/gpio.h"

#define PIN 5

static void set_pin_mode(struct stm32_gpio_state *g, int pin, mm_u8 mode)
{
    mm_u32 moder = g->regs[STM32_GPIO_MODER_OFFSET / 4];
    moder &= ~(0x3u << (pin * 2));
    moder |= ((mm_u32)mode & 0x3u) << (pin * 2);
    g->regs[STM32_GPIO_MODER_OFFSET / 4] = moder;
}

static mm_u32 read_idr(struct stm32_gpio_ctx *ctx)
{
    mm_u32 v = 0;
    stm32_gpio_read(ctx, STM32_GPIO_IDR_OFFSET, 4, &v);
    return v;
}

static void init_ctx(struct stm32_gpio_state *g, struct stm32_gpio_ctx *ctx)
{
    memset(g, 0, sizeof(*g));
    memset(ctx, 0, sizeof(*ctx));
    stm32_gpio_reset(g, 0);
    ctx->gpio = g;
    ctx->is_secure_alias = MM_FALSE;
    ctx->bank_index = 0;
    ctx->clock_enabled = 0;
    ctx->exti_update = 0;
}

static int test_write_guard_all_non_input_modes(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;
    mm_u8 modes[3] = { 1u, 2u, 3u };
    int i;

    init_ctx(&g, &ctx);
    set_pin_mode(&g, PIN, 0u);
    if (!stm32_gpio_set_external_input(&ctx, PIN, MM_TRUE)) return 1;
    if ((read_idr(&ctx) & (1u << PIN)) == 0u) return 1;

    for (i = 0; i < 3; ++i) {
        init_ctx(&g, &ctx);
        set_pin_mode(&g, PIN, modes[i]);
        if (stm32_gpio_set_external_input(&ctx, PIN, MM_TRUE)) return 1;
        if ((read_idr(&ctx) & (1u << PIN)) != 0u) return 1;
    }
    return 0;
}

static int test_analog_mode_forces_idr_zero(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;

    init_ctx(&g, &ctx);
    set_pin_mode(&g, PIN, 0u);
    if (!stm32_gpio_set_external_input(&ctx, PIN, MM_TRUE)) return 1;
    if ((read_idr(&ctx) & (1u << PIN)) == 0u) return 1;

    set_pin_mode(&g, PIN, 3u);
    if ((g.regs[STM32_GPIO_IDR_OFFSET / 4] & (1u << PIN)) == 0u) return 1;
    if ((read_idr(&ctx) & (1u << PIN)) != 0u) return 1;

    set_pin_mode(&g, PIN, 0u);
    if ((read_idr(&ctx) & (1u << PIN)) == 0u) return 1;

    return 0;
}

static int test_analog_mask_is_per_pin(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;

    init_ctx(&g, &ctx);
    set_pin_mode(&g, 3, 0u);
    set_pin_mode(&g, PIN, 0u);
    if (!stm32_gpio_set_external_input(&ctx, 3, MM_TRUE)) return 1;
    if (!stm32_gpio_set_external_input(&ctx, PIN, MM_TRUE)) return 1;

    set_pin_mode(&g, PIN, 3u);

    if ((read_idr(&ctx) & (1u << 3)) == 0u) return 1;
    if ((read_idr(&ctx) & (1u << PIN)) != 0u) return 1;
    return 0;
}

static int g_exti_calls;
static mm_u32 g_exti_old, g_exti_new;

static void fake_exti_update(int bank, mm_u32 old_level, mm_u32 new_level)
{
    (void)bank;
    g_exti_calls++;
    g_exti_old = old_level;
    g_exti_new = new_level;
}

static void write_moder_mode(struct stm32_gpio_ctx *ctx, int pin, mm_u8 mode)
{
    mm_u32 moder = ctx->gpio->regs[STM32_GPIO_MODER_OFFSET / 4];
    moder &= ~(0x3u << (pin * 2));
    moder |= ((mm_u32)mode & 0x3u) << (pin * 2);
    stm32_gpio_write(ctx, STM32_GPIO_MODER_OFFSET, 4, moder);
}

static int test_input_mode_picks_up_pad_level(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;
    mm_u8 modes[3] = { 1u, 2u, 3u };
    int i;

    for (i = 0; i < 3; ++i) {
        init_ctx(&g, &ctx);
        ctx.exti_update = fake_exti_update;
        write_moder_mode(&ctx, PIN, modes[i]);
        if (stm32_gpio_set_external_input(&ctx, PIN, MM_TRUE)) return 1;
        if ((read_idr(&ctx) & (1u << PIN)) != 0u) return 1;

        g_exti_calls = 0;
        write_moder_mode(&ctx, PIN, 0u);
        if ((read_idr(&ctx) & (1u << PIN)) == 0u) return 1;
        if (g_exti_calls != 1) return 1;
        if ((g_exti_old & (1u << PIN)) != 0u || (g_exti_new & (1u << PIN)) == 0u) return 1;
    }
    return 0;
}

static int test_odr_write_keeps_driven_input(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;

    init_ctx(&g, &ctx);
    write_moder_mode(&ctx, PIN, 0u);
    write_moder_mode(&ctx, 6, 1u);
    if (!stm32_gpio_set_external_input(&ctx, PIN, MM_TRUE)) return 1;

    stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 1u << 6);
    if ((read_idr(&ctx) & (1u << PIN)) == 0u) return 1;
    if ((read_idr(&ctx) & (1u << 6)) == 0u) return 1;

    stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 0u);
    if ((read_idr(&ctx) & (1u << PIN)) == 0u) return 1;
    if ((read_idr(&ctx) & (1u << 6)) != 0u) return 1;
    return 0;
}

static int test_output_mode_reads_back_odr(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;
    mm_u8 modes[2] = { 1u, 2u };
    int i;

    for (i = 0; i < 2; ++i) {
        init_ctx(&g, &ctx);
        write_moder_mode(&ctx, PIN, 0u);
        if (!stm32_gpio_set_external_input(&ctx, PIN, MM_TRUE)) return 1;
        if ((read_idr(&ctx) & (1u << PIN)) == 0u) return 1;

        write_moder_mode(&ctx, PIN, modes[i]);
        if ((read_idr(&ctx) & (1u << PIN)) != 0u) return 1;

        write_moder_mode(&ctx, PIN, 0u);
        if ((read_idr(&ctx) & (1u << PIN)) == 0u) return 1;
    }
    return 0;
}

#define PAD_EVENTS_MAX 64

static int g_pad_count;
static int g_pad_bank[PAD_EVENTS_MAX];
static int g_pad_pin[PAD_EVENTS_MAX];
static mm_u8 g_pad_level[PAD_EVENTS_MAX];

static void fake_pad_listener(void *opaque, int bank, int pin, mm_u8 level)
{
    (void)opaque;
    if (g_pad_count < PAD_EVENTS_MAX) {
        g_pad_bank[g_pad_count] = bank;
        g_pad_pin[g_pad_count] = pin;
        g_pad_level[g_pad_count] = level;
    }
    g_pad_count++;
}

static int pad_event_is(int idx, int pin, mm_u8 level)
{
    return (idx < g_pad_count && g_pad_bank[idx] == 0 && g_pad_pin[idx] == pin &&
            g_pad_level[idx] == level) ? 1 : 0;
}

static int test_pad_events_follow_odr(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;
    int rc = 1;

    init_ctx(&g, &ctx);
    mm_gpio_set_pad_listener(fake_pad_listener, 0);
    g_pad_count = 0;
    write_moder_mode(&ctx, PIN, 1u);
    if (g_pad_count != 1 || !pad_event_is(0, PIN, 0u)) goto out;
    stm32_gpio_write(&ctx, STM32_GPIO_BSRR_OFFSET, 4, 1u << PIN);
    if (g_pad_count != 2 || !pad_event_is(1, PIN, 1u)) goto out;
    stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 1u << PIN);
    if (g_pad_count != 2) goto out;
    stm32_gpio_write(&ctx, STM32_GPIO_BRR_OFFSET, 4, 1u << PIN);
    if (g_pad_count != 3 || !pad_event_is(2, PIN, 0u)) goto out;
    stm32_gpio_write(&ctx, STM32_GPIO_BSRR_OFFSET, 4, 1u << (PIN + 16));
    if (g_pad_count != 3) goto out;
    rc = 0;
out:
    mm_gpio_set_pad_listener(0, 0);
    return rc;
}

static int test_pad_events_open_drain_and_modes(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;
    int rc = 1;

    init_ctx(&g, &ctx);
    mm_gpio_set_pad_listener(fake_pad_listener, 0);
    g_pad_count = 0;
    stm32_gpio_write(&ctx, STM32_GPIO_OTYPER_OFFSET, 4, 1u << PIN);
    stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 1u << PIN);
    if (g_pad_count != 0) goto out;
    write_moder_mode(&ctx, PIN, 1u);
    if (g_pad_count != 0) goto out;
    stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 0u);
    if (g_pad_count != 1 || !pad_event_is(0, PIN, 0u)) goto out;
    stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 1u << PIN);
    if (g_pad_count != 2 || !pad_event_is(1, PIN, MM_GPIO_LEVEL_Z)) goto out;
    stm32_gpio_write(&ctx, STM32_GPIO_OTYPER_OFFSET, 4, 0u);
    if (g_pad_count != 3 || !pad_event_is(2, PIN, 1u)) goto out;
    write_moder_mode(&ctx, PIN, 2u);
    if (g_pad_count != 4 || !pad_event_is(3, PIN, MM_GPIO_LEVEL_Z)) goto out;
    write_moder_mode(&ctx, PIN, 0u);
    if (g_pad_count != 4) goto out;
    rc = 0;
out:
    mm_gpio_set_pad_listener(0, 0);
    return rc;
}

static int test_pad_events_on_reset(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;
    int rc = 1;
    int pin;

    init_ctx(&g, &ctx);
    write_moder_mode(&ctx, PIN, 1u);
    stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 1u << PIN);
    mm_gpio_set_pad_listener(fake_pad_listener, 0);
    g_pad_count = 0;
    stm32_gpio_reset(&g, 0);
    if (g_pad_count != 16) goto out;
    for (pin = 0; pin < 16; ++pin) {
        if (!pad_event_is(pin, pin, MM_GPIO_LEVEL_Z)) goto out;
    }
    rc = 0;
out:
    mm_gpio_set_pad_listener(0, 0);
    return rc;
}

int main(void)
{
    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        { "write_guard_all_non_input_modes", test_write_guard_all_non_input_modes },
        { "analog_mode_forces_idr_zero", test_analog_mode_forces_idr_zero },
        { "analog_mask_is_per_pin", test_analog_mask_is_per_pin },
        { "input_mode_picks_up_pad_level", test_input_mode_picks_up_pad_level },
        { "odr_write_keeps_driven_input", test_odr_write_keeps_driven_input },
        { "output_mode_reads_back_odr", test_output_mode_reads_back_odr },
        { "pad_events_follow_odr", test_pad_events_follow_odr },
        { "pad_events_open_drain_and_modes", test_pad_events_open_drain_and_modes },
        { "pad_events_on_reset", test_pad_events_on_reset },
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
