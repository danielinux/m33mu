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

#ifndef M33MU_STM32H5_ADC_H
#define M33MU_STM32H5_ADC_H

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "m33mu/types.h"
#include "stm32_adc.h"

/* ADC1/2 are the same block on every H5 part; ADC3 exists only on some.
 * Reset and tick do nothing before the first init. */
void stm32h5_adc_init(struct mmio_bus *bus, struct mm_nvic *nvic);
void stm32h5_adc_init_with_adc3(struct mmio_bus *bus, struct mm_nvic *nvic);
void stm32h5_adc_reset(void);
void stm32h5_adc_tick(mm_u64 cycles);
mm_u64 stm32h5_adc_cycles_until_event(void);
const struct stm32_adc_state *stm32h5_adc_state(void);
const struct stm32_adc_state *stm32h5_adc3_state(void);

#endif /* M33MU_STM32H5_ADC_H */
