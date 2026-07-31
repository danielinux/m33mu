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

#ifndef M33MU_CPU_STM32H563_CONFIG_H
#define M33MU_CPU_STM32H563_CONFIG_H

/* STM32H563 memory map (Secure / Non-secure aliases) */
#include "m33mu/target.h"

#define STM32H563_FLASH_BASE_S   0x0C000000u
#define STM32H563_FLASH_BASE_NS  0x08000000u
#define STM32H563_FLASH_SIZE     0x00200000u  /* 2 MB */

#define STM32H563_RAM_BASE_S     0x30000000u
#define STM32H563_RAM_BASE_NS    0x20000000u
#define STM32H563_RAM_SIZE       0x000A0000u  /* 640 KB */

#define STM32H563_PERIPH_BASE_S  0x50000000u
#define STM32H563_PERIPH_BASE_NS 0x40000000u

/* Every entry here gets its own slice of the RAM backing store
 * (mm_memmap_configure_ram assigns each one a running offset), so two
 * entries never alias each other however their addresses relate. */
static const struct mm_ram_region STM32H563_RAM_REGIONS[] = {
    /* Scratch block, not modelled silicon: the STM32H5 has no TCM and no
     * memory at 0x0A000000. Independent storage, not an alias of the
     * SRAM banks below. */
    { 0x0A000000u, 0x0A000000u, 0x00020000u, -1 },
    { 0x30000000u, 0x20000000u, 0x00040000u, 0 }, /* SRAM1 256 KB */
    { 0x30040000u, 0x20040000u, 0x00010000u, 1 }, /* SRAM2 64 KB */
    { 0x30050000u, 0x20050000u, 0x00050000u, 2 }  /* SRAM3 320 KB */
};

#define STM32H563_RAM_REGION_COUNT (sizeof(STM32H563_RAM_REGIONS) / sizeof(STM32H563_RAM_REGIONS[0]))
#define STM32H563_MPCBB_BLOCK_SIZE 512u

#define STM32H563_SOC_RESET      mm_stm32h563_mmio_reset
#define STM32H563_SOC_REGISTER   mm_stm32h563_register_mmio
#define STM32H563_FLASH_BIND     mm_stm32h563_flash_bind
#define STM32H563_CLOCK_GET_HZ   mm_stm32h563_cpu_hz
#define STM32H563_USART_INIT     mm_stm32h563_usart_init
#define STM32H563_USART_RESET    mm_stm32h563_usart_reset
#define STM32H563_USART_POLL     mm_stm32h563_usart_poll

#define STM32H563_SPI_INIT       mm_stm32h563_spi_init
#define STM32H563_SPI_RESET      mm_stm32h563_spi_reset
#define STM32H563_SPI_POLL       mm_stm32h563_spi_poll

#define STM32H563_ETH_INIT       stm32h5_eth_init
#define STM32H563_ETH_RESET      stm32h5_eth_reset
#define STM32H563_ETH_POLL       stm32h5_eth_poll

#define STM32H563_TIMER_INIT  mm_stm32h563_timers_init
#define STM32H563_TIMER_RESET mm_stm32h563_timers_reset
#define STM32H563_TIMER_TICK  mm_stm32h563_timers_tick
#define STM32H563_TZ_ATTR     mm_stm32h563_tz_attr_for_addr

#define STM32H563_FLAGS (MM_TARGET_FLAG_NVM_WRITEONCE | MM_TARGET_FLAG_FPU)

#endif /* M33MU_CPU_STM32H563_CONFIG_H */
