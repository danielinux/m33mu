/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef M33MU_STM32H5F4_I2C_H
#define M33MU_STM32H5F4_I2C_H

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"

void mm_stm32h5f4_i2c_init(struct mmio_bus *bus, struct mm_nvic *nvic);
void mm_stm32h5f4_i2c_reset(void);

#endif /* M33MU_STM32H5F4_I2C_H */
