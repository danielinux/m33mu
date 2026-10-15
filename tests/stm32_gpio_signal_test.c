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

static void write_moder_mode(struct stm32_gpio_ctx *ctx, int pin, mm_u8 mode)
{
    mm_u32 moder = ctx->gpio->regs[STM32_GPIO_MODER_OFFSET / 4];
    moder &= ~(0x3u << (pin * 2));
    moder |= ((mm_u32)mode & 0x3u) << (pin * 2);
    stm32_gpio_write(ctx, STM32_GPIO_MODER_OFFSET, 4, moder);
}

static void write_pupdr(struct stm32_gpio_ctx *ctx, int pin, mm_u32 pull)
{
    mm_u32 pupdr = ctx->gpio->regs[STM32_GPIO_PUPDR_OFFSET / 4];
    pupdr &= ~(0x3u << (pin * 2));
    pupdr |= (pull & 0x3u) << (pin * 2);
    stm32_gpio_write(ctx, STM32_GPIO_PUPDR_OFFSET, 4, pupdr);
}

static int idr_bit(struct stm32_gpio_ctx *ctx, int pin)
{
    mm_u32 v = 0;
    stm32_gpio_read(ctx, STM32_GPIO_IDR_OFFSET, 4, &v);
    return (int)((v >> pin) & 1u);
}

static int test_write_guard_per_mode(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;
    mm_u8 modes[4] = { 0u, 1u, 2u, 3u };
    mm_bool visible[4] = { MM_TRUE, MM_FALSE, MM_TRUE, MM_FALSE };
    int i;

    for (i = 0; i < 4; ++i) {
        init_ctx(&g, &ctx);
        set_pin_mode(&g, PIN, modes[i]);
        if (stm32_gpio_set_external_input(&ctx, PIN, MM_TRUE) != visible[i]) return 1;
        if (((read_idr(&ctx) & (1u << PIN)) != 0u) != (visible[i] != MM_FALSE)) return 1;
    }
    return 0;
}

static int test_analog_mode_forces_idr_zero(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;

    init_ctx(&g, &ctx);
    write_moder_mode(&ctx, PIN, 0u);
    if (!stm32_gpio_set_external_input(&ctx, PIN, MM_TRUE)) return 1;
    if ((read_idr(&ctx) & (1u << PIN)) == 0u) return 1;

    write_moder_mode(&ctx, PIN, 3u);
    if ((g.regs[STM32_GPIO_IDR_OFFSET / 4] & (1u << PIN)) != 0u) return 1;
    if ((read_idr(&ctx) & (1u << PIN)) != 0u) return 1;
    write_pupdr(&ctx, PIN, 1u);
    stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 1u << PIN);
    if (stm32_gpio_set_external_input(&ctx, PIN, MM_TRUE)) return 1;
    if ((g.regs[STM32_GPIO_IDR_OFFSET / 4] & (1u << PIN)) != 0u) return 1;
    if ((read_idr(&ctx) & (1u << PIN)) != 0u) return 1;

    write_moder_mode(&ctx, PIN, 0u);
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

static int test_input_mode_picks_up_pad_level(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;
    mm_u8 modes[2] = { 1u, 3u };
    int i;

    for (i = 0; i < 2; ++i) {
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

    init_ctx(&g, &ctx);
    write_moder_mode(&ctx, PIN, 0u);
    if (!stm32_gpio_set_external_input(&ctx, PIN, MM_TRUE)) return 1;
    if (idr_bit(&ctx, PIN) != 1) return 1;

    write_moder_mode(&ctx, PIN, 1u);
    if (idr_bit(&ctx, PIN) != 0) return 1;

    write_moder_mode(&ctx, PIN, 0u);
    if (idr_bit(&ctx, PIN) != 1) return 1;
    return 0;
}

static int test_pulled_input_to_output_reads_odr(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;

    init_ctx(&g, &ctx);
    ctx.exti_update = fake_exti_update;
    write_moder_mode(&ctx, PIN, 0u);
    write_pupdr(&ctx, PIN, 1u);
    if (idr_bit(&ctx, PIN) != 1) return 1;
    stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 0u);
    if (idr_bit(&ctx, PIN) != 1) return 1;

    g_exti_calls = 0;
    write_moder_mode(&ctx, PIN, 1u);
    if (idr_bit(&ctx, PIN) != 0) return 1;
    if (g_exti_calls != 1) return 1;
    if ((g_exti_old & (1u << PIN)) == 0u || (g_exti_new & (1u << PIN)) != 0u) return 1;
    return 0;
}

static int test_odr_written_while_input_shows_on_output(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;

    init_ctx(&g, &ctx);
    write_moder_mode(&ctx, PIN, 0u);
    stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 1u << PIN);
    if (idr_bit(&ctx, PIN) != 0) return 1;
    write_moder_mode(&ctx, PIN, 1u);
    if (idr_bit(&ctx, PIN) != 1) return 1;
    return 0;
}

static int test_af_pin_follows_injection(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;

    init_ctx(&g, &ctx);
    ctx.exti_update = fake_exti_update;
    write_moder_mode(&ctx, PIN, 2u);
    stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 1u << PIN);
    if (idr_bit(&ctx, PIN) != 1) return 1;
    stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 0u);
    if (idr_bit(&ctx, PIN) != 0) return 1;

    g_exti_calls = 0;
    if (!stm32_gpio_set_external_input(&ctx, PIN, MM_TRUE)) return 1;
    if (idr_bit(&ctx, PIN) != 1) return 1;
    if (g_exti_calls != 1) return 1;
    if ((g_exti_old & (1u << PIN)) != 0u || (g_exti_new & (1u << PIN)) == 0u) return 1;

    if (!stm32_gpio_set_external_input(&ctx, PIN, MM_FALSE)) return 1;
    if (idr_bit(&ctx, PIN) != 0) return 1;
    if (g_exti_calls != 2) return 1;
    stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 1u << PIN);
    if (idr_bit(&ctx, PIN) != 0) return 1;
    return 0;
}

static int test_open_drain_released_follows_pad(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;

    init_ctx(&g, &ctx);
    write_moder_mode(&ctx, PIN, 1u);
    stm32_gpio_write(&ctx, STM32_GPIO_OTYPER_OFFSET, 4, 1u << PIN);
    if (stm32_gpio_set_external_input(&ctx, PIN, MM_TRUE)) return 1;
    if (idr_bit(&ctx, PIN) != 0) return 1;
    stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 1u << PIN);
    if (idr_bit(&ctx, PIN) != 1) return 1;
    if (!stm32_gpio_set_external_input(&ctx, PIN, MM_FALSE)) return 1;
    if (idr_bit(&ctx, PIN) != 0) return 1;

    init_ctx(&g, &ctx);
    write_moder_mode(&ctx, PIN, 1u);
    stm32_gpio_write(&ctx, STM32_GPIO_OTYPER_OFFSET, 4, 1u << PIN);
    stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 1u << PIN);
    if (idr_bit(&ctx, PIN) != 1) return 1;
    write_pupdr(&ctx, PIN, 1u);
    if (idr_bit(&ctx, PIN) != 1) return 1;
    write_pupdr(&ctx, PIN, 2u);
    if (idr_bit(&ctx, PIN) != 1) return 1;
    stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 0u);
    if (idr_bit(&ctx, PIN) != 0) return 1;
    stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 1u << PIN);
    stm32_gpio_write(&ctx, STM32_GPIO_OTYPER_OFFSET, 4, 0u);
    if (idr_bit(&ctx, PIN) != 1) return 1;
    return 0;
}

static int test_push_pull_ignores_injection(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;

    init_ctx(&g, &ctx);
    write_moder_mode(&ctx, PIN, 1u);
    stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 1u << PIN);
    if (stm32_gpio_set_external_input(&ctx, PIN, MM_FALSE)) return 1;
    if (idr_bit(&ctx, PIN) != 1) return 1;
    stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 0u);
    if (stm32_gpio_set_external_input(&ctx, PIN, MM_TRUE)) return 1;
    if (idr_bit(&ctx, PIN) != 0) return 1;
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

static int test_input_reads_its_pull(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;
    mm_bool secure;

    for (secure = MM_FALSE; secure <= MM_TRUE; ++secure) {
        init_ctx(&g, &ctx);
        ctx.is_secure_alias = secure;
        write_moder_mode(&ctx, PIN, 0u);
        if ((read_idr(&ctx) & (1u << PIN)) != 0u) return 1;
        write_pupdr(&ctx, PIN, 1u);
        if ((read_idr(&ctx) & (1u << PIN)) == 0u) return 1;
        write_pupdr(&ctx, PIN, 2u);
        if ((read_idr(&ctx) & (1u << PIN)) != 0u) return 1;
        write_pupdr(&ctx, PIN, 3u);
        if ((read_idr(&ctx) & (1u << PIN)) != 0u) return 1;
        write_pupdr(&ctx, PIN, 1u);
        stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 0u);
        if ((read_idr(&ctx) & (1u << PIN)) == 0u) return 1;
        write_pupdr(&ctx, PIN, 0u);
        stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 1u << PIN);
        if ((read_idr(&ctx) & (1u << PIN)) != 0u) return 1;
    }
    return 0;
}

static int test_injection_beats_pull(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;

    init_ctx(&g, &ctx);
    write_moder_mode(&ctx, PIN, 0u);
    write_pupdr(&ctx, PIN, 1u);
    if (!stm32_gpio_set_external_input(&ctx, PIN, MM_FALSE)) return 1;
    if ((read_idr(&ctx) & (1u << PIN)) != 0u) return 1;
    write_pupdr(&ctx, PIN, 2u);
    if (!stm32_gpio_set_external_input(&ctx, PIN, MM_TRUE)) return 1;
    if ((read_idr(&ctx) & (1u << PIN)) == 0u) return 1;
    return 0;
}

static int test_pull_write_raises_exti_edge(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;

    init_ctx(&g, &ctx);
    ctx.exti_update = fake_exti_update;
    write_moder_mode(&ctx, PIN, 0u);
    g_exti_calls = 0;
    write_pupdr(&ctx, PIN, 1u);
    if (g_exti_calls != 1) return 1;
    if ((g_exti_old & (1u << PIN)) != 0u || (g_exti_new & (1u << PIN)) == 0u) return 1;
    write_pupdr(&ctx, PIN, 1u);
    if (g_exti_calls != 1) return 1;
    write_pupdr(&ctx, PIN, 2u);
    if (g_exti_calls != 2) return 1;
    if ((g_exti_old & (1u << PIN)) == 0u || (g_exti_new & (1u << PIN)) != 0u) return 1;
    return 0;
}

static int test_pull_ignored_outside_input_mode(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;
    mm_u8 modes[3] = { 1u, 2u, 3u };
    int i;

    for (i = 0; i < 3; ++i) {
        init_ctx(&g, &ctx);
        write_moder_mode(&ctx, PIN, modes[i]);
        write_pupdr(&ctx, PIN, 1u);
        if ((read_idr(&ctx) & (1u << PIN)) != 0u) return 1;
        write_moder_mode(&ctx, PIN, 0u);
        if ((read_idr(&ctx) & (1u << PIN)) == 0u) return 1;
    }
    return 0;
}

static int test_mode_and_pull_decode_all_pins(void)
{
    struct stm32_gpio_state g;
    struct stm32_gpio_ctx ctx;

    init_ctx(&g, &ctx);
    stm32_gpio_write(&ctx, STM32_GPIO_MODER_OFFSET, 4, 0u);
    stm32_gpio_write(&ctx, STM32_GPIO_PUPDR_OFFSET, 4, 0x55555555u);
    if (read_idr(&ctx) != 0xFFFFu) return 1;
    stm32_gpio_write(&ctx, STM32_GPIO_PUPDR_OFFSET, 4, 0x9999AAAAu);
    if (read_idr(&ctx) != 0x5500u) return 1;
    stm32_gpio_write(&ctx, STM32_GPIO_ODR_OFFSET, 4, 0xA5A5u);
    stm32_gpio_write(&ctx, STM32_GPIO_MODER_OFFSET, 4, 0x55555555u);
    if (read_idr(&ctx) != 0xA5A5u) return 1;
    stm32_gpio_write(&ctx, STM32_GPIO_MODER_OFFSET, 4, 0xAAAAAAAAu);
    if (read_idr(&ctx) != 0xA5A5u) return 1;
    stm32_gpio_write(&ctx, STM32_GPIO_MODER_OFFSET, 4, 0x5555FFFFu);
    if (read_idr(&ctx) != 0xA500u) return 1;
    return 0;
}

static int test_reset_values(void)
{
    struct stm32_gpio_state g;

    stm32_gpio_reset(&g, 0);
    if (g.regs[STM32_GPIO_MODER_OFFSET / 4] != 0xABFFFFFFu) return 1;
    if (g.regs[STM32_GPIO_PUPDR_OFFSET / 4] != 0x64000000u) return 1;
    stm32_gpio_reset(&g, 1);
    if (g.regs[STM32_GPIO_MODER_OFFSET / 4] != 0xFFFFFEBFu) return 1;
    if (g.regs[STM32_GPIO_PUPDR_OFFSET / 4] != 0x00000100u) return 1;
    stm32_gpio_reset(&g, 2);
    if (g.regs[STM32_GPIO_MODER_OFFSET / 4] != 0xFFFFFFFFu) return 1;
    if (g.regs[STM32_GPIO_PUPDR_OFFSET / 4] != 0u) return 1;
    return 0;
}

int main(void)
{
    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        { "write_guard_per_mode", test_write_guard_per_mode },
        { "analog_mode_forces_idr_zero", test_analog_mode_forces_idr_zero },
        { "analog_mask_is_per_pin", test_analog_mask_is_per_pin },
        { "input_mode_picks_up_pad_level", test_input_mode_picks_up_pad_level },
        { "odr_write_keeps_driven_input", test_odr_write_keeps_driven_input },
        { "output_mode_reads_back_odr", test_output_mode_reads_back_odr },
        { "pulled_input_to_output_reads_odr", test_pulled_input_to_output_reads_odr },
        { "odr_written_while_input_shows_on_output", test_odr_written_while_input_shows_on_output },
        { "af_pin_follows_injection", test_af_pin_follows_injection },
        { "open_drain_released_follows_pad", test_open_drain_released_follows_pad },
        { "push_pull_ignores_injection", test_push_pull_ignores_injection },
        { "pad_events_follow_odr", test_pad_events_follow_odr },
        { "pad_events_open_drain_and_modes", test_pad_events_open_drain_and_modes },
        { "pad_events_on_reset", test_pad_events_on_reset },
        { "input_reads_its_pull", test_input_reads_its_pull },
        { "injection_beats_pull", test_injection_beats_pull },
        { "pull_write_raises_exti_edge", test_pull_write_raises_exti_edge },
        { "pull_ignored_outside_input_mode", test_pull_ignored_outside_input_mode },
        { "mode_and_pull_decode_all_pins", test_mode_and_pull_decode_all_pins },
        { "reset_values", test_reset_values },
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
