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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 */

#include <stdint.h>

#define NVIC_ICTR    (*(volatile uint32_t *)0xE000E004u)
#define NVIC_ISER(n) (*(volatile uint32_t *)(0xE000E100u + 4u * (n)))
#define NVIC_ISPR(n) (*(volatile uint32_t *)(0xE000E200u + 4u * (n)))
#define NVIC_IPR(n)  (*(volatile uint8_t *)(0xE000E400u + (n)))

#define ADC3_IRQ    149u
#define OTG_HS_IRQ  161u

#define SEEN_ADC3   (1u << 0)
#define SEEN_OTG_HS (1u << 1)

static volatile uint32_t irq_seen;

void ADC3_IRQHandler(void)
{
    irq_seen |= SEEN_ADC3;
}

void OTG_HS_IRQHandler(void)
{
    irq_seen |= SEEN_OTG_HS;
}

static void fail(void)
{
    __asm volatile("bkpt #0x01");
    while (1) {
    }
}

static void fire_irq(uint32_t irq, uint32_t seen_bit)
{
    uint32_t spins = 0u;

    NVIC_IPR(irq) = 0x80u;
    NVIC_ISER(irq / 32u) = 1u << (irq % 32u);
    NVIC_ISPR(irq / 32u) = 1u << (irq % 32u);
    __asm volatile("dsb\n isb");
    while ((irq_seen & seen_bit) == 0u) {
        if (++spins > 100000u) {
            fail();
        }
    }
}

void Reset_Handler(void)
{
    irq_seen = 0u;
    if ((NVIC_ICTR & 0xFu) < (OTG_HS_IRQ / 32u)) {
        fail();
    }
    fire_irq(ADC3_IRQ, SEEN_ADC3);
    fire_irq(OTG_HS_IRQ, SEEN_OTG_HS);
    __asm volatile("bkpt #0x7f");
    while (1) {
    }
}
