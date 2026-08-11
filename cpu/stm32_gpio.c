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
#include "stm32_gpio.h"

static mm_u32 gpio_mask_to_2bit(mm_u32 mask)
{
    mm_u32 out = 0;
    int i;
    for (i = 0; i < 16; ++i) {
        if ((mask & (1u << i)) != 0u) {
            out |= (3u << (i * 2));
        }
    }
    return out;
}

void stm32_gpio_update_pin_af(struct stm32_gpio_state *g)
{
    mm_u32 afrl = g->regs[STM32_GPIO_AFRL_OFFSET / 4];
    mm_u32 afrh = g->regs[STM32_GPIO_AFRH_OFFSET / 4];
    int i;
    for (i = 0; i < 8; ++i) {
        g->pin_af[i] = (mm_u8)((afrl >> (i * 4)) & 0xFu);
        g->pin_af[i + 8] = (mm_u8)((afrh >> (i * 4)) & 0xFu);
    }
}

mm_u8 stm32_gpio_get_pin_mode(const struct stm32_gpio_state *g, int pin)
{
    mm_u32 moder = g->regs[STM32_GPIO_MODER_OFFSET / 4];
    return (mm_u8)((moder >> (pin * 2)) & 0x3u);
}

mm_u8 stm32_gpio_get_pin_af(const struct stm32_gpio_state *g, int pin)
{
    if (pin < 0 || pin >= 16) {
        return 0;
    }
    return g->pin_af[pin];
}

void stm32_gpio_reset(struct stm32_gpio_state *gpio, int bank)
{
    memset(gpio, 0, sizeof(*gpio));
    /* Default MODER: analog mode for all pins (0xFFFFFFFF) */
    gpio->regs[STM32_GPIO_MODER_OFFSET / 4] = 0xFFFFFFFFu;
    /* GPIOA: PA13=AF0(JTMS), PA14=AF0(JTCK), PA15=AF0(JTDI) */
    if (bank == 0) {
        gpio->regs[STM32_GPIO_MODER_OFFSET / 4] = 0xABFFFFFFu;
    }
    gpio->locked = MM_FALSE;
}

static void gpio_apply_brr(struct stm32_gpio_state *g, mm_u32 bits, mm_u32 mask)
{
    g->regs[STM32_GPIO_ODR_OFFSET / 4] &= ~(bits & mask);
}

static void gpio_apply_bsrr(struct stm32_gpio_state *g, mm_u32 val, mm_u32 mask)
{
    mm_u32 set = val & 0xFFFFu;
    mm_u32 reset = (val >> 16) & 0xFFFFu;
    mm_u32 odr = g->regs[STM32_GPIO_ODR_OFFSET / 4];
    odr |= (set & mask);
    odr &= ~(reset & mask);
    g->regs[STM32_GPIO_ODR_OFFSET / 4] = odr;
}

static void gpio_sync_odr(struct stm32_gpio_ctx *ctx, mm_u32 old_odr)
{
    mm_u32 new_odr = ctx->gpio->regs[STM32_GPIO_ODR_OFFSET / 4];
    if (new_odr != old_odr) {
        ctx->gpio->regs[STM32_GPIO_IDR_OFFSET / 4] = new_odr;
        if (ctx->exti_update) {
            ctx->exti_update(ctx->bank_index, old_odr, new_odr);
        }
    }
}

mm_bool stm32_gpio_set_external_input(struct stm32_gpio_ctx *ctx, int pin, mm_bool level)
{
    mm_u32 old_idr;
    mm_u32 new_idr;
    struct stm32_gpio_state *g = ctx->gpio;

    if (pin < 0 || pin >= 16) {
        return MM_FALSE;
    }
    /* Only pins actually configured as input (MODER == 0b00 for that pin)
     * accept an external drive; a pin in output/AF/analog mode is owned by
     * firmware's own ODR->IDR mirror (gpio_sync_odr) and must not be
     * clobbered here. */
    if (stm32_gpio_get_pin_mode(g, pin) != 0u) {
        return MM_FALSE;
    }

    old_idr = g->regs[STM32_GPIO_IDR_OFFSET / 4];
    new_idr = level ? (old_idr | (1u << pin)) : (old_idr & ~(1u << (mm_u32)pin));
    if (new_idr != old_idr) {
        g->regs[STM32_GPIO_IDR_OFFSET / 4] = new_idr;
        if (ctx->exti_update) {
            ctx->exti_update(ctx->bank_index, old_idr, new_idr);
        }
    }
    return MM_TRUE;
}

mm_bool stm32_gpio_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                        mm_u32 *value_out)
{
    struct stm32_gpio_ctx *ctx = (struct stm32_gpio_ctx *)opaque;
    struct stm32_gpio_state *g = ctx->gpio;
    mm_u32 seccfgr = g->regs[STM32_GPIO_SECCFGR_OFFSET / 4];
    mm_u32 v = 0;
    mm_u32 mask = ~seccfgr; /* pins accessible to NS */

    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (offset >= STM32_GPIO_REG_SIZE) return MM_FALSE;

    /* On STM32, GPIO registers remain readable even when the clock is gated. */

    if (offset < sizeof(g->regs)) {
        memcpy(&v, (mm_u8 *)g->regs + offset, size_bytes);
    }

    if (!ctx->is_secure_alias) {
        if (offset == STM32_GPIO_SECCFGR_OFFSET) {
            v = 0;
        } else if (offset == STM32_GPIO_IDR_OFFSET || offset == STM32_GPIO_ODR_OFFSET ||
                   offset == STM32_GPIO_BSRR_OFFSET || offset == STM32_GPIO_LCKR_OFFSET ||
                   offset == STM32_GPIO_AFRL_OFFSET || offset == STM32_GPIO_AFRH_OFFSET ||
                   offset == STM32_GPIO_BRR_OFFSET || offset == STM32_GPIO_HSLVR_OFFSET) {
            v &= mask;
        } else if (offset == STM32_GPIO_MODER_OFFSET || offset == STM32_GPIO_OSPEEDR_OFFSET ||
                   offset == STM32_GPIO_PUPDR_OFFSET) {
            mm_u32 m2 = gpio_mask_to_2bit(mask & 0xFFFFu);
            v &= m2;
        } else if (offset == STM32_GPIO_OTYPER_OFFSET) {
            v &= mask;
        }
    }

    *value_out = v;
    return MM_TRUE;
}

mm_bool stm32_gpio_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 value)
{
    struct stm32_gpio_ctx *ctx = (struct stm32_gpio_ctx *)opaque;
    struct stm32_gpio_state *g = ctx->gpio;
    mm_u32 seccfgr = g->regs[STM32_GPIO_SECCFGR_OFFSET / 4];
    mm_u32 mask = ~seccfgr;
    static mm_u32 lock_seq = 0;

    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if (offset >= STM32_GPIO_REG_SIZE) return MM_FALSE;

    if (ctx->clock_enabled && !ctx->clock_enabled(ctx->bank_index)) {
        return MM_TRUE; /* WI if disabled */
    }

    /* Non-secure alias write handling */
    if (!ctx->is_secure_alias) {
        if (offset == STM32_GPIO_SECCFGR_OFFSET) {
            return MM_TRUE; /* SECCFGR is Secure-only: WI */
        }
        if (offset == STM32_GPIO_BSRR_OFFSET) {
            mm_u32 old_odr = g->regs[STM32_GPIO_ODR_OFFSET / 4];
            gpio_apply_bsrr(g, value, mask & 0xFFFFu);
            gpio_sync_odr(ctx, old_odr);
            return MM_TRUE;
        }
        if (offset == STM32_GPIO_BRR_OFFSET) {
            mm_u32 old_odr = g->regs[STM32_GPIO_ODR_OFFSET / 4];
            gpio_apply_brr(g, value & 0xFFFFu, mask & 0xFFFFu);
            gpio_sync_odr(ctx, old_odr);
            return MM_TRUE;
        }
        if (offset == STM32_GPIO_AFRL_OFFSET || offset == STM32_GPIO_AFRH_OFFSET) {
            mm_u32 af_mask = 0;
            int base_pin = (offset == STM32_GPIO_AFRL_OFFSET) ? 0 : 8;
            int i;
            for (i = 0; i < 8; ++i) {
                if ((mask & (1u << (base_pin + i))) != 0u) {
                    af_mask |= (0xFu << (i * 4));
                }
            }
            value &= af_mask;
            g->regs[offset / 4] = (g->regs[offset / 4] & ~af_mask) | value;
            stm32_gpio_update_pin_af(g);
            return MM_TRUE;
        }
        if (offset == STM32_GPIO_MODER_OFFSET || offset == STM32_GPIO_OSPEEDR_OFFSET ||
            offset == STM32_GPIO_PUPDR_OFFSET) {
            mm_u32 m2 = gpio_mask_to_2bit(mask & 0xFFFFu);
            value &= m2;
            g->regs[offset / 4] = (g->regs[offset / 4] & ~m2) | value;
            return MM_TRUE;
        }
        if (offset == STM32_GPIO_OTYPER_OFFSET) {
            value &= mask;
            g->regs[offset / 4] = (g->regs[offset / 4] & ~mask) | value;
            return MM_TRUE;
        }
    }

    if (offset == STM32_GPIO_BSRR_OFFSET) {
        mm_u32 old_odr = g->regs[STM32_GPIO_ODR_OFFSET / 4];
        gpio_apply_bsrr(g, value, 0xFFFFu);
        gpio_sync_odr(ctx, old_odr);
        return MM_TRUE;
    }
    if (offset == STM32_GPIO_BRR_OFFSET) {
        mm_u32 old_odr = g->regs[STM32_GPIO_ODR_OFFSET / 4];
        gpio_apply_brr(g, value & 0xFFFFu, 0xFFFFu);
        gpio_sync_odr(ctx, old_odr);
        return MM_TRUE;
    }
    if (offset == STM32_GPIO_ODR_OFFSET) {
        mm_u32 old_odr = g->regs[STM32_GPIO_ODR_OFFSET / 4];
        g->regs[offset / 4] = value;
        gpio_sync_odr(ctx, old_odr);
        return MM_TRUE;
    }
    if (offset == STM32_GPIO_AFRL_OFFSET || offset == STM32_GPIO_AFRH_OFFSET) {
        g->regs[offset / 4] = value;
        stm32_gpio_update_pin_af(g);
        return MM_TRUE;
    }
    if (offset == STM32_GPIO_LCKR_OFFSET) {
        mm_u32 lckk = (value >> 16) & 1u;
        if (g->locked) {
            return MM_TRUE; /* Already locked, ignore writes */
        }
        if (lock_seq == 0 && lckk == 1u) {
            lock_seq = 1;
        } else if (lock_seq == 1 && lckk == 0u) {
            lock_seq = 2;
        } else if (lock_seq == 2 && lckk == 1u) {
            lock_seq = 3;
            g->locked = MM_TRUE;
            g->regs[STM32_GPIO_LCKR_OFFSET / 4] = value | (1u << 16);
            return MM_TRUE;
        } else {
            lock_seq = 0;
        }
        g->regs[STM32_GPIO_LCKR_OFFSET / 4] = value;
        return MM_TRUE;
    }

    if (offset < sizeof(g->regs)) {
        g->regs[offset / 4] = value;
    }
    return MM_TRUE;
}
