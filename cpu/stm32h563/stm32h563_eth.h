/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef M33MU_STM32H563_ETH_H
#define M33MU_STM32H563_ETH_H

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "m33mu/types.h"

mm_bool mm_stm32h563_eth_register_mmio(struct mmio_bus *bus);
void mm_stm32h563_eth_set_nvic(struct mm_nvic *nvic);
void mm_stm32h563_eth_init(struct mmio_bus *bus, struct mm_nvic *nvic);
void mm_stm32h563_eth_reset(void);
void mm_stm32h563_eth_poll(void);
mm_bool mm_stm32h563_eth_get_mac(mm_u8 mac[6]);

#endif /* M33MU_STM32H563_ETH_H */
