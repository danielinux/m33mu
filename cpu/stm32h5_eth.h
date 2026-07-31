/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef M33MU_STM32H5_ETH_H
#define M33MU_STM32H5_ETH_H

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "m33mu/types.h"

/* Ethernet MAC shared by the STM32H5 parts that carry one (H533, H563,
 * H5F4). The block sits at the same address on all of them and differs
 * only in which RCC register bank gates its clocks, which the caller
 * supplies at registration time. */

mm_bool stm32h5_eth_register_mmio(struct mmio_bus *bus, mm_u32 *rcc_regs);
void stm32h5_eth_set_nvic(struct mm_nvic *nvic);
void stm32h5_eth_init(struct mmio_bus *bus, struct mm_nvic *nvic);
void stm32h5_eth_reset(void);
void stm32h5_eth_poll(void);
mm_bool stm32h5_eth_get_mac(mm_u8 mac[6]);

#endif /* M33MU_STM32H5_ETH_H */
