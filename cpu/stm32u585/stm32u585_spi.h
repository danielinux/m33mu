/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef M33MU_STM32U585_SPI_H
#define M33MU_STM32U585_SPI_H

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"

void mm_stm32u585_spi_init(struct mmio_bus *bus, struct mm_nvic *nvic);
void mm_stm32u585_spi_poll(void);
void mm_stm32u585_spi_reset(void);

#endif /* M33MU_STM32U585_SPI_H */
