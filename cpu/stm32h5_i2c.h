/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef M33MU_STM32H5_I2C_H
#define M33MU_STM32H5_I2C_H

#include <stddef.h>

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"

/* count is how many I2C instances the part carries, counting from I2C1;
 * H533 has three, H563 and H5F4 four. Returns MM_FALSE if a region could
 * not be registered. */
mm_bool stm32h5_i2c_init(struct mmio_bus *bus, struct mm_nvic *nvic, size_t count);
void stm32h5_i2c_reset(void);

#endif /* M33MU_STM32H5_I2C_H */
