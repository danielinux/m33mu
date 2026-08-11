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

#ifndef M33MU_STM32_GPIO_H
#define M33MU_STM32_GPIO_H

#include "m33mu/types.h"
#include "m33mu/mmio.h"

/* GPIO register offsets (common to STM32H5/L5/U5) */
#define STM32_GPIO_MODER_OFFSET   0x00u
#define STM32_GPIO_OTYPER_OFFSET  0x04u
#define STM32_GPIO_OSPEEDR_OFFSET 0x08u
#define STM32_GPIO_PUPDR_OFFSET   0x0Cu
#define STM32_GPIO_IDR_OFFSET     0x10u
#define STM32_GPIO_ODR_OFFSET     0x14u
#define STM32_GPIO_BSRR_OFFSET    0x18u
#define STM32_GPIO_LCKR_OFFSET    0x1Cu
#define STM32_GPIO_AFRL_OFFSET    0x20u
#define STM32_GPIO_AFRH_OFFSET    0x24u
#define STM32_GPIO_BRR_OFFSET     0x28u
#define STM32_GPIO_HSLVR_OFFSET   0x2Cu
#define STM32_GPIO_SECCFGR_OFFSET 0x30u
#define STM32_GPIO_REG_SIZE       0x400u

struct stm32_gpio_state {
    mm_u32 regs[STM32_GPIO_REG_SIZE / 4];
    mm_u8  pin_af[16]; /* computed AF per pin (0-15) */
    mm_bool locked;    /* LCKR lock sequence completed */
};

/* Context structure passed to read/write functions */
struct stm32_gpio_ctx {
    struct stm32_gpio_state *gpio;
    mm_bool is_secure_alias;
    int bank_index;
    mm_bool (*clock_enabled)(int bank);
    void (*exti_update)(int bank, mm_u32 old_level, mm_u32 new_level);
};

void stm32_gpio_reset(struct stm32_gpio_state *gpio, int bank);
void stm32_gpio_update_pin_af(struct stm32_gpio_state *g);
mm_u8 stm32_gpio_get_pin_mode(const struct stm32_gpio_state *g, int pin);
mm_u8 stm32_gpio_get_pin_af(const struct stm32_gpio_state *g, int pin);

mm_bool stm32_gpio_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                        mm_u32 *value_out);
mm_bool stm32_gpio_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 value);

/* External-signal-injection entry point (src/signal.c). Drives pin's IDR
 * bit and fans out through the existing EXTI path exactly like a real
 * pin transition would, but only if the pin is currently configured as
 * input; returns MM_FALSE (no-op) otherwise so a pin briefly repurposed
 * as output mid-test cannot have its ODR-driven IDR value clobbered by a
 * stale binding. */
mm_bool stm32_gpio_set_external_input(struct stm32_gpio_ctx *ctx, int pin, mm_bool level);

#endif /* M33MU_STM32_GPIO_H */
