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

#ifndef M33MU_CPU_STM32H533_CONFIG_H
#define M33MU_CPU_STM32H533_CONFIG_H

/* STM32H533 memory map (Secure / Non-secure aliases) */
#include "m33mu/target.h"

#define STM32H533_FLASH_BASE_S   0x0C000000u
#define STM32H533_FLASH_BASE_NS  0x08000000u
#define STM32H533_FLASH_SIZE     0x00080000u  /* 512 KB */

#define STM32H533_RAM_BASE_S     0x30000000u
#define STM32H533_RAM_BASE_NS    0x20000000u
#define STM32H533_RAM_SIZE       0x00044000u  /* 272 KB (SRAM1/2/3) */

#define STM32H533_PERIPH_BASE_S  0x50000000u
#define STM32H533_PERIPH_BASE_NS 0x40000000u

static const struct mm_ram_region STM32H533_RAM_REGIONS[] = {
    { 0x0A000000u, 0x0A000000u, 0x00020000u, -1 }, /* TCM 128 KB */
    { 0x30000000u, 0x20000000u, 0x00020000u, 0 }, /* SRAM1 128 KB */
    { 0x30020000u, 0x20020000u, 0x00014000u, 1 }, /* SRAM2 80 KB */
    { 0x30034000u, 0x20034000u, 0x00010000u, 2 }, /* SRAM3 64 KB */
    { 0x50036400u, 0x40036400u, 0x00000800u, -1 } /* BKPSRAM 2 KB */
};

#define STM32H533_RAM_REGION_COUNT (sizeof(STM32H533_RAM_REGIONS) / sizeof(STM32H533_RAM_REGIONS[0]))
#define STM32H533_MPCBB_BLOCK_SIZE 512u

#define STM32H533_SOC_RESET      mm_stm32h533_mmio_reset
#define STM32H533_SOC_REGISTER   mm_stm32h533_register_mmio
#define STM32H533_FLASH_BIND     mm_stm32h533_flash_bind
#define STM32H533_CLOCK_GET_HZ   mm_stm32h533_cpu_hz
#define STM32H533_USART_INIT     mm_stm32h533_usart_init
#define STM32H533_USART_RESET    mm_stm32h533_usart_reset
#define STM32H533_USART_POLL     mm_stm32h533_usart_poll

#define STM32H533_SPI_INIT       mm_stm32h533_spi_init
#define STM32H533_SPI_RESET      mm_stm32h533_spi_reset
#define STM32H533_SPI_POLL       mm_stm32h533_spi_poll

#define STM32H533_ETH_INIT       mm_stm32h533_eth_init
#define STM32H533_ETH_RESET      mm_stm32h533_eth_reset
#define STM32H533_ETH_POLL       mm_stm32h533_eth_poll

#define STM32H533_TIMER_INIT  mm_stm32h533_timers_init
#define STM32H533_TIMER_RESET mm_stm32h533_timers_reset
#define STM32H533_TIMER_TICK  mm_stm32h533_timers_tick

#define STM32H533_FLAGS (MM_TARGET_FLAG_NVM_WRITEONCE | MM_TARGET_FLAG_FPU)

#endif /* M33MU_CPU_STM32H533_CONFIG_H */
